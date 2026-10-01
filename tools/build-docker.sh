#!/usr/bin/env bash
# Linux/macOS equivalent of tools/build-docker.ps1: builds Mesa NVK for Switch in
# Docker and, optionally, the game NRO or the game-free Vulkan probe.
#
#   tools/build-docker.sh mesa          # .tools/mesa-sdk/.../libvulkan.a
#   tools/build-docker.sh vk-probe      # out/probe/vk-probe.nro (needs mesa)
#   tools/build-docker.sh game          # app/out/switch/superman_returns.nro
#                                       # (needs mesa, codegen output and SDK exports)
#
# Environment: JOBS (default: nproc, max 16), BUILD_VOLUME (default
# superman-returns-nx-build). Nothing from a Windows checkout is required: the Mesa
# source tarball is produced here from a fresh clone at the pinned commit.
#
# Behind a TLS-intercepting HTTPS proxy (CI, sandboxes) set BUILD_PROXY_CA to the
# proxy's CA bundle and HTTPS_PROXY to the proxy URL. The script then derives a
# local base image that trusts the CA and uses HTTPS apt mirrors, and runs every
# container with host networking and the proxy variables. Without BUILD_PROXY_CA
# the pinned image is used unchanged.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MESA="$ROOT/.tools/mesa-switch"
MESA_COMMIT=1a8c1a66d6fd8d65f10107c4627ffc3606ba5631
MESA_IMAGE=superman-returns-nx-mesa:build
BASE_IMAGE=devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282
VOLUME=${BUILD_VOLUME:-superman-returns-nx-build}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}
[ "$JOBS" -gt 16 ] && JOBS=16

die() { echo "build-docker: $*" >&2; exit 1; }

docker info --format '{{.ServerVersion}}' >/dev/null || die "Docker daemon is not reachable"

DOCKER_NET=()
DOCKER_ENV=()
PROXY_ARGS=()
if [ -n "${BUILD_PROXY_CA:-}" ]; then
    [ -f "$BUILD_PROXY_CA" ] || die "BUILD_PROXY_CA=$BUILD_PROXY_CA does not exist"
    [ -n "${HTTPS_PROXY:-}" ] || die "BUILD_PROXY_CA needs HTTPS_PROXY"
    ctx=$(mktemp -d)
    cp "$BUILD_PROXY_CA" "$ctx/proxy-ca.crt"
    ca=/etc/ssl/certs/ca-certificates.crt
    {
        echo "FROM $BASE_IMAGE"
        echo "COPY proxy-ca.crt /usr/local/share/ca-certificates/build-proxy-ca.crt"
        echo "RUN update-ca-certificates && sed -i 's|http://deb.debian.org|https://deb.debian.org|g' /etc/apt/sources.list.d/*"
        echo "ENV SSL_CERT_FILE=$ca CURL_CA_BUNDLE=$ca GIT_SSL_CAINFO=$ca PIP_CERT=$ca CARGO_HTTP_CAINFO=$ca"
    } > "$ctx/Dockerfile"
    BASE_IMAGE=superman-returns-nx-base:proxy
    DOCKER_NET=(--network host)
    DOCKER_ENV=(-e "HTTPS_PROXY=$HTTPS_PROXY" -e "https_proxy=$HTTPS_PROXY")
    PROXY_ARGS=(--build-arg "HTTPS_PROXY=$HTTPS_PROXY" --build-arg "https_proxy=$HTTPS_PROXY")
    docker build "${DOCKER_NET[@]}" "${PROXY_ARGS[@]}" -t "$BASE_IMAGE" "$ctx"
    rm -rf "$ctx"
fi

prepare_mesa_source() {
    if [ ! -d "$MESA/.git" ]; then
        mkdir -p "$ROOT/.tools"
        git clone -c core.autocrlf=false --no-checkout --depth 1 \
            https://github.com/danfromtico/mesa-switch.git "$MESA"
        git -C "$MESA" fetch --depth 1 origin "$MESA_COMMIT"
        git -C "$MESA" checkout --detach FETCH_HEAD
    fi
    [ "$(git -C "$MESA" rev-parse HEAD)" = "$MESA_COMMIT" ] ||
        die "Mesa checkout must be $MESA_COMMIT; the existing checkout was preserved"
    # The NFSMW patch, then this port's fixes on top. Accept either stage
    # already applied; otherwise require a clean checkout.
    local nfsmw="$ROOT/mesa/mesa-switch-nfsmw.patch" superman="$ROOT/mesa/mesa-switch-superman.patch"
    if ! git -C "$MESA" apply --reverse --check "$superman" 2>/dev/null; then
        if ! git -C "$MESA" apply --reverse --check "$nfsmw" 2>/dev/null; then
            git -C "$MESA" diff --quiet || die "Mesa has other changes; existing files were preserved"
            git -C "$MESA" apply "$nfsmw"
        fi
        git -C "$MESA" apply --check "$superman" ||
            die "mesa-switch-superman.patch does not apply; existing files were preserved"
        git -C "$MESA" apply "$superman"
    fi
    tar -cf "$ROOT/.tools/mesa-build-source.tar" -C "$MESA" .
}

run_in() {
    local image=$1; shift
    docker run --rm "${DOCKER_NET[@]}" "${DOCKER_ENV[@]}" \
        --mount "type=bind,source=$ROOT,target=/project" \
        --mount "type=volume,source=$VOLUME,target=/work" \
        -e "JOBS=$JOBS" "$image" "$@"
}

build_mesa() {
    prepare_mesa_source
    docker build --progress plain "${DOCKER_NET[@]}" "${PROXY_ARGS[@]}" \
        --build-arg "BASE=$BASE_IMAGE" \
        -f "$ROOT/tools/switch/Dockerfile.mesa" -t "$MESA_IMAGE" "$ROOT/tools/switch"
    run_in "$MESA_IMAGE" bash /project/tools/switch/build-mesa.sh
}

require_mesa() {
    [ -f "$ROOT/.tools/mesa-sdk/opt/devkitpro/portlibs/switch/lib/libvulkan.a" ] ||
        die "run '$0 mesa' first (.tools/mesa-sdk is missing)"
}

case "${1:-}" in
    mesa) build_mesa ;;
    vk-probe)
        require_mesa
        run_in "$BASE_IMAGE" bash /project/tools/switch/build-vk-probe.sh ;;
    game)
        require_mesa
        [ -d "$ROOT/app/generated/default" ] || die "app/generated/default is missing; run tools/project.py codegen"
        tar -cf "$ROOT/.tools/project-build-source.tar" --exclude=app/out --exclude=sdk/out \
            -C "$ROOT" app sdk tools/switch/cmake
        run_in "$MESA_IMAGE" bash /project/tools/switch/build-game.sh ;;
    *) die "usage: $0 mesa|vk-probe|game" ;;
esac
