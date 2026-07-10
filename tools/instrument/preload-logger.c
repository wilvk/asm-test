/*
 * preload-logger.c — an LD_PRELOAD value-trace prototype (MVP Step 2).
 *
 * See docs/internal/analysis/capture-args-returns.md. This is the "LD_PRELOAD /
 * wrapper library" approach: we interpose a small default set of libc I/O
 * functions, call the real symbol, and log the entry arguments, the return
 * value, and (bounded, best-effort) the pointed-to buffer as JSON-lines. It is
 * the fastest path to rich, semantic value traces without ptrace or a DBI
 * harness — build it, point LD_PRELOAD at it, run the target.
 *
 * Wrapped default set (matches the analysis doc's "small default set"):
 *     read, write, send, recv, fread, fwrite
 *
 * Each traced call emits TWO JSON-line records so entry and exit are distinct:
 *   {"phase":"enter", ...}   arguments (and input buffers for write/send/fwrite)
 *   {"phase":"return", ...}  return value (and output buffers for read/recv/fread)
 * The two records of one call share a monotonic "call" id (per process). Fields:
 *   phase, call, ts_ns (CLOCK_MONOTONIC), pid, tid, func,
 *   caller (return-address / call-site IP), args{...},
 *   ret (return phase), and — where a buffer is in scope —
 *   data_total (bytes the call names), data_len (bytes we captured),
 *   data_truncated (bool), data_hex (lowercase hex of the captured bytes).
 *
 * Configuration (environment):
 *   ASMTEST_VALTRACE_DIR      output directory        (default: build/traces)
 *   ASMTEST_VALTRACE_MAX_BUF  max bytes per buffer    (default: 4096, cap 65536)
 *   ASMTEST_VALTRACE_SAMPLE   log 1 in every N calls  (default: 1 = all)
 *   ASMTEST_VALTRACE_FUNCS    comma-list allow-filter (default: all wrapped)
 * Output file: <dir>/valtrace-<pid>.jsonl (append; one per process, fork-aware).
 *
 * Usage:
 *   make preload-logger
 *   LD_PRELOAD=$PWD/build/preload-logger.so ./your_program
 *   cat build/traces/valtrace-*.jsonl
 *
 * Safety notes (per the doc's "Safety & privacy" / "never trust pointer
 * validity"): pointed-to memory is read through process_vm_readv() against our
 * OWN pid, so an unmapped/short buffer yields a short capture instead of a
 * SIGSEGV; reads are hard-capped by ASMTEST_VALTRACE_MAX_BUF. Captured bytes can
 * contain secrets — treat the traces as sensitive and run in an isolated lab.
 *
 * Limitations (prototype): Linux only (process_vm_readv, /proc semantics); only
 * dynamic-symbol calls are seen (a static-linked or raw-syscall caller is
 * invisible); the wrapped set is fixed at build time — adding a function means
 * adding a wrapper here and rebuilding (ASMTEST_VALTRACE_FUNCS only *filters*
 * the built-in set). The emit path serializes lines with a mutex (so a line is
 * never torn or interleaved) and guards reentrancy with a thread-local flag: a
 * wrapped call made from a signal handler that fires *while this thread is
 * mid-emit* skips logging instead of deadlocking on the mutex or corrupting the
 * in-flight line. Cross-thread contention just serializes — it does not drop
 * lines, so enter/return records stay paired.
 */
#define _GNU_SOURCE

#ifdef __linux__

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define EXPORT   __attribute__((visibility("default")))
#define HARD_CAP ((size_t)65536) /* absolute ceiling on a per-buffer capture */

/* ------------------------------------------------------------------ */
/* Configuration + output state (set once under g_once, reopened on fork).      */
/* ------------------------------------------------------------------ */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_emit_lock = PTHREAD_MUTEX_INITIALIZER;

static char g_dirbuf[4096];
static const char *g_dir = "build/traces";
static size_t g_max_buf = 4096;
static uint64_t g_sample = 1;
static char g_funcs_norm[512]; /* ",read,write,..," or "" for "all allowed" */

static int g_fd = -1;    /* output file descriptor (append)                  */
static pid_t g_pid = -1; /* pid the fd was opened for (fork detection)        */
static int g_ready = 0;  /* 1 once the output is open and logging is live     */
static uint64_t g_calls; /* monotonic call counter (sampling + pairing id)    */

