-- LuaRocks manifest for the asm-test Lua binding (LuaJIT). The rock exposes the
-- reusable library module (asmtest.lua) — `require("asmtest")` — not the
-- conformance test runner. `make lua-package` stages the prebuilt libasmtest_emu
-- into native/<plat>/ from build/dist/native/ before packing, so the rock
-- installs the native library the LuaJIT `ffi` binding loads at run time, one
-- slot per platform present in the collected payload (see docs/packaging.md).
package = "asmtest"
version = "1.1.0-1"

source = {
   url = "git+https://github.com/wilvk/asm-test.git",
}

description = {
   summary = "Lua (LuaJIT) binding for the asm-test assembly unit-testing framework",
   detailed = [[
      Run, capture, and emulate assembly routines through asm-test from LuaJIT
      via its `ffi`, over the opaque-handle binding ABI. The emulator native lib
      links libunicorn at run time.
   ]],
   homepage = "https://github.com/wilvk/asm-test",
   -- asm-test is MIT, but the bundled native payload conveys Unicorn + Keystone
   -- (GPL-2.0) and Capstone (BSD-3-Clause), so the rock as distributed is
   -- effectively GPL-2.0. See native/<plat>/THIRD-PARTY-LICENSES.
   license = "MIT AND GPL-2.0-only AND BSD-3-Clause",
}

supported_platforms = { "linux", "macosx" }

dependencies = {
   "lua >= 5.1",  -- LuaJIT
}

-- Install the binding module plus the staged native library. The lib name is
-- platform-resolved by `make lua-package`; LuaJIT's ffi loads it via ASMTEST_LIB
-- or the installed lib dir.
build = {
   type = "builtin",
   modules = {
      asmtest = "asmtest.lua",
   },
   install = {
      lib = {},  -- populated by `make lua-package` from native/<plat>/
   },
}
