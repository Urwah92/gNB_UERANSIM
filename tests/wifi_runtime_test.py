#!/usr/bin/env python3
"""Exercise real Wi-Fi AP/UE tasks over loopback with all network commands mocked.

Only UDP loopback sockets are real. PATH contains only the test's fake ip,
iptables, and tc programs, which record argv and never configure the host.
Failure artifacts remain in /tmp for inspection.
"""

import json
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time


AP_ADDRESS = "127.0.0.4"
UE_ADDRESS = "127.0.0.6"
ROUTING_TABLE = "10001"


def replace_scalar(text, key, value):
    pattern = rf"^{re.escape(key)}:.*$"
    changed, count = re.subn(pattern, f"{key}: {value}", text, count=1, flags=re.MULTILINE)
    if count != 1:
        raise AssertionError(f"Expected one scalar setting {key}")
    return changed


def read_text(path):
    return path.read_text(errors="replace") if path.exists() else ""


def commands(path):
    text = read_text(path)
    return [json.loads(line) for line in text.splitlines() if line.strip()]


def wait_for(condition, processes, description, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for name, process in processes:
            if process.poll() is not None:
                raise AssertionError(f"{name} exited early with status {process.returncode}: {description}")
        if condition():
            return
        time.sleep(0.1)
    raise AssertionError(f"Timed out waiting for {description}")


def stop(process, name):
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
            raise AssertionError(f"{name} did not stop gracefully after SIGTERM")
    if process.returncode != 0:
        raise AssertionError(f"{name} stopped with status {process.returncode}")


def mock_tools(folder, log):
    bindir = folder / "mock-bin"
    bindir.mkdir()
    script = f"""#!{sys.executable}
import json
import os
import pathlib
import sys
record = json.dumps([pathlib.Path(sys.argv[0]).name] + sys.argv[1:]) + '\\n'
fd = os.open(os.environ['MOCK_COMMAND_LOG'], os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
try:
    os.write(fd, record.encode())
finally:
    os.close(fd)
"""
    for name in ("ip", "iptables", "tc"):
        executable = bindir / name
        executable.write_text(script)
        executable.chmod(0o700)
    environment = os.environ.copy()
    environment["PATH"] = str(bindir)
    environment["MOCK_COMMAND_LOG"] = str(log)
    return environment


def verify_owned_cleanup(recorded):
    iptables = [argv[1:] for argv in recorded if argv[0] == "iptables"]
    ip = [argv[1:] for argv in recorded if argv[0] == "ip"]
    if any(argv[0] not in ("ip", "iptables") for argv in recorded):
        raise AssertionError(f"Unexpected external command: {recorded}")
    created = [argv[argv.index("-N") + 1] for argv in iptables if "-N" in argv]
    if len(created) != 1 or not created[0].startswith("UERANSIM_"):
        raise AssertionError(f"Missing exclusively owned firewall chain: {iptables}")
    chain = created[0]
    for argv in iptables:
        for operation in ("-F", "-X"):
            if operation in argv and argv[argv.index(operation) + 1:] != [chain]:
                raise AssertionError(f"Unscoped firewall cleanup: {argv}")
        for operation in ("-I", "-A", "-D", "-C"):
            if operation in argv:
                target = argv[argv.index(operation) + 1]
                if target not in (chain, "FORWARD"):
                    raise AssertionError(f"Unexpected firewall chain: {argv}")
                if target == "FORWARD" and ("-j" not in argv or argv[argv.index("-j") + 1] != chain):
                    raise AssertionError(f"Unscoped forwarding rule: {argv}")
        if "-s" in argv or "-d" in argv:
            option = "-s" if "-s" in argv else "-d"
            if argv[argv.index(option) + 1] != UE_ADDRESS + "/32":
                raise AssertionError(f"Rule affects another client: {argv}")
    for argv in ip:
        if len(argv) < 2 or argv[0] not in ("route", "rule") or "table" not in argv:
            raise AssertionError(f"Unscoped route operation: {argv}")
        if argv[argv.index("table") + 1] != ROUTING_TABLE:
            raise AssertionError(f"Unexpected routing table: {argv}")
        if argv[0] == "rule" and ("from" not in argv or argv[argv.index("from") + 1] != UE_ADDRESS + "/32"):
            raise AssertionError(f"Unscoped source policy rule: {argv}")
        if argv[0] == "route" and ("proto" not in argv or argv[argv.index("proto") + 1] != "186"):
            raise AssertionError(f"Route is not owned by the simulator: {argv}")
    required = [
        ["-w", "-D", "FORWARD", "-i", "mockue", "-o", "mockwan", "-j", chain],
        ["-w", "-D", "FORWARD", "-i", "mockwan", "-o", "mockue", "-j", chain],
        ["-w", "-F", chain],
        ["-w", "-X", chain],
    ]
    if any(argv not in iptables for argv in required):
        raise AssertionError(f"Owned firewall rules were not cleaned up: {iptables}")
    if not any(argv[:2] == ["rule", "del"] for argv in ip):
        raise AssertionError(f"Source policy rule not removed: {ip}")
    if not any(argv[:2] == ["route", "del"] for argv in ip):
        raise AssertionError(f"Owned route not removed: {ip}")


def run_case(root, binary_gnb, binary_ue, source, moving):
    folder = root / ("moving" if moving else "stationary")
    folder.mkdir()
    command_log = folder / "commands.jsonl"
    environment = mock_tools(folder, command_log)
    ap = (source / "config/examples/wifi-ap.yaml").read_text()
    for key in ("linkIp", "ngapIp", "gtpIp"):
        ap = replace_scalar(ap, key, AP_ADDRESS)
    ap = replace_scalar(ap, "UeInterface", "mockue")
    ap = replace_scalar(ap, "nextHopInterface", "mockwan")
    ap_config = folder / "ap.yaml"
    ap_config.write_text(ap)
    ue = (source / "config/examples/ue-wifi.yaml").read_text()
    ue = ue.replace("10.200.0.1", AP_ADDRESS)
    ue = replace_scalar(ue, "staticIP", UE_ADDRESS)
    ue = replace_scalar(ue, "Interface", "lo")
    ue = replace_scalar(ue, "wifiRoutingTable", ROUTING_TABLE)
    if moving:
        ue = ue.replace("x: [10]", "x: [10, 250]")
        ue = ue.replace("y: [0]", "y: [0, 0]").replace("z: [0]", "z: [0, 0]")
        ue = replace_scalar(ue, "velocity", "35")
    ue_config = folder / "ue.yaml"
    ue_config.write_text(ue)
    processes = []
    try:
        with (folder / "ap.log").open("w") as ap_output, (folder / "ue.log").open("w") as ue_output:
            ap_process = subprocess.Popen([str(binary_gnb), "-c", str(ap_config), "-l"],
                cwd=folder, env=environment, stdout=ap_output, stderr=subprocess.STDOUT)
            processes.append(("AP", ap_process))
            wait_for(lambda: any("-N" in argv for argv in commands(command_log)), processes, "AP firewall initialization")
            ue_process = subprocess.Popen([str(binary_ue), "-c", str(ue_config), "-l"],
                cwd=folder, env=environment, stdout=ue_output, stderr=subprocess.STDOUT)
            processes.append(("UE", ue_process))
            wait_for(lambda: "forwarding enabled" in read_text(folder / "ap.log") and
                f"gateway[{AP_ADDRESS}] selected" in read_text(folder / "ue.log"), processes,
                "AP heartbeat exchange, forwarding, and UE route selection")
            if moving:
                wait_for(lambda: "forwarding removed" in read_text(folder / "ap.log") and any(
                    argv[:3] == ["ip", "route", "del"] for argv in commands(command_log)),
                    processes, "coverage loss removes AP forwarding and UE route")
            stop(ue_process, "UE")
            stop(ap_process, "AP")
        verify_owned_cleanup(commands(command_log))
        for name in ("ap", "ue"):
            output = read_text(folder / f"{name}.log")
            if any(marker in output for marker in ("Segmentation fault", "terminate called", "Unhandled NTS", "Assertion")):
                raise AssertionError(f"Unexpected runtime failure in {name}: {output}")
    finally:
        for name, process in reversed(processes):
            if process.poll() is None:
                try:
                    stop(process, name)
                except AssertionError:
                    pass


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: wifi_runtime_test.py /path/to/nr-gnb /path/to/nr-ue")
    binary_gnb, binary_ue = (pathlib.Path(path).resolve() for path in sys.argv[1:])
    source = pathlib.Path(__file__).resolve().parents[1]
    root = pathlib.Path(tempfile.mkdtemp(prefix="ueransim-wifi-runtime-"))
    try:
        run_case(root, binary_gnb, binary_ue, source, moving=False)
        run_case(root, binary_gnb, binary_ue, source, moving=True)
    except BaseException:
        print(f"Wi-Fi runtime test artifacts: {root}", file=sys.stderr)
        for output in sorted(root.glob("*/*.log")):
            print(f"--- {output} ---\n{read_text(output)}", file=sys.stderr)
        raise
    else:
        shutil.rmtree(root)
    print("Wi-Fi runtime tests passed (stationary shutdown and moving out of coverage; network commands mocked)")


if __name__ == "__main__":
    main()
