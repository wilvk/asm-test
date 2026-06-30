"""Low-level loading of the asm-test shared library and ABI manifest.

This is the only module that touches ctypes and the filesystem. It locates and
dlopen()s the shared library (``libasmtest_emu`` if present, else the
capture-only ``libasmtest``), loads the machine-readable layout manifest
(``asmtest_abi.json`` from ``make manifest``), and declares the binding-ABI
prototypes. Everything else in the package reads struct fields at the offsets
the manifest publishes — never hand-transcribed — so a layout change in the C
headers flows through automatically (and the C ``_Static_assert``s guarantee the
manifest matches the real structs).
"""
import ctypes as C
import json
import os
import sys
from pathlib import Path

# bindings/python/asmtest/_native.py -> repo root is three parents up.
_REPO_ROOT = Path(__file__).resolve().parents[3]
# Native libs + manifest bundled into the wheel by `make python-package`.
_LIBS = Path(__file__).resolve().parent / "_libs"


def _lib_names():
    if sys.platform == "darwin":
        return ["libasmtest_emu.dylib", "libasmtest.dylib"]
    return ["libasmtest_emu.so", "libasmtest.so"]


def find_library():
    """Return (CDLL, path). Prefer the emulator lib (a superset of capture)."""
    cands = []
    env = os.environ.get("ASMTEST_LIB")
    if env:
        cands.append(Path(env))
    cands += [_LIBS / n for n in _lib_names()]  # bundled in a published wheel
    cands += [_REPO_ROOT / "build" / n for n in _lib_names()]
    cands += [Path(n) for n in _lib_names()]  # fall back to the system search
    errors = []
    for c in cands:
        try:
            return C.CDLL(str(c)), str(c)
        except OSError as e:  # noqa: PERF203 - small, readable loop
            errors.append(f"  {c}: {e}")
    raise OSError(
        "asm-test shared library not found; build it with `make shared-emu` "
        "(or `make shared`) or set ASMTEST_LIB.\n" + "\n".join(errors)
    )


def find_manifest():
    """Parse and return the ``asmtest_abi`` object from the layout manifest."""
    cands = []
    env = os.environ.get("ASMTEST_MANIFEST")
    if env:
        cands.append(Path(env))
    cands += [_LIBS / "asmtest_abi.json"]  # bundled in a published wheel
    cands += [_REPO_ROOT / "asmtest_abi.json", _REPO_ROOT / "build" / "asmtest_abi.json"]
    for c in cands:
        if c.is_file():
            return json.loads(c.read_text())["asmtest_abi"]
    raise FileNotFoundError(
        "asmtest_abi.json not found; run `make manifest` or set ASMTEST_MANIFEST."
    )


def declare(lib):
    """Attach argtypes/restype to the binding-ABI entry points present in lib."""
    v = C.c_void_p
    lib.asm_call_capture.argtypes = [v, v, v]
    lib.asm_call_capture.restype = None
    lib.asm_call_capture_fp.argtypes = [v, v, v, v]
    lib.asm_call_capture_fp.restype = None
    lib.asm_call_capture_vec.argtypes = [v, v, v, v]
    lib.asm_call_capture_vec.restype = None
    # AVX2 256-bit capture (Track D) + the CPUID feature probes that gate it.
    lib.asm_call_capture_vec256.argtypes = [v, v, v, v]
    lib.asm_call_capture_vec256.restype = None
    # AVX-512 512-bit capture (Track D) — the analog over the full zmm file.
    lib.asm_call_capture_vec512.argtypes = [v, v, v, v]
    lib.asm_call_capture_vec512.restype = None
    lib.asmtest_cpu_has_avx2.restype = C.c_int
    lib.asmtest_cpu_has_avx512f.restype = C.c_int
    # Non-jumping verdict shim (Track 0.2): returns 0 when ABI-preserved.
    lib.asmtest_check_abi.argtypes = [v, C.c_char_p, C.c_size_t]
    lib.asmtest_check_abi.restype = C.c_int

    # Emulator tier is optional — only libasmtest_emu exports it.
    if has_emu(lib):
        lib.emu_open.restype = v
        lib.emu_close.argtypes = [v]
        lib.emu_call.argtypes = [v, v, C.c_size_t, v, C.c_int, C.c_uint64, v]
        lib.emu_call.restype = C.c_bool
        _declare_emu_ext(lib, v)


