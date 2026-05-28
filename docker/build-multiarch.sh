#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

PLATFORMS=${PLATFORMS:-linux/amd64,linux/arm64}
BUILD_CONTEXT=${BUILD_CONTEXT:-$REPO_ROOT}
DOCKERFILE=${DOCKERFILE:-$SCRIPT_DIR/Dockerfile}
BUILDER_NAME=${BUILDER_NAME:-altair8800v2-multiarch}
IMAGE_NAME=${IMAGE_NAME:-altair8800v2}
DOCKER_TAG=${DOCKER_TAG:-latest}

if [ -z "${DOCKER_IMAGE:-}" ]; then
    if [ -z "${DOCKER_USER:-}" ]; then
        printf 'Docker Hub username: '
        IFS= read -r DOCKER_USER
    fi

    : "${DOCKER_USER:?Set DOCKER_USER to your Docker Hub username, or set DOCKER_IMAGE directly}"
    DOCKER_IMAGE=${DOCKER_USER}/${IMAGE_NAME}:${DOCKER_TAG}
fi

if ! docker buildx inspect "$BUILDER_NAME" >/dev/null 2>&1; then
    docker buildx create --name "$BUILDER_NAME" --use >/dev/null
else
    docker buildx use "$BUILDER_NAME" >/dev/null
fi

docker buildx inspect --bootstrap >/dev/null

docker buildx build \
    --no-cache \
    --platform "$PLATFORMS" \
    -f "$DOCKERFILE" \
    -t "$DOCKER_IMAGE" \
    --push \
    "$BUILD_CONTEXT"