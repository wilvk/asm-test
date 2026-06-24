# Ruby binding image (Track C). Ruby (Fiddle is stdlib — no gem needed).
# Build context = repo root.
ARG BASE_IMAGE=asmtest-bindings-base
FROM ${BASE_IMAGE}
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends ruby \
 && rm -rf /var/lib/apt/lists/*
COPY . .
CMD ["make", "ruby-test"]
