// Call-descent resolver/denylist upcall trampolines (asmtest_descent_set_resolver /
// asmtest_descent_set_denylist).
//
// These two functions are the C-callable entry points the out-of-process single-step
// engine (src/ptrace_backend.c, descend_decide) calls back into at descent levels 2/3
// to ask "descend into this callee?". They live in their OWN file because a cgo source
// that uses //export may only carry DECLARATIONS in its preamble — hwtrace.go's
// preamble is full of static function DEFINITIONS, so the //export directives cannot
// share that file. hwtrace.go declares these two symbols `extern` and installs them
// through the set-callback bridges; the linker binds the two cgo-generated C files.
//
// GC safety (the go(cgo) hazard the plan flags): the trampolines are stable C symbols,
// but the Go CLOSURES they dispatch to must not be passed through C (cgo forbids it and
// a moving GC could relocate them). Instead each Descent keeps its closures in the
// package-level descentRegistry, keyed by the native handle, for the handle's whole
// lifetime; the C side is handed only the handle (as the resolver's `user` pointer),
// which the trampoline turns back into the registry key. That keeps the closures
// reachable and immovable while the stepper is mid-single-step.
package asmtest

/*
#include <stdint.h>
*/
import "C"

import (
	"sync"
	"unsafe"
)

// descentRegistry holds the live resolver/denylist closures, keyed by the descent
// handle (as a uintptr, so the GC never treats the key as a live pointer into C
// memory). A mutex guards it because separate traces can run on separate goroutines /
// OS threads concurrently.
var descentRegistry = struct {
	sync.Mutex
	resolvers map[uintptr]DescentResolver
	denylists map[uintptr]DescentDenylist
}{
	resolvers: map[uintptr]DescentResolver{},
	denylists: map[uintptr]DescentDenylist{},
}

func descentSetResolver(h unsafe.Pointer, fn DescentResolver) {
	descentRegistry.Lock()
	descentRegistry.resolvers[uintptr(h)] = fn
	descentRegistry.Unlock()
}

func descentSetDenylist(h unsafe.Pointer, fn DescentDenylist) {
	descentRegistry.Lock()
	descentRegistry.denylists[uintptr(h)] = fn
	descentRegistry.Unlock()
}

func descentUnregister(h unsafe.Pointer) {
	descentRegistry.Lock()
	delete(descentRegistry.resolvers, uintptr(h))
	delete(descentRegistry.denylists, uintptr(h))
	descentRegistry.Unlock()
}

//export goDescentResolverTramp
func goDescentResolverTramp(callee C.uint64_t, user unsafe.Pointer, baseOut, lenOut *C.uint64_t) C.int {
	descentRegistry.Lock()
	fn := descentRegistry.resolvers[uintptr(user)]
	descentRegistry.Unlock()
	if fn == nil {
		return 0
	}
	descend, base, length := fn(uint64(callee))
	if descend && length != 0 {
		*baseOut = C.uint64_t(base)
		*lenOut = C.uint64_t(length)
		return 1
	}
	return 0
}

//export goDescentDenylistTramp
func goDescentDenylistTramp(callee C.uint64_t, user unsafe.Pointer) C.int {
	descentRegistry.Lock()
	fn := descentRegistry.denylists[uintptr(user)]
	descentRegistry.Unlock()
	if fn == nil {
		return 0
	}
	if fn(uint64(callee)) {
		return 1
	}
	return 0
}
