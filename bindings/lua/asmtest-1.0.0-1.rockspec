-- Packaging scaffolding (see docs/packaging.md). A LuaRocks manifest for the
-- asm-test Lua binding (LuaJIT). `make lua-package` stages the prebuilt
-- libasmtest_emu into native/<plat>/ before packing, so the rock installs the
-- native library the LuaJIT `ffi` binding loads at run time. A real release
-- builds the native/<plat>/ payload for each target platform.
package = "asmtest"
version = "1.0.0-1"

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
   license = "MIT",
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
      asmtest = "conformance.lua",
   },
   install = {
      lib = {},  -- populated by `make lua-package` from native/<plat>/
   },
}
