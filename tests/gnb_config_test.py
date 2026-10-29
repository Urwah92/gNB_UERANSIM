#!/usr/bin/env python3
"""Validate AP configuration and reject unsafe interfaces without opening sockets."""

import pathlib
import re
import subprocess
import sys
import tempfile


def set_interface(text, key, value):
    pattern = rf"^{re.escape(key)}:.*$"
    # Python repr produces a valid single/double-quoted YAML string for these names.
    replacement = "" if value is None else f"{key}: {value!r}"
    changed, count = re.subn(pattern, replacement, text, count=1, flags=re.MULTILINE)
    if count != 1:
        raise AssertionError(f"Missing expected example setting {key}")
    return changed


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    base = pathlib.Path(sys.argv[2]).read_text()
    cases = [("valid AP example", base, True)]
    for key in ("UeInterface", "nextHopInterface"):
        for value in ("", ".", "..", "-eth0", "eth0;false", "a b", "a/b", "1234567890123456", None):
            cases.append((f"{key}={value!r}", set_interface(base, key, value), False))
    cases.append(("same interface for both directions", set_interface(base, "nextHopInterface", "eth0"), False))
    with tempfile.TemporaryDirectory(prefix="ueransim-gnb-config-test-") as temporary:
        folder = pathlib.Path(temporary)
        for index, (name, contents, valid) in enumerate(cases):
            config = folder / f"{index}.yaml"
            config.write_text(contents)
            result = subprocess.run([str(binary), "--check-config", "-c", str(config)],
                cwd=folder, capture_output=True, text=True, timeout=5)
            expected = 0 if valid else 1
            if result.returncode != expected or (valid and "Configuration valid" not in result.stdout):
                raise AssertionError(f"{name}: exit={result.returncode}, stdout={result.stdout}, stderr={result.stderr}")
        if (folder / "logs").exists():
            raise AssertionError("AP configuration validation created a logs directory")
    print(f"gNB AP configuration tests passed ({len(cases)} cases)")


if __name__ == "__main__":
    main()