/* ------------------------------------------------------------------ */
/* Real-symbol resolution (RTLD_NEXT: the definitions *after* this .so).        */
/* The emit path uses real_write() so our own logging never re-enters the       */
/* write() wrapper.                                                            */
/* ------------------------------------------------------------------ */
typedef ssize_t (*read_fn_t)(int, void *, size_t);
typedef ssize_t (*write_fn_t)(int, const void *, size_t);
typedef ssize_t (*send_fn_t)(int, const void *, size_t, int);
typedef ssize_t (*recv_fn_t)(int, void *, size_t, int);
typedef size_t (*fread_fn_t)(void *, size_t, size_t, FILE *);
typedef size_t (*fwrite_fn_t)(const void *, size_t, size_t, FILE *);

#define REAL(type, name)                                                       \
    static type real_##name(void) {                                            \
        static type f;                                                         \
        if (f == NULL)                                                         \
            f = (type)dlsym(RTLD_NEXT, #name);                                 \
        return f;                                                              \
    }
REAL(read_fn_t, read)
REAL(write_fn_t, write)
REAL(send_fn_t, send)
REAL(recv_fn_t, recv)
REAL(fread_fn_t, fread)
REAL(fwrite_fn_t, fwrite)
#undef REAL

/* ------------------------------------------------------------------ */
/* Small utilities                                                              */
/* ------------------------------------------------------------------ */
static __thread int tl_tid; /* cached gettid() (reset in the fork child)     */
static __thread int
    tl_in_emit; /* this thread is mid-emit (reentrancy guard)   */

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

static int get_tid(void) {
    if (tl_tid == 0)
        tl_tid = (int)syscall(SYS_gettid);
    return tl_tid;
}

/* Bounded, fault-tolerant read of our own memory: process_vm_readv against our
 * own pid returns a short count (or -EFAULT) for an unmapped/short buffer rather
 * than crashing. Falls back to memcpy only when the syscall is unavailable. */
static size_t safe_copy(void *dst, const void *src, size_t n) {
    if (src == NULL || n == 0)
        return 0;
    struct iovec local = {dst, n};
    struct iovec remote = {(void *)(uintptr_t)src, n};
    ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    if (got >= 0)
        return (size_t)got;
    if (errno == ENOSYS) {
        memcpy(dst, src, n); /* best effort where the syscall is blocked */
        return n;
    }
    return 0;
}

static void write_all(int fd, const char *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = real_write()(fd, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (w == 0)
            break;
        off += (size_t)w;
    }
}

/* ------------------------------------------------------------------ */
/* Per-thread string builder for one JSON line + a scratch capture buffer.      */
/* ------------------------------------------------------------------ */
static __thread char *g_sb;
static __thread size_t g_sbcap;
static __thread size_t g_sblen;
static __thread unsigned char *tl_raw;
static __thread size_t tl_rawcap;

static const char HEX[] = "0123456789abcdef";

static int sb_reserve(size_t extra) {
    if (g_sblen + extra <= g_sbcap)
        return 1;
    size_t ncap = g_sbcap ? g_sbcap : 1024;
    while (ncap < g_sblen + extra)
        ncap *= 2;
    char *p = realloc(g_sb, ncap);
    if (p == NULL)
        return 0;
    g_sb = p;
    g_sbcap = ncap;
    return 1;
}

static void sb_reset(void) { g_sblen = 0; }

static void sb_putc(char c) {
    if (sb_reserve(1))
        g_sb[g_sblen++] = c;
}

static void sb_write(const char *s, size_t n) {
    if (sb_reserve(n)) {
        memcpy(g_sb + g_sblen, s, n);
        g_sblen += n;
    }
}

static void sb_puts(const char *s) { sb_write(s, strlen(s)); }

static void sb_printf(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) {
        va_end(ap);
        return;
    }
    if (sb_reserve((size_t)n + 1)) {
        vsnprintf(g_sb + g_sblen, (size_t)n + 1, fmt, ap);
        g_sblen += (size_t)n;
    }
    va_end(ap);
}

