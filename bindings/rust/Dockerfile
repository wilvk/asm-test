# Rust binding image (Track R). Isolated toolchain on the shared base.
# Build context = repo root.
ARG BASE_IMAGE=asmtest-bindings-base
FROM ${BASE_IMAGE}
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends cargo rustc \
 && rm -rf /var/lib/apt/lists/*
COPY . .
CMD ["make", "rust-test"]
