# Configuration file for the Sphinx documentation builder.
#
# Full reference:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

import datetime
import os

# -- Project information -----------------------------------------------------

project = "asm-test"
author = "asm-test contributors"
copyright = f"{datetime.date.today().year}, {author}"

# Read the version from the repo VERSION file (the same source check-version and
# sync-version use) so the published docs banner never drifts from the shipped
# ASMTEST_VERSION.
with open(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "VERSION"),
    encoding="utf-8",
) as _vf:
    release = _vf.read().strip()  # full semver, e.g. "1.1.0"
version = ".".join(release.split(".")[:2])  # "major.minor", e.g. "1.1"

# -- General configuration ---------------------------------------------------

extensions = [
    "myst_parser",          # author the docs in Markdown
    "sphinx_copybutton",    # a copy button on every code block
    "sphinxcontrib.mermaid",  # render ```mermaid diagrams (architecture diagrams)
]

# Author pages in Markdown (.md) via MyST; .rst still works if ever needed.
source_suffix = {
    ".md": "markdown",
    ".rst": "restructuredtext",
}

master_doc = "index"
language = "en"

exclude_patterns = ["_build", "Thumbs.db", ".DS_Store", "plans/*", "analysis/*", "reviews/*", "summaries/*"]

# -- MyST (Markdown) configuration -------------------------------------------

myst_enable_extensions = [
    "colon_fence",     # ::: fenced directives
    "deflist",         # definition lists
    "fieldlist",
    "smartquotes",
    "substitution",
    "tasklist",
]

# Generate anchors for headings down to <h3> so cross-page links resolve.
myst_heading_anchors = 3

# Treat ```mermaid fenced code blocks as the `mermaid` directive, so the diagrams
# are authored as plain fenced blocks that also render natively on GitHub.
myst_fence_as_directive = ["mermaid"]

# -- sphinxcontrib-mermaid ---------------------------------------------------

# The HTML and ePub builders render diagrams client-side with mermaid.js (no
# extra tooling). The LaTeX/PDF builder instead rasterizes each diagram with the
# Mermaid CLI (`mmdc`), which Read the Docs installs via the nodejs build job in
# .readthedocs.yaml. `mmdc` drives a headless Chromium through Puppeteer; in the
# RTD container that must run with --no-sandbox (see docs/puppeteer-config.json).
mermaid_params = [
    "-p",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "puppeteer-config.json"),
]

# -- Options for HTML output -------------------------------------------------

html_theme = "furo"
html_title = f"asm-test {version}"

html_theme_options = {
    "source_repository": "https://github.com/wilvk/asm-test",
    "source_branch": "main",
    "source_directory": "docs/",
    "navigation_with_keys": True,
}

# A best-effort link to the repo even when the theme option is unused.
html_context = {
    "display_github": True,
    "github_user": "wilvk",
    "github_repo": "asm-test",
    "github_version": "main",
    "conf_py_path": "/docs/",
}

pygments_style = "default"
pygments_dark_style = "monokai"

# -- sphinx-copybutton -------------------------------------------------------

# Strip common shell/REPL prompts when copying code samples.
copybutton_prompt_text = r">>> |\.\.\. |\$ "
copybutton_prompt_is_regexp = True
