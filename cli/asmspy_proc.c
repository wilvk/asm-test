/*
 * asmspy_proc.c — /proc process enumeration + an ELF function-symbol resolver.
 *
 * The library resolves JIT methods (perf-map / jitdump) and module extents
 * (/proc/<pid>/maps) but has no reader for ordinary ELF .symtab/.dynsym symbols,
 * so asmspy carries its own: for every ELF file mapped into the target it reads
 * the STT_FUNC symbols and offsets each by that module's load bias, yielding a
 * runtime-address function table used both to PICK a function to trace (forward,
 * by name) and to name the callees in the call-graph view (reverse, by address).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "asmspy.h"

/* ================================================================== */
/* Process list                                                        */
/* ================================================================== */

static int is_all_digits(const char *s) {
    if (!*s)
        return 0;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s))
            return 0;
    return 1;
}

static void read_first_line(const char *path, char *buf, size_t n) {
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    if (fgets(buf, (int)n, f))
        buf[strcspn(buf, "\n")] = '\0';
    fclose(f);
}

static uid_t proc_uid(const char *pid) {
    char path[300];
    snprintf(path, sizeof path, "/proc/%s/status", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return (uid_t)-1;
    char line[256];
    uid_t uid = (uid_t)-1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            unsigned real = 0;
            if (sscanf(line + 4, "%u", &real) == 1)
                uid = (uid_t)real;
            break;
        }
    }
    fclose(f);
    return uid;
}

static void proc_cmdline(const char *pid, char *out, size_t n) {
    char path[300];
    snprintf(path, sizeof path, "/proc/%s/cmdline", pid);
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    size_t got = fread(out, 1, n - 1, f);
    fclose(f);
    if (got == 0) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < got; i++)
        if (out[i] == '\0')
            out[i] = ' ';
    out[got] = '\0';
    while (got && (out[got - 1] == ' '))
        out[--got] = '\0';
}

/* Total CPU jiffies (utime + stime) of a pid, from /proc/<pid>/stat. The comm
 * field can contain spaces/parens, so parse everything AFTER the last ')'. */
