---
name: Bug report
about: A defect in the framework, a binding, or the build
title: ""
labels: bug
assignees: ""
---

## Summary

A clear, concise description of the bug.

## Environment

- asm-test version / commit:
- OS + arch (e.g. macOS 14 arm64, Ubuntu 24.04 x86-64):
- Compiler (`cc --version`):
- Backend / tier: GAS / NASM / emulator / in-line asm / native-trace / Win64
- Binding (if applicable) + its toolchain version:

## Reproduction

Steps and a minimal example (assembly routine + test, or the failing `make`
target). The smaller the repro, the faster the fix.

```sh
# commands you ran
```

## Expected vs. actual

What you expected to happen, and what actually happened (include the TAP/JUnit
output or error message).

## Notes

Anything else — does it reproduce under `make docker-test`? On both arches?
