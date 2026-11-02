# UERANSIM

UERANSIM builds cellular UE and gNB simulators, plus simulated Wi-Fi station and AP modes, from one source tree. Each process reads a YAML configuration.

## Programs and modes

| Program | `wifi` | Role | Active tasks |
| --- | --- | --- | --- |
| `nr-gnb` | `false` | Cellular gNB | App, RLS, RRC, NGAP, SCTP, GTP-U |
| `nr-ue` | `false` | Cellular UE | App, RLS, RRC, NAS |
| `nr-gnb` | `true` | Simulated Wi-Fi AP | App, RLS |
| `nr-ue` | `true` | Simulated Wi-Fi station | App, RLS |

Radio links are simulated over UDP. Wi-Fi modes use existing Linux interfaces and routing; they do not implement a physical 802.11 radio.

## Protocol components

| Component | Responsibility |
| --- | --- |
| RLS — Radio Link Simulation | Discovery, synthetic signal strength, and transport of radio signaling and user data over UDP. |
| RRC — Radio Resource Control | Cellular connection management between the UE and gNB. |
| NAS — Non-Access Stratum | UE registration, authentication, and PDU session procedures with the core. |
| NGAP / SCTP | gNB–AMF control signaling over N2, including NGAP Path Switch Requests. |
| GTP-U | gNB–UPF user traffic over N3. |
| TUN | Linux interface for a UE's cellular PDU session. |
| App / CLI | Local commands and application coordination, including Wi-Fi station routing. |


## YAML configuration

Use one file per process. Keys are case sensitive. Both programs use `wifi` to select the mode and `mcc`/`mnc` for the PLMN; quote values such as `'001'` and `'01'` to preserve leading zeros.

### gNB and AP fields

| Component | Main fields | Purpose |
| --- | --- | --- |
| Identity | `nci`, `idLength`, `tac` | Cell identity, gNB ID length, and tracking area. Use distinct cell and derived gNB IDs for concurrent nodes. |
| Interfaces | `linkIp`, `ngapIp`, `gtpIp`, `gtpAdvertiseIp` | Local RLS, N2, and N3 addresses; optional advertised GTP address. |
| Core and slices | `amfConfigs`, `slices`, `ignoreStreamIds` | Remote AMF endpoints, supported slices, and NGAP SCTP stream checks. |
| AP forwarding | `UeInterface`, `nextHopInterface` | Distinct Linux interfaces facing stations and the uplink. |

### UE fields

| Component | Main fields | Purpose |
| --- | --- | --- |
| Subscriber | `supi`, `key`, `op`, `opType`, `amf` | Subscriber identity and authentication values. `amf` is the Authentication Management Field. |
| Discovery | `gnbSearchList` | Reachable gNB/AP `linkIp` addresses for RLS discovery. |
| Sessions and slices | `sessions`, `default-nssai`, `configured-nssai`, `tunName` | PDU sessions, DNN (`sessions[].apn`), slices, and optional TUN name. |
| Security and access | `integrity`, `ciphering`, `integrityMaxRate`, `uacAic`, `uacAcc` | Supported algorithms, integrity limits, and access control settings. |
| Wi-Fi routing | `staticIP`, `Interface`, `wifiRoutingTable` | Existing local IPv4 address, its Linux interface, and the policy routing table. |

Use complete templates: the shared parsers still require some cellular fields in Wi-Fi mode. The [gNB/AP reference](docs/GNB_CONFIGURATION.md) and [UE reference](docs/UE_CONFIGURATION.md) list all fields, requirements, and examples.

Templates: [source gNB](config/container/source-gnb.yml), [target gNB](config/container/target-gnb.yml), [cellular UE](config/container/ue-cellular.yaml), [Wi-Fi AP](config/container/wifi-ap.yml), and [Wi-Fi station](config/container/ue-wifi.yaml). Adapt addresses, interfaces, and subscriber settings before use.

## Mobility model

A gNB/AP has one fixed `phyLocation`, such as `{x: 0, y: 0, z: 0}`. A UE uses equal-length coordinate arrays as waypoints in simulated metres:

```yaml
phyLocation: {x: [0, 200], y: [0, 0], z: [0, 0]}
velocity: 10
pattern: linear
```

This UE travels 200 metres in 20 seconds. `linear` stops at the final waypoint; `loop` returns to the first and repeats. One waypoint or `velocity: 0` keeps it stationary. The legacy `mobility` field aliases `velocity`.

RLS samples the position about once per second. Each gNB/AP derives synthetic signal strength from its distance to the UE. List all candidate nodes in `gnbSearchList`; simulated movement keeps the UE's Linux IP address unchanged.

### Wi-Fi AP selection

The station selects the strongest fresh AP above `-120 dBm` and updates its source-specific route. With APs at `x=0` and `x=200`, the preferred AP can change near `x=100`. AP forwarding follows simulated coverage; the station removes its route when no AP is eligible. Configure interface addresses, IPv4 forwarding, and upstream routing in Linux.

### Inter-gNB handover

Cellular mobility uses **UE-relayed context transfer and NGAP path switching**:

1. Source signal below `-120 dBm` triggers session context export.
2. The UE relays the context to the strongest eligible target gNB.
3. The target sends an NGAP Path Switch Request to the AMF.
4. After confirmation, the UE switches cell and the source releases the session.

A stronger neighbor alone does not trigger this procedure. It uses custom RLS messages rather than direct XnAP. See the [integration record](docs/INTEGRATION.md) for compatibility, tests, and limits.

## Build and configuration references

Requires a C++17 compiler, CMake 3.17+, `make`, and SCTP development headers. Debian/Ubuntu packages: `build-essential`, `cmake`, and `libsctp-dev`. Build locally from source; generated build directories contain absolute paths.

```bash
make
./build/nr-gnb -c config/container/source-gnb.yml --check-config
./build/nr-ue -c config/container/ue-cellular.yaml --check-config
```

`--check-config` validates YAML without starting the node. Run `make test` for the local test suite. Container deployment and startup steps belong in the scenario guide.

Original project documentation: [upstream README](docs/UPSTREAM_README.md). Copyright notices and the [license](LICENSE) are preserved.