static unsigned long long proc_cpu(const char *pid) {
    char path[300];
    snprintf(path, sizeof path, "/proc/%s/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[1024];
    unsigned long long cpu = 0;
    if (fgets(line, sizeof line, f)) {
        char *p = strrchr(line, ')');
        if (p) {
            unsigned long ut = 0, st = 0;
            /* after ')': state ppid pgrp session tty tpgid flags minflt cminflt
             * majflt cmajflt utime stime ... (utime=field14, stime=field15) */
            if (sscanf(p + 1,
                       " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
                       &ut, &st) == 2)
                cpu = (unsigned long long)ut + st;
        }
    }
    fclose(f);
    return cpu;
}

/* Parent pid from /proc/<pid>/stat — the field right after the ')' state char
 * (state ppid pgrp ...). Same last-')' handling as proc_cpu. 0 if unavailable. */
static pid_t proc_ppid(const char *pid) {
    char path[300];
    snprintf(path, sizeof path, "/proc/%s/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[1024];
    int ppid = 0;
    if (fgets(line, sizeof line, f)) {
        char *p = strrchr(line, ')');
        if (p)
            sscanf(p + 1, " %*c %d", &ppid);
    }
    fclose(f);
    return (pid_t)ppid;
}

static int proc_cpu_cmp(const void *a, const void *b) {
    const asmspy_proc_t *x = a, *y = b;
    if (x->cpu != y->cpu)
        return x->cpu < y->cpu ? 1 : -1; /* most-active first */
    return x->pid < y->pid ? -1 : x->pid > y->pid ? 1 : 0;
}

/* Quick "string-likelihood" scan: sample a few readable, non-code mappings and
 * return the per-mille fraction of alphanumeric bytes. Higher => the process's
 * memory looks more string-rich (interesting to inspect). Bounded to ~64 KB so
 * it stays quick even across many processes. Requires ptrace-read access, so a
 * non-attachable process scores 0 (process_vm_readv would EPERM anyway). */
static unsigned proc_string_score(pid_t pid) {
    char mp[64];
    snprintf(mp, sizeof mp, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(mp, "r");
    if (!f)
        return 0;
    const size_t CHUNK = 4096;
    const unsigned long long CAP = 64 * 1024; /* max bytes sampled per process */
    unsigned char buf[4096];
    char line[512];
    unsigned long long alnum = 0, total = 0;
    while (total < CAP && fgets(line, sizeof line, f)) {
        uint64_t start = 0, end = 0;
        char perms[8] = "";
        if (sscanf(line, "%" SCNx64 "-%" SCNx64 " %7s", &start, &end, perms) < 3)
            continue;
        if (perms[0] != 'r' || perms[2] == 'x') /* readable, non-code data */
            continue;
        uint64_t sz = end - start;
        if (sz < CHUNK)
            continue;
        uint64_t off = start + sz / 2; /* sample the middle, past any header */
        struct iovec l = {buf, CHUNK};
        struct iovec r = {(void *)(uintptr_t)off, CHUNK};
        ssize_t got = process_vm_readv(pid, &l, 1, &r, 1, 0);
        if (got <= 0)
            continue;
        for (ssize_t i = 0; i < got; i++)
            if (isalnum(buf[i]))
                alnum++;
        total += (unsigned long long)got;
    }
    fclose(f);
    return total ? (unsigned)((alnum * 1000) / total) : 0;
}

static int proc_scan_cmp(const void *a, const void *b) {
    const asmspy_proc_t *x = a, *y = b;
    if (x->scan != y->scan)
        return x->scan < y->scan ? 1 : -1; /* string-rich first */
    if (x->cpu != y->cpu)
        return x->cpu < y->cpu ? 1 : -1; /* then most recently active */
    return x->pid < y->pid ? -1 : x->pid > y->pid ? 1 : 0;
}

/* Cheap runtime badge for the process list: classify from argv0 (or comm) and
 * the presence of a perf-map — no maps/ELF read, so it stays fast across every
 * pid on every rescan. "" for a plain native binary. The richer, maps+ELF-based
 * classification lives in asmspy_fingerprint (the details panel). */
static void runtime_badge_cheap(const char *comm, const char *cmdline,
                                pid_t pid, char *out, size_t n) {
    out[0] = '\0';
    char a0[160];
    snprintf(a0, sizeof a0, "%s", (cmdline && cmdline[0]) ? cmdline : comm);
    char *sp = strchr(a0, ' '); /* argv0 = first token */
    if (sp)
        *sp = '\0';
    const char *b = strrchr(a0, '/');
    b = b ? b + 1 : a0;

    const char *rt = "";
    if (strncmp(b, "java", 4) == 0)
        rt = "JVM";
    else if (strncmp(b, "python", 6) == 0)
        rt = "py";
    else if (strcmp(b, "node") == 0 || strcmp(b, "nodejs") == 0)
        rt = "node";
    else if (strncmp(b, "ruby", 4) == 0)
        rt = "rb";
    else if (strncmp(b, "perl", 4) == 0)
        rt = "pl";
    else if (strcmp(b, "dotnet") == 0)
        rt = ".NET";
    else if (strncmp(b, "mono", 4) == 0)
        rt = "mono";
    else if (strncmp(b, "beam", 4) == 0)
        rt = "beam";
    else if (strncmp(b, "php", 3) == 0)
        rt = "php";

    if (rt[0]) {
        snprintf(out, n, "%s", rt);
        return;
    }
    /* not obviously a managed launcher — a perf-map still means a live JIT */
    char pm[64];
    snprintf(pm, sizeof pm, "/tmp/perf-%d.map", (int)pid);
    if (access(pm, F_OK) == 0)
        snprintf(out, n, "jit");
}

int asmspy_proclist(asmspy_proc_t **out, size_t *count, asmspy_sort_t sort) {
    DIR *d = opendir("/proc");
    if (!d)
        return -1;
    uid_t me = geteuid();

    size_t cap = 256, n = 0;
    asmspy_proc_t *v = malloc(cap * sizeof *v);
    if (!v) {
        closedir(d);
        return -1;
    }

    struct dirent *e;
    while ((e = readdir(d))) {
        if (!is_all_digits(e->d_name))
            continue;
        if (n == cap) {
            size_t ncap = cap * 2;
            asmspy_proc_t *nv = realloc(v, ncap * sizeof *v);
            if (!nv)
                break;
            v = nv;
            cap = ncap;
        }
        asmspy_proc_t *p = &v[n];
        p->pid = (pid_t)atoi(e->d_name);
        p->ppid = proc_ppid(e->d_name);

        char path[300], comm[64];
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        read_first_line(path, comm, sizeof comm);
        if (!comm[0])
            snprintf(comm, sizeof comm, "?");

        proc_cmdline(e->d_name, p->cmd, sizeof p->cmd);
        if (!p->cmd[0])
            snprintf(p->cmd, sizeof p->cmd, "[%s]", comm);

        runtime_badge_cheap(comm, p->cmd, p->pid, p->runtime,
                            sizeof p->runtime);

        uid_t uid = proc_uid(e->d_name);
        struct passwd *pw = (uid != (uid_t)-1) ? getpwuid(uid) : NULL;
        if (pw)
            snprintf(p->user, sizeof p->user, "%s", pw->pw_name);
        else
            snprintf(p->user, sizeof p->user, "%u", (unsigned)uid);

        p->attachable = (me == 0) || (uid == me);
        /* first CPU snapshot (becomes a delta below for the sampled sorts) */
        int sampled = (sort == ASMSPY_SORT_ACTIVE || sort == ASMSPY_SORT_SCAN);
        p->cpu = sampled ? proc_cpu(e->d_name) : 0;
        p->scan = 0;
        n++;
    }
    closedir(d);

    if (sort == ASMSPY_SORT_ACTIVE || sort == ASMSPY_SORT_SCAN) {
        /* second snapshot after a short window -> per-process CPU delta */
        struct timespec ts = {0, 150L * 1000 * 1000};
        nanosleep(&ts, NULL);
        for (size_t i = 0; i < n; i++) {
            char pids[16];
            snprintf(pids, sizeof pids, "%d", (int)v[i].pid);
            unsigned long long c1 = proc_cpu(pids);
            v[i].cpu = (c1 > v[i].cpu) ? c1 - v[i].cpu : 0;
            /* the scan also samples readable memory for alphanumeric density */
            if (sort == ASMSPY_SORT_SCAN && v[i].attachable)
                v[i].scan = proc_string_score(v[i].pid);
        }
        qsort(v, n, sizeof *v,
              sort == ASMSPY_SORT_SCAN ? proc_scan_cmp : proc_cpu_cmp);
    } else {
        /* pid-sorted for a stable list */
        for (size_t i = 1; i < n; i++) { /* small insertion sort */
            asmspy_proc_t key = v[i];
            size_t j = i;
            while (j > 0 && v[j - 1].pid > key.pid) {
                v[j] = v[j - 1];
                j--;
            }
            v[j] = key;
        }
    }

    *out = v;
    *count = n;
    return (int)n;
}

/* ================================================================== */
/* ELF function-symbol resolver                                        */
/* ================================================================== */

/* One backing ELF file mapped into the target. */
typedef struct {
    char path[PATH_MAX]; /* pathname from /proc/<pid>/maps               */
    uint64_t load_start; /* start of its file-offset-0 mapping           */
    int is_exe;          /* opened via /proc/<pid>/exe (robust) if set   */
} module_t;

/* Parse /proc/<pid>/maps into the set of distinct ELF modules (each with the
 * base of its offset-0 mapping). Returns count, or -1. */
static int scan_modules(pid_t pid, module_t **out, char *exe_path,
                        size_t exe_cap) {
    char mp[64];
    snprintf(mp, sizeof mp, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(mp, "r");
    if (!f)
        return -1;

    /* the main executable's real path, so we can read it via /proc/pid/exe */
    exe_path[0] = '\0';
    char el[64];
    snprintf(el, sizeof el, "/proc/%d/exe", (int)pid);
    ssize_t r = readlink(el, exe_path, exe_cap - 1);
    if (r > 0)
        exe_path[r] = '\0';
    else
        exe_path[0] = '\0';

    size_t cap = 64, n = 0;
    module_t *mods = malloc(cap * sizeof *mods);
    if (!mods) {
        fclose(f);
        return -1;
    }

    char line[PATH_MAX + 128];
    while (fgets(line, sizeof line, f)) {
        uint64_t start = 0, end = 0, off = 0;
        char perms[8];
        int pathpos = 0;
        /* "start-end perms offset dev inode path" */
        if (sscanf(line, "%" SCNx64 "-%" SCNx64 " %7s %" SCNx64 " %*x:%*x %*u %n",
                   &start, &end, perms, &off, &pathpos) < 4)
            continue;
        if (pathpos <= 0)
            continue;
        char *path = line + pathpos;
        path[strcspn(path, "\n")] = '\0';
        if (path[0] != '/') /* skip [heap],[stack],[vdso],anon */
            continue;
        /* strip a " (deleted)" suffix */
        char *del = strstr(path, " (deleted)");
        if (del)
            *del = '\0';
        if (off != 0) /* only the ELF-header (offset 0) mapping fixes the base */
            continue;

        /* dedup by path; keep the lowest load_start */
        size_t k;
        for (k = 0; k < n; k++)
            if (strcmp(mods[k].path, path) == 0)
                break;
        if (k < n) {
            if (start < mods[k].load_start)
                mods[k].load_start = start;
            continue;
        }
        if (n == cap) {
            size_t nc = cap * 2;
            module_t *nm = realloc(mods, nc * sizeof *mods);
            if (!nm)
                break;
            mods = nm;
            cap = nc;
        }
        snprintf(mods[n].path, sizeof mods[n].path, "%s", path);
        mods[n].load_start = start;
        mods[n].is_exe = (exe_path[0] && strcmp(path, exe_path) == 0);
        n++;
    }
    fclose(f);
    *out = mods;
    return (int)n;
}

/* Itanium C++ ABI demangler, resolved from libstdc++ at link time (-lstdc++).
 * Its real prototype lives in the C++-only <cxxabi.h>, so declare it here for the
 * C build. Returns a malloc'd demangled string (caller frees) or NULL. */
extern char *__cxa_demangle(const char *mangled, char *out, size_t *len,
                            int *status);

/* Demangle a C++ (Itanium ABI) symbol name. Returns a malloc'd demangled string,
 * or NULL when `name` is not a mangled name (or demangling fails) — the caller
 * then keeps the raw name. A trailing "@plt" (appended for PLT stubs, see below)
 * is peeled off, the base demangled, and "@plt" re-appended, so an imported C++
 * call reads "std::foo(int)@plt" rather than "_ZSt3fooi@plt". */
static char *demangle_dup(const char *name) {
    size_t len = strlen(name);
    size_t base = len;
    int is_plt = (len > 4 && memcmp(name + len - 4, "@plt", 4) == 0);
    if (is_plt)
        base = len - 4;
    /* Itanium mangled names start with "_Z"; skip the call (and a malloc) for the
     * overwhelmingly common plain-C-symbol case. */
    if (base < 2 || name[0] != '_' || name[1] != 'Z')
        return NULL;
    char buf[512];
    if (base >= sizeof buf)
        return NULL; /* absurdly long mangling — keep it raw */
    memcpy(buf, name, base);
    buf[base] = '\0';
    int status = 0;
    char *dem = __cxa_demangle(buf, NULL, NULL, &status);
    if (status != 0 || !dem)
        return NULL;
    if (!is_plt)
        return dem;
    /* re-append the "@plt" tag we peeled off before demangling */
    size_t dl = strlen(dem);
    char *out = malloc(dl + 5); /* "@plt" + NUL */
    if (out) {
        memcpy(out, dem, dl);
        memcpy(out + dl, "@plt", 5);
    }
    free(dem);
    return out;
}

/* Append one STT_FUNC symbol to a growing table. The stored name is C++-demangled
 * when the raw symbol is mangled (guarded on "_Z"); otherwise it is kept verbatim. */
static int sym_push(asmspy_symtab_t *t, size_t *cap, uint64_t addr,
                    uint64_t size, const char *name, const char *module) {
    if (!name[0])
        return 0;
    if (t->n == *cap) {
        size_t nc = *cap ? *cap * 2 : 512;
        asmspy_sym_t *nv = realloc(t->v, nc * sizeof *nv);
        if (!nv)
            return -1;
        t->v = nv;
        *cap = nc;
    }
    asmspy_sym_t *s = &t->v[t->n];
    s->addr = addr;
    s->size = size;
    char *dem = demangle_dup(name); /* C++ names; NULL for plain-C / on failure */
    s->name = dem ? dem : strdup(name);
    s->module = strdup(module);
    if (!s->name || !s->module) {
        free(s->name);
        free(s->module);
        return -1;
    }
    t->n++;
    return 0;
}

/* mmap a file read-only. Returns base + sets *len, or NULL. */
static uint8_t *map_file(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        close(fd);
        return NULL;
    }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return NULL;
    *len = (size_t)st.st_size;
    return m;
}

/* Read the STT_FUNC symbols of one ELF file into the table, biased to runtime. */
static void load_module_syms(pid_t pid, const module_t *mod,
                             asmspy_symtab_t *t, size_t *cap) {
    char exe[64];
    const char *open_path = mod->path;
    if (mod->is_exe) {
        snprintf(exe, sizeof exe, "/proc/%d/exe", (int)pid);
        open_path = exe;
    }
    size_t flen = 0;
    uint8_t *base = map_file(open_path, &flen);
    if (!base)
        return;

    const char *modname = strrchr(mod->path, '/');
    modname = modname ? modname + 1 : mod->path;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS64)
        goto done;

    /* load bias = base of the offset-0 mapping minus the lowest PT_LOAD vaddr;
     * reduces to 0 for ET_EXEC (absolute symbols) and to load_start for ET_DYN. */
    uint64_t min_vaddr = UINT64_MAX;
    if (eh->e_phoff && eh->e_phentsize >= sizeof(Elf64_Phdr)) {
        for (unsigned i = 0; i < eh->e_phnum; i++) {
            uint64_t o = eh->e_phoff + (uint64_t)i * eh->e_phentsize;
            if (o + sizeof(Elf64_Phdr) > flen)
                break;
            const Elf64_Phdr *ph = (const Elf64_Phdr *)(base + o);
            if (ph->p_type == PT_LOAD && ph->p_vaddr < min_vaddr)
                min_vaddr = ph->p_vaddr;
        }
    }
    if (min_vaddr == UINT64_MAX)
        min_vaddr = 0;
    uint64_t bias = mod->load_start - min_vaddr;

    if (!eh->e_shoff || eh->e_shentsize < sizeof(Elf64_Shdr))
        goto done;
    /* overflow-safe: never add two attacker-controlled uint64 (they can wrap) */
    if (eh->e_shoff > flen ||
        eh->e_shnum > (flen - eh->e_shoff) / eh->e_shentsize)
        goto done;

    /* Stride section headers by e_shentsize (>= sizeof(Elf64_Shdr), checked above),
     * NOT sizeof — a non-standard ELF with a larger entsize would otherwise misalign
     * every header past index 0. Mirrors the sh_entsize walk in the symbol loop. */
#define ASMSPY_SHDR(idx)                                                        \
    ((const Elf64_Shdr *)(base + eh->e_shoff + (uint64_t)(idx) * eh->e_shentsize))

    /* prefer .symtab (superset); fall back to .dynsym for stripped libs */
    const int order[2] = {SHT_SYMTAB, SHT_DYNSYM};
    for (int oi = 0; oi < 2; oi++) {
        int want = order[oi];
        int found = 0;
        for (unsigned i = 0; i < eh->e_shnum; i++) {
            const Elf64_Shdr *s = ASMSPY_SHDR(i);
            if ((int)s->sh_type != want)
                continue;
            found = 1;
            if (s->sh_entsize < sizeof(Elf64_Sym) || s->sh_link >= eh->e_shnum)
                continue;
            const Elf64_Shdr *strh = ASMSPY_SHDR(s->sh_link);
            if (strh->sh_offset >= flen)
                continue;
            const char *strtab = (const char *)(base + strh->sh_offset);
            /* overflow-safe clamp (sh_offset < flen already checked above) */
            size_t strmax = strh->sh_size <= flen - strh->sh_offset
                                ? strh->sh_size
                                : flen - strh->sh_offset;
            if (s->sh_offset > flen || s->sh_size > flen - s->sh_offset)
                continue;
            size_t count = s->sh_size / s->sh_entsize;
            for (size_t j = 0; j < count; j++) {
                const Elf64_Sym *sy =
                    (const Elf64_Sym *)(base + s->sh_offset + j * s->sh_entsize);
                if (ELF64_ST_TYPE(sy->st_info) != STT_FUNC)
                    continue;
                if (sy->st_shndx == SHN_UNDEF || sy->st_value == 0)
                    continue;
                if (sy->st_name >= strmax)
                    continue;
                const char *nm = strtab + sy->st_name;
                /* the name must be NUL-terminated inside the mapped strtab,
                 * else strdup's strlen would over-read past the mmap end */
                if (memchr(nm, '\0', strmax - sy->st_name) == NULL)
                    continue;
                if (sym_push(t, cap, bias + sy->st_value, sy->st_size, nm,
                             modname) != 0)
                    goto done;
            }
        }
        if (found) /* symtab present -> don't also read dynsym (dupes) */
            break;
    }

    /* PLT thunks: name each stub "<sym>@plt" so a call THROUGH the PLT resolves
     * to the imported function instead of an anonymous stub. Each .rela.plt[i]
     * (a JUMP_SLOT reloc -> a .dynsym name) maps to a stub address: .plt.sec[i]
     * on CET builds (where the caller's `call` lands), else .plt[i+1] (slot 0 is
     * the resolver PLT0). The loop index i also indexes the slot, so IRELATIVE
     * (ifunc) entries with no symbol are skipped without misaligning the rest. */
    if (eh->e_shstrndx && eh->e_shstrndx < eh->e_shnum) {
        const Elf64_Shdr *shstr = ASMSPY_SHDR(eh->e_shstrndx);
        if (shstr->sh_offset < flen) {
            const char *shnames = (const char *)(base + shstr->sh_offset);
            size_t shmax = shstr->sh_size <= flen - shstr->sh_offset
                               ? shstr->sh_size
                               : flen - shstr->sh_offset;
            const Elf64_Shdr *rela = NULL, *plt = NULL, *pltsec = NULL;
            for (unsigned i = 0; i < eh->e_shnum; i++) {
                const Elf64_Shdr *s = ASMSPY_SHDR(i);
                if (s->sh_name >= shmax)
                    continue;
                const char *nm = shnames + s->sh_name;
                if (memchr(nm, '\0', shmax - s->sh_name) == NULL)
                    continue;
                if (s->sh_type == SHT_RELA && strcmp(nm, ".rela.plt") == 0)
                    rela = s;
                else if (strcmp(nm, ".plt") == 0)
                    plt = s;
                else if (strcmp(nm, ".plt.sec") == 0)
                    pltsec = s;
            }
            const Elf64_Shdr *stub = pltsec ? pltsec : plt;
            if (rela && stub && rela->sh_entsize >= sizeof(Elf64_Rela) &&
                rela->sh_offset < flen && rela->sh_size <= flen - rela->sh_offset &&
                rela->sh_link < eh->e_shnum) {
                const Elf64_Shdr *dsym = ASMSPY_SHDR(rela->sh_link);
                const Elf64_Shdr *dstr =
                    dsym->sh_link < eh->e_shnum ? ASMSPY_SHDR(dsym->sh_link) : NULL;
                if (dstr && dsym->sh_entsize >= sizeof(Elf64_Sym) &&
                    dsym->sh_offset < flen &&
                    dsym->sh_size <= flen - dsym->sh_offset &&
                    dstr->sh_offset < flen) {
                    const char *dyns = (const char *)(base + dstr->sh_offset);
                    size_t dynmax = dstr->sh_size <= flen - dstr->sh_offset
                                        ? dstr->sh_size
                                        : flen - dstr->sh_offset;
                    uint64_t es = stub->sh_entsize ? stub->sh_entsize : 16;
                    uint64_t slot0 = (stub == plt) ? 1 : 0; /* skip PLT0 in .plt */
                    size_t ndsym = dsym->sh_size / dsym->sh_entsize;
                    size_t nrela = rela->sh_size / rela->sh_entsize;
                    for (size_t i = 0; i < nrela; i++) {
                        const Elf64_Rela *r = (const Elf64_Rela *)(base +
                            rela->sh_offset + i * rela->sh_entsize);
                        if (ELF64_R_TYPE(r->r_info) != R_X86_64_JUMP_SLOT)
                            continue;
                        uint64_t si = ELF64_R_SYM(r->r_info);
                        if (si == 0 || si >= ndsym)
                            continue;
                        const Elf64_Sym *ds = (const Elf64_Sym *)(base +
                            dsym->sh_offset + si * dsym->sh_entsize);
                        if (ds->st_name >= dynmax)
                            continue;
                        const char *nm = dyns + ds->st_name;
                        if (!nm[0] ||
                            memchr(nm, '\0', dynmax - ds->st_name) == NULL)
                            continue;
                        char pn[160];
                        snprintf(pn, sizeof pn, "%s@plt", nm);
                        uint64_t a = bias + stub->sh_addr + (slot0 + i) * es;
                        if (sym_push(t, cap, a, es, pn, modname) != 0)
                            goto done;
                    }
                }
            }
        }
    }
#undef ASMSPY_SHDR

done:
    munmap(base, flen);
}

static int sym_cmp_addr(const void *a, const void *b) {
    const asmspy_sym_t *x = a, *y = b;
    if (x->addr < y->addr)
        return -1;
    if (x->addr > y->addr)
        return 1;
    return 0;
}

int asmspy_symtab_load(pid_t pid, asmspy_symtab_t *out) {
    out->v = NULL;
    out->n = 0;

    module_t *mods = NULL;
    char exe_path[PATH_MAX];
    int nm = scan_modules(pid, &mods, exe_path, sizeof exe_path);
    if (nm < 0)
        return -1;

    size_t cap = 0;
    for (int i = 0; i < nm; i++)
        load_module_syms(pid, &mods[i], out, &cap);
    free(mods);

    if (out->n)
        qsort(out->v, out->n, sizeof *out->v, sym_cmp_addr);
    return 0;
}

void asmspy_symtab_free(asmspy_symtab_t *t) {
    if (!t || !t->v)
        return;
    for (size_t i = 0; i < t->n; i++) {
        free(t->v[i].name);
        free(t->v[i].module);
    }
    free(t->v);
    t->v = NULL;
    t->n = 0;
}

/* ================================================================== */
/* Process fingerprint — runtime / threads / modules / ELF traits      */
/* ================================================================== */

/* Pull Threads / VmRSS / TracerPid / Seccomp out of /proc/<pid>/status. */
static void fp_read_status(pid_t pid, asmspy_fingerprint_t *fp) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "Threads:", 8) == 0)
            sscanf(line + 8, "%d", &fp->threads);
        else if (strncmp(line, "VmRSS:", 6) == 0)
            sscanf(line + 6, "%lu", &fp->rss_kb);
        else if (strncmp(line, "TracerPid:", 10) == 0) {
            int t = 0;
            sscanf(line + 10, "%d", &t);
            fp->tracer_pid = (pid_t)t;
        } else if (strncmp(line, "Seccomp:", 8) == 0)
            sscanf(line + 8, "%d", &fp->seccomp);
    }
    fclose(f);
}