/* Append the buffer-capture fields for a pointer naming `total` bytes. */
static void sb_data_fields(const void *ptr, unsigned long long total) {
    size_t want =
        total > (unsigned long long)g_max_buf ? g_max_buf : (size_t)total;
    size_t got = 0;
    if (want > 0) {
        if (tl_rawcap < want) {
            unsigned char *p = realloc(tl_raw, want);
            if (p != NULL) {
                tl_raw = p;
                tl_rawcap = want;
            } else {
                want = tl_rawcap;
            }
        }
        if (want > 0)
            got = safe_copy(tl_raw, ptr, want);
    }
    int trunc = total > (unsigned long long)got;
    sb_printf(",\"data_total\":%llu,\"data_len\":%zu,\"data_truncated\":%s,"
              "\"data_hex\":\"",
              total, got, trunc ? "true" : "false");
    if (sb_reserve(2 * got)) {
        for (size_t i = 0; i < got; i++) {
            unsigned char b = tl_raw[i];
            g_sb[g_sblen++] = HEX[b >> 4];
            g_sb[g_sblen++] = HEX[b & 0x0f];
        }
    }
    sb_putc('"');
}

/* ------------------------------------------------------------------ */
/* Init / output-file management                                                */
/* ------------------------------------------------------------------ */
static void build_funcs_filter(const char *f) {
    g_funcs_norm[0] = '\0';
    if (f == NULL || *f == '\0')
        return; /* empty filter => everything allowed */
    size_t o = 0;
    g_funcs_norm[o++] = ',';
    for (const char *p = f; *p != '\0'; ++p) {
        char c = *p;
        if (c == ' ' || c == '\t')
            continue;
        if (c == ',') {
            if (o > 0 && g_funcs_norm[o - 1] != ',')
                g_funcs_norm[o++] = ',';
        } else if (o < sizeof(g_funcs_norm) - 2) {
            g_funcs_norm[o++] = c;
        }
    }
    if (o > 0 && g_funcs_norm[o - 1] != ',')
        g_funcs_norm[o++] = ',';
    g_funcs_norm[o] = '\0';
}

static int func_allowed(const char *fn) {
    if (g_funcs_norm[0] == '\0')
        return 1;
    char needle[24];
    snprintf(needle, sizeof needle, ",%s,", fn);
    return strstr(g_funcs_norm, needle) != NULL;
}

static void read_config(void) {
    const char *d = getenv("ASMTEST_VALTRACE_DIR");
    if (d != NULL && *d != '\0') {
        snprintf(g_dirbuf, sizeof g_dirbuf, "%s", d);
        g_dir = g_dirbuf;
    }
    const char *m = getenv("ASMTEST_VALTRACE_MAX_BUF");
    if (m != NULL && *m != '\0')
        g_max_buf = (size_t)strtoul(m, NULL, 0);
    if (g_max_buf > HARD_CAP)
        g_max_buf = HARD_CAP;
    const char *s = getenv("ASMTEST_VALTRACE_SAMPLE");
    if (s != NULL && *s != '\0') {
        long sv = strtol(s, NULL, 0);
        g_sample = sv < 1 ? 1 : (uint64_t)sv;
    }
    build_funcs_filter(getenv("ASMTEST_VALTRACE_FUNCS"));
}

static void mkdir_p(const char *path) {
    char tmp[sizeof g_dirbuf];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t n = strlen(tmp);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755); /* ignore EEXIST */
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void open_output(pid_t pid) {
    mkdir_p(g_dir);
    char path[4200];
    snprintf(path, sizeof path, "%s/valtrace-%d.jsonl", g_dir, (int)pid);
    g_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    g_pid = pid;
}

static void reopen_for_pid(pid_t pid) {
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    open_output(pid);
}

/* After fork the child inherits the parent's fd + a possibly-locked emit mutex,
 * and this (surviving) thread's cached tid is stale. Reset all three; the file
 * reopens lazily on the next emit (g_pid mismatch). */
static void atfork_child(void) {
    tl_tid = 0;
    tl_in_emit = 0; /* fork may have interrupted our own emit on this thread */
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    g_pid = -1;
    pthread_mutex_init(&g_emit_lock, NULL);
}

static void init_once(void) {
    read_config();
    pthread_atfork(NULL, NULL, atfork_child);
    open_output(getpid());
    g_ready = (g_fd >= 0);
    if (!g_ready)
        fprintf(stderr,
                "[preload-logger] disabled: cannot open output under '%s'\n",
                g_dir);
}

static int config_ready(void) {
    pthread_once(&g_once, init_once);
    return g_ready;
}

