FROM --platform=$BUILDPLATFORM alpine:3.24 AS fetch

ARG VERSION=latest
ARG TARGETARCH
ARG ARCH

RUN set -eux; \
    case "${ARCH:-$TARGETARCH}" in \
        amd64)   asset=x86_64 ;; \
        arm64)   asset=arm64 ;; \
        arm)     asset=arm32v7hf ;; \
        386)     asset=i686 ;; \
        mips64le) asset=mips64el ;; \
        *)       asset="${ARCH:-$TARGETARCH}" ;; \
    esac; \
    if [ "$VERSION" = "latest" ]; then \
        url="https://github.com/MikeWang000000/FakeSIP/releases/latest/download"; \
    else \
        url="https://github.com/MikeWang000000/FakeSIP/releases/download/$VERSION"; \
    fi; \
    apk add --no-cache curl tar; \
    curl -fsSL -o /tmp/fakesip.tar.gz "$url/fakesip-linux-$asset.tar.gz"; \
    tar -xzf /tmp/fakesip.tar.gz -C /tmp; \
    cp "/tmp/fakesip-linux-$asset/fakesip" /tmp/fakesip; \
    chmod 755 /tmp/fakesip

FROM alpine:3.24

RUN apk add --no-cache iptables nftables

COPY --from=fetch /tmp/fakesip /usr/sbin/fakesip

ENTRYPOINT ["/usr/sbin/fakesip"]