/* Collect up to N distinct per-task comm names from the process's task dir,
 * which on a managed runtime self-identify it ("C2 CompilerThread0",
 * "GC Thread#0", ".NET Finalizer", "V8 DefaultWorke"). Sets more_threadnames
 * if any distinct name was dropped past the cap. */
static void fp_read_threadnames(pid_t pid, asmspy_fingerprint_t *fp) {
    char tp[64];
    snprintf(tp, sizeof tp, "/proc/%d/task", (int)pid);
    DIR *d = opendir(tp);
    if (!d)
        return;
    const int CAP = (int)(sizeof fp->threadnames / sizeof fp->threadnames[0]);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!is_all_digits(e->d_name))
            continue;
        char cp[96], nm[64];
        snprintf(cp, sizeof cp, "/proc/%d/task/%.20s/comm", (int)pid, e->d_name);
        read_first_line(cp, nm, sizeof nm);
        if (!nm[0])
            continue;
        int dup = 0;
        for (int i = 0; i < fp->n_threadnames; i++)
            if (strcmp(fp->threadnames[i], nm) == 0) {
                dup = 1;
                break;
            }
        if (dup)
            continue;
        if (fp->n_threadnames < CAP)
            snprintf(fp->threadnames[fp->n_threadnames++],
                     sizeof fp->threadnames[0], "%.19s", nm);
        else {
            fp->more_threadnames = 1;
            break;
        }
    }
    closedir(d);
}

