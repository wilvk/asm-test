# frozen_string_literal: true

# Ruby data-flow binding smoke (Phase 6): GC-move canonicalizer + method resolver,
# mirroring the Python/C++/Node suites' semantics.
require_relative "dataflow"

DF = Asmtest::DataFlow
$n = 0
$failed = false

def check(cond, desc)
  $n += 1
  puts((cond ? "ok " : "not ok ") + $n.to_s + " - " + desc)
  $failed = true unless cond
end

# --- GC-move canonicalizer (forward-to-final) --- #
check(DF.gcmove_canon([], 0, 0x1234) == 0x1234, "gcmove: empty move set is identity")
mv = [[0x1000, 0x2000, 0x100, 5]]
check(DF.gcmove_canon(mv, 3, 0x1010) == 0x2010, "gcmove: pre-move addr forwards to final")
check(DF.gcmove_canon(mv, 3, 0x1000) == 0x2000, "gcmove: object base forwards")
check(DF.gcmove_canon(mv, 3, 0x10FF) == 0x20FF, "gcmove: last byte of half-open window forwards")
check(DF.gcmove_canon(mv, 3, 0x1100) == 0x1100, "gcmove: one past the window not forwarded")
check(DF.gcmove_canon(mv, 5, 0x1010) == 0x1010, "gcmove: at-move-step observation not forwarded")
check(DF.gcmove_canon(mv, 3, 0x3000) == 0x3000, "gcmove: out-of-range addr unchanged")
mv2 = [[0x1000, 0x2000, 0x100, 3], [0x2000, 0x3000, 0x100, 6]]
check(DF.gcmove_canon(mv2, 1, 0x1010) == 0x3010, "gcmove: two compactions compose to final")

# --- method resolver (tiered re-JIT aware) --- #
ms = [[0x1000, 0x40, "Foo", 3], [0x2000, 0x20, "Bar", 1], [0x3000, 0, "Baz", 2]]
check(DF.method_resolve_pc(ms, 0x1000) == 0, "method: Foo range start")
check(DF.method_resolve_pc(ms, 0x103F) == 0, "method: Foo last byte (half-open)")
check(DF.method_resolve_pc(ms, 0x1040) == -1, "method: one past Foo -> none")
check(DF.method_resolve_pc(ms, 0x2010) == 1, "method: Bar range")
check(DF.method_resolve_pc(ms, 0x3000) == 2, "method: Baz point match")
check(DF.method_resolve_pc(ms, 0x3001) == -1, "method: Baz is point-only")
rj = [[0x1000, 0x40, "Foo", 1], [0x1000, 0x40, "Foo", 5]]
check(DF.method_resolve_pc(rj, 0x1010) == 1, "method: tiered re-JIT newest version wins")
check(DF.method_resolve_pc([], 0x1000) == -1, "method: empty map -> -1")

puts "1..#{$n}"
exit($failed ? 1 : 0)
