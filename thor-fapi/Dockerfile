# syntax=docker/dockerfile:1

ARG UBUNTU_VERSION=24.04

FROM ubuntu:${UBUNTU_VERSION} AS builder

ARG TARGETARCH
RUN test "${TARGETARCH}" = "amd64"

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential \
       cmake \
       libsctp-dev \
       zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

ARG THOR_GIT_COMMIT_HASH=container
ARG THOR_GIT_COMMIT_DATE=unknown
RUN cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=ON \
      -DTHOR_GIT_COMMIT_HASH="${THOR_GIT_COMMIT_HASH}" \
      -DTHOR_GIT_COMMIT_DATE="${THOR_GIT_COMMIT_DATE}" \
    && cmake --build build --parallel

FROM builder AS test
CMD ["bash", "tests/run_docker_tests.sh"]

FROM ubuntu:${UBUNTU_VERSION} AS runtime

ARG TARGETARCH
RUN test "${TARGETARCH}" = "amd64"

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       libsctp1 \
       zlib1g \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/nfapi-proxy /usr/local/bin/nfapi-proxy
COPY --from=builder /src/build/control_client /usr/local/bin/control_client

ENV THOR_CTRL_SOCK=/var/run/thor_nfapi_proxy.sock
LABEL org.opencontainers.image.title="Thor nFAPI Proxy" \
      org.opencontainers.image.architecture="amd64"
EXPOSE 50002/sctp
EXPOSE 50012/udp

HEALTHCHECK --interval=10s --timeout=2s --start-period=5s --retries=3 \
  CMD ["control_client", "list_l1"]

ENTRYPOINT ["nfapi-proxy"]