/* ELF traits of the main executable (via /proc/<pid>/exe): 32/64-bit, PIE,
 * static vs its PT_INTERP loader, and whether a .note.go.buildid marks it as a
 * Go binary (*go set). Bounds-checked like load_module_syms. */
static void fp_read_elf(pid_t pid, asmspy_fingerprint_t *fp, int *go) {
    *go = 0;
    char exe[64];
    snprintf(exe, sizeof exe, "/proc/%d/exe", (int)pid);
    size_t flen = 0;
    uint8_t *base = map_file(exe, &flen);
    if (!base)
        return;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0)
        goto done;
    fp->elf_class = eh->e_ident[EI_CLASS] == ELFCLASS32   ? 32
                    : eh->e_ident[EI_CLASS] == ELFCLASS64 ? 64
                                                          : 0;
    if (fp->elf_class != 64) /* 32-bit headers differ; report class only */
        goto done;
    fp->pie = (eh->e_type == ET_DYN);

    /* PT_INTERP => dynamically linked; its payload is the loader path */
    fp->static_linked = 1;
    if (eh->e_phoff && eh->e_phentsize >= sizeof(Elf64_Phdr)) {
        for (unsigned i = 0; i < eh->e_phnum; i++) {
            uint64_t o = eh->e_phoff + (uint64_t)i * eh->e_phentsize;
            if (o + sizeof(Elf64_Phdr) > flen)
                break;
            const Elf64_Phdr *ph = (const Elf64_Phdr *)(base + o);
            if (ph->p_type != PT_INTERP)
                continue;
            fp->static_linked = 0;
            if (ph->p_offset < flen) {
                const char *ip = (const char *)(base + ph->p_offset);
                if (memchr(ip, '\0', flen - ph->p_offset)) {
                    const char *ib = strrchr(ip, '/');
                    snprintf(fp->interp, sizeof fp->interp, "%s",
                             ib ? ib + 1 : ip);
                }
            }
        }
    }

    /* .note.go.buildid section name => a Go binary (survives stripping) */
    if (eh->e_shoff && eh->e_shentsize >= sizeof(Elf64_Shdr) &&
        eh->e_shstrndx && eh->e_shstrndx < eh->e_shnum &&
        eh->e_shoff <= flen &&
        eh->e_shnum <= (flen - eh->e_shoff) / eh->e_shentsize) {
        const Elf64_Shdr *shstr =
            (const Elf64_Shdr *)(base + eh->e_shoff +
                                 (uint64_t)eh->e_shstrndx * eh->e_shentsize);
        if (shstr->sh_offset < flen) {
            const char *names = (const char *)(base + shstr->sh_offset);
            size_t nmax = flen - shstr->sh_offset;
            for (unsigned i = 0; i < eh->e_shnum; i++) {
                const Elf64_Shdr *s =
                    (const Elf64_Shdr *)(base + eh->e_shoff +
                                         (uint64_t)i * eh->e_shentsize);
                if (s->sh_name >= nmax)
                    continue;
                const char *snm = names + s->sh_name;
                if (memchr(snm, '\0', nmax - s->sh_name) == NULL)
                    continue;
                if (strcmp(snm, ".note.go.buildid") == 0) {
                    *go = 1;
                    break;
                }
            }
        }
    }
