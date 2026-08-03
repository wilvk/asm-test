#!/usr/bin/env python3
"""verify-shot-recordings.py — assert each capture carries the event kinds its
scene needs, so a producer regression surfaces here rather than as an empty
scene in a committed screenshot.

NOTE: df_step operands live under the key "ops". "vals" does not exist; reading
it yields zero records and looks exactly like a producer gap. That is a measured
fact about the schema, not a guess."""
import json
import sys

REC_DIR = sys.argv[1] if len(sys.argv) > 1 else "build/shots/rec"


def load(name):
    kinds, wide_regs = {}, set()
    with open(f"{REC_DIR}/{name}", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                o = json.loads(line)
            except json.JSONDecodeError:
                continue
            if not isinstance(o, dict):
                continue
            k = o.get("k", "?")
            kinds[k] = kinds.get(k, 0) + 1
            if k == "df_step":
                for v in o.get("ops") or []:
                    if v.get("wide") and v.get("space") == "reg":
                        wide_regs.add(v["reg"])
    return kinds, wide_regs


failures = []
report = []


def need(name, kinds, kind, atleast, why):
    got = kinds.get(kind, 0)
    report.append(f"  {name:24s} {kind:12s} {got:6d}  (need >= {atleast})")
    if got < atleast:
        failures.append(f"{name}: {kind} count {got} < {atleast} — {why}")


k, _ = load("tree.asmtrace")
need("tree.asmtrace", k, "call", 50, "ModuleRibbon has no call tree")

k, _ = load("trace-blend.asmtrace")
need("trace-blend.asmtrace", k, "trace", 100, "Invocation has no instructions")
need("trace-blend.asmtrace", k, "coverage", 4,
     "a coverage event CLOSES an invocation; <4 gives too few slabs")

for side in ("df-a.asmtrace", "df-b.asmtrace"):
    k, wide = load(side)
    need(side, k, "df_step", 20, "Plane/LanePrism have no dataflow")
    need(side, k, "mem", 10, "the data-cell layers have no addresses")
    need(side, k, "statediff", 1, "Divergence cannot diff without statediff")
    report.append(f"  {side:24s} {'wide regs':12s} {len(wide):6d}  (need >= 1)")
    if not wide:
        failures.append(
            f"{side}: no wide reg records — LanePrism would be empty. "
            "Check that blend_tile still compiles to SSE.")

print("\n".join(report))
if failures:
    print("FAIL:")
    for f in failures:
        print("  " + f)
    sys.exit(1)
print("PASS: all four recordings carry the kinds their scenes need")
