#!/usr/bin/env python3
"""OpenAI-compatible TTS proxy backed by edge-tts."""

from __future__ import annotations

import argparse
import asyncio
import dataclasses
import hmac
import io
import logging
import math
import os
import re
import tempfile
import wave
from pathlib import Path
from typing import Any, Optional

from aiohttp import web
import edge_tts
import miniaudio
from edge_tts.exceptions import NoAudioReceived

LOG = logging.getLogger("edge_tts_proxy")

OPENAI_GENERIC_VOICES = {
    "alloy",
    "nova",
    "echo",
    "fable",
    "shimmer",
    "onyx",
}

EDGE_VOICE_RE = re.compile(r"^[a-z]{2}-[A-Z]{2}-.+Neural$")


@dataclasses.dataclass(frozen=True)
class ProxyConfig:
    host: str = "127.0.0.1"
    port: int = 3000
    token: str = ""
    default_voice: str = "zh-CN-XiaoxiaoNeural"
    wav_sample_rate: int = 24000
    request_timeout_s: int = 120


def build_error(
    status: int,
    message: str,
    *,
    error_type: str = "invalid_request_error",
    param: Optional[str] = None,
) -> web.Response:
    payload: dict[str, Any] = {
        "error": {
            "message": message,
            "type": error_type,
        }
    }
    if param:
        payload["error"]["param"] = param
    return web.json_response(payload, status=status)


def extract_bearer_token(request: web.Request) -> str:
    auth = request.headers.get("Authorization", "").strip()
    if not auth:
        return ""
    if auth.lower().startswith("bearer "):
        return auth[7:].strip()
    return ""


def is_supported_edge_voice(name: str) -> bool:
    return bool(name) and bool(EDGE_VOICE_RE.match(name))


def normalize_voice(requested: Optional[str], default_voice: str) -> str:
    voice = (requested or "").strip()
    if not voice:
        return default_voice
    if voice.lower() in OPENAI_GENERIC_VOICES:
        return default_voice
    return voice


def speed_to_edge_rate(speed_value: Any) -> str:
    try:
        speed = float(speed_value)
    except (TypeError, ValueError):
        speed = 1.0

    if not math.isfinite(speed):
        speed = 1.0

    speed = max(0.5, min(2.0, speed))
    percent = int(round((speed - 1.0) * 100.0))
    if percent >= 0:
        return f"+{percent}%"
    return f"{percent}%"


def mp3_file_to_wav_bytes(mp3_path: Path, sample_rate: int) -> bytes:
    decoded = miniaudio.decode_file(
        str(mp3_path),
        output_format=miniaudio.SampleFormat.SIGNED16,
        nchannels=1,
        sample_rate=sample_rate,
    )

    pcm_bytes = decoded.samples.tobytes()
    with io.BytesIO() as buf:
        with wave.open(buf, "wb") as wav_file:
            wav_file.setnchannels(decoded.nchannels)
            wav_file.setsampwidth(2)
            wav_file.setframerate(decoded.sample_rate)
            wav_file.writeframes(pcm_bytes)
        return buf.getvalue()


async def synthesize_wav(cfg: ProxyConfig, text: str, voice: str, speed: float) -> bytes:
    rate = speed_to_edge_rate(speed)
    voice = normalize_voice(voice, cfg.default_voice)
    if not is_supported_edge_voice(voice) and voice != cfg.default_voice:
        LOG.warning("Voice %r does not look like a known Edge voice; edge-tts may reject it", voice)

    with tempfile.TemporaryDirectory(prefix="edge-tts-proxy-") as tmpdir:
        tmp = Path(tmpdir)
        mp3_path = tmp / "speech.mp3"

        communicate = edge_tts.Communicate(text, voice=voice, rate=rate)
        await communicate.save(str(mp3_path))
        return mp3_file_to_wav_bytes(mp3_path, cfg.wav_sample_rate)


async def health_handler(request: web.Request) -> web.Response:
    app_cfg: ProxyConfig = request.app["cfg"]
    payload = {
        "ok": True,
        "service": "edge-tts-proxy",
        "default_voice": app_cfg.default_voice,
        "sample_rate": app_cfg.wav_sample_rate,
    }
    return web.json_response(payload)


async def root_handler(request: web.Request) -> web.Response:
    return web.json_response(
        {
            "service": "edge-tts-proxy",
            "endpoints": ["/health", "/v1/audio/speech"],
        }
    )


