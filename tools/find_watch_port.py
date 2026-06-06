#!/usr/bin/env python3
from __future__ import annotations

import glob
import os
import subprocess
import sys
from pathlib import Path


def tty_candidates() -> list[str]:
    ports = sorted(glob.glob("/dev/ttyACM*")) + sorted(glob.glob("/dev/ttyUSB*"))
    return [p for p in ports if Path(p).exists()]


def run_esptool(port: str) -> str:
    python_bin = os.environ.get("ESP_PYTHON", sys.executable)
    cmd = [
        python_bin,
        "-m",
        "esptool",
        "--chip",
        "auto",
        "-p",
        port,
        "chip_id",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=12)
    return (proc.stdout or "") + (proc.stderr or "")


def pick_watch_port() -> tuple[str | None, list[tuple[str, str]]]:
    reports: list[tuple[str, str]] = []
    for port in tty_candidates():
        output = run_esptool(port)
        reports.append((port, output))
        upper = output.upper()
        if "ESP32-S3" in upper and "ESP32-P4" not in upper:
            return port, reports
    return None, reports


def main() -> int:
    port, reports = pick_watch_port()
    if port:
        print(port)
        return 0

    sys.stderr.write("No ESP32-S3 watch port found.\n")
    for candidate, report in reports:
        first_line = next((line for line in report.splitlines() if line.strip()), "no response")
        sys.stderr.write(f"{candidate}: {first_line}\n")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
