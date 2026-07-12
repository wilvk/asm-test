// Go data-flow binding smoke (Phase 6): GC-move canonicalizer + method resolver,
// mirroring the other language suites. cgo dlopen's libasmtest_dataflow at runtime
// (like the hwtrace.go binding) and calls the two pure helpers.
//
//	cd bindings/go && ASMTEST_DATAFLOW_LIB=<lib> go run ./cmd/dataflowsmoke
package main

/*
#cgo LDFLAGS: -ldl
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>

typedef struct { uint64_t old_base; uint64_t new_base; uint64_t len; uint32_t step; } df_gcmove_t;
typedef struct { uint64_t addr; uint64_t size; const char *name; uint64_t version; } df_method_t;

static uint64_t (*p_canon)(const df_gcmove_t*, size_t, uint32_t, uint64_t);
static int (*p_resolve)(const df_method_t*, size_t, uint64_t);

static int df_load(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) return -1;
    p_canon   = (uint64_t(*)(const df_gcmove_t*, size_t, uint32_t, uint64_t))dlsym(h, "asmtest_gcmove_canon");
    p_resolve = (int(*)(const df_method_t*, size_t, uint64_t))dlsym(h, "asmtest_method_resolve_pc");
    return (p_canon && p_resolve) ? 0 : -1;
}
static uint64_t df_canon(const df_gcmove_t *m, size_t n, uint32_t s, uint64_t phys) { return p_canon(m, n, s, phys); }
static int df_resolve(const df_method_t *m, size_t n, uint64_t pc) { return p_resolve(m, n, pc); }
*/
import "C"

import (
	"fmt"
	"os"
	"unsafe"
)

var n int
var failed bool

func check(cond bool, desc string) {
	n++
	if cond {
		fmt.Printf("ok %d - %s\n", n, desc)
	} else {
		fmt.Printf("not ok %d - %s\n", n, desc)
		failed = true
	}
}

func gcmove(moves [][4]uint64, step uint32, phys uint64) uint64 {
	if len(moves) == 0 {
		return uint64(C.df_canon(nil, 0, C.uint32_t(step), C.uint64_t(phys)))
	}
	arr := make([]C.df_gcmove_t, len(moves))
	for i, m := range moves {
		arr[i].old_base = C.uint64_t(m[0])
		arr[i].new_base = C.uint64_t(m[1])
		arr[i].len = C.uint64_t(m[2])
		arr[i].step = C.uint32_t(m[3])
	}
	return uint64(C.df_canon(&arr[0], C.size_t(len(arr)), C.uint32_t(step), C.uint64_t(phys)))
}

type meth struct {
	addr, size uint64
	name       string
	version    uint64
}

func method(methods []meth, pc uint64) int {
	if len(methods) == 0 {
		return int(C.df_resolve(nil, 0, C.uint64_t(pc)))
	}
	arr := make([]C.df_method_t, len(methods))
	cnames := make([]*C.char, len(methods))
	for i, m := range methods {
		cnames[i] = C.CString(m.name)
		arr[i].addr = C.uint64_t(m.addr)
		arr[i].size = C.uint64_t(m.size)
		arr[i].name = cnames[i]
		arr[i].version = C.uint64_t(m.version)
	}
	r := int(C.df_resolve(&arr[0], C.size_t(len(arr)), C.uint64_t(pc)))
	for _, p := range cnames {
		C.free(unsafe.Pointer(p))
	}
	return r
}

func main() {
	path := os.Getenv("ASMTEST_DATAFLOW_LIB")
	if path == "" {
		path = "../../build/libasmtest_dataflow.so"
	}
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	if C.df_load(cpath) != 0 {
		fmt.Println("# SKIP go dataflow: cannot dlopen libasmtest_dataflow")
		fmt.Println("1..0 # skipped")
		return
	}

	// GC-move canonicalizer
	check(gcmove(nil, 0, 0x1234) == 0x1234, "gcmove: empty move set is identity")
	mv := [][4]uint64{{0x1000, 0x2000, 0x100, 5}}
	check(gcmove(mv, 3, 0x1010) == 0x2010, "gcmove: pre-move addr forwards to final")
	check(gcmove(mv, 3, 0x1000) == 0x2000, "gcmove: object base forwards")
	check(gcmove(mv, 3, 0x10FF) == 0x20FF, "gcmove: last byte of half-open window forwards")
	check(gcmove(mv, 3, 0x1100) == 0x1100, "gcmove: one past the window not forwarded")
	check(gcmove(mv, 5, 0x1010) == 0x1010, "gcmove: at-move-step observation not forwarded")
	check(gcmove(mv, 3, 0x3000) == 0x3000, "gcmove: out-of-range addr unchanged")
	mv2 := [][4]uint64{{0x1000, 0x2000, 0x100, 3}, {0x2000, 0x3000, 0x100, 6}}
	check(gcmove(mv2, 1, 0x1010) == 0x3010, "gcmove: two compactions compose to final")

	// method resolver
	ms := []meth{{0x1000, 0x40, "Foo", 3}, {0x2000, 0x20, "Bar", 1}, {0x3000, 0, "Baz", 2}}
	check(method(ms, 0x1000) == 0, "method: Foo range start")
	check(method(ms, 0x103F) == 0, "method: Foo last byte (half-open)")
	check(method(ms, 0x1040) == -1, "method: one past Foo -> none")
	check(method(ms, 0x2010) == 1, "method: Bar range")
	check(method(ms, 0x3000) == 2, "method: Baz point match")
	check(method(ms, 0x3001) == -1, "method: Baz is point-only")
	rj := []meth{{0x1000, 0x40, "Foo", 1}, {0x1000, 0x40, "Foo", 5}}
	check(method(rj, 0x1010) == 1, "method: tiered re-JIT newest version wins")
	check(method(nil, 0x1000) == -1, "method: empty map -> -1")

	fmt.Printf("1..%d\n", n)
	if failed {
		os.Exit(1)
	}
}
