# packaging/homebrew/asmtest.rb — Homebrew formula for the asm-test C core.
#
# Installs the documented primary artifact: the static libasmtest.a, its headers,
# and the asmtest.pc pkg-config file. MIT only — the core links nothing
# third-party (the GPL engines are conveyed solely by the dlopen *binding*
# payloads, which this formula does NOT ship). The trace tiers stay make-time
# opt-ins, so the from-source build here is toolchain-light (a C compiler + make).
#
# Verified in CI by `make docker-syspkg-brew` (Homebrew runs on Linux):
# build-from-source + `brew test`/`audit`/`style`. That lane is hermetic — it
# repoints `url` at the reproducible `make package-source` tarball it just built,
# because the v1.1.0 release asset pinned below is published by
# distribution-packaging.md T3 (a maintainer/credential action) and does not
# exist in the tree yet. The `sha256` is the digest of that tarball at authoring;
# it is finalized against the real asset at the tag, and the lane recomputes it
# from a local build, so CI verification never depends on this constant.
#
# Publish (maintainer-gated): create the GitHub repo `wilvk/homebrew-asmtest`
# holding `Formula/asmtest.rb`; users then `brew tap wilvk/asmtest && brew install
# asmtest`. Homebrew *core* is out of reach until the acceptable-formulae bar is
# met (>=90 forks OR >=90 watchers OR >=225 stars for a self-submitted formula).
class Asmtest < Formula
  desc "C-hosted unit-testing framework for assembly language"
  homepage "https://github.com/wilvk/asm-test"
  url "https://github.com/wilvk/asm-test/releases/download/v1.1.0/asm-test-1.1.0.tar.gz"
  sha256 "9d44fbcd88745348f0886c20baedeed1a9bc332ba5162dab5fe78fbc0c62197d"
  license "MIT"

  depends_on "pkg-config" => [:build, :test]

  def install
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    # libasmtest.a provides main() (the TAP runner); a consumer supplies TEST()
    # cases and links the lib via pkg-config — the whole chain a package must get
    # right (headers resolve, static lib links, a real symbol runs).
    (testpath/"consumer.c").write <<~C
      #include <asmtest.h>
      TEST(brew, links_and_runs) {
        void *p = asmtest_guarded_alloc(64);
        ASSERT_TRUE(p != NULL);
        asmtest_guarded_free(p, 64);
      }
    C
    flags = Utils.safe_popen_read("pkg-config", "--cflags", "--libs", "asmtest").split
    system ENV.cc, "consumer.c", "-o", "consumer", *flags
    system "./consumer"
  end
end
