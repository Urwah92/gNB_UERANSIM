# UE and simulated Wi-Fi station YAML reference

Use `nr-ue` for both roles. `wifi: false` runs a cellular UE; `wifi: true` runs a simulated Wi-Fi station without NAS registration or a cellular PDU/TUN interface. YAML names are case sensitive. The filename may end in `.yaml` or `.yml`; pass its path with `-c`.

Start with one of these complete, editable examples:

| Role | Example | Run in that node |
| --- | --- | --- |
| Cellular UE | [`config/container/ue-cellular.yaml`](../config/container/ue-cellular.yaml) | `./build/nr-ue -c /ue.yaml` |
| Simulated Wi-Fi station | [`config/container/ue-wifi.yaml`](../config/container/ue-wifi.yaml) | `./build/nr-ue -c /ue.yaml` |

Copy the chosen example to `/ue.yaml` **in the UE container**. The cellular example looks for source `10.1.1.10` and target `10.1.1.14`. The Wi-Fi example looks for AP `10.1.1.10` and uses `10.1.1.2` already assigned to its `eth1`. These are lab examples: use your own reachable addresses and, for cellular mode, subscriber settings provisioned in your core. Do not share the sample subscriber secret values outside a test lab.

## Subscriber and radio discovery

| YAML field | Required? | Meaning and example |
| --- | --- | --- |
| `supi` | For a working cellular subscriber | Subscriber identity, usually `imsi-` plus the IMSI: `supi: 'imsi-001010000000001'`. Provision exactly this IMSI in the core. Parser permits omission, but cellular authentication/registration needs a usable identity. In Wi-Fi mode the shared parser accepts it but NAS does not start. |
| `mcc` | Yes | Three digit home mobile country code, quoted to preserve leading zeros: `mcc: '001'`. |
| `mnc` | Yes | Two or three digit home mobile network code, quoted: `mnc: '01'`. The length is significant. Match the gNB and core for cellular service. |
| `key` | Yes | 128 bit permanent subscriber key: exactly 32 hexadecimal characters, for example `key: '8baf473f2f8fd09487cccbd7097c6862'`. Must match the core subscriber record for cellular mode. |
| `op` | Yes | 128 bit OP or OPc: exactly 32 hexadecimal characters, for example `op: '8e27b6af0e692e750f32667a3b14605d'`. Set `opType` to say which one you supplied. |
| `opType` | Yes | `OP` or `OPC`, matching the `op` value: `opType: 'OPC'`. |
| `amf` | Yes | Four hexadecimal characters for the subscriber **Authentication Management Field**, for example `amf: '8000'`. This is *not* the AMF server address. |
| `gnbSearchList` | Yes, nonempty | Peer **gNB/AP `linkIp`** addresses for simulated radio discovery over UDP port `4997`. Example: `gnbSearchList: [10.1.1.10, 10.1.1.14]`. Do not put the AMF IP here. Wi-Fi station example has only its AP address. |
| `routingIndicator` | No | One to four character SUCI routing indicator, commonly `routingIndicator: '0000'`. Omission uses `0000`. Relevant to cellular mode. |
| `protectionScheme` | No | SUCI scheme: `0` (null, default) or `1` (Profile A). Example: `protectionScheme: 0`. Although the parser accepts `0` through `255`, other schemes, including Profile B (`2`), are not implemented at runtime. |
| `homeNetworkPublicKeyId` | No | Key ID used with SUCI Profile A, `0` through `255`: `homeNetworkPublicKeyId: 1`. |
| `homeNetworkPublicKey` | No | 32 byte home network public key for Profile A, exactly 64 hexadecimal characters. Example: `homeNetworkPublicKey: '5a8d38864820197c3394b92613b20b91633cbd897119273bf8e4a6f4eec0a650'`. |
| `imei` | No | Device identity with exactly 15 characters, for example `imei: '356938035643803'`. |
| `imeiSv` | No | Device identity and software version with exactly 16 characters, for example `imeiSv: '4370816125816151'`. |

The parser still requires `mcc`, `mnc`, `key`, `op`, `opType`, `amf`, `integrity`, `ciphering`, `integrityMaxRate`, `uacAic`, `uacAcc` and `gnbSearchList` when `wifi: true`. Their cellular protocol tasks are skipped in that mode. In the Wi-Fi example, the key/OPc/AMF values satisfy parser requirements; no 5G subscription is used.

## Cellular service fields

