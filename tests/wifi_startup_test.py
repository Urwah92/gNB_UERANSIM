#!/usr/bin/env python3
"""Check clean Wi-Fi startup failures with all host network commands mocked.

AP construction opens only a loopback UDP socket. The PATH contains exclusively
fake commands, so these cases cannot invoke the host's iptables, ip, or tc.
"""

import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

from wifi_runtime_test import AP_ADDRESS, commands, mock_tools, read_text, replace_scalar


UNASSIGNED_ADDRESS = "198.51.100.239"


def run_failure(binary, config, folder, environment):
    try:
        result = subprocess.run([str(binary), "-c", str(config), "-l"], cwd=folder,
            env=environment, capture_output=True, text=True, timeout=15)
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout.decode(errors="replace") if isinstance(error.stdout, bytes) else error.stdout or ""
        stderr = error.stderr.decode(errors="replace") if isinstance(error.stderr, bytes) else error.stderr or ""
        (folder / "output.log").write_text(stdout + stderr)
        raise AssertionError("Startup failure left the process running instead of exiting cleanly") from error
    output = result.stdout + result.stderr
    (folder / "output.log").write_text(output)
    if result.returncode != 1:
        raise AssertionError(f"Expected clean exit1, got {result.returncode}:\n{output}")
    if any(marker in output for marker in ("terminate called", "Segmentation fault", "core dumped", "Assertion")):
        raise AssertionError(f"Startup failure aborted instead of reporting an error:\n{output}")
    return output


def failure_mock(folder, command_log, status, diagnostic):
    environment = mock_tools(folder, command_log)
    executable = folder / "mock-bin" / "iptables"
    with executable.open("a") as script:
        script.write(f"\nprint({diagnostic!r}, file=sys.stderr)\nraise SystemExit({status})\n")
    return environment


def ap_failure(root, source, binary, name, status=None, diagnostic=""):
    folder = root / name
    folder.mkdir()
    command_log = folder / "commands.jsonl"
    if status is None:
        environment = mock_tools(folder, command_log)
        (folder / "mock-bin" / "iptables").unlink()
    else:
        environment = failure_mock(folder, command_log, status, diagnostic)
    config = (source / "config/examples/wifi-ap.yaml").read_text()
    for key in ("linkIp", "ngapIp", "gtpIp"):
        config = replace_scalar(config, key, AP_ADDRESS)
    config = replace_scalar(config, "UeInterface", "mockue")
    config = replace_scalar(config, "nextHopInterface", "mockwan")
    path = folder / "ap.yaml"
    path.write_text(config)
    output = run_failure(binary, path, folder, environment)
    recorded = commands(command_log)
    if "iptables" not in output.lower():
        raise AssertionError(f"Missing command name in startup diagnostic: {output}")
    if status is None:
        if recorded:
            raise AssertionError(f"Missing executable caused unexpected host commands: {recorded}")
        if not any(message in output.lower() for message in ("not found", "no such file", "enoent", "cannot find")):
            raise AssertionError(f"Missing executable lacks an actionable diagnostic: {output}")
    else:
        if len(recorded) != 1 or len(recorded[0]) != 4 or recorded[0][:3] != ["iptables", "-w", "-N"]:
            raise AssertionError(f"Failed -N mutated hooks or cleaned a preexisting chain: {recorded}")
        if not recorded[0][-1].startswith("UERANSIM_"):
            raise AssertionError(f"Unexpected chain name: {recorded}")
        if diagnostic not in output:
            raise AssertionError(f"Tool's useful diagnostic was hidden: {output}")
        if not re.search(rf"(?:exit(?:[ -](?:status|code))?|status)[^\n\d]*{status}\b", output, re.IGNORECASE):
            raise AssertionError(f"Child exit{status} was not distinguished from a spawn error: {output}")
    if "forwarding enabled" in output:
        raise AssertionError("Failed AP startup reported successful forwarding")


def unassigned_ue(root, source, binary):
    folder = root / "unassigned-ue-address"
    folder.mkdir()
    command_log = folder / "commands.jsonl"
    environment = mock_tools(folder, command_log)
    config = (source / "config/examples/ue-wifi.yaml").read_text()
    config = replace_scalar(config, "staticIP", UNASSIGNED_ADDRESS)
    config = replace_scalar(config, "Interface", "lo")
    path = folder / "ue.yaml"
    path.write_text(config)
    output = run_failure(binary, path, folder, environment)
    if UNASSIGNED_ADDRESS not in output or "staticIP" not in output or "Interface" not in output or "lo" not in output:
        raise AssertionError(f"Unavailable address diagnostic does not identify staticIP and Interface: {output}")
    if not any(message in output.lower() for message in ("not assigned", "unavailable", "cannot assign", "not available")):
        raise AssertionError(f"Unavailable address diagnostic does not explain the failure: {output}")
    if commands(command_log):
        raise AssertionError("Invalid UE bind address modified network configuration before startup failed")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: wifi_startup_test.py /path/to/nr-gnb /path/to/nr-ue")
    binary_gnb, binary_ue = (pathlib.Path(path).resolve() for path in sys.argv[1:])
    source = pathlib.Path(__file__).resolve().parents[1]
    root = pathlib.Path(tempfile.mkdtemp(prefix="ueransim-wifi-startup-"))
    try:
        ap_failure(root, source, binary_gnb, "missing-iptables")
        ap_failure(root, source, binary_gnb, "existing-chain", 1, "iptables: Chain already exists.")
        ap_failure(root, source, binary_gnb, "permission-denied", 4,
            "iptables: Permission denied (you must be root or have CAP_NET_ADMIN).")
        unassigned_ue(root, source, binary_ue)
    except BaseException:
        print(f"Wi-Fi startup test artifacts: {root}", file=sys.stderr)
        for output in sorted(root.glob("*/output.log")):
            print(f"--- {output} ---\n{read_text(output)}", file=sys.stderr)
        raise
    else:
        shutil.rmtree(root)
    print("Wi-Fi startup failure tests passed (missing tool, existing chain, permissions, unavailable UE IP; commands mocked)")


if __name__ == "__main__":
    main()
