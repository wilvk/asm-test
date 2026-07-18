import os

from conan import ConanFile
from conan.tools.files import copy, get

# Conan 2 recipe for the asm-test C core: the MIT static library + headers
# (the GPL engines are conveyed only by the language bindings, never by this
# package). Upstream is a hand-written Makefile (no CMake/autotools), so the
# recipe drives `make lib` directly. Pins the v1.1.0 release tarball asset by
# sha256 via conandata.yml — that asset is published by distribution-packaging.md
# T3 (a maintainer action) and does not exist yet, so `make docker-syspkg-conan`
# is hermetic: it repoints conandata's url at the reproducible make-package-source
# tarball and matches its digest. Upstreaming to conan-center-index (recipe PR +
# first-time CLA) is the maintainer-gated step (T12 step 4).


class AsmtestConan(ConanFile):
    name = "asmtest"
    description = (
        "C-hosted unit-testing framework for assembly language "
        "(static core: library, headers, pkg-config)."
    )
    license = "MIT"
    url = "https://github.com/conan-io/conan-center-index"
    homepage = "https://github.com/wilvk/asm-test"
    topics = ("testing", "unit-testing", "assembly", "x86-64", "aarch64")
    package_type = "static-library"
    settings = "os", "arch", "compiler", "build_type"

    def source(self):
        get(self, **self.conan_data["sources"][self.version], strip_root=True)

    def build(self):
        self.run("make lib", cwd=self.source_folder)

    def package(self):
        copy(self, "LICENSE",
             src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))
        copy(self, "*.h",
             src=os.path.join(self.source_folder, "include"),
             dst=os.path.join(self.package_folder, "include", "asmtest"))
        copy(self, "asm_nasm.inc",
             src=os.path.join(self.source_folder, "include"),
             dst=os.path.join(self.package_folder, "include", "asmtest"))
        copy(self, "libasmtest.a",
             src=os.path.join(self.source_folder, "build"),
             dst=os.path.join(self.package_folder, "lib"))

    def package_info(self):
        self.cpp_info.libs = ["asmtest"]
        self.cpp_info.includedirs = [os.path.join("include", "asmtest")]
