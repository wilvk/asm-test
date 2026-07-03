# RubyGems manifest for the asm-test Ruby binding. The gem exposes the reusable
# library module (asmtest.rb) — `require "asmtest"` — not the conformance test
# runner. `make ruby-package` stages the prebuilt libasmtest_emu into
# native/<plat>/ from build/dist/native/ before `gem build`, so the gem bundles
# the native library the stdlib-Fiddle binding dlopen()s at run time, one slot
# per platform present in the collected payload (see docs/packaging.md).
Gem::Specification.new do |s|
  s.name        = "asmtest"
  s.version     = "1.1.0"
  s.summary     = "Ruby binding for the asm-test assembly unit-testing framework"
  s.description = "Run, capture, and emulate assembly routines through asm-test " \
                  "from Ruby via the stdlib Fiddle FFI over the opaque-handle ABI."
  s.authors     = ["asm-test contributors"]
  s.homepage    = "https://github.com/wilvk/asm-test"
  # asm-test's own code is MIT, but the bundled native payload conveys the
  # Unicorn + Keystone engines (GPL-2.0) and Capstone (BSD-3-Clause), so the gem
  # as distributed is effectively GPL-2.0. See native/<plat>/THIRD-PARTY-LICENSES.
  s.licenses    = ["MIT", "GPL-2.0-only", "BSD-3-Clause"]
  s.required_ruby_version = ">= 2.6"

  # The reusable library module + README + whatever native payloads have been
  # staged — the superset libasmtest_emu, the vendored Unicorn/Keystone/Capstone,
  # and THIRD-PARTY-LICENSES (conformance.rb is the dev test runner, not shipped).
  s.files = ["asmtest.rb", "README.md"] + Dir["native/**/*"]
  s.require_paths = ["."]

  # The emulator native lib links libunicorn at run time.
  s.requirements << "libunicorn (loaded by the bundled libasmtest_emu)"
  s.metadata = {
    "source_code_uri" => "https://github.com/wilvk/asm-test",
    "documentation_uri" => "https://asm-test.readthedocs.io",
  }
end
