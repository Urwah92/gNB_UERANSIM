# Integration record

## Sources

| Source | Commit / branch | Use |
| --- | --- | --- |
| `Urwah92/gNB_UERANSIM` | `63d846e`, master | Combined UE/gNB/AP baseline and gNB history |
| `Haw984/UE_UERANSIM` | `3a8c333`, master | Original UE mobility/Wi-Fi comparison and compatibility |
| `Haw984/UE_UERANSIM` | `ac7fe7c`, ue_5gHandover_working_update5 | Later UE handover and QoS allocation correction |

Both histories share upstream commit `392b714`. The gNB baseline already contained
almost all UE update5 files: only `src/ue/rrc/cells.cpp` differed under `src/ue`,
primarily in latency values. Copying UE master over that code would remove required
relay handlers. This integration retains the gNB baseline and reconciles UE
improvements in shared code. The merge commit's second parent is UE update5,
which includes UE master. Source remotes are retained; original working trees are unchanged.

## Roles and handover

One build supplies `nr-gnb` and `nr-ue`. Their `wifi` setting selects separate
cellular or simulated Wi-Fi roles. AP mode omits NGAP/SCTP/GTP, and Wi-Fi UE mode
omits NAS/RRC. CLI status/info works in either mode; cellular commands are rejected
when those tasks are absent.

1. Source gNB detects weak simulated signal and exports independent session snapshots.
2. UE relays each snapshot to a fresh, reachable target, tracking source/target per PSI.
3. Target sends an ASN.1-encoded NGAP Path Switch Request with its downlink GTP endpoint.
4. Target correlates ACKs by AMF, UE identifiers and PSI, applies uplink tunnel updates,
   and creates the target session.
5. UE receives a custom completion, requests release of the corresponding source PSI,
   and switches serving cell.
6. Source releases only confirmed sessions and removes local UE context after its last PSI.

Pending/failing requests leave source resources intact. Weak-source heartbeats
retain the mapping, with retry after 10 seconds; timeout and failure are handled
per session. Genuine heartbeat loss follows the ordinary radio-loss path.

## Shared protocol compatibility

Custom RLS type 8 now includes payload version 1, a 64-bit AMF UE ID, and an explicit
optional security-capability length. Rebuild every UE/gNB peer from this repository;
old unversioned type 8 frames are rejected. Standard heartbeat/data/ACK layouts and
custom completion/release layouts are retained. The codec validates lengths,
counts, PSI, QFI and tunnel addresses before consuming bytes.

## Improvements

- Correct ASN allocation/destruction for every QoS flow, bounded decoding and virtual base destruction.
- Matching optional capability encoding/decoding and full 40-bit AMF identifier preservation.
- Per-UE/session pending requests, real response correlation, ACK tunnel updates, failure/timeout handling.
- Independent source snapshots, confirmed source cleanup and sender-bound release.
- Both UE/PSI and downlink TEID indexes maintained, including subsequent handover exports.
- Per-PSI UE relay tracking, stale-target rejection and idempotent repeated target switching.
- Per-UE synchronized Wi-Fi/security state and protected UDP endpoint maps.
- Bounded-memory mobility interpolation, stationary defaults and validated legacy/current config keys.
- Configurable opt-in traffic shaping instead of hardcoded eth1.
- Owned AP firewall chains and source-policy routing instead of global firewall/default-route changes.
- Direct external-command argument vectors instead of shell expansion.
- Role-specific task construction and ordered shutdown; signal handlers only request termination.
- Configuration-only CLI validation before sockets, logs or network changes.

## Verification and limits

Local verification on 2026-10-29: Ubuntu 22.04, GCC 11.4, CMake 3.22.1, Debug build.
All binaries compiled and all 10 default CTest cases passed, including copied/moved
build-cache regression checks. The optional Wi-Fi runtime case also passed with
real loopback sockets and mocked network commands. The UE parser covers 23 scenarios;
the AP parser covers 20. Codec tests also passed under AddressSanitizer and
UndefinedBehaviorSanitizer; LeakSanitizer was unavailable under sandbox ptrace.
SCTP development headers were extracted into ignored `.deps/sctp` for this build,
using the existing system `libsctp.so.1`; no system packages were installed.

Default tests use no sockets. They cover RLS round trips, truncation/malformed
frames, optional capabilities, multiple QoS flows, mobility and command argument
fidelity. Synthetic gNB tests exercise actual ASN encoding and queued GTP updates,
out-of-order ACKs, uplink tunnel changes, failure/timeout isolation, repeated export
and source cleanup. Synthetic UE tests exercise response correlation and switching.
Parser tests invoke the real executables.

Optional Wi-Fi tests launch real AP/UE tasks over loopback with all networking
commands mocked. They verify connection, movement out of coverage, scoped rule
operations and SIGTERM cleanup. They do not verify kernel forwarding or Internet
connectivity. Test live bidirectional traffic with your own core and lab interfaces.

Remaining scope:

- Custom UE-relayed handover, not direct XnAP; no native 802.11 or seamless Wi-Fi/5G mobility.
- The transfer preserves QFIs, session AMBR and GTP endpoints, not a full standardized
  Xn UE/security/QoS context or PDCP forwarding state.
- Each PSI currently sends its own Path Switch Request. Correlation is tested
  synthetically; interoperability depends on your core.
- Intended setup is the same serving AMF; inter-AMF selection and real-radio target
  key derivation are outside this implementation.
- Wi-Fi requires preconfigured interfaces, forwarding and upstream routing, with
  dedicated routing tables per UE. Automated tests never change host networking.

## Forced termination recovery

Ctrl-C/SIGTERM performs cleanup. After SIGKILL, inspect the stopped instance's
configuration and remove only its resources. For the Wi-Fi UE example:

```sh
sudo ip rule del priority 10001 from 10.200.0.2/32 table 10001
sudo ip route del table 10001 default via 10.200.0.1 dev eth0 proto 186
```

For the AP example, NCI 0x30 with ID length 32 gives gNB ID 3:

```sh
sudo iptables -w -D FORWARD -i eth0 -o eth1 -j UERANSIM_3
sudo iptables -w -D FORWARD -i eth1 -o eth0 -j UERANSIM_3
sudo iptables -w -F UERANSIM_3
sudo iptables -w -X UERANSIM_3
```

Do not flush the global firewall or delete the host's default route.
