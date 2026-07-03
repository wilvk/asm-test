# docs.mk — Sphinx / Read the Docs documentation targets.
#
# Included by ../Makefile (split out by concern for maintainability). All
# variables/knobs (CSTD, WERROR, ASM_SYNTAX, BUILD, ...) come from the parent
# Makefile, which reads this file in place; edit targets here, knobs there.

# --- Documentation (Sphinx → Read the Docs) --------------------------------
# `make docs`           build the HTML docs into docs/_build/html
# `make docs-serve`     build, then serve them on http://localhost:$(DOCS_PORT)
# `make docs-linkcheck` verify external links resolve
# `make docs-clean`     remove the built docs
# Mirrors the Read the Docs build (config in docs/conf.py + .readthedocs.yaml);
# SPHINXOPTS defaults to -W to match its fail_on_warning. Install the optional
# doc toolchain with `pip install -r docs/requirements.txt`.
SPHINXBUILD ?= sphinx-build
SPHINXOPTS  ?= -W --keep-going
DOCS_SRC    := docs
DOCS_OUT    := docs/_build
DOCS_PORT   ?= 8000

.PHONY: docs docs-serve docs-linkcheck docs-clean
docs:
	$(SPHINXBUILD) -b html $(SPHINXOPTS) $(DOCS_SRC) $(DOCS_OUT)/html
	@echo "docs: built $(DOCS_OUT)/html/index.html"

docs-serve: docs
	@echo "serving docs on http://localhost:$(DOCS_PORT)/ (Ctrl-C to stop)"
	cd $(DOCS_OUT)/html && python3 -m http.server $(DOCS_PORT)

docs-linkcheck:
	$(SPHINXBUILD) -b linkcheck $(SPHINXOPTS) $(DOCS_SRC) $(DOCS_OUT)/linkcheck

docs-clean:
	rm -rf $(DOCS_OUT)

# --- Documentation in Docker (no host Sphinx toolchain needed) -------------
# `make docker-docs`           build the HTML docs in a throwaway Python image
# `make docker-docs-linkcheck` link-check the docs the same way
# Mirrors `make docs` / `make docs-linkcheck` (same SPHINXOPTS, same docs/conf.py
# and docs/requirements.txt) but installs the Sphinx toolchain inside a container,
# so a contributor without a local Python/Sphinx install can still build and
# warning-check the docs. Runs as the invoking user (venv in /tmp), so the built
# $(DOCS_OUT) on the host stays owned by you. Override the image with DOCS_IMAGE.
DOCKER     ?= docker
DOCS_IMAGE ?= python:3.12-slim

# Bring the toolchain up in a container and hand off to the given sphinx-build
# invocation (reusing the same knobs as the host targets — one source of truth).
# $(1) = sphinx-build argument tail.
_docker_docs_run = $(DOCKER) run --rm -v "$(CURDIR)":/work -w /work -e HOME=/tmp \
	--user $$(id -u):$$(id -g) $(DOCS_IMAGE) sh -euc \
	'python -m venv /tmp/venv && . /tmp/venv/bin/activate && \
	 pip install --quiet -r docs/requirements.txt && \
	 $(SPHINXBUILD) $(1)'

.PHONY: docker-docs docker-docs-linkcheck
docker-docs:
	$(call _docker_docs_run,-b html $(SPHINXOPTS) $(DOCS_SRC) $(DOCS_OUT)/html)
	@echo "docker-docs: built $(DOCS_OUT)/html/index.html"

docker-docs-linkcheck:
	$(call _docker_docs_run,-b linkcheck $(SPHINXOPTS) $(DOCS_SRC) $(DOCS_OUT)/linkcheck)