done:
    munmap(base, flen);
}

/* Is a mapped library basename worth surfacing? Keep only real shared objects
 * (drops locale archives, gconv caches, the JDK module image and other data
 * mmaps), minus the handful of ubiquitous libs, so the list stays distinctive. */
static int fp_module_notable(const char *b) {
    if (!strstr(b, ".so")) /* shared objects only; skip locale/data mmaps */
        return 0;
    static const char *skip[] = {
        "libc.so",   "libm.so",       "libdl.so",   "libpthread.so",
        "librt.so",  "ld-linux",      "ld-musl",    "libgcc_s.so",
        "libresolv", "libnss_",       "libselinux",
    };
    for (size_t i = 0; i < sizeof skip / sizeof skip[0]; i++)
        if (strncmp(b, skip[i], strlen(skip[i])) == 0)
            return 0;
    return 1;
}

int asmspy_fingerprint(pid_t pid, asmspy_fingerprint_t *out) {
    memset(out, 0, sizeof *out);
    out->seccomp = -1; /* "unknown" until /proc/<pid>/status says otherwise */

    char el[64];
    snprintf(el, sizeof el, "/proc/%d/exe", (int)pid);
    ssize_t r = readlink(el, out->exe, sizeof out->exe - 1);
    out->exe[r > 0 ? r : 0] = '\0';

    char cp[64], comm[64] = "";
    snprintf(cp, sizeof cp, "/proc/%d/comm", (int)pid);
    read_first_line(cp, comm, sizeof comm);

    fp_read_status(pid, out);
    fp_read_threadnames(pid, out);

    int go_note = 0;
    fp_read_elf(pid, out, &go_note);

    char pm[64];
    snprintf(pm, sizeof pm, "/tmp/perf-%d.map", (int)pid);
    out->jitting = (access(pm, F_OK) == 0);

    /* scan mapped modules: pick a runtime from a signature library, and collect
     * the notable (non-ubiquitous) basenames for display */
    module_t *mods = NULL;
    char exe_path[PATH_MAX];
    int nm = scan_modules(pid, &mods, exe_path, sizeof exe_path);
    /* lib_ev is COPIED (not a pointer into mods) — mods is freed below before the
     * runtime verdict reads it, so a borrowed pointer would dangle. */
    const char *lib_rt = NULL;
    char lib_ev[64] = "";
    const int MODCAP = (int)(sizeof out->modules / sizeof out->modules[0]);
    for (int i = 0; i < nm; i++) {
        const char *b = strrchr(mods[i].path, '/');
        b = b ? b + 1 : mods[i].path;
        if (!lib_rt) {
            const char *m = NULL;
            if (strstr(b, "libjvm.so"))
                m = "JVM";
            else if (strstr(b, "libcoreclr") || strstr(b, "libclrjit"))
                m = ".NET";
            else if (strstr(b, "libpython"))
                m = "CPython";
            else if (strstr(b, "libnode") || strstr(b, "libv8"))
                m = "Node/V8";
            else if (strstr(b, "libruby"))
                m = "Ruby";
            else if (strstr(b, "libperl"))
                m = "Perl";
            else if (strstr(b, "libmono"))
                m = "Mono";
            else if (strstr(b, "libphp"))
                m = "PHP";
            if (m) {
                lib_rt = m;
                snprintf(lib_ev, sizeof lib_ev, "%.63s", b);
            }
        }
        if (!mods[i].is_exe && fp_module_notable(b)) {
            int dup = 0;
            for (int k = 0; k < out->n_modules; k++)
                if (strcmp(out->modules[k], b) == 0) {
                    dup = 1;
                    break;
                }
            if (!dup) {
                if (out->n_modules < MODCAP)
                    snprintf(out->modules[out->n_modules++],
                             sizeof out->modules[0], "%.47s", b);
                else
                    out->more_modules = 1;
            }
        }
    }
    free(mods);

    /* runtime verdict, strongest evidence first */
    const char *base = strrchr(out->exe[0] ? out->exe : comm, '/');
    base = base ? base + 1 : (out->exe[0] ? out->exe : comm);
    if (go_note) {
        snprintf(out->runtime, sizeof out->runtime, "Go");
        snprintf(out->evidence, sizeof out->evidence, ".note.go.buildid");
    } else if (lib_rt) {
        snprintf(out->runtime, sizeof out->runtime, "%s", lib_rt);
        snprintf(out->evidence, sizeof out->evidence, "%.95s", lib_ev);
    } else if (base[0]) {
        /* interpreters that embed their VM statically show no signature lib
         * (node embeds V8; beam.smp is Erlang) — fall back to the exe name */
        const char *rt = NULL;
        if (strncmp(base, "java", 4) == 0)
            rt = "JVM";
        else if (strncmp(base, "python", 6) == 0)
            rt = "CPython";
        else if (strcmp(base, "node") == 0 || strcmp(base, "nodejs") == 0)
            rt = "Node/V8";
        else if (strncmp(base, "ruby", 4) == 0)
            rt = "Ruby";
        else if (strncmp(base, "perl", 4) == 0)
            rt = "Perl";
        else if (strncmp(base, "beam", 4) == 0)
            rt = "Erlang/BEAM";
        else if (strcmp(base, "dotnet") == 0)
            rt = ".NET";
        else if (strncmp(base, "mono", 4) == 0)
            rt = "Mono";
        else if (strncmp(base, "php", 3) == 0)
            rt = "PHP";
        if (rt) {
            snprintf(out->runtime, sizeof out->runtime, "%s", rt);
            snprintf(out->evidence, sizeof out->evidence, "%.95s", base);
        }
    }
    if (!out->runtime[0]) {
        /* last resort: a distinctive thread name betrays some runtimes */
        for (int i = 0; i < out->n_threadnames && !out->runtime[0]; i++) {
            const char *t = out->threadnames[i];
            if (strstr(t, "CompilerThread") || strstr(t, "GC Thread"))
                snprintf(out->runtime, sizeof out->runtime, "JVM");
            else if (strstr(t, ".NET"))
                snprintf(out->runtime, sizeof out->runtime, ".NET");
            else if (strstr(t, "V8 "))
                snprintf(out->runtime, sizeof out->runtime, "Node/V8");
            if (out->runtime[0])
                snprintf(out->evidence, sizeof out->evidence, "thread \"%s\"", t);
        }
    }
    if (!out->runtime[0]) {
        /* a readable ELF with no runtime signature is an ordinary native binary;
         * if we couldn't even read the exe, say so */
        snprintf(out->runtime, sizeof out->runtime, "%s",
                 out->elf_class ? "native" : "?");
    }
    return 0;
}

