# UERANSIM

This repository builds a simulated 5G UE and gNB, plus simulated Wi-Fi station and AP modes. The same source tree builds both programs. Each process reads a YAML file that selects its behavior; you can place processes on hosts or in containers to suit your network topology. There is no required number of containers.

| Role | Program | YAML setting | Main behavior |
| --- | --- | --- | --- |
| Cellular gNB | `nr-gnb` | `wifi: false` | Simulated radio link, RRC, NGAP/SCTP and GTP-U |
| Cellular UE | `nr-ue` | `wifi: false` | Simulated radio link, RRC, NAS and PDU/TUN interfaces |
| Simulated Wi-Fi AP | `nr-gnb` | `wifi: true` | AP discovery and Linux packet forwarding |
| Simulated Wi-Fi station | `nr-ue` | `wifi: true` | AP selection and source-specific Linux routing |

The Wi-Fi modes use existing Linux network interfaces; they do not create a physical 802.11 AP or perform 5G NAS registration. The inter-gNB handover in this project is a **simulation with UE-relayed context transfer and NGAP path switching**. It does not use direct XnAP between gNBs. See [integration details](docs/INTEGRATION.md).

## 1. Copy the source and a configuration file

Share a **source-only** copy of this directory. `build/`, `.deps/` and any `build.stale*/` directories are generated locally and should be left out. A CMake cache contains absolute paths, so a copied `build/` may fail on another machine. These directories are ignored by Git, but tools such as `docker cp` still copy them if they are present in the folder.

Put `UERANSIM/` and the YAML file for the process wherever they are convenient. For example, in a container:

```text
/
├── UERANSIM/      # source code
└── node.yaml      # configuration for the process running here
```

`node.yaml` is only an example filename. It can also be `ue.yaml`, `gnb.yml`, or any other path; pass that exact path to `-c`. YAML keys are case sensitive.

If using Docker, the same copy pattern works for any container. Replace `my-node` and `node.yaml` with your actual container and configuration file:

```sh
# Run on the Docker host, from the directory containing UERANSIM/ and node.yaml.
docker exec my-node mkdir -p /UERANSIM
docker cp UERANSIM/. my-node:/UERANSIM/
docker cp node.yaml my-node:/node.yaml
```

Copy and build the source wherever a process will run. Processes may share a host or network namespace when their local addresses, ports, AP firewall chains and routing tables do not conflict. A Git archive of this repository is another way to distribute tracked source without generated build directories.

## 2. Install dependencies and build

On Debian or Ubuntu, install these packages in the build environment. Use administrator privileges for package installation; the example assumes a root shell:

```sh
apt-get update
apt-get install -y build-essential cmake libsctp-dev lksctp-tools iproute2 iptables procps python3 iputils-ping
cd /UERANSIM
make
```

If you put the source elsewhere, change the `cd` path. The project requires CMake **3.17 or newer**, a C++17 compiler and SCTP development headers. `make` creates `build/` and produces `build/nr-ue`, `build/nr-gnb`, `build/nr-cli`, `build/nr-binder` and `build/libdevbnd.so`. Both UE and gNB binaries are built, regardless of which role you plan to run. No extra YAML library package is needed; bundled dependencies are in `src/ext/`.

For a low-memory environment, use `make JOBS=1`. To run the default tests, use `make test`. They do not need a 5G core. If an old `build/` was copied by mistake, `make` preserves its stale CMake cache under `build.stale*` and configures a new build. Leave generated directories out of the next copy.

## 3. Configure a role

Use one YAML file per running process. The [UE parameter reference](docs/UE_CONFIGURATION.md) and [gNB/AP parameter reference](docs/GNB_CONFIGURATION.md) explain every supported field, whether it is required, and sample values. Complete **illustrative** files are available below. Edit all IP addresses, interface names, subscriber values and core settings for your environment before running them:

| Role | Example YAML |
| --- | --- |
| Cellular gNB | [source gNB](config/container/source-gnb.yml) or [target gNB](config/container/target-gnb.yml) |
| Cellular UE | [cellular UE](config/container/ue-cellular.yaml) |
| Simulated Wi-Fi AP | [Wi-Fi AP](config/container/wifi-ap.yml) |
| Simulated Wi-Fi station | [Wi-Fi UE](config/container/ue-wifi.yaml) |

For cellular handover, configure the required gNBs with distinct IDs and reachable radio addresses, and put their `linkIp` addresses in the UE's `gnbSearchList`. The gNBs must reach an AMF and UPF, and the UE's subscriber credentials, PLMN, DNN and slice must agree with the 5G core. The number and placement of gNBs and UEs depend on your deployment.

For Wi-Fi mode, the AP and station need IP connectivity on their selected interfaces. The AP needs two distinct Linux interfaces, IPv4 forwarding, an uplink route and an upstream return route or NAT. The station's `staticIP` must already be assigned to its `Interface` and that interface must be up. The application does not assign these addresses or configure the AP's upstream routing.

### Key YAML fields

