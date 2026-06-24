# Node binding image (Track N). Node.js + koffi (installed globally; NODE_PATH
# makes require() find it). Build context = repo root.
ARG BASE_IMAGE=asmtest-bindings-base
FROM ${BASE_IMAGE}
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends nodejs npm \
 && rm -rf /var/lib/apt/lists/* \
 && npm install -g koffi
ENV NODE_PATH=/usr/local/lib/node_modules:/usr/lib/node_modules
COPY . .
CMD ["make", "node-test"]
