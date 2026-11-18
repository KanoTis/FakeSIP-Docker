# FakeSIP-Docker

Docker container for [FakeSIP](https://github.com/MikeWang000000/FakeSIP).

Disguise your UDP traffic as SIP protocol to evade DPI detection, using Netfilter Queue (NFQUEUE).

The image is built `FROM scratch` and only ships two static binaries
(`fakesip` + a statically linked `nft`), so it weighs in at ~1 MB.


## Quick Start

```sh
docker run --rm \
    --net=host \
    --cap-add NET_ADMIN \
    --cap-add NET_RAW \
    ghcr.io/kanotis/fakesip-docker -i eth0
```


## Usage

All arguments are passed straight to `fakesip`, e.g.:

```sh
# work on eth0, use a custom SIP URI
docker run --rm --net=host \
    --cap-add NET_ADMIN --cap-add NET_RAW \
    ghcr.io/kanotis/fakesip-docker -i eth0 -u sip:1000@example.com

# IPv4 only, daemonized, log to stderr
docker run -d --net=host \
    --cap-add NET_ADMIN --cap-add NET_RAW \
    ghcr.io/kanotis/fakesip-docker -4 -d -w /dev/stderr
```

Run `docker run --rm ghcr.io/kanotis/fakesip-docker` for the full option list.

Notes:

- `--net=host` is required for the firewall rules to apply to host traffic.
- The container ships a static `nft`; the host kernel must provide
  `nf_tables` and `nfnetlink_queue` support.
- `iptables` is **not** bundled (that is what keeps the image tiny). The `-z`
  flag and the automatic iptables fallback therefore do not work inside this
  container; use `-f` to skip rule setup and manage the rules yourself if your
  host only has iptables.
- `CAP_SYS_NICE` is optional, it only silences the `setpriority()` warning.


## Build

```sh
docker build -t fakesip-docker .
```

Pin a specific FakeSIP version:

```sh
docker build --build-arg VERSION=0.9.1 -t fakesip-docker .
```

Multi-arch (the FakeSIP release binary is picked to match the target platform):

```sh
docker buildx build --platform linux/amd64,linux/arm64,linux/arm/v7,linux/386 -t fakesip-docker .
```

The static `nft` is compiled inside the build; toolchain/library versions can be
overridden with `--build-arg LIBMNL_VERSION=`, `--build-arg LIBNFTNL_VERSION=`
and `--build-arg NFTABLES_VERSION=`.


## License

GNU General Public License v3.0
