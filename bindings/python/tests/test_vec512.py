"""AVX-512 512-bit capture (Track D) through the binding. Self-skips where the host
lacks AVX-512F; the routine `vec_add8d` (zmm0 += zmm1, 8 packed doubles) lives in the
corpus lib."""
import struct

import pytest

import asmtest


def test_avx512_capture_512bit(routine):
    if not asmtest.cpu_has_avx512f():
        pytest.skip("AVX-512F not available on this host")
    a = asmtest.vec512_f64(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0)
    b = asmtest.vec512_f64(10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0)
    out = asmtest.capture_vec512(routine("vec_add8d"), [a, b])
    # The 8th lane lives in the upper 256 bits — proves the full 512-bit capture.
    assert struct.unpack("<8d", out[0]) == (11.0, 22.0, 33.0, 44.0, 55.0, 66.0, 77.0, 88.0)
