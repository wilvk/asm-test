//go:build asmtest_corpus

// Test-only corpus fixtures. The conformance suite drives canonical routines
// (add_signed, fp_add, vec_add4f, ...) built into libasmtest_corpus — a fixture
// lib that `make install-shared` never stages, so it must not be forced on
// consumers who `go get` this module. Keeping CorpusRoutine and the
// `-lasmtest_corpus` link directive behind the `asmtest_corpus` build tag means
// the default `go build` links only libasmtest_emu, while `make go-test`
// (which builds with `-tags asmtest_corpus`) pulls the fixtures in for the
// conformance and cross-arch tests.
package asmtest

/*
#cgo LDFLAGS: -L${SRCDIR}/../../build -lasmtest_corpus
#include <stdlib.h>

// name -> routine address (the code under test), resolved by the fixture lib's
// asmtest_corpus_routine name table.
extern void *asmtest_corpus_routine(const char *name);

// vec_add8d (AVX-512 corpus routine) is linked in via -lasmtest_corpus, but the
// shared asmtest_corpus_routine() name table does not register it; take its
// address directly so the conformance test can drive it without corpus glue.
extern void vec_add8d(void);
*/
import "C"

import "unsafe"

// CorpusRoutine resolves a canonical corpus routine (e.g. "add_signed") to its
// address, or nil if the name is unknown. Test-only: available under the
// `asmtest_corpus` build tag.
func CorpusRoutine(name string) Routine {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	return Routine(C.asmtest_corpus_routine(cs))
}

// vecAdd8dRoutine returns the address of the AVX-512 corpus routine vec_add8d.
// It is taken directly rather than via CorpusRoutine because the shared
// asmtest_corpus_routine() name table does not register vec_add8d.
func vecAdd8dRoutine() Routine { return Routine(C.vec_add8d) }