| YAML field | Required? | Meaning and example |
| --- | --- | --- |
| `sessions` | No | Initial PDU sessions to request. Cellular example: `sessions: [{type: IPv4, apn: internet, slice: {sst: 1}}]`. Omit or use `[]` for no automatic session. Not used in Wi-Fi mode. |
| `sessions[].type` | For each session | `IPv4`, `IPv6`, `IPv4v6`, `Ethernet`, or `Unstructured` parses. Use `type: 'IPv4'` for this project's supported user plane; the other types are not implemented end to end. |
| `sessions[].apn` | No | Data network name (DNN) requested from the core: `apn: 'internet'`. Optional in YAML, but set it to the DNN your core offers. |
| `sessions[].slice.sst` | If `slice` is set | Slice/service type from `0` through `255`: `sst: 1`. |
| `sessions[].slice.sd` | No | Slice differentiator from `0` through `0xFFFFFF`: `sd: 0x010203`. Include only when the matching core/gNB slice uses an SD. |
| `default-nssai` | No | List of default configured UE slices, e.g. `default-nssai: [{sst: 1}]`. Each item needs `sst` (`0` through `255`) and may have `sd` (`0` through `0xFFFFFF`). |
| `configured-nssai` | No | List of configured UE slices, e.g. `configured-nssai: [{sst: 1, sd: 0x010203}]`. Each item follows the same `sst`/optional `sd` rules. |
| `tunName` | No | Linux cellular PDU TUN interface name, 1–12 characters: `tunName: uesimtun0`. Omit for generated names. No TUN is created in Wi-Fi mode. |
| `integrity` | Yes | Supported cellular NAS integrity algorithms. All three booleans are required: `integrity: {IA1: true, IA2: true, IA3: true}`. |
| `ciphering` | Yes | Supported cellular NAS ciphering algorithms. All three booleans are required: `ciphering: {EA1: true, EA2: true, EA3: true}`. |
| `integrityMaxRate` | Yes | User plane integrity maximum data rates. Both `uplink` and `downlink` are required and each must be `full` or `64kbps`: `integrityMaxRate: {uplink: full, downlink: full}`. |
| `uacAic` | Yes | Unified Access Control access identities. Both booleans required: `uacAic: {mps: false, mcs: false}`. |
| `uacAcc` | Yes | Unified Access Control access classes. `normalClass` is an integer `0`–`9`; `class11` through `class15` are required booleans. Example: `uacAcc: {normalClass: 0, class11: false, class12: false, class13: false, class14: false, class15: false}`. |

For `sessions`, `default-nssai` and `configured-nssai`, a slice `sd` is optional. The examples in `config/container/` use SST `1` without SD; if your core requires an SD, add the same one throughout the UE, gNB and core configuration.

## Position, movement and Wi-Fi routing

| YAML field | Required? | Meaning and example |
| --- | --- | --- |
| `phyLocation` | No | UE waypoints as equal length, nonempty integer arrays: `phyLocation: {x: [10, 100, 180], y: [2, 2, 2], z: [0, 0, 0]}`. Omission means a stationary origin `(0, 0, 0)`. Unlike a gNB, a UE uses arrays. |
| `pattern` | No | `linear` (default) moves to the last waypoint and stops; `loop` returns to the first and repeats: `pattern: linear`. |
| `velocity` | No | Finite speed in simulated metres per second, `0` or greater: `velocity: 10`. `0` keeps the UE at its first waypoint. |
| `mobility` | No | Older name for `velocity`: `mobility: 10`. When both are present, `velocity` wins. Prefer `velocity` in new files. |
| `trafficShapingInterface` | No | Linux interface on which to apply optional `tc netem` radio delay/loss, e.g. `trafficShapingInterface: eth1`. Omit for no traffic shaping. Use an isolated lab interface, because this changes that interface's traffic. |
| `wifi` | No | `false` by default for cellular UE. `true` starts simulated Wi-Fi station discovery and policy routing instead of NAS/RRC and cellular TUN tasks. |
| `staticIP` | In Wi-Fi mode | **Existing local IPv4 address** on the UE's `Interface`, with no `/prefix`: `staticIP: 10.1.1.2`. It is the source address for the Wi-Fi UDP socket and route rule. It is not an assigned 5G PDU address. |
| `Interface` | In Wi-Fi mode | UE Linux interface that already has `staticIP`: `Interface: eth1`. Uppercase `I` matters. It must be up and have a valid interface name of at most 15 characters. |
| `wifiRoutingTable` | No | Linux policy routing table for this Wi-Fi UE, `10000` through `60000`, default `10001`: `wifiRoutingTable: 10001`. Use a distinct table for each Wi-Fi UE sharing a network namespace. |

Wi-Fi mode accepts only one UE per process; do not use `nr-ue -n 2` with `wifi: true`. It discovers APs from `gnbSearchList`, chooses one based on simulated signal, and installs a route/rule for traffic **sourced from `staticIP`**. Applications must bind to that source address (for example `ping -I 10.1.1.2 ...`) to use the selected gateway. You must assign the IP to the interface before starting; `--check-config` only validates the YAML, while normal startup checks the live interface and bind address.

The UE configuration does not set a Linux default route for every process, and the AP does not automatically configure upstream routes or NAT. See the [README setup guide](../README.md).
