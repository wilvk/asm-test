# C++ binding image (Track X). No extra toolchain — g++ ships with the base's
# build-essential. Build context = repo root.
ARG BASE_IMAGE=asmtest-bindings-base
FROM ${BASE_IMAGE}
COPY . .
CMD ["make", "cpp-test"]