/* Decide whether this call is logged; on yes, hand back its pairing id. Also
 * the single place the sampling counter advances. Reentrant calls (a wrapped
 * function invoked from a signal handler that interrupted our own emit on this
 * thread) are declined so they neither re-lock the mutex nor clobber the
 * per-thread line buffer. */
static int should_log(const char *fn, uint64_t *out_call) {
    if (tl_in_emit)
        return 0;
    if (!config_ready())
        return 0;
    if (!func_allowed(fn))
        return 0;
    uint64_t id = __atomic_add_fetch(&g_calls, 1, __ATOMIC_RELAXED);
    if (g_sample > 1 && (id % g_sample) != 0)
        return 0;
    *out_call = id;
    return 1;
}

/* Start a JSON line into the per-thread builder (leaves it open for more fields
 * plus the closing "}\n"). pid is read live so the fork child is labelled right
 * before its file is reopened. */
static void line_head(const char *phase, uint64_t call, const char *func,
                      void *caller) {
    sb_reset();
    sb_printf("{\"phase\":\"%s\",\"call\":%llu,\"ts_ns\":%lld,\"pid\":%d,"
              "\"tid\":%d,\"func\":\"%s\",\"caller\":\"0x%llx\"",
              phase, (unsigned long long)call, now_ns(), (int)getpid(),
              get_tid(), func, (unsigned long long)(uintptr_t)caller);
}

/* Close and flush the current line as one atomic write under the emit mutex, so
 * concurrent threads never interleave a line. Only ever reached with tl_in_emit
 * set (the wrapper guards it), so a same-thread signal handler cannot re-enter
 * and self-deadlock on the mutex. */
static void line_flush(void) {
    sb_puts("}\n");
    if (!config_ready())
        return;
    pthread_mutex_lock(&g_emit_lock);
    pid_t pid = getpid();
    if (pid != g_pid)
        reopen_for_pid(pid);
    if (g_fd >= 0)
        write_all(g_fd, g_sb, g_sblen);
    pthread_mutex_unlock(&g_emit_lock);
}

static int fd_of(FILE *stream) {
    if (stream == NULL)
        return -1;
    int fd = fileno(stream);
    return fd; /* -1 already on a non-file stream */
}

/* Claim the reentrancy guard for a to-be-logged call (so the real() call and
 * both phases run with tl_in_emit set); log_end releases it. A reentrant call
 * short-circuits inside should_log and never reaches log_end, so it cannot clear
 * the outer call's guard. */
static int log_begin(const char *fn, uint64_t *call) {
    if (!should_log(fn, call))
        return 0;
    tl_in_emit = 1;
    return 1;
}

static void log_end(void) { tl_in_emit = 0; }

/* ------------------------------------------------------------------ */
/* Interposed wrappers. Pattern: log enter -> call real (save errno) -> log      */
/* return -> restore errno. Input buffers (write/send/fwrite) are captured at    */
/* enter; output buffers (read/recv/fread) at return.                           */
/* ------------------------------------------------------------------ */

EXPORT ssize_t read(int fd, void *buf, size_t count) {
    read_fn_t real = real_read();
    uint64_t call = 0;
    int log = log_begin("read", &call);
    void *ra = __builtin_return_address(0);
    if (log) {
        line_head("enter", call, "read", ra);
        sb_printf(",\"args\":{\"fd\":%d,\"buf\":\"0x%llx\",\"count\":%zu}", fd,
                  (unsigned long long)(uintptr_t)buf, count);
        line_flush();
    }
    ssize_t ret = real(fd, buf, count);
    int saved = errno;
    if (log) {
        line_head("return", call, "read", ra);
        sb_printf(",\"ret\":%lld", (long long)ret);
        if (ret > 0)
            sb_data_fields(buf, (unsigned long long)ret);
        line_flush();
        log_end();
    }
    errno = saved;
    return ret;
}

EXPORT ssize_t write(int fd, const void *buf, size_t count) {
    write_fn_t real = real_write();
    uint64_t call = 0;
    int log = log_begin("write", &call);
    void *ra = __builtin_return_address(0);
    if (log) {
        line_head("enter", call, "write", ra);
        sb_printf(",\"args\":{\"fd\":%d,\"buf\":\"0x%llx\",\"count\":%zu}", fd,
                  (unsigned long long)(uintptr_t)buf, count);
        sb_data_fields(buf, (unsigned long long)count);
        line_flush();
    }
    ssize_t ret = real(fd, buf, count);
    int saved = errno;
    if (log) {
        line_head("return", call, "write", ra);
        sb_printf(",\"ret\":%lld", (long long)ret);
        line_flush();
        log_end();
    }
    errno = saved;
    return ret;
}

