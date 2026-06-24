# Java binding image (Track J). JDK 21 for the FFM/Panama API (preview).
# Build context = repo root.
ARG BASE_IMAGE=asmtest-bindings-base
FROM ${BASE_IMAGE}
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends openjdk-21-jdk-headless \
 && rm -rf /var/lib/apt/lists/*
COPY . .
CMD ["make", "java-test"]
