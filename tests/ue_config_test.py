#!/usr/bin/env python3
"""Exercise the real UE parser without opening sockets or changing network state."""

import pathlib
import subprocess
import sys
import tempfile


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).resolve().parents[1]
    base = (source / "config/free5gc-ue.yaml").read_text()
    cases = [
        ("stock stationary defaults", "", True),
        ("stationary multi-point path", "phyLocation: {x: [0, 20], y: [0, 0], z: [0, 0]}\nvelocity: 0\n", True),
        ("legacy speed alias", "mobility: 2.5\npattern: loop\n", True),
        ("new speed precedes alias", "velocity: 2.5\nmobility: -1\n", True),
        ("negative speed", "velocity: -1\n", False),
        ("nonfinite speed", "velocity: .nan\n", False),
        ("infinite speed", "velocity: .inf\n", False),
        ("invalid path pattern", "pattern: arbitrary\n", False),
        ("empty coordinates", "phyLocation: {x: [], y: [], z: []}\n", False),
        ("unequal coordinate arrays", "phyLocation: {x: [0, 1], y: [0], z: [0]}\n", False),
        ("scalar coordinates", "phyLocation: {x: 0, y: 0, z: 0}\n", False),
        ("invalid coordinate value", "phyLocation: {x: [abc], y: [0], z: [0]}\n", False),
        ("unsafe shaping interface", "trafficShapingInterface: 'eth0;false'\n", False),
        ("option-shaped interface", "trafficShapingInterface: '-eth0'\n", False),
        ("dot shaping interface", "trafficShapingInterface: '.'\n", False),
        ("dot-dot shaping interface", "trafficShapingInterface: '..'\n", False),
        ("invalid Wi-Fi IP", "wifi: true\nstaticIP: 999.1.2.3\nInterface: eth0\n", False),
        ("invalid Wi-Fi interface", "wifi: true\nstaticIP: 10.200.0.2\nInterface: 'eth0;false'\n", False),
        ("dot Wi-Fi interface", "wifi: true\nstaticIP: 10.200.0.2\nInterface: '.'\n", False),
        ("dot-dot Wi-Fi interface", "wifi: true\nstaticIP: 10.200.0.2\nInterface: '..'\n", False),
        ("invalid Wi-Fi table", "wifiRoutingTable: 254\n", False),
        ("valid Wi-Fi config without bound IP", "wifi: true\nstaticIP: 10.200.0.2\nInterface: eth0\nwifiRoutingTable: 10001\n", True),
    ]
    with tempfile.TemporaryDirectory(prefix="ueransim-config-test-") as temporary:
        folder = pathlib.Path(temporary)
        for index, (name, suffix, valid) in enumerate(cases):
            config = folder / f"{index}.yaml"
            config.write_text(base + "\n" + suffix)
            result = subprocess.run(
                [str(binary), "-c", str(config), "--check-config"],
                cwd=folder, capture_output=True, text=True, timeout=5,
            )
            expected = 0 if valid else 1
            if result.returncode != expected or (valid and "Configuration valid" not in result.stdout):
                raise AssertionError(f"{name}: exit={result.returncode}, stdout={result.stdout}, stderr={result.stderr}")
        # The last case is a valid Wi-Fi config. Multiple copies would share a
        # Linux source address/table and could tear down one another's routes.
        result = subprocess.run(
            [str(binary), "-c", str(config), "--check-config", "-n", "2"],
            cwd=folder, capture_output=True, text=True, timeout=5,
        )
        if result.returncode != 1 or "one UE per process" not in result.stderr:
            raise AssertionError(f"Wi-Fi -n2 unexpectedly accepted: {result.stdout}\n{result.stderr}")
        if (folder / "logs").exists():
            raise AssertionError("Configuration validation created a logs directory")
        for example in sys.argv[2:]:
            result = subprocess.run(
                [str(binary), "-c", str(pathlib.Path(example).resolve()), "--check-config"],
                cwd=folder, capture_output=True, text=True, timeout=5,
            )
            if result.returncode != 0 or "Configuration valid" not in result.stdout:
                raise AssertionError(f"Example {example}: {result.stdout}\n{result.stderr}")
    print(f"UE configuration tests passed ({len(cases) + 1} cases)")


if __name__ == "__main__":
    main()
