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