| Field | Used by | Meaning |
| --- | --- | --- |
| `wifi` | Both programs | `false` runs cellular tasks; `true` runs simulated Wi-Fi tasks. |
| `mcc`, `mnc` | Both | Mobile country/network codes. Quote values such as `'001'` and `'01'` to preserve leading zeros. |
| `nci`, `idLength`, `tac` | gNB/AP | NR cell identity, gNB ID length and tracking area. The derived gNB ID must be distinct for concurrent AP firewall chains. |
| `linkIp` | gNB/AP | **Local** IP bound for simulated radio UDP traffic on port `4997`. |
| `ngapIp`, `gtpIp` | gNB/AP | **Local** N2/N3 IPs for cellular mode. YAML still requires them in AP mode, although its NGAP and GTP tasks do not start. |
| `amfConfigs` | gNB/AP | **Remote** AMF IP and port for cellular mode; `[]` is valid in AP mode. |
| `slices` | gNB/AP | Supported cellular SST and optional SD; `[]` is valid in AP mode. |
| `UeInterface`, `nextHopInterface` | Wi-Fi AP | Different Linux interfaces facing stations and the routed uplink. Capital letters matter. |
| `gnbSearchList` | UE | Reachable gNB/AP **`linkIp`** addresses, not AMF addresses. |
| `supi`, `key`, `op`, `opType`, `amf` | UE | Subscriber identity and authentication values. The UE's `amf` is a **hex authentication field**, not an AMF server IP. The shared parser requires these fields even in Wi-Fi mode. |
| `sessions` | Cellular UE | Initial PDU sessions, including type, DNN (`apn`) and optional slice. IPv4 is the supported user-plane choice. |
| `phyLocation` | Both | Fixed gNB/AP position, or UE arrays of waypoints. Use `velocity` and `pattern` for UE movement. |
| `staticIP`, `Interface` | Wi-Fi UE | Existing local IPv4 address and Linux interface carrying it. `staticIP` has no CIDR suffix in YAML. |
| `wifiRoutingTable` | Wi-Fi UE | Linux policy routing table; default `10001`. Use distinct tables for Wi-Fi UE processes sharing a network namespace. |

The old AP keys `sessionIP` and `nextHop` are **not read** by this code. Put the UE address in the UE's `staticIP`; configure an AP upstream gateway through Linux routing on `nextHopInterface`.

## 4. Validate and run

From the source directory, first validate the YAML for the program you will run. Replace `/node.yaml` with that process's actual configuration path:

```sh
cd /UERANSIM
./build/nr-gnb -c /node.yaml --check-config  # gNB or Wi-Fi AP
./build/nr-ue  -c /node.yaml --check-config  # cellular or Wi-Fi UE
```

Run **only the command for the chosen role** with its matching YAML file:

```sh
./build/nr-gnb -c /node.yaml  # gNB or Wi-Fi AP
./build/nr-ue  -c /node.yaml  # cellular or Wi-Fi UE
```

`--check-config` confirms that YAML parses; it cannot verify live addresses, core connectivity or subscriber provisioning. Check local addresses with `ip -br -4 addr` and routes with `ip route`. In Wi-Fi mode, the station adds a policy rule for traffic sourced from `staticIP`; applications need to use that source address to take the selected AP route. Wi-Fi mode supports one UE per `nr-ue` process.

### Runtime requirements

| Role | Required host/container access |
| --- | --- |
| Cellular gNB | Reachable AMF/UPF and SCTP support in the Linux kernel. |
| Cellular UE | `/dev/net/tun` and `CAP_NET_ADMIN` for the PDU TUN and routes. |
| Wi-Fi AP | `CAP_NET_ADMIN`, `iptables`, IPv4 forwarding and appropriate upstream routing/return path. |
| Wi-Fi UE | `CAP_NET_ADMIN` for `ip route` and `ip rule`; `staticIP` assigned to `Interface`. |

Root inside a container may still lack `CAP_NET_ADMIN`. When creating a Docker container, the relevant options are `--cap-add=NET_ADMIN` and, for a cellular UE, `--device=/dev/net/tun`. Configure equivalent access in your container manager when needed.

## Troubleshooting

| Problem | Check |
| --- | --- |
| CMake mentions a different source path | Do not copy `build/`. Run `make` in the new source directory to regenerate its cache. |
| SCTP headers or library not found | Install `libsctp-dev` in the build environment, then run `make`. |
| `Cannot assign requested address` | Check `ip -br -4 addr`. A Wi-Fi UE's `staticIP` must already be assigned to its exact `Interface`; gNB `linkIp` and cellular `ngapIp`/`gtpIp` must also be local where used. |
| AP needs `UeInterface` and `nextHopInterface` | Supply two different valid Linux interface names in AP YAML. |
| AP firewall setup fails | Check `command -v iptables`, container `CAP_NET_ADMIN` and `iptables -w -S UERANSIM_<gnb-id>` using the ID derived from `nci` and `idLength`. Inspect an existing chain before removing it. |
| UE cannot discover a gNB/AP | Check the peer's `linkIp`, the UE's `gnbSearchList`, network reachability and UDP port `4997`. |
| Cellular registration or session fails | Check AMF/UPF reachability, subscriber credentials, PLMN, DNN and slices in the core and node YAML files. |
| Wi-Fi station selects an AP but has no upstream traffic | Check AP forwarding, Linux uplink route, and the upstream return route or NAT for the station subnet. |

Original project documentation is archived at the [upstream README](docs/UPSTREAM_README.md). Existing copyright notices and the [license](LICENSE) are preserved.
