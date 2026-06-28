# test_drtrace.rb — standalone smoke test for the Ruby DynamoRIO native-trace
# wrapper (drtrace.rb). Mirrors bindings/python/tests/test_drtrace.py.
#
# Run it directly:
#   cd bindings/ruby
#   ASMTEST_DRAPP_LIB=$PWD/../../build/libasmtest_drapp.so ruby test_drtrace.rb
#
# Self-skips unless the tier is built AND DynamoRIO is resolvable (and the DR client
# is set): prints "SKIP: ..." and exits 0 so an unconfigured host passes cleanly. On
# a DR-capable host build with `make shared-drtrace drtrace-client DYNAMORIO_HOME=...`
# and export ASMTEST_DRCLIENT (and ASMTEST_DR_LIB / DYNAMORIO_HOME).
require_relative "drtrace"

NativeTrace = Asmtest::DrTrace::NativeTrace
NativeCode  = Asmtest::DrTrace::NativeCode

# mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
ROUTINE = [0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
           0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3].pack("C*")

def skip(msg)
  puts "SKIP: #{msg}"
  exit 0
end

# Self-skip cleanly when the tier can't run (lib absent / no DynamoRIO / no client).
skip("DynamoRIO native-trace tier unavailable (self-skip)") unless NativeTrace.available?
skip("ASMTEST_DRCLIENT not set (build the DR client)") if ENV["ASMTEST_DRCLIENT"].to_s.empty?

begin
  NativeTrace.start
rescue RuntimeError => e
  skip("dr_init/start failed: #{e.message}")
end

# ---- block coverage + accumulation across two regions ----
code = NativeCode.from_bytes(ROUTINE)
tr = NativeTrace.create(blocks: 64, instructions: 0)
tr.register("add2", code)

r = nil
tr.region("add2") { r = code.call(20, 22) }
raise "expected 42, got #{r}" unless r == 42
raise "entry block not covered" unless tr.covered?(0)

before = tr.blocks_len
r2 = nil
tr.region("add2") { r2 = code.call(60, 60) } # 120 > 100 -> dec -> 119, other block
raise "expected 119, got #{r2}" unless r2 == 119
raise "blocks_len shrank" unless tr.blocks_len >= before
raise "marker imbalance" unless NativeTrace.marker_error == 0

tr.unregister("add2")
code.free
tr.free

# ---- instruction mode: an ordered instruction stream is recorded ----
code2 = NativeCode.from_bytes(ROUTINE)
tr2 = NativeTrace.create(blocks: 64, instructions: 64)
tr2.register("add2i", code2)
r3 = nil
tr2.region("add2i") { r3 = code2.call(1, 2) }
raise "expected 3, got #{r3}" unless r3 == 3
raise "no instructions recorded" unless tr2.insns_total >= 4

tr2.unregister("add2i")
code2.free
tr2.free

NativeTrace.shutdown
puts "PASS"
