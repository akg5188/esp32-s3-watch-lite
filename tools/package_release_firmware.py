#!/usr/bin/env python3
"""Package ESP-IDF build outputs for GitHub Releases."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


PROJECT_NAME = "watch_ai"
TARGET_NAME = "esp32s3"


def load_flasher_args(build_dir: Path) -> dict:
    flasher_args_path = build_dir / "flasher_args.json"
    if not flasher_args_path.exists():
        raise FileNotFoundError(f"missing {flasher_args_path}")
    return json.loads(flasher_args_path.read_text(encoding="utf-8"))


def copy_flash_files(build_dir: Path, out_dir: Path, flasher_args: dict) -> list[tuple[str, Path]]:
    copied: list[tuple[str, Path]] = []
    for offset, rel_path in sorted(
        flasher_args["flash_files"].items(),
        key=lambda item: int(item[0], 16),
    ):
        src = build_dir / rel_path
        dst = out_dir / Path(rel_path).name
        if not src.exists():
            raise FileNotFoundError(f"missing flash file {src}")
        shutil.copy2(src, dst)
        copied.append((offset, dst))
    return copied


def merge_binary(build_dir: Path, out_dir: Path, flasher_args: dict) -> Path:
    settings = flasher_args.get("flash_settings", {})
    extra_args = flasher_args.get("extra_esptool_args", {})
    chip = extra_args.get("chip", TARGET_NAME)
    merged_path = out_dir / f"{PROJECT_NAME}-{TARGET_NAME}-merged.bin"
    esptool = shutil.which("esptool.py") or shutil.which("esptool")
    esptool_cmd = [esptool] if esptool else [sys.executable, "-m", "esptool"]

    cmd = [
        *esptool_cmd,
        "--chip",
        chip,
        "merge_bin",
        "-o",
        str(merged_path),
        "--flash_mode",
        settings.get("flash_mode", "dio"),
        "--flash_freq",
        settings.get("flash_freq", "80m"),
        "--flash_size",
        settings.get("flash_size", "32MB"),
    ]

    for offset, rel_path in sorted(
        flasher_args["flash_files"].items(),
        key=lambda item: int(item[0], 16),
    ):
        cmd.extend([offset, str(build_dir / rel_path)])

    subprocess.run(cmd, check=True)
    return merged_path


def write_flashing_notes(out_dir: Path, flasher_args: dict, copied_files: list[tuple[str, Path]], merged_path: Path) -> Path:
    settings = flasher_args.get("flash_settings", {})
    notes_path = out_dir / "FLASHING.txt"
    flash_mode = settings.get("flash_mode", "dio")
    flash_freq = settings.get("flash_freq", "80m")
    flash_size = settings.get("flash_size", "32MB")

    separate_args = " \\\n    ".join(f"{offset} {path.name}" for offset, path in copied_files)
    notes = f"""Watch Lite firmware release package

Target hardware:
  Waveshare ESP32-S3 Touch AMOLED 2.06

Merged firmware flash command:
  python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \\
    --before default_reset --after hard_reset write_flash \\
    --flash_mode {flash_mode} --flash_freq {flash_freq} --flash_size {flash_size} \\
    0x0 {merged_path.name}

Separate firmware flash command:
  python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \\
    --before default_reset --after hard_reset write_flash \\
    --flash_mode {flash_mode} --flash_freq {flash_freq} --flash_size {flash_size} \\
    {separate_args}

If your serial port is not /dev/ttyACM0, replace it with the device shown by:
  ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
"""
    notes_path.write_text(notes, encoding="utf-8")
    return notes_path


def make_zip(out_dir: Path) -> Path:
    zip_path = out_dir / f"{PROJECT_NAME}-{TARGET_NAME}-firmware.zip"
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(out_dir.iterdir()):
            if path == zip_path or not path.is_file():
                continue
            archive.write(path, arcname=path.name)
    return zip_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path, help="ESP-IDF build directory")
    parser.add_argument("out_dir", type=Path, help="directory for packaged release files")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    out_dir = args.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    flasher_args = load_flasher_args(build_dir)
    shutil.copy2(build_dir / "flasher_args.json", out_dir / "flasher_args.json")
    copied_files = copy_flash_files(build_dir, out_dir, flasher_args)
    merged_path = merge_binary(build_dir, out_dir, flasher_args)
    write_flashing_notes(out_dir, flasher_args, copied_files, merged_path)
    make_zip(out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
