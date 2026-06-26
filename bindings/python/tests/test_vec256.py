"""AVX2 256-bit capture (Track D) through the binding. Self-skips where the host
lacks AVX2; the routine `vec_add4d` (ymm0 += ymm1, 4 packed doubles) lives in the
corpus lib."""
import struct

import pytest

import asmtest


def test_avx2_capture_256bit(routine):
    if not asmtest.cpu_has_avx2():
        pytest.skip("AVX2 not available on this host")
    a = asmtest.vec256_f64(1.0, 2.0, 3.0, 4.0)
    b = asmtest.vec256_f64(10.0, 20.0, 30.0, 40.0)
    out = asmtest.capture_vec256(routine("vec_add4d"), [a, b])
    # The 4th lane lives in the upper 128 bits — proves the full 256-bit capture.
    assert struct.unpack("<4d", out[0]) == (11.0, 22.0, 33.0, 44.0)