def _declare_emu_ext(lib, v):
    """Declare the extended emulator surface: x86 FP/vector/Win64/traced calls,
    the cross-arch guests (AArch64 / RISC-V / ARM32, which run raw bytes on any
    host), and the opaque trace + per-arch result accessors."""
    u64, i, sz, dbl = C.c_uint64, C.c_int, C.c_size_t, C.c_double

    # x86-64 guest, array-form core calls (Python marshals the arg arrays).
    lib.emu_call_fp.argtypes = [v, v, sz, v, i, v, i, u64, v]
    lib.emu_call_fp.restype = C.c_bool
    lib.emu_call_vec.argtypes = [v, v, sz, v, i, v, i, u64, v]
    lib.emu_call_vec.restype = C.c_bool
    lib.emu_call_win64.argtypes = [v, v, sz, v, i, u64, v]
    lib.emu_call_win64.restype = C.c_bool
    lib.emu_call_traced.argtypes = [v, v, sz, v, i, u64, v, v]
    lib.emu_call_traced.restype = C.c_bool

    # Cross-arch guests: open/close + raw-bytes call (+ FP / vector / traced).
    for arch in ("arm64", "riscv", "arm"):
        getattr(lib, f"emu_{arch}_open").restype = v
        getattr(lib, f"emu_{arch}_close").argtypes = [v]
        getattr(lib, f"emu_{arch}_call").argtypes = [v, v, sz, v, i, u64, v]
        getattr(lib, f"emu_{arch}_call").restype = C.c_bool
        getattr(lib, f"emu_{arch}_call_fp").argtypes = [v, v, sz, v, i, v, i, u64, v]
        getattr(lib, f"emu_{arch}_call_fp").restype = C.c_bool
        getattr(lib, f"emu_{arch}_call_traced").argtypes = [v, v, sz, v, i, u64, v, v]
        getattr(lib, f"emu_{arch}_call_traced").restype = C.c_bool
    for arch in ("arm64", "arm"):  # RISC-V has no Unicorn vector file
        getattr(lib, f"emu_{arch}_call_vec").argtypes = [v, v, sz, v, i, v, i, u64, v]
        getattr(lib, f"emu_{arch}_call_vec").restype = C.c_bool

    # Opaque per-arch result handles + register accessors (read by name).
    for arch in ("arm64", "riscv", "arm"):
        getattr(lib, f"asmtest_emu_{arch}_result_new").restype = v
        getattr(lib, f"asmtest_emu_{arch}_result_free").argtypes = [v]
        getattr(lib, f"asmtest_emu_{arch}_reg").argtypes = [v, C.c_char_p]
        getattr(lib, f"asmtest_emu_{arch}_reg").restype = C.c_uint64
    lib.asmtest_emu_arm64_vec_f64.argtypes = [v, i, i]
    lib.asmtest_emu_arm64_vec_f64.restype = dbl
    lib.asmtest_emu_arm64_vec_f32.argtypes = [v, i, i]
    lib.asmtest_emu_arm64_vec_f32.restype = C.c_float
    lib.asmtest_emu_arm_q_f64.argtypes = [v, i, i]
    lib.asmtest_emu_arm_q_f64.restype = dbl
    lib.asmtest_emu_riscv_f_f64.argtypes = [v, i, i]
    lib.asmtest_emu_riscv_f_f64.restype = dbl
    # Fault/ok reads share one layout, so they take any result handle.
    for fn, rt in (("faulted", C.c_int), ("fault_addr", C.c_uint64),
                   ("fault_kind", C.c_int), ("ok", C.c_int)):
        getattr(lib, f"asmtest_emu_result_{fn}").argtypes = [v]
        getattr(lib, f"asmtest_emu_result_{fn}").restype = rt

    # Opaque execution-trace handle.
    lib.asmtest_emu_trace_new.argtypes = [sz, sz]
    lib.asmtest_emu_trace_new.restype = v
    lib.asmtest_emu_trace_free.argtypes = [v]
    lib.asmtest_emu_trace_covered.argtypes = [v, C.c_uint64]
    lib.asmtest_emu_trace_covered.restype = C.c_int
    lib.asmtest_emu_trace_insns_total.argtypes = [v]
    lib.asmtest_emu_trace_insns_total.restype = C.c_uint64
    lib.asmtest_emu_trace_blocks_len.argtypes = [v]
    lib.asmtest_emu_trace_blocks_len.restype = C.c_uint64

    # Mid-execution guards (Track F): arm on the handle (the arming functions take
    # plain pointers/scalars, so they are called directly); the violation lands in
    # an opaque emu_watch_t / emu_reg_guard_t read through the accessors.
    u32 = C.c_uint
    lib.emu_map.argtypes = [v, u64, sz]
    lib.emu_map.restype = C.c_bool
    lib.emu_watch_writes.argtypes = [v, u64, sz, i, v]
    lib.emu_watch_clear.argtypes = [v]
    lib.emu_guard_reg.argtypes = [v, C.c_char_p, u64, v]
    lib.emu_guard_reg.restype = C.c_bool
    lib.emu_guard_reg_clear.argtypes = [v]
    lib.asmtest_emu_watch_new.restype = v
    lib.asmtest_emu_watch_free.argtypes = [v]
    lib.asmtest_emu_watch_violated.argtypes = [v]
    lib.asmtest_emu_watch_violated.restype = i
    lib.asmtest_emu_watch_size.argtypes = [v]
    lib.asmtest_emu_watch_size.restype = u32
    lib.asmtest_emu_reg_guard_new.restype = v
    lib.asmtest_emu_reg_guard_free.argtypes = [v]
    lib.asmtest_emu_reg_guard_violated.argtypes = [v]
    lib.asmtest_emu_reg_guard_violated.restype = i
    for nm in ("asmtest_emu_watch_addr", "asmtest_emu_watch_rip_off",
               "asmtest_emu_reg_guard_got", "asmtest_emu_reg_guard_rip_off"):
        getattr(lib, nm).argtypes = [v]
        getattr(lib, nm).restype = u64

    # Coverage-guided fuzzing + mutation testing (Track E): drivers take plain
    # pointers/scalars; the totals land in opaque stat handles.
    lib.emu_fuzz_cover1.argtypes = [v, v, sz, C.c_long, C.c_long, u64, u64, v, v]
    lib.emu_fuzz_cover1.restype = C.c_bool
    lib.emu_mutation_test1.argtypes = [v, v, sz, v, sz, u64, u64, v]
    lib.emu_mutation_test1.restype = sz
    lib.asmtest_emu_fuzz_stat_new.restype = v
    lib.asmtest_emu_fuzz_stat_free.argtypes = [v]
    lib.asmtest_emu_mutation_stat_new.restype = v
    lib.asmtest_emu_mutation_stat_free.argtypes = [v]
    for nm in ("asmtest_emu_fuzz_blocks_reached", "asmtest_emu_fuzz_corpus_len",
               "asmtest_emu_fuzz_iterations", "asmtest_emu_mutation_mutants",
               "asmtest_emu_mutation_killed", "asmtest_emu_mutation_survived"):
        getattr(lib, nm).argtypes = [v]
        getattr(lib, nm).restype = u64

    # In-line assembler (Keystone) ships in libasmtest_emu (the superset), so it
    # is normally present; the probe still guards against an older/leaner lib.
    # The widened run shim takes six scalar args + syntax + a cap;
    # asmtest_asm_bytes is multi-arch text->bytes; the last-error accessor
    # carries the Keystone diagnostic.
    if has_asm(lib):
        lib.asmtest_emu_call_asm6.argtypes = [
            v, C.c_char_p, C.c_int, C.c_long, C.c_long, C.c_long, C.c_long,
            C.c_long, C.c_long, C.c_int, C.c_uint64, v,
        ]
        lib.asmtest_emu_call_asm6.restype = C.c_int
        lib.asmtest_asm_bytes.argtypes = [
            C.c_int, C.c_int, C.c_char_p, C.c_uint64, v, C.c_int,
        ]
        lib.asmtest_asm_bytes.restype = C.c_int
        lib.asmtest_asm_last_error.argtypes = []
        lib.asmtest_asm_last_error.restype = C.c_char_p

    # Disassembler (Capstone) ships in libasmtest_emu too (the superset carries
    # it); the probe still guards against an older/leaner lib.
    # emu_disas decodes one instruction at `off` into a text buffer and
    # returns its byte length; emu_disas_available() reports whether Capstone is
    # linked at all (a Capstone build can still self-skip an arch it lacks).
    if has_disas(lib):
        lib.emu_disas_available.argtypes = []
        lib.emu_disas_available.restype = C.c_bool
        lib.emu_disas.argtypes = [
            C.c_int, C.c_char_p, C.c_size_t, C.c_uint64, C.c_uint64,
            C.c_char_p, C.c_size_t,
        ]
        lib.emu_disas.restype = C.c_size_t


def has_emu(lib):
    """True if the loaded library exports the emulator entry points."""
    try:
        lib.emu_open  # ctypes resolves the symbol lazily via dlsym
        return True
    except AttributeError:
        return False


def has_asm(lib):
    """True if the loaded library exports the in-line assembler (Keystone)."""
    try:
        lib.asmtest_emu_call_asm6
        return True
    except AttributeError:
        return False


def has_disas(lib):
    """True if the loaded library exports the disassembler (Capstone)."""
    try:
        lib.emu_disas
        return True
    except AttributeError:
        return False
