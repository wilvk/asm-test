# Packaging scaffolding (see docs/packaging.md). A RubyGems manifest for the
# asm-test Ruby binding. `make ruby-package` stages the prebuilt libasmtest_emu
# into native/<plat>/ before `gem build`, so the gem bundles the native library
# the stdlib-Fiddle binding dlopen()s at run time. A real release stages a
# native/<plat>/ payload for each target platform (or ships per-platform gems).
Gem::Specification.new do |s|
  s.name        = "asmtest"
  s.version     = "1.0.0"
  s.summary     = "Ruby binding for the asm-test assembly unit-testing framework"
  s.description = "Run, capture, and emulate assembly routines through asm-test " \
                  "from Ruby via the stdlib Fiddle FFI over the opaque-handle ABI."
  s.authors     = ["asm-test contributors"]
  s.homepage    = "https://github.com/wilvk/asm-test"
  s.license     = "MIT"
  s.required_ruby_version = ">= 2.6"

  # The binding source + README + whatever native payloads have been staged.
  s.files = ["conformance.rb", "README.md"] + Dir["native/**/*"]
  s.require_paths = ["."]

  # The emulator native lib links libunicorn at run time.
  s.requirements << "libunicorn (loaded by the bundled libasmtest_emu)"
  s.metadata = {
    "source_code_uri" => "https://github.com/wilvk/asm-test",
    "documentation_uri" => "https://asm-test.readthedocs.io",
  }
end
