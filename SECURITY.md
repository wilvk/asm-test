# Security Policy

## Supported versions

asm-test follows semantic versioning (see [VERSION](VERSION) and
[CHANGELOG.md](CHANGELOG.md)). Security fixes land on the latest released minor
series.

| Version | Supported          |
| ------- | ------------------ |
| 1.0.x   | :white_check_mark: |
| < 1.0   | :x:                |

## Reporting a vulnerability

Please report security issues **privately** — do not open a public issue for a
suspected vulnerability.

1. Preferred: open a private report via GitHub Security Advisories
   ("Report a vulnerability") at
   <https://github.com/wilvk/asm-test/security/advisories/new>.
2. Alternatively, email the maintainer at <willvk@gmail.com> with details and,
   if possible, a minimal reproducer.

We aim to acknowledge a report within a few days and to agree on a disclosure
timeline once the issue is confirmed. Please give us a reasonable window to ship
a fix before any public disclosure.

## Scope notes

asm-test is a developer testing framework, not a sandbox. By design it runs the
assembly routines under test — natively through the real ABI, or inside the
optional emulator/in-line-assembler tiers (Unicorn/Keystone) — and the
property-testing and fuzzing modes feed those routines generated input. **Do not
run untrusted routines or untrusted assembly strings through it** expecting
isolation; the guard-page and crash-containment features catch *bugs*, they are
not a security boundary.

Reports we are particularly interested in: memory-safety defects in the
framework's own C runtime (`src/`), in the capture trampoline, or in the
bindings' FFI marshalling — i.e. cases where a *well-formed* routine or input
corrupts or crashes the **framework** itself rather than the code under test.
