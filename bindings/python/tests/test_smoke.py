"""Smoke tests: the library loads and the manifest-driven layout is sane."""
import asmtest


def test_library_loads():
    ctx = asmtest.load()
    assert ctx.version
    assert ctx.arch in ("x86_64", "aarch64")
    # The manifest must describe the structs the binding reads.
    assert ctx.size("regs_t") > 0
    assert ctx.offset("regs_t", "ret") == 0
    # Condition-flag masks come from the manifest, never hand-coded.
    assert "CF" in ctx.flags and "ZF" in ctx.flags


def test_version_matches_package():
    assert asmtest.load().version == asmtest.__version__
