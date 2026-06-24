# Lua binding image (Track C). LuaJIT for its `ffi`. Build context = repo root.
ARG BASE_IMAGE=asmtest-bindings-base
FROM ${BASE_IMAGE}
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends luajit \
 && rm -rf /var/lib/apt/lists/*
COPY . .
CMD ["make", "lua-test"]
