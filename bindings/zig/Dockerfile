# Zig binding image (Track Z). Fetches the official Zig tarball for the build
# arch (no apt package). Build context = repo root.
ARG BASE_IMAGE=asmtest-bindings-base
FROM ${BASE_IMAGE}
ARG ZIG_VERSION=0.13.0
RUN set -eux; \
    arch="$(uname -m)"; \
    url="https://ziglang.org/download/${ZIG_VERSION}/zig-linux-${arch}-${ZIG_VERSION}.tar.xz"; \
    curl -fsSL "$url" -o /tmp/zig.tar.xz; \
    mkdir -p /opt/zig; \
    tar -xJf /tmp/zig.tar.xz -C /opt/zig --strip-components=1; \
    rm /tmp/zig.tar.xz; \
    ln -s /opt/zig/zig /usr/local/bin/zig; \
    zig version
COPY . .
CMD ["make", "zig-test"]
