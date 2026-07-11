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
    return sorted(
        (b["name"], b["arch"], b["value"], b.get("blocks"))
        for b in doc.get("benchmarks", []))


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        golden = key_rows(json.load(open(sys.argv[1])))
        fresh = key_rows(json.load(open(sys.argv[2])))
    except (OSError, ValueError) as e:
        print("bench-golden-check: %s" % e, file=sys.stderr)
        return 2
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
