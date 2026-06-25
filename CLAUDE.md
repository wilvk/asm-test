# Tooling

Use Docker containers via the Makefile's `docker-*` targets where possible
instead of installing tools locally. Only install on the host when no
`docker-*` target covers the need. Run `make help` to see the targets.
