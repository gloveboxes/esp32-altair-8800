# syntax=docker/dockerfile:1.7

FROM alpine:3.22 AS build

ARG BUILDPLATFORM
ARG TARGETPLATFORM
ARG TARGETARCH

RUN echo "Building altair-local on ${BUILDPLATFORM} for ${TARGETPLATFORM} (${TARGETARCH})" \
    && apk add --no-cache build-base cmake curl-dev

WORKDIR /src

COPY altair_local/ altair_local/
COPY altair8800/ altair8800/
COPY port_drivers/ port_drivers/
# altair_local shares i8080_disasm.c / cpu_state.c from main/ and
# virtual_monitor.c from front_panel/ (see altair_local/CMakeLists.txt).
COPY main/ main/
COPY front_panel/ front_panel/
COPY disks/ disks/
COPY Apps/ Apps/

RUN cmake -S altair_local -B /tmp/altair-local-build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /tmp/altair-local-build --parallel \
    && strip /tmp/altair-local-build/altair-local

FROM alpine:3.22 AS runtime

ARG TARGETPLATFORM
ARG TARGETARCH

RUN apk add --no-cache ca-certificates libcurl

WORKDIR /opt/altair

COPY --from=build /tmp/altair-local-build/altair-local /usr/local/bin/altair-local
COPY --from=build /src/Apps ./Apps
COPY --from=build /src/disks ./disks
COPY docker/docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
COPY docker/altair_env.txt ./runtime/altair_env.txt

RUN chmod +x /usr/local/bin/docker-entrypoint.sh

LABEL org.opencontainers.image.description="Altair 8800 local runner" \
    org.opencontainers.image.licenses="MIT" \
    org.opencontainers.image.source="https://github.com/dave/esp32-altair-8800" \
    org.opencontainers.image.vendor="dave" \
    org.opencontainers.image.architecture="$TARGETARCH" \
    org.opencontainers.image.platform="$TARGETPLATFORM"

ENV ALTAIR_APPS_ROOT=/opt/altair/Apps \
    ALTAIR_ENV_FILE=/opt/altair/runtime/altair_env.txt \
    ALTAIR_DISKS_DIR=/opt/altair/disks \
    ALTAIR_DRIVE_A=cpm63k.dsk \
    ALTAIR_DRIVE_B=bdsc-v1.60.dsk \
    ALTAIR_DRIVE_C=escape-posix.dsk \
    ALTAIR_DRIVE_D=blank.dsk

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
