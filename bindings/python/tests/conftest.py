"""Shared pytest fixtures for the binding tests.

The canonical routines under test live in the fixture lib built by
`make python-test` and pointed to by ASMTEST_CORPUS_LIB.
"""
import ctypes
import os

import pytest

_CORPUS_LIB = os.environ.get("ASMTEST_CORPUS_LIB")

# test_hwtrace.py exercises the standalone libasmtest_hwtrace shared library, which
# is built and located only by the dedicated `make hwtrace-python-test` lane (it sets
# ASMTEST_HWTRACE_LIB). The general `make python-test` run builds only the emulator
# superset + corpus, so its loader can't find libasmtest_hwtrace — skip-collect the
# hwtrace module there. This mirrors the other nine bindings, whose hwtrace test is a
# separate target outside the general bindings lane (see mk/native-trace.mk).
collect_ignore = []
if "ASMTEST_HWTRACE_LIB" not in os.environ:
    collect_ignore.append("test_hwtrace.py")


@pytest.fixture(scope="session")
def routines():
    if not _CORPUS_LIB:
        pytest.skip("ASMTEST_CORPUS_LIB not set (run via `make python-test`)")
    return ctypes.CDLL(_CORPUS_LIB)


@pytest.fixture(scope="session")
def routine(routines):
    """Return a function mapping a routine name to its address."""
    def _addr(name):
        return ctypes.cast(getattr(routines, name), ctypes.c_void_p).value
    return _addr
