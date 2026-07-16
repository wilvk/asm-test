# Tooling

Use Docker containers via the Makefile's `docker-*` targets where possible
instead of installing tools locally. Only install on the host when no
`docker-*` target covers the need. Run `make help` to see the targets.

# Missing dependencies

A missing dependency is not a blocker. Add it and continue — do not stop, narrow
the feature, or leave behind a lane that can only self-skip.

Add it where the work runs: the relevant `Dockerfile.*` and its `docker-*` rule,
not the host. Follow the pattern already in the tree, and **pin the version**:

- prebuilt release tarball — `ARG DR_VERSION` + `curl` + `ENV DYNAMORIO_HOME`
  (`Dockerfile.drtrace`, DynamoRIO 11.91.20630); this block drops into any image
- built from pinned source — `scripts/build-capstone.sh` (Capstone 5.0.1)
- fetched headers — `GCPROBE_RT_TAG ?= v8.0.8` sparse checkout (`mk/native-trace.mk`)
- apt, where a pinned distro package suffices (`libipt-dev`, `libncurses-dev`)

If a lane lacks what a feature needs in order to be **tested**, extend that
image. A test that can only ever self-skip is not a test, and "no lane can test
it" is a fact about the image, not about the feature.

This does not apply to what cannot be installed: **hardware** (Intel PT, ARM
CoreSight, Apple silicon, a specific CPU generation) and **credentials**. Those
are real gates — record them and self-skip. Everything else, add.
