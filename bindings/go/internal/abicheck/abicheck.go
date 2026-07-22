// Package abicheck fails to BUILD if a hand-mirrored FFI struct in the Go
// binding drifts from the C header it mirrors (repo-review B5). The Go binding
// hand-writes cgo-preamble / plain-Go mirrors of ~13 C structs with no
// compile-time link to the headers, so a header layout change (a new field, a
// widened type) silently desyncs the mirror until some runtime path happens to
// exercise it. This package exports nothing and links no library; the
// `_Static_assert`s are the entire point, and `go test ./...` (the go-test
// target) compiles it with no Makefile change.
//
// A separate leaf package is required: cgo forbids `import "C"` in _test.go
// files, and the same C typedef cannot be defined twice across a package's
// preambles.
package abicheck

/*
#cgo CFLAGS: -I${SRCDIR}/../../../../include
#include <stddef.h>
#include "asmtest_hwtrace.h"
#include "asmtest_trace_auto.h"
#include "asmtest_codeimage.h"
#include "asmtest_ptrace.h"
#include "asmtest_valtrace.h"
#include "asmtest_drtrace.h"

// Frozen expected layout of every struct the Go binding hand-mirrors; a header
// change that shifts any size or load-bearing offset fails the build here,
// forcing the mirror (hwtrace.go / drtrace.go / cmd/dataflowsmoke/main.go) to be
// updated in the same change. Sizes/offsets measured on x86-64 (LP64).
_Static_assert(sizeof(asmtest_hwtrace_options_t) == 56, "hwtrace.go:42 options");
_Static_assert(sizeof(asmtest_hwtrace_status_t) == 180 &&
                   offsetof(asmtest_hwtrace_status_t, reason) == 20,
               "hwtrace.go:55 status");
_Static_assert(sizeof(asmtest_trace_choice_t) == 16, "hwtrace.go:68 choice");
_Static_assert(sizeof(asmtest_hwtrace_scope_t) == 12 &&
                   offsetof(asmtest_hwtrace_scope_t, arm_tid) == 8,
               "hwtrace.go:85 scope");
_Static_assert(sizeof(asmtest_jitdump_entry_t) == 32, "hwtrace.go:140 jitdump");
_Static_assert(sizeof(asmtest_codeimage_event_t) == 40 &&
                   offsetof(asmtest_codeimage_event_t, fd) == 36,
               "hwtrace.go:150 event");
_Static_assert(sizeof(asmtest_drtrace_options_t) == 32, "drtrace.go:35 dr options");
_Static_assert(sizeof(asmtest_exec_code_t) == 16, "drtrace.go:42 exec_code");
_Static_assert(sizeof(asmtest_gcmove_t) == 32 &&
                   offsetof(asmtest_gcmove_t, step) == 24,
               "dataflowsmoke gcmove");
_Static_assert(sizeof(asmtest_method_t) == 32 &&
                   offsetof(asmtest_method_t, version) == 24,
               "dataflowsmoke method");
_Static_assert(sizeof(at_val_rec_t) == 72 &&
                   offsetof(at_val_rec_t, value) == 56 &&
                   offsetof(at_val_rec_t, step) == 64,
               "dataflowsmoke valrec");
*/
import "C"
