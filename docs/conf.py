# Configuration file for the Sphinx documentation builder.
#
# Full reference:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

import datetime

# -- Project information -----------------------------------------------------

project = "asm-test"
author = "asm-test contributors"
copyright = f"{datetime.date.today().year}, {author}"

# Keep in sync with ASMTEST_VERSION in include/asmtest.h.
release = "1.0.0"
version = "1.0"

# -- General configuration ---------------------------------------------------

extensions = [
    "myst_parser",        # author the docs in Markdown
    "sphinx_copybutton",  # a copy button on every code block
]

# Author pages in Markdown (.md) via MyST; .rst still works if ever needed.
source_suffix = {
    ".md": "markdown",
    ".rst": "restructuredtext",
}

master_doc = "index"
language = "en"

exclude_patterns = ["_build", "Thumbs.db", ".DS_Store", "plans/*", "analysis/*"]

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
