#!/usr/bin/env python3
"""gen-bindings.py — PROTOTYPE binding-FFI generator (Track: bindings codegen).

Emits the *raw FFI layer* of a language binding — the mechanical "attach the C
function signatures to the loaded library" part — from the single source of
truth that already exists in the tree: the C function prototypes in the public
headers, anchored by the layout `asmtest_abi.json` manifest. It does NOT generate
the ergonomic wrapper API (the `Guest`/handle classes in each binding's core
module) — that is the hand-written craft a generator can't replace.

Why this exists: each binding hand-transcribes ~40 function signatures × ~3
type-facts each (argtypes, restype, arg count); ten bindings ⇒ ~1200 facts kept
in sync by hand against the headers. This turns that into "parse the headers,
type-map, print", so adding a C-core entry point becomes a regenerate step.

Scope of this prototype: two back-ends — Python (`ctypes`) and Lua (LuaJIT
`ffi.cdef`) — chosen to show the range (real type-mapping vs. near-verbatim C).
The selection rule (binding-ABI name prefixes, minus `FILE*`/variadic) is a
stand-in for the production approach of tagging the prototypes in the header.

Usage:
    scripts/gen-bindings.py python      # ctypes declare(lib) to stdout
    scripts/gen-bindings.py lua         # ffi.cdef[[...]] block to stdout
    scripts/gen-bindings.py list        # the parsed FFI surface (name -> sig)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADERS = [ROOT / "include" / h
           for h in ("asmtest.h", "asmtest_emu.h", "asmtest_assemble.h")]

# A function is part of the binding ABI when its name starts with one of these
# (the opaque-handle FFI + the capture/emulator entry points bindings call). In
# production this would be a `/* @abi */` tag on the prototype; a prefix set is
# the prototype's lighter stand-in.
ABI_PREFIXES = ("asm_call_capture", "asmtest_check_abi", "emu_", "asmtest_emu_",
                "asmtest_asm_")
# Drop a few that share a prefix but are not a simple FFI call: anything taking
# a FILE* (reporters), the by-value asm_result_t helpers, and the disassembly
# helpers (which live in a separate, optional translation unit).
NAME_DENY = {"asmtest_assemble", "asmtest_asm_free", "emu_disas",
             "emu_trace_disasm", "emu_fault_describe", "emu_watch_describe",
             "emu_disas_available"}


class Func:
    __slots__ = ("ret", "name", "args")

    def __init__(self, ret, name, args):
        self.ret, self.name, self.args = ret, name, args


def _strip(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)   # block comments
    src = re.sub(r"//[^\n]*", " ", src)                # line comments
    src = re.sub(r"__attribute__\s*\(\(.*?\)\)", " ", src, flags=re.S)
    # Preprocessor lines incl. backslash-continuations (drops the ASSERT macros).
    src = re.sub(r"(?m)^[ \t]*#(?:\\\n|[^\n])*", " ", src)
    return src


def parse_headers():
    """Return the binding-ABI Funcs, de-duplicated, in source order. Split on
    ';' so each declaration is matched independently (no shared delimiter to
    consume), then keep the chunks shaped exactly like a prototype."""
    funcs, seen = [], set()
    proto = re.compile(
        r"^\s*([A-Za-z_][\w\s]*?[\w])\s*(\*+)?\s*"  # base return type + opt stars
        r"([A-Za-z_]\w*)\s*\(([^)]*)\)\s*$",         # name(args) — `*` may hug the name
        flags=re.S)
    for h in HEADERS:
        for chunk in _strip(h.read_text()).split(";"):
            m = proto.match(chunk)
            if not m:
                continue
            ret = m.group(1).strip() + (" " + m.group(2) if m.group(2) else "")
            name, raw = m.group(3), m.group(4).strip()
            if not name.startswith(ABI_PREFIXES) or name in NAME_DENY:
                continue
            if name in seen or "..." in raw:    # skip dupes + variadic
                continue
            if re.search(r"\bFILE\b", raw):       # not a plain FFI call
                continue
            args = [] if raw in ("", "void") else [a.strip() for a in raw.split(",")]
            funcs.append(Func(ret, name, args))
            seen.add(name)
    return funcs


# --- type mapping ----------------------------------------------------------

def _base(t: str) -> str:
    return re.sub(r"\bconst\b", "", t).strip()


def _argtype(a: str) -> str:
    """The C type of a parameter, with the parameter NAME stripped: a pointer
    keeps everything up to its last `*` ("const long *args" -> "long *"); a
    scalar drops its trailing identifier ("size_t code_len" -> "size_t")."""
    a = _base(a)
    if "*" in a:
        return a[:a.rfind("*") + 1].strip()
    toks = a.split()
    return " ".join(toks[:-1]) if len(toks) > 1 else a


def to_ctypes(t: str, is_ret: bool) -> str:
    """Map a C type to its ctypes spelling (C.<name>), or 'None' for void ret."""
    t = _base(t)
    if "*" in t:
        return "C.c_char_p" if t.replace("*", "").strip() == "char" else "C.c_void_p"
    return {
        "void": "None",
        "bool": "C.c_bool", "int": "C.c_int", "long": "C.c_long",
        "size_t": "C.c_size_t", "uint64_t": "C.c_uint64",
        "unsigned long long": "C.c_uint64", "unsigned long": "C.c_ulong",
        "unsigned": "C.c_uint", "double": "C.c_double", "float": "C.c_float",
    }.get(t, "C.c_int")  # enums (asm_arch_t, …) cross the FFI as int


def emit_python(funcs):
    out = [
        "# GENERATED by scripts/gen-bindings.py — do not edit.",
        "# The raw FFI layer: C function signatures attached to the loaded lib.",
        "import ctypes as C",
        "",
        "",
        "def _sig(lib, name, argtypes, restype):",
        "    fn = getattr(lib, name, None)  # optional tiers/symbols self-skip",
        "    if fn is None:",
        "        return",
        "    fn.argtypes = argtypes",
        "    fn.restype = restype",
        "",
        "",
        "def declare(lib):",
        '    """Attach argtypes/restype for every binding-ABI symbol present."""',
    ]
    for f in funcs:
        args = ", ".join(to_ctypes(_argtype(a), False) for a in f.args)
        ret = to_ctypes(f.ret, True)
        out.append(f'    _sig(lib, "{f.name}", [{args}], {ret})')
    return "\n".join(out) + "\n"


def emit_lua(funcs):
    """LuaJIT ffi.cdef takes C declarations near-verbatim; opaque pointers
    become void* so no struct typedefs are needed."""
    out = ["-- GENERATED by scripts/gen-bindings.py — do not edit.",
           "local ffi = require('ffi')", "ffi.cdef[[" ]

    def luac(t):
        t = _base(t)
        if "*" in t:
            return "const char *" if t.replace("*", "").strip() == "char" else "void *"
        return {"bool": "bool", "uint64_t": "uint64_t", "unsigned long long": "uint64_t",
                "size_t": "size_t"}.get(t, t)
    for f in funcs:
        args = "void" if not f.args else ", ".join(luac(_argtype(a)) for a in f.args)
        out.append(f"  {luac(f.ret)} {f.name}({args});")
    out += ["]]", ""]
    return "\n".join(out)


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "list"
    funcs = parse_headers()
    if what == "python":
        sys.stdout.write(emit_python(funcs))
    elif what == "lua":
        sys.stdout.write(emit_lua(funcs))
    else:
        for f in funcs:
            print(f"{f.ret} {f.name}({', '.join(f.args) or 'void'})")
        print(f"\n# {len(funcs)} binding-ABI functions parsed from the headers",
              file=sys.stderr)


if __name__ == "__main__":
    main()
