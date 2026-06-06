#!/usr/bin/env python3
"""Overnight guard for the watch voice gateway.

The guard keeps the local gateway alive and periodically verifies the fast
local voice-command path. It intentionally avoids printing secrets.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.request
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
LOG_PATH = ROOT / "tools" / "watch_gateway" / "voice_guard.log"
GATEWAY_LOG = ROOT / "tools" / "watch_gateway" / "voice_gateway.log"
GATEWAY_URL = "http://127.0.0.1:8790"
WATCH_URL = "http://192.168.1.69"
SAVED_CONFIG_PATH = ROOT / "tools" / "watch_gateway" / "watch_gateway_config.json"


def saved_config() -> dict[str, str]:
    try:
        raw = json.loads(SAVED_CONFIG_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return {str(k): str(v) for k, v in raw.items() if v is not None}


SAVED_CONFIG = saved_config()
GATEWAY_CMD = [
    ".venv_voice_test/bin/python",
    "-u",
    "tools/voice_gateway.py",
    "--host",
    "0.0.0.0",
    "--port",
    "8790",
    "--upstream-api-url",
    os.getenv("WATCH_GATEWAY_API_URL") or SAVED_CONFIG.get("api_url", "https://ai.orbitlink.me/v1"),
    "--upstream-model",
    os.getenv("WATCH_GATEWAY_MODEL") or SAVED_CONFIG.get("model", "gpt-5.4"),
    "--stt-provider",
    os.getenv("WATCH_VOICE_STT_PROVIDER", "upstream"),
    "--stt-model",
    os.getenv("WATCH_VOICE_STT_MODEL", "whisper-1"),
    "--tts-provider",
    os.getenv("WATCH_VOICE_TTS_PROVIDER", "local"),
    "--no-preload-whisper",
    "--preload-tts",
    "--whisper-model",
    "tiny",
    "--tts-sample-rate",
    "12000",
    "--log-level",
    "INFO",
]


VOICE_TESTS: tuple[tuple[str, dict[str, Any]], ...] = (
    ("今天有什么新闻", {"local": False}),
    ("现在北京时间几点", {"local": False}),
    ("比特币为什么大跌", {"local": False}),
)

TEST_CONTEXT = {
    "battery_percent": 87,
    "market_time": "guard",
    "markets": [
        {"code": "BTC", "price": "102000", "change": "+1.2%"},
        {"code": "ETH", "price": "3400", "change": "-0.4%"},
        {"code": "XAU", "price": "2360", "change": "+0.2%"},
        {"code": "WTI", "price": "76.5", "change": "-1.1%"},
        {"code": "DXY", "price": "104.2", "change": "+0.1%"},
        {"code": "CNY", "price": "7.25", "change": "0.0%"},
        {"code": "EUR", "price": "1.08", "change": "+0.1%"},
        {"code": "VIX", "price": "14.5", "change": "-2.0%"},
        {"code": "SHC", "price": "3100", "change": "+0.3%"},
        {"code": "HSI", "price": "18500", "change": "+0.8%"},
        {"code": "N225", "price": "39000", "change": "-0.2%"},
        {"code": "DAX", "price": "18300", "change": "+0.4%"},
        {"code": "UKX", "price": "8200", "change": "-0.1%"},
        {"code": "SPX", "price": "5300", "change": "+0.5%"},
        {"code": "NDX", "price": "19000", "change": "+0.6%"},
    ],
}


def log(message: str) -> None:
    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    with LOG_PATH.open("a", encoding="utf-8") as fh:
        fh.write(f"[{stamp}] {message}\n")


def http_json(url: str, timeout: float = 5.0) -> dict[str, Any]:
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8", errors="replace"))


def post_json(path: str, payload: dict[str, Any], timeout: float = 90.0) -> bytes:
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        GATEWAY_URL + path,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def multipart(fields: dict[str, Any], file_bytes: bytes) -> tuple[str, bytes]:
    boundary = f"----watchguard{int(time.time() * 1000)}"
    chunks: list[bytes] = []
    for key, value in fields.items():
        raw = value if isinstance(value, bytes) else str(value).encode("utf-8")
        chunks.append(
            f"--{boundary}\r\n"
            f"Content-Disposition: form-data; name=\"{key}\"\r\n\r\n".encode()
            + raw
            + b"\r\n"
        )
    chunks.append(
        f"--{boundary}\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n".encode()
        + file_bytes
        + b"\r\n"
    )
    chunks.append(f"--{boundary}--\r\n".encode())
    return boundary, b"".join(chunks)


def post_voice(wav: bytes, timeout: float = 180.0) -> dict[str, Any]:
    boundary, body = multipart(
        {
            "language": "zh",
            "prompt": "中文普通话语音转写，只输出用户说的话，不要翻译。",
            "context": json.dumps(TEST_CONTEXT, ensure_ascii=False),
        },
        wav,
    )
    req = urllib.request.Request(
        GATEWAY_URL + "/api/watch/voice",
        data=body,
        headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8", errors="replace"))


def restart_gateway() -> None:
    log("gateway restart requested")
    subprocess.run(["screen", "-S", "watch_voice_gateway", "-X", "quit"], cwd=ROOT, check=False)
    time.sleep(1)
    ss = subprocess.run(
        ["ss", "-ltnp", "sport = :8790"],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    for token in ss.stdout.replace(",", " ").split():
        if token.startswith("pid="):
            pid = token.split("=", 1)[1]
            if pid.isdigit():
                subprocess.run(["kill", pid], check=False)
    time.sleep(1)
    cmd = (
        f"cd '{ROOT}' && exec "
        + " ".join(GATEWAY_CMD)
        + f" >> '{GATEWAY_LOG}' 2>&1"
    )
    subprocess.run(["screen", "-dmS", "watch_voice_gateway", "bash", "-lc", cmd], cwd=ROOT, check=False)
    time.sleep(20)


def health_check() -> bool:
    try:
        gateway = http_json(GATEWAY_URL + "/health")
        watch = http_json(WATCH_URL + "/api/status")
    except Exception as exc:
        log(f"health failed: {exc!r}")
        return False
    stt_provider = str(gateway.get("stt_provider") or "local")
    gateway_ok = gateway.get("ok") is True and (
        stt_provider == "upstream" or gateway.get("whisper_loaded") is True
    )
    watch_ok = watch.get("ai_state") == "READY" and not watch.get("ai_inflight") and not watch.get("tts_inflight")
    log(
        "health "
        + json.dumps(
            {
                "gateway_ok": gateway_ok,
                "watch_ok": watch_ok,
                "watch_state": watch.get("ai_state"),
                "tts_cache_items": gateway.get("tts_cache_items"),
            },
            ensure_ascii=False,
        )
    )
    return bool(gateway_ok and watch_ok)


def regression_check() -> bool:
    failures: list[dict[str, Any]] = []
    times: list[float] = []
    for phrase, expect in VOICE_TESTS:
        start = time.monotonic()
        try:
            wav = post_json(
                "/v1/audio/speech",
                {"input": phrase, "voice": "zh-CN-XiaoxiaoNeural", "response_format": "wav"},
            )
            result = post_voice(wav)
        except Exception as exc:
            failures.append({"phrase": phrase, "error": repr(exc)})
            continue
        elapsed = time.monotonic() - start
        times.append(elapsed)
        reasons = []
        for key, value in expect.items():
            if result.get(key) != value:
                reasons.append(f"{key} got {result.get(key)!r} expected {value!r}")
        if result.get("local") is False and not str(result.get("reply") or "").strip():
            reasons.append("empty remote reply")
        if reasons:
            failures.append(
                {
                    "phrase": phrase,
                    "text": result.get("text"),
                    "action": result.get("action"),
                    "asset": result.get("asset"),
                    "reasons": reasons,
                }
            )
    summary = {
        "count": len(VOICE_TESTS),
        "failures": failures,
        "min_s": round(min(times), 3) if times else None,
        "max_s": round(max(times), 3) if times else None,
        "avg_s": round(sum(times) / len(times), 3) if times else None,
    }
    log("regression " + json.dumps(summary, ensure_ascii=False))
    return not failures


def run_once(run_regression: bool) -> bool:
    ok = health_check()
    if not ok:
        restart_gateway()
        ok = health_check()
    if run_regression:
        regression_ok = regression_check()
        if not regression_ok:
            restart_gateway()
            regression_ok = regression_check()
        ok = ok and regression_ok
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description="Guard watch voice gateway overnight")
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--regression", action="store_true")
    parser.add_argument("--interval-s", type=int, default=600)
    parser.add_argument("--regression-every", type=int, default=0)
    args = parser.parse_args()

    tick = 0
    while True:
        run_regression = args.regression or (args.regression_every > 0 and tick % args.regression_every == 0)
        ok = run_once(run_regression)
        if args.once:
            return 0 if ok else 1
        tick += 1
        time.sleep(max(30, args.interval_s))


if __name__ == "__main__":
    sys.exit(main())