const asmspy_sym_t *asmspy_symtab_by_name(const asmspy_symtab_t *t,
                                          const char *name) {
    for (size_t i = 0; i < t->n; i++)
        if (strcmp(t->v[i].name, name) == 0)
            return &t->v[i];
    return NULL;
}

/* Binary-search a by-address-sorted asmspy_sym_t array for the entry whose
 * [addr, addr+size) (or exact addr when size==0) contains `query`, else NULL.
 * Shared by the ELF symtab and the JIT map — both keep this same sorted layout. */
static const asmspy_sym_t *sym_at(const asmspy_sym_t *v, size_t n,
                                  uint64_t query) {
    if (n == 0)
        return NULL;
    size_t lo = 0, hi = n; /* greatest addr <= query */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (v[mid].addr <= query)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return NULL;
    const asmspy_sym_t *s = &v[lo - 1];
    if (s->size ? (query < s->addr + s->size) : (query == s->addr))
        return s;
    return NULL;
}

const asmspy_sym_t *asmspy_symtab_at(const asmspy_symtab_t *t, uint64_t addr) {
    return sym_at(t->v, t->n, addr);
}

/* ================================================================== */
/* JIT / perf-map resolver                                             */
/* ================================================================== */

/* Every JIT method shares this module tag (the perf-map names no backing file);
 * it is a non-owned literal, so asmspy_jitmap_free frees only the `name`s. */
