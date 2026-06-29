<!-- Thanks for contributing! Keep PRs focused; see CONTRIBUTING.md. -->

## What & why

<!-- What does this change, and why? Link any related issue (e.g. Closes #123). -->

## How it was tested

<!-- Which tiers did you run? Tick what applies. -->

- [ ] `make WERROR=1 test && make WERROR=1 check` (clean build, warnings as errors)
- [ ] Relevant optional tier(s): emulator / in-line asm / native-trace / Win64
- [ ] Affected language binding(s): `make <lang>-test`
- [ ] Docker lane(s): `make docker-…`
- [ ] N/A (docs-only)

<!-- Note any lane you could not run locally so reviewers know to watch CI. -->

## Checklist

- [ ] Commit messages follow Conventional Commits with a scope (e.g. `fix(emu): …`)
- [ ] Portability preserved (x86-64 + AArch64; new routines carry both bodies,
      and the NASM counterpart for x86-64 example routines)
- [ ] Docs under `docs/` updated if behavior/API changed
- [ ] `CHANGELOG.md` `[Unreleased]` updated for user-visible changes
