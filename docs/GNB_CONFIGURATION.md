# gNB and simulated Wi-Fi AP YAML reference

Use `nr-gnb` for both roles. `wifi: false` runs a cellular gNB; `wifi: true` runs the simulated Wi-Fi AP/router. YAML names are case sensitive. The filename may end in `.yaml` or `.yml`; pass its path with `-c`.

Start with one of these complete, editable examples:

| Role | Example | Run in that node |
| --- | --- | --- |
| Source cellular gNB | [`config/container/source-gnb.yml`](../config/container/source-gnb.yml) | `./build/nr-gnb -c /gnb.yml` |
| Target cellular gNB | [`config/container/target-gnb.yml`](../config/container/target-gnb.yml) | `./build/nr-gnb -c /gnb.yml` |
| Simulated Wi-Fi AP | [`config/container/wifi-ap.yml`](../config/container/wifi-ap.yml) | `./build/nr-gnb -c /gnb.yml` |

Copy the appropriate example to `/gnb.yml` **in its container** and edit the IPs and interface names for that container. The examples assume source gNB `10.1.1.10`/`10.100.1.4`, target gNB `10.1.1.14`/`10.100.1.5`, and AMF `10.100.1.1`. The AP example reuses `10.1.1.10` in a *separate* Wi-Fi lab. It cannot share that address/interface with the cellular source gNB at the same time.

## Identity and network fields

| YAML field | Required? | Meaning and example |
| --- | --- | --- |
| `mcc` | Yes | Three digit mobile country code, quoted to keep leading zeros: `mcc: '001'`. Use the PLMN served by your core in cellular mode. |
| `mnc` | Yes | Two or three digit mobile network code, quoted: `mnc: '01'`. `'01'` and `'001'` are different MNC lengths. |
| `nci` | Yes | 36 bit NR Cell Identity, from `0` through `0xFFFFFFFFF`: `nci: '0x000000010'`. Each cellular gNB needs a distinct value. |
| `idLength` | Yes | Number of high NCI bits used as the gNB ID, `22` through `32`: `idLength: 32`. With this length, NCI `0x10` gives gNB ID `1`, `0x20` gives `2`, and `0x30` gives `3`. |
| `tac` | Yes | Tracking Area Code, `0` through `0xFFFFFF`: `tac: 1`. Set it to a TAC accepted by the 5G core. |
| `linkIp` | Yes | **Local** address on which this process receives simulated radio UDP traffic (port `4997`): `linkIp: 10.1.1.10`. The UE lists this IP in `gnbSearchList`. In AP mode this is the address reached by Wi-Fi stations. |
| `ngapIp` | Yes | **Local** source/bind address for NGAP over SCTP toward the AMF: `ngapIp: 10.100.1.4`. The AP parser still requires it, but AP mode starts no NGAP task. |
| `gtpIp` | Yes | **Local** address for GTP-U traffic toward the UPF: `gtpIp: 10.100.1.4`. The AP parser still requires it, but AP mode starts no GTP task. |
| `gtpAdvertiseIp` | No | IP advertised for GTP-U when it differs from the local `gtpIp`, for example behind address translation: `gtpAdvertiseIp: 10.100.1.40`. Omit it to advertise `gtpIp`. Used in cellular mode. |

The derived gNB ID is `nci >> (36 - idLength)`. For example, `nci: '0x000000001'` with `idLength: 32` produces ID `0`, so an AP creates firewall chain `UERANSIM_0`. Changing only the last four NCI bits will **not** give a different gNB ID at this length. Give concurrently running APs distinct IDs and ensure no existing chain uses the same name.

`linkIp`, `ngapIp`, and `gtpIp` are addresses of the **gNB container**, not the AMF. `--check-config` checks YAML syntax and values; it does not prove those addresses are assigned or reachable. Check them with `ip -br -4 addr` inside the container before starting.

## Core, slice and simulation fields

| YAML field | Required? | Meaning and example |
| --- | --- | --- |
| `amfConfigs` | Yes | List of **remote** AMF endpoints. Cellular example: `amfConfigs: [{address: 10.100.1.1, port: 38412}]`. In AP mode use `amfConfigs: []`; the AP does not connect to an AMF. |
| `amfConfigs[].address` | For each AMF | IP address of that AMF: `address: 10.100.1.1`. |
| `amfConfigs[].port` | For each AMF | AMF NGAP/SCTP port, `1024` through `65535`: `port: 38412`. |
| `slices` | Yes | S-NSSAI list advertised by a cellular gNB: `slices: [{sst: 1}]`. Use `slices: []` for a simulated AP. Align this with the core and cellular UE. |
| `slices[].sst` | For each slice | Slice/service type, `0` through `255`: `sst: 1`. |
| `slices[].sd` | No | Optional slice differentiator, `0` through `0xFFFFFF`: `sd: 0x010203`. Include the same SD on core, gNB and UE if your slice uses one. |
| `ignoreStreamIds` | Yes | `false` normally checks NGAP SCTP stream IDs; `true` ignores stream ID errors for core compatibility: `ignoreStreamIds: false`. It has no effect in AP mode. |
| `phyLocation` | No | Fixed simulated position. Example: `phyLocation: {x: 10, y: 2, z: 0}`. If omitted, all coordinates are `0`. The UE uses these coordinates to rank radio/AP signal strength. |
| `phyLocation.x`, `.y`, `.z` | If `phyLocation` is set | Integer coordinates in simulated metres. Unlike the UE's waypoint arrays, each gNB coordinate is one number. |
| `wifi` | No | `false` by default for a cellular gNB. `true` starts the AP/router path and skips SCTP, NGAP, RRC and GTP tasks. |
| `UeInterface` | In AP mode | Name of the AP interface facing Wi-Fi UEs: `UeInterface: eth1`. The AP example expects `10.1.1.10/24` here. Capital `U` and `I` matter. |
| `nextHopInterface` | In AP mode | Name of a **different** AP interface facing the routed uplink: `nextHopInterface: eth2`. The example expects `10.100.1.4/24` here. |

AP interface names must be distinct, valid Linux names of at most 15 characters. The AP installs an `iptables` forwarding chain between these two interfaces. It does not assign their IP addresses, enable kernel IPv4 forwarding, or create upstream routes/NAT. Configure those in your lab/container network.

The old `sessionIP` and `nextHop` keys are **not read by this gNB parser**. A Wi-Fi UE's address belongs in its own `staticIP`, and the AP's Linux uplink/interface configuration determines its next hop. See the [UE YAML reference](UE_CONFIGURATION.md) and the [README setup guide](../README.md).
