# Python binding image (Track P). Isolated toolchain on the shared base.
# Build context = repo root: docker build -f bindings/python/Dockerfile .
ARG BASE_IMAGE=asmtest-bindings-base
FROM ${BASE_IMAGE}
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends python3 python3-pytest \
 && rm -rf /var/lib/apt/lists/*
COPY . .
CMD ["make", "python-test"]
