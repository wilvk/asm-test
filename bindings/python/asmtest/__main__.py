"""``python -m asmtest`` — a tiny loader self-report.

``--where`` prints the absolute path of each native library this package actually
resolved (the core capture/emulator lib, plus the optional DynamoRIO and
hardware-trace tiers). A clean-room install test asserts these land **under the
installed package** — not a leaked ``build/`` checkout, a Homebrew dylib, or a
``$ASMTEST_*_LIB`` override — so "install fresh, no override" means what it says.
See docs/plans/macos-clean-test-plan.md (Track A) and
docs/plans/bundle-native-trace-tiers-plan.md.
"""
import sys


def _core_path():
    from . import _native
    _lib, path = _native.find_library()
    return path


def _guarded(fn):
    try:
        return fn()
    except Exception as e:  # not built / not bundled / unavailable on this host
        return f"unavailable ({type(e).__name__})"


def _tier_path(module):
    def _resolve():
        mod = __import__(f"asmtest.{module}", fromlist=["library_path"])
        return mod.library_path()
    return _guarded(_resolve)


def _where():
    print(f"core    {_guarded(_core_path)}")
    print(f"drtrace {_tier_path('drtrace')}")
    print(f"hwtrace {_tier_path('hwtrace')}")


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv == ["--where"]:
        _where()
        return 0
    prog = "python -m asmtest"
    if argv in (["-h"], ["--help"]):
        print(f"usage: {prog} --where\n\n  --where   print the resolved native library paths")
        return 0
    sys.stderr.write(f"usage: {prog} --where\n")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
