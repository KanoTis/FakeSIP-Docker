# syntax=docker/dockerfile:1

ARG ALPINE=alpine:3.24

###############################################################################
# 1. Toolchain (only needed to build a static nft binary)
###############################################################################
FROM ${ALPINE} AS toolchain

RUN apk add --no-cache \
        bash \
        bzip2 \
        curl \
        file \
        gcc \
        linux-headers \
        make \
        musl-dev \
        pkgconf \
        xz

###############################################################################
# 2. Static nft binary (musl + mini-gmp, no readline, no json)
###############################################################################
FROM toolchain AS nft

ARG LIBMNL_VERSION=1.0.5
ARG LIBNFTNL_VERSION=1.3.1
ARG NFTABLES_VERSION=1.1.6

ENV CFLAGS="-Os -fno-pie -ffunction-sections -fdata-sections" \
    LDFLAGS="-static -no-pie -Wl,--gc-sections" \
    CONFIG_SHELL=/bin/bash

WORKDIR /src

RUN set -eux; \
    curl -fsSL --retry 10 --retry-all-errors --retry-delay 3 -O \
        "https://www.netfilter.org/projects/libmnl/files/libmnl-${LIBMNL_VERSION}.tar.bz2"; \
    curl -fsSL --retry 10 --retry-all-errors --retry-delay 3 -O \
        "https://www.netfilter.org/projects/libnftnl/files/libnftnl-${LIBNFTNL_VERSION}.tar.xz"; \
    curl -fsSL --retry 10 --retry-all-errors --retry-delay 3 -O \
        "https://www.netfilter.org/projects/nftables/files/nftables-${NFTABLES_VERSION}.tar.xz"; \
    tar xf "libmnl-${LIBMNL_VERSION}.tar.bz2"; \
    tar xf "libnftnl-${LIBNFTNL_VERSION}.tar.xz"; \
    tar xf "nftables-${NFTABLES_VERSION}.tar.xz"

RUN set -eux; \
    cd "libmnl-${LIBMNL_VERSION}"; \
    ./configure --prefix=/usr --enable-static --disable-shared; \
    make -j"$(nproc)"; \
    make install

RUN set -eux; \
    cd "libnftnl-${LIBNFTNL_VERSION}"; \
    ./configure --prefix=/usr --enable-static --disable-shared --disable-json; \
    make -j"$(nproc)"; \
    make install

RUN set -eux; \
    cd "nftables-${NFTABLES_VERSION}"; \
    ./configure \
        --prefix=/usr \
        --enable-static \
        --disable-shared \
        --disable-man-doc \
        --disable-debug \
        --without-cli \
        --with-mini-gmp; \
    make LDFLAGS="-all-static -no-pie -Wl,--gc-sections" -j"$(nproc)"; \
    make install; \
    strip /usr/sbin/nft; \
    ls -l /usr/sbin/nft; \
    readelf -h /usr/sbin/nft | grep 'Machine:'; \
    ldd /usr/sbin/nft || true

###############################################################################
# 3. fakesip release binary
###############################################################################
FROM ${ALPINE} AS fakesip

ARG VERSION=latest
ARG TARGETARCH
ARG ARCH

RUN set -eux; \
    case "${ARCH:-$TARGETARCH}" in \
        amd64)    asset=x86_64 ;; \
        arm64)    asset=arm64 ;; \
        arm)      asset=arm32v7hf ;; \
        386)      asset=i686 ;; \
        mips64le) asset=mips64el ;; \
        *)        asset="${ARCH:-$TARGETARCH}" ;; \
    esac; \
    if [ "$VERSION" = "latest" ]; then \
        url="https://github.com/MikeWang000000/FakeSIP/releases/latest/download"; \
    else \
        url="https://github.com/MikeWang000000/FakeSIP/releases/download/$VERSION"; \
    fi; \
    apk add --no-cache curl tar; \
    curl -fsSL --retry 10 --retry-all-errors --retry-delay 3 \
        -o /tmp/fakesip.tar.gz "$url/fakesip-linux-$asset.tar.gz"; \
    tar -xzf /tmp/fakesip.tar.gz -C /tmp; \
    cp "/tmp/fakesip-linux-$asset/fakesip" /fakesip; \
    chmod 755 /fakesip

###############################################################################
# 4. Runtime: scratch + static nft + static fakesip
###############################################################################
FROM scratch

COPY --from=nft /usr/sbin/nft /usr/sbin/nft
COPY --from=fakesip /fakesip /usr/sbin/fakesip

ENV HOME=/root \
    PATH=/usr/sbin:/usr/bin:/sbin:/bin

ENTRYPOINT ["/usr/sbin/fakesip"]
