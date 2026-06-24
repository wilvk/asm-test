# Go binding image (Track G). The distro Go toolchain + cgo (the C toolchain is
# already in the base). Build context = repo root.
ARG BASE_IMAGE=asmtest-bindings-base
FROM ${BASE_IMAGE}
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends golang-go \
 && rm -rf /var/lib/apt/lists/*
# Use the installed toolchain as-is and stay offline: the module has no deps.
ENV GOTOOLCHAIN=local GOFLAGS=-mod=mod GOPROXY=off
COPY . .
CMD ["make", "go-test"]