EXPORT ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    send_fn_t real = real_send();
    uint64_t call = 0;
    int log = log_begin("send", &call);
    void *ra = __builtin_return_address(0);
    if (log) {
        line_head("enter", call, "send", ra);
        sb_printf(",\"args\":{\"fd\":%d,\"buf\":\"0x%llx\",\"len\":%zu,"
                  "\"flags\":%d}",
                  sockfd, (unsigned long long)(uintptr_t)buf, len, flags);
        sb_data_fields(buf, (unsigned long long)len);
        line_flush();
    }
    ssize_t ret = real(sockfd, buf, len, flags);
    int saved = errno;
    if (log) {
        line_head("return", call, "send", ra);
        sb_printf(",\"ret\":%lld", (long long)ret);
        line_flush();
        log_end();
    }
    errno = saved;
    return ret;
}

EXPORT ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    recv_fn_t real = real_recv();
    uint64_t call = 0;
    int log = log_begin("recv", &call);
    void *ra = __builtin_return_address(0);
    if (log) {
        line_head("enter", call, "recv", ra);
        sb_printf(",\"args\":{\"fd\":%d,\"buf\":\"0x%llx\",\"len\":%zu,"
                  "\"flags\":%d}",
                  sockfd, (unsigned long long)(uintptr_t)buf, len, flags);
        line_flush();
    }
    ssize_t ret = real(sockfd, buf, len, flags);
    int saved = errno;
    if (log) {
        line_head("return", call, "recv", ra);
        sb_printf(",\"ret\":%lld", (long long)ret);
        if (ret > 0)
            sb_data_fields(buf, (unsigned long long)ret);
        line_flush();
        log_end();
    }
    errno = saved;
    return ret;
}

EXPORT size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    fread_fn_t real = real_fread();
    uint64_t call = 0;
    int log = log_begin("fread", &call);
    void *ra = __builtin_return_address(0);
    if (log) {
        line_head("enter", call, "fread", ra);
        sb_printf(",\"args\":{\"ptr\":\"0x%llx\",\"size\":%zu,\"nmemb\":%zu,"
                  "\"stream\":\"0x%llx\",\"fd\":%d}",
                  (unsigned long long)(uintptr_t)ptr, size, nmemb,
                  (unsigned long long)(uintptr_t)stream, fd_of(stream));
        line_flush();
    }
    size_t ret = real(ptr, size, nmemb, stream);
    int saved = errno;
    if (log) {
        line_head("return", call, "fread", ra);
        sb_printf(",\"ret\":%zu", ret);
        if (ret > 0 && size > 0)
            sb_data_fields(ptr, (unsigned long long)ret * size);
        line_flush();
        log_end();
    }
    errno = saved;
    return ret;
}

EXPORT size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    fwrite_fn_t real = real_fwrite();
    uint64_t call = 0;
    int log = log_begin("fwrite", &call);
    void *ra = __builtin_return_address(0);
    if (log) {
        line_head("enter", call, "fwrite", ra);
        sb_printf(",\"args\":{\"ptr\":\"0x%llx\",\"size\":%zu,\"nmemb\":%zu,"
                  "\"stream\":\"0x%llx\",\"fd\":%d}",
                  (unsigned long long)(uintptr_t)ptr, size, nmemb,
                  (unsigned long long)(uintptr_t)stream, fd_of(stream));
        sb_data_fields(ptr, (unsigned long long)size * nmemb);
        line_flush();
    }
    size_t ret = real(ptr, size, nmemb, stream);
    int saved = errno;
    if (log) {
        line_head("return", call, "fwrite", ra);
        sb_printf(",\"ret\":%zu", ret);
        line_flush();
        log_end();
    }
    errno = saved;
    return ret;
}

#else /* !__linux__ */

/* The prototype relies on process_vm_readv() + /proc semantics; it is Linux
 * only. Compiling elsewhere yields an empty translation unit (the Makefile
 * gates the target to Linux). */
typedef int asmtest_preload_logger_not_supported;

#endif /* __linux__ */
