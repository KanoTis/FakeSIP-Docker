# FakeSIP-Docker

Docker container for [FakeSIP](https://github.com/MikeWang000000/FakeSIP).

Disguise your UDP traffic as SIP protocol to evade DPI detection, using Netfilter Queue (NFQUEUE).


## Quick Start

```sh
docker run --rm \
    --net=host \
    --cap-add CAP_NET_ADMIN \
    --cap-add CAP_NET_RAW \
    --cap-add CAP_SYS_NICE \
    fakesip-docker -i eth0
```

Build the image locally:

```sh
docker build -t fakesip-docker .
```

Or build for another architecture (the FakeSIP release binary matching the platform is fetched automatically):

```sh
docker buildx build --platform linux/arm64 -t fakesip-docker .
```

Pin a specific FakeSIP version:

```sh
docker build --build-arg VERSION=0.9.1 -t fakesip-docker .
```


## Usage

All arguments are passed straight to `fakesip`, e.g.:

```sh
# work on eth0, use a custom SIP URI
docker run --rm --net=host \
    --cap-add CAP_NET_ADMIN --cap-add CAP_NET_RAW --cap-add CAP_SYS_NICE \
    fakesip-docker -i eth0 -u sip:1000@example.com

# IPv4 only, daemonized, log to stderr
docker run -d --net=host \
    --cap-add CAP_NET_ADMIN --cap-add CAP_NET_RAW --cap-add CAP_SYS_NICE \
    fakesip-docker -4 -d -w /dev/stderr
```

Run `docker run --rm fakesip-docker` for the full option list.

Notes:

- `--net=host` is required for the firewall rules to apply to host traffic.
- The container ships `nft` and `iptables`; the host kernel must provide
  `nfnetlink_queue` / nftables `queue` support.
- If the host cannot run the rules from the container, use `-f` to skip rule
  setup, or `-z` to use iptables instead of nft.


## License

GNU General Public License v3.0
