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

        char path[300], comm[64];
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        read_first_line(path, comm, sizeof comm);
        if (!comm[0])
            snprintf(comm, sizeof comm, "?");

        proc_cmdline(e->d_name, p->cmd, sizeof p->cmd);
        if (!p->cmd[0])
            snprintf(p->cmd, sizeof p->cmd, "[%s]", comm);

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

const asmspy_sym_t *asmspy_symtab_by_name(const asmspy_symtab_t *t,
                                          const char *name) {
    for (size_t i = 0; i < t->n; i++)
        if (strcmp(t->v[i].name, name) == 0)
            return &t->v[i];
    return NULL;
}

const asmspy_sym_t *asmspy_symtab_at(const asmspy_symtab_t *t, uint64_t addr) {
    /* sorted by addr: binary-search the greatest addr <= query */
    if (t->n == 0)
        return NULL;
    size_t lo = 0, hi = t->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (t->v[mid].addr <= addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return NULL;
    const asmspy_sym_t *s = &t->v[lo - 1];
    if (s->size ? (addr < s->addr + s->size) : (addr == s->addr))
        return s;
    return NULL;
}
