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
    # Non-jumping verdict shim (Track 0.2): returns 0 when ABI-preserved.
    lib.asmtest_check_abi.argtypes = [v, C.c_char_p, C.c_size_t]
    lib.asmtest_check_abi.restype = C.c_int

    # Emulator tier is optional — only libasmtest_emu exports it.
    if has_emu(lib):
        lib.emu_open.restype = v
        lib.emu_close.argtypes = [v]
        lib.emu_call.argtypes = [v, v, C.c_size_t, v, C.c_int, C.c_uint64, v]
        lib.emu_call.restype = C.c_bool


def has_emu(lib):
    """True if the loaded library exports the emulator entry points."""
    try:
        lib.emu_open  # ctypes resolves the symbol lazily via dlsym
        return True
    except AttributeError:
        return False
