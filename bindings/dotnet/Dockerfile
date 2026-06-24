# .NET binding image (Track D). The .NET SDK for the P/Invoke console app.
# Build context = repo root.
ARG BASE_IMAGE=asmtest-bindings-base
FROM ${BASE_IMAGE}
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends dotnet-sdk-8.0 \
 && rm -rf /var/lib/apt/lists/*
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
COPY . .
CMD ["make", "dotnet-test"]
