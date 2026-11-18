# FakeSIP-Docker

Docker container for [FakeSIP](https://github.com/MikeWang000000/FakeSIP).

Disguise your UDP traffic as SIP protocol to evade DPI detection, using Netfilter Queue (NFQUEUE).

The image is built `FROM scratch`. It ships the `fakesip` binary, a statically
linked `nft`, and `iptables`/`ip6tables` (nft + legacy variants, musl, with the
xtables extensions FakeSIP needs). Total size: ~2.6 MB.


## Quick Start

```sh
docker run --rm \
    --net=host \
    --cap-add NET_ADMIN \
    --cap-add NET_RAW \
    ghcr.io/kanotis/fakesip-docker -i eth0
```


## Kernel modules

FakeSIP needs `xt_connbytes`, `xt_NFQUEUE` and `nfnetlink_queue` in the host
kernel. Many systems (Synology DSM, OpenWrt, ...) ship them as modules that are
not autoloaded. Mount the host module tree and grant `CAP_SYS_MODULE` to have
the container load them itself:

```sh
docker run --rm     --net=host     --cap-add NET_ADMIN     --cap-add NET_RAW     --cap-add SYS_MODULE     -v /lib/modules:/lib/modules:ro     ghcr.io/kanotis/fakesip-docker -i eth0 -z
```

If `xt_connbytes` is missing from the kernel altogether, the `-m connbytes`
rule is rewritten automatically, in this order:

1. per-flow `hashlimit` emulation — first N packets of each flow
   (`-m hashlimit --hashlimit-upto 10/hour --hashlimit-burst N
   --hashlimit-mode srcip,dstip,dstport`)
2. `-m conntrack --ctstate NEW` — first packet of each flow only

Both cases are logged, e.g.:

```
fakesip-docker: kernel has no connbytes match, emulating it with a per-flow hashlimit (first N packets of each flow)
fakesip-docker: kernel has neither connbytes nor hashlimit, degrading to "-m conntrack --ctstate NEW" (first packet of each flow only)
```

Check what the kernel actually provides with:

```sh
tr ' ' '
' < /proc/net/ip_tables_matches | grep -E 'connbytes|hashlimit|NFQUEUE'
```

Set `FAKESIP_NO_MODPROBE=1` to disable module loading, or
`FAKESIP_NO_CONNBYTES=1` to force the degraded rule.


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
- `-z` (use `iptables` instead of `nft`) works too: `iptables`/`ip6tables` are
  a small shim that tries the nft-backed xtables binary and transparently falls
  back to `iptables-legacy`/`ip6tables-legacy` when `nf_tables` is missing or
  broken (the same legacy backend FakeHTTP-Docker uses). The choice is cached
  per address family in `/run/xtables.iptables` and `/run/xtables.ip6tables`,
  so one family never mixes backends. `iptables-legacy`, `ip6tables-legacy`,
  `iptables-nft` and `ip6tables-nft` are available as explicit aliases.
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
