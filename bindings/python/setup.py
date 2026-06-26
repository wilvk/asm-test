# Metadata lives in pyproject.toml; this file exists only to make the wheel
# *platform-specific*. asmtest has no compiled extension — it bundles a prebuilt
# native lib (asmtest/_libs/) as package data — so setuptools would tag the wheel
# py3-none-any (i.e. "runs anywhere"), which is wrong: the bundled .so/.dylib is
# for one platform. has_ext_modules() = True makes it platlib and root_is_pure =
# False tags it py3-none-<platform> (linux_x86_64, macosx_11_0_arm64, …), so a
# release builds one wheel per platform and pip installs the matching one. The
# release workflow then repairs each — auditwheel -> manylinux (Linux), delocate
# (macOS) — vendoring libunicorn; both need the libs in platlib, hence the above.
from setuptools import setup
from setuptools.dist import Distribution

try:  # setuptools >= 70.1 ships its own bdist_wheel; older defers to the wheel pkg
    from setuptools.command.bdist_wheel import bdist_wheel as _bdist_wheel
except ImportError:
    from wheel.bdist_wheel import bdist_wheel as _bdist_wheel


class BinaryDistribution(Distribution):
    # The bundled .so/.dylib make this a *binary* (platlib) distribution even
    # though there is no compiled extension — so the libs land in platlib, which
    # is where auditwheel/delocate expect them when repairing to manylinux.
    def has_ext_modules(self):
        return True


class bdist_wheel(_bdist_wheel):
    def finalize_options(self):
        _bdist_wheel.finalize_options(self)
        self.root_is_pure = False  # platform-specific (has a platform tag)

    def get_tag(self):
        # Keep the wheel Python- and ABI-agnostic (pure ctypes, no extension),
        # but platform-specific: py3-none-<platform>.
        _python, _abi, plat = _bdist_wheel.get_tag(self)
        # A universal2 macOS interpreter tags the wheel ..._universal2, but the
        # bundled lib is single-arch (built for this runner) — retag to the host
        # arch so delocate doesn't demand a slice (x86_64) that isn't present.
        if "universal2" in plat:
            import platform
            plat = plat.replace("universal2", platform.machine())
        return "py3", "none", plat


setup(distclass=BinaryDistribution, cmdclass={"bdist_wheel": bdist_wheel})