static char JIT_MODULE[] = "jit";

/* On a double miss (ELF + current JIT map), asmspy_resolve re-reads the perf-map
 * at most once per this many misses — so a non-JIT target (no map file) doesn't
 * fopen it on every unknown address, while a busy JIT (misses spike as it emits
 * code) still gets its new methods named within a short window. */
#define JIT_MISS_COOLDOWN 64u

void asmspy_jitmap_init(asmspy_jitmap_t *j, pid_t pid) {
    j->v = NULL;
    j->n = j->cap = 0;
    j->pid = pid;
    j->miss_budget = 0; /* first double-miss refreshes immediately */
}

void asmspy_jitmap_free(asmspy_jitmap_t *j) {
    if (!j)
        return;
    for (size_t i = 0; i < j->n; i++)
        free(j->v[i].name); /* module is the shared JIT_MODULE literal */
    free(j->v);
    j->v = NULL;
    j->n = j->cap = 0;
}

static int jit_push(asmspy_jitmap_t *j, uint64_t addr, uint64_t size,
                    const char *name) {
    if (j->n == j->cap) {
        size_t nc = j->cap ? j->cap * 2 : 256;
        asmspy_sym_t *nv = realloc(j->v, nc * sizeof *nv);
        if (!nv)
            return -1;
        j->v = nv;
        j->cap = nc;
    }
    asmspy_sym_t *s = &j->v[j->n];
    s->addr = addr;
    s->size = size;
    s->name = strdup(name); /* JIT names are already human-readable — no demangle */
    if (!s->name)
        return -1;
    s->module = JIT_MODULE;
    j->n++;
    return 0;
}

