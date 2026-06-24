/* platform.h — thin platform seam for the runner (src/asmtest.c).
 *
 * POSIX is the native build of the framework. The Win32 side backs the native
 * Win64 tier (see docs/plans/win64-native-tier-plan.md, Phase 4): the runner's
 * platform primitives (guard-page allocator, --filter glob, isolated execution,
 * in-process crash-to-failure) are provided by src/platform_win32.c +
 * src/glob_match.c rather than the POSIX fork/signal/mmap/fnmatch call sites.
 *
 * This header isolates the include and small-shim differences; the larger
 * execution-model differences are gated inline in asmtest.c by !defined(_WIN32).
 */
#ifndef ASMTEST_PLATFORM_H
#define ASMTEST_PLATFORM_H

#if defined(_WIN32)

#include <io.h>
#include <windows.h>

#include "glob_match.h"
#include "platform_win32.h"

/* fnmatch returns 0 on a match; the portable matcher returns 1. Normalise to the
 * fnmatch convention so the runner's `== 0` call sites are unchanged. */
#define ASMTEST_FNMATCH(pattern, str) (asmtest_glob_match((pattern), (str)) ? 0 : 1)
#define ASMTEST_ISATTY() _isatty(_fileno(stdout))
#define ASMTEST_GETPID() ((long)GetCurrentProcessId())

#else /* POSIX */

#include <fnmatch.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define ASMTEST_FNMATCH(pattern, str) fnmatch((pattern), (str), 0)
#define ASMTEST_ISATTY() isatty(STDOUT_FILENO)
#define ASMTEST_GETPID() ((long)getpid())

#endif

#endif /* ASMTEST_PLATFORM_H */
