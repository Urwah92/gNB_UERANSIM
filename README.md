# Unified UERANSIM: UE, gNB and simulated Wi-Fi AP

One source tree builds matching UE and gNB implementations, with mobility,
UE-relayed inter-gNB handover using NGAP path switching, and a separate simulated
Wi-Fi gateway/station mode. Both original repositories remain independent.

Session context travels source gNB → UE → target gNB; the target sends an NGAP
Path Switch Request to the AMF. This is a custom handover simulation. Direct XnAP,
native 802.11 association, and simultaneous cellular/Wi-Fi UE attachment are outside its scope.

## Build and test

On Ubuntu/Debian:

```sh
sudo apt-get install build-essential cmake libsctp-dev lksctp-tools iproute2 iptables
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Outputs: `build/nr-gnb`, `build/nr-ue`, `build/nr-cli`, `build/libdevbnd.so`.
`make` also builds these targets and copies `nr-binder` into `build/`.
Python 3 enables parser regression tests. The default tests need no sockets or 5G core.
For a custom SCTP prefix, set `SCTP_INCLUDE_DIR` and `SCTP_LIBRARY` with CMake.

Optional runtime tests start real AP/UE processes over loopback UDP. Every
`ip`, `iptables`, and `tc` invocation is mocked; they do not configure host networking:

```sh
cmake -S . -B build -DUERANSIM_NETWORK_TESTS=ON
ctest --test-dir build --output-on-failure
```

These optional tests require permission to create local sockets.

## Validate configurations

`--check-config` opens no sockets, creates no logs and changes no networking:

```sh
./build/nr-gnb -c config/examples/source-gnb.yaml --check-config
./build/nr-gnb -c config/examples/target-gnb.yaml --check-config
./build/nr-gnb -c config/examples/wifi-ap.yaml --check-config
./build/nr-ue -c config/examples/ue-5g.yaml --check-config
./build/nr-ue -c config/examples/ue-wifi.yaml --check-config
```

## Cellular handover example

Configure your existing core for PLMN `208/93`, SST `1`, SD `010203`, DNN `internet`,
and the subscriber credentials in `ue-5g.yaml`. The example AMF is at
`127.0.0.5:38412`; adjust this and the local bind addresses for your lab. Your
AMF/SMF must support path switching and update the UPF tunnel.

Run each command in a separate terminal from this directory:

```sh
./build/nr-gnb -c config/examples/source-gnb.yaml
./build/nr-gnb -c config/examples/target-gnb.yaml
sudo ./build/nr-ue -c config/examples/ue-5g.yaml
```

The gNBs bind `127.0.0.2` and `127.0.0.3`, with distinct node IDs. The UE moves
from coordinate 0 to 180 at 10 metres/second. The inherited signal model approximates
dBm as negative distance in metres, with coverage ending at 120 m; these cells
therefore overlap. This is a mobility abstraction, not an RF propagation model.

Look for `Path Switch Request sent`, `Path switch complete`, and
`Released source handover session` in logs. Verify bidirectional traffic on the
UE TUN before and after switching: signaling logs alone do not prove continuity.

## Separate Wi-Fi gateway/station example

Use separate lab nodes or namespaces, with interfaces configured before starting:

| Role | Configuration |
| --- | --- |
| AP | `eth0 = 10.200.0.1/24`, separate routed uplink `eth1`, IPv4 forwarding enabled |
| UE | `eth0 = 10.200.0.2/24`, gateway `10.200.0.1` reachable on that interface |

Provide upstream return routes for `10.200.0.0/24`, or configure NAT in your lab.
The program does not automatically enable forwarding or configure NAT.

```sh
# In the AP node/namespace:
sudo ./build/nr-gnb -c config/examples/wifi-ap.yaml

# In the Wi-Fi UE node/namespace:
sudo ./build/nr-ue -c config/examples/ue-wifi.yaml
ping -I 10.200.0.2 <reachable-upstream-address>
```

Wi-Fi UE mode uses the configured Linux interface and source-specific routing.
It creates no cellular TUN and performs no NAS registration. Bind traffic to
`staticIP` so the policy rule selects the Wi-Fi gateway.

The AP manages its own `UERANSIM_<gnb-id>` firewall chain and two interface-specific
FORWARD hooks. The UE manages its source rule and route in `wifiRoutingTable`
(default 10001, route protocol 186). Give each AP a distinct gNB ID and each Wi-Fi
UE a dedicated routing table. Wi-Fi `-n` greater than one is rejected; run separate
configured instances instead. Ctrl-C/SIGTERM cleans up owned rules. SIGKILL cannot
run cleanup; see [recovery instructions](docs/INTEGRATION.md#forced-termination-recovery).

## Mobility settings

- UE `phyLocation` has equal, nonempty `x`, `y`, `z` integer arrays; gNB coordinates
  are scalars. Missing coordinates default to the origin.
- `velocity` is a finite nonnegative speed in metres/second; zero is stationary.
  The legacy `mobility` key remains accepted, with `velocity` taking precedence.
- `pattern: linear` stops at the final waypoint; `pattern: loop` returns to the start.
- Optional `trafficShapingInterface` enables Linux netem delay/loss simulation.
  It is disabled by default; use a dedicated lab interface.

The original `custom-*`, `open5gs-*`, and `free5gc-*` configurations remain available.
Align their credentials, core addresses, slices and DNNs with your deployment.

See [integration details](docs/INTEGRATION.md) for source commits, fixes, protocol
compatibility and remaining limitations. Live-core handover and actual kernel
forwarding still need testing in your lab.

Original documentation: [upstream README](docs/UPSTREAM_README.md).
Existing copyright notices and [LICENSE](LICENSE) are preserved.
