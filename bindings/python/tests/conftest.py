"""Shared pytest fixtures for the binding tests.

The canonical routines under test live in the fixture lib built by
`make python-test` and pointed to by ASMTEST_CORPUS_LIB.
"""
import ctypes
import os

import pytest

_CORPUS_LIB = os.environ.get("ASMTEST_CORPUS_LIB")


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
