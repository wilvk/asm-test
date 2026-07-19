#!/usr/bin/env python3
"""bench-golden-check — gate the deterministic emu-count golden file.

    scripts/bench-golden-check.py <golden.json> <fresh emu-bench json>

The emulated instruction/block counts are a function of the guest code alone —
host- and OS-independent — so the committed golden file must match a fresh run on
every CI leg. A mismatch means the code (or a fixture) changed: regenerate with
`make bench-record` and review the diff. Mirrors the ABI-manifest / conformance
golden gates. Exit 0 on match, 1 on drift (with a readable diff), 2 on I/O error.
"""
import json
import sys


def key_rows(doc):
    """The deterministic insns-count rows, keyed for comparison. `model_cost` rows
    are deliberately EXCLUDED: they exist only where Capstone is linked and their
    value depends on the Capstone version's decode (repo pin 5.0.1; MSYS2 pours
    5.0.9), so they are deterministic PER BUILD but not "one file identical on
    every leg" material — the golden invariant this gate enforces."""
    return sorted(
        (b["name"], b["arch"], b["value"], b.get("blocks"))
        for b in doc.get("benchmarks", [])
        if b.get("kind", "insns") == "insns")


def missing_model_siblings(fresh):
    """Anti-vacuity: if the fresh producer emitted ANY model_cost row, every insns
    row must have a model sibling (same name+arch) — a missing one means the
    producer half-ran (Capstone linked but classification skipped). Returns the
    sorted list of orphaned insns keys, or [] when consistent / no model rows."""
    rows = fresh.get("benchmarks", [])
    model_keys = {(b["name"], b["arch"])
                  for b in rows if b.get("kind") == "model_cost"}
    if not model_keys:
        return []
    insns_keys = {(b["name"], b["arch"])
                  for b in rows if b.get("kind", "insns") == "insns"}
    return sorted(insns_keys - model_keys)


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        golden_doc = json.load(open(sys.argv[1]))
        fresh_doc = json.load(open(sys.argv[2]))
    except (OSError, ValueError) as e:
        print("bench-golden-check: %s" % e, file=sys.stderr)
        return 2
    orphans = missing_model_siblings(fresh_doc)
    if orphans:
        print("bench-golden-check: model_cost rows present but %d insns row(s) "
              "lack a model sibling (the producer half-ran): %s"
              % (len(orphans), ", ".join("%s/%s" % k for k in orphans)),
              file=sys.stderr)
        return 1
    golden = key_rows(golden_doc)
    fresh = key_rows(fresh_doc)
    if golden == fresh:
        print("bench-golden-check: OK (%d deterministic count rows match)"
              % len(golden))
        return 0
    print("bench-golden-check: DRIFT — the emulated counts changed.\n"
          "  Regenerate with `make bench-record` and review the diff.\n",
          file=sys.stderr)
    g = {(n, a): (v, b) for (n, a, v, b) in golden}
    f = {(n, a): (v, b) for (n, a, v, b) in fresh}
    for k in sorted(set(g) | set(f)):
        if g.get(k) != f.get(k):
            print("  %-24s golden=%s  fresh=%s" % (
                "%s/%s" % k, g.get(k, "(absent)"), f.get(k, "(absent)")),
                file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