async def audio_speech_handler(request: web.Request) -> web.Response:
    cfg: ProxyConfig = request.app["cfg"]

    if cfg.token:
        presented = extract_bearer_token(request)
        if not presented or not hmac.compare_digest(presented, cfg.token):
            resp = build_error(401, "missing or invalid bearer token", error_type="authentication_error")
            resp.headers["WWW-Authenticate"] = 'Bearer realm="edge-tts-proxy"'
            return resp

    try:
        body = await request.json()
    except Exception:
        return build_error(400, "request body must be valid JSON", param="body")

    if not isinstance(body, dict):
        return build_error(400, "request body must be a JSON object", param="body")

    text = body.get("input", "")
    if not isinstance(text, str) or not text.strip():
        return build_error(400, "`input` must be a non-empty string", param="input")
    text = text.strip()

    response_format = body.get("response_format", "wav")
    if not isinstance(response_format, str):
        return build_error(400, "`response_format` must be a string when provided", param="response_format")
    if response_format.lower() != "wav":
        return build_error(400, "this proxy only returns wav audio", param="response_format")

    voice = body.get("voice", "")
    if not isinstance(voice, str):
        return build_error(400, "`voice` must be a string when provided", param="voice")

    speed = body.get("speed", 1.0)
    try:
        speed = float(speed)
    except (TypeError, ValueError):
        return build_error(400, "`speed` must be a number", param="speed")

    if not math.isfinite(speed):
        return build_error(400, "`speed` must be finite", param="speed")

    try:
        LOG.info("TTS request chars=%d voice=%s speed=%.2f", len(text), voice or cfg.default_voice, speed)
        wav_bytes = await asyncio.wait_for(
            synthesize_wav(cfg, text, voice, speed),
            timeout=cfg.request_timeout_s,
        )
    except asyncio.TimeoutError:
        LOG.exception("TTS synthesis timed out")
        return build_error(504, "TTS synthesis timed out", error_type="timeout")
    except FileNotFoundError as exc:
        LOG.exception("Missing runtime dependency")
        return build_error(500, f"missing runtime dependency: {exc}", error_type="server_error")
    except RuntimeError as exc:
        LOG.exception("Audio conversion failed")
        return build_error(500, str(exc), error_type="server_error")
    except NoAudioReceived as exc:
        LOG.exception("edge-tts returned no audio")
        return build_error(502, str(exc), error_type="upstream_error")
    except Exception as exc:  # pragma: no cover - defensive catch-all
        LOG.exception("Unexpected synthesis error")
        return build_error(500, str(exc), error_type="server_error")

    return web.Response(body=wav_bytes, content_type="audio/wav")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="OpenAI-compatible TTS proxy powered by edge-tts")
    parser.add_argument("--host", default=os.getenv("EDGE_TTS_HOST", "127.0.0.1"), help="Bind host")
    parser.add_argument("--port", type=int, default=int(os.getenv("EDGE_TTS_PORT", "3000")), help="Bind port")
    parser.add_argument("--token", default=os.getenv("EDGE_TTS_TOKEN", ""), help="Optional bearer token")
    parser.add_argument(
        "--default-voice",
        default=os.getenv("EDGE_TTS_DEFAULT_VOICE", "zh-CN-XiaoxiaoNeural"),
        help="Fallback voice when the request uses OpenAI voices like alloy/nova",
    )
    parser.add_argument(
        "--sample-rate",
        type=int,
        default=int(os.getenv("EDGE_TTS_WAV_SAMPLE_RATE", "24000")),
        help="WAV sample rate used for the final output",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=int(os.getenv("EDGE_TTS_REQUEST_TIMEOUT_S", "120")),
        help="Request timeout in seconds",
    )
    parser.add_argument(
        "--log-level",
        default=os.getenv("EDGE_TTS_LOG_LEVEL", "INFO"),
        help="Logging level",
    )
    return parser.parse_args()


def build_app(cfg: ProxyConfig) -> web.Application:
    app = web.Application(client_max_size=64 * 1024)
    app["cfg"] = cfg
    app.router.add_get("/", root_handler)
    app.router.add_get("/health", health_handler)
    app.router.add_post("/v1/audio/speech", audio_speech_handler)
    return app


def main() -> None:
    args = parse_args()
    logging.basicConfig(
        level=getattr(logging, str(args.log_level).upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    cfg = ProxyConfig(
        host=args.host,
        port=args.port,
        token=args.token,
        default_voice=args.default_voice,
        wav_sample_rate=args.sample_rate,
        request_timeout_s=args.timeout,
    )

    LOG.info(
        "Starting edge-tts proxy on %s:%d, default voice=%s, sample_rate=%d",
        cfg.host,
        cfg.port,
        cfg.default_voice,
        cfg.wav_sample_rate,
    )
    if cfg.token:
        LOG.info("Bearer token authentication is enabled")

    web.run_app(build_app(cfg), host=cfg.host, port=cfg.port)


if __name__ == "__main__":
    main()