int asmspy_jitmap_refresh(asmspy_jitmap_t *j) {
    for (size_t i = 0; i < j->n; i++) /* drop the previous snapshot */
        free(j->v[i].name);
    j->n = 0;
    j->miss_budget = JIT_MISS_COOLDOWN; /* rearm the rate limiter */

    char path[64];
    snprintf(path, sizeof path, "/tmp/perf-%d.map", (int)j->pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1; /* no JIT (or not readable): an empty map, resolves nothing */

    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) != -1) {
        unsigned long long start = 0, size = 0;
        int off = 0;
        /* "<hex start> <hex size> <name...>"; %n gives the name's offset. The
         * same format V8/Node, .NET, and OpenJDK (+perf-map-agent) all write. */
        if (sscanf(line, "%llx %llx %n", &start, &size, &off) < 2 || off == 0)
            continue;
        char *nm = line + off;
        size_t l = strlen(nm);
        while (l > 0 && (nm[l - 1] == '\n' || nm[l - 1] == '\r' ||
                         nm[l - 1] == ' ' || nm[l - 1] == '\t'))
            nm[--l] = '\0';
        if (l == 0)
            continue;
        if (jit_push(j, start, size, nm) != 0)
            break; /* OOM: keep what we have */
    }
    free(line);
    fclose(f);
    /* Sort by addr for the reverse search. A method recompiled at a reused
     * address (tiered/OSR) leaves the stale line too; both share a start so the
     * extent check still names the right method. */
    if (j->n)
        qsort(j->v, j->n, sizeof *j->v, sym_cmp_addr);
    return (int)j->n;
}

const asmspy_sym_t *asmspy_jitmap_at(const asmspy_jitmap_t *j, uint64_t addr) {
    return j ? sym_at(j->v, j->n, addr) : NULL;
}

const asmspy_sym_t *asmspy_resolve(const asmspy_symtab_t *syms,
                                   asmspy_jitmap_t *jit, uint64_t addr) {
    const asmspy_sym_t *s = syms ? asmspy_symtab_at(syms, addr) : NULL;
    if (s)
        return s; /* ELF symbols win: they are static and never go stale */
    if (!jit)
        return NULL;
    s = asmspy_jitmap_at(jit, addr);
    if (s)
        return s;
    /* Missed both. A method compiled since the last refresh may now be in the
     * perf-map — refresh (rate-limited) and retry the JIT tier once. */
    if (jit->miss_budget == 0) {
        asmspy_jitmap_refresh(jit); /* rearms miss_budget */
        return asmspy_jitmap_at(jit, addr);
    }
    jit->miss_budget--;
    return NULL;
}
