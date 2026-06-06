#!/usr/bin/env python3
"""OpenAI-compatible local voice gateway for the watch.

The watch can point its API URL at this gateway. Chat requests are forwarded to
the configured upstream. STT/TTS can run locally or be proxied upstream.
"""

from __future__ import annotations

import argparse
import asyncio
import dataclasses
import datetime as dt
import email.utils
import html
import hmac
import io
import json
import logging
import math
import os
import re
import threading
import tempfile
import time
import unicodedata
import wave
from pathlib import Path
from typing import Any
import urllib.error
import urllib.parse
import urllib.request
import struct
import xml.etree.ElementTree as ET

from aiohttp import web

try:
    from edge_tts.exceptions import NoAudioReceived
except ImportError:
    class NoAudioReceived(Exception):
        pass


ROOT = Path(__file__).resolve().parent
WATCH_FONT_PATH = ROOT.parent / "main" / "watch_ai_cn_24.c"
CONFIG_PATH = ROOT / "watch_gateway" / "watch_gateway_config.json"
DEBUG_DIR = ROOT / "watch_gateway"
LAST_VOICE_PATH = DEBUG_DIR / "last_voice.wav"
LAST_VOICE_INFO_PATH = DEBUG_DIR / "last_voice.json"
LOG = logging.getLogger("voice_gateway")


OPENAI_GENERIC_VOICES = {"alloy", "nova", "echo", "fable", "shimmer", "onyx"}
DEFAULT_UPSTREAM_API_URL = "https://ai.orbitlink.me/v1"
SILENCE_AVG_ABS_THRESHOLD = 300
SILENCE_PEAK_THRESHOLD = 1800
SILENCE_MIN_DURATION_S = 0.25
TTS_CACHE_MAX_ITEMS = 128
COMMON_TTS_PRELOAD_TEXTS = (
    "已回到首页。",
    "已打开聊天。",
    "已进入睡眠。",
    "我能打开图表、查价格、刷新行情、调亮调暗、回首页、打开聊天。",
    "语音正常。",
    "我在，请直接说。",
    "已打开 BTC 图表。",
    "正在刷新 BTC 图表。",
    "已打开 ETH 图表。",
    "正在刷新 ETH 图表。",
    "已打开 XAU 图表。",
    "已打开 WTI 图表。",
    "已打开 DXY 图表。",
    "已打开 CNY 图表。",
    "已打开 EUR 图表。",
    "已打开 VIX 图表。",
    "已打开 SHC 图表。",
    "已打开 HSI 图表。",
    "已打开 N225 图表。",
    "已打开 DAX 图表。",
    "已打开 UKX 图表。",
    "已打开 NDX 图表。",
    "已打开 SPX 图表。",
    "正在刷新行情，请稍等。",
    "已调亮屏幕。",
    "已调暗屏幕。",
)
PROMPT_HALLUCINATION_MARKERS = (
    "语音转写",
    "只输出用户说",
    "不要翻译",
    "中文普通话",
)
STT_INITIAL_PROMPT = (
    "以下是智能手表中文语音命令和聊天内容。"
    "常见词：手表、语音、测试、打开、返回、首页、聊天、图表、亮度、调亮、调暗、"
    "刷新、行情、比特币、以太坊、黄金、原油、美元、人民币、欧元、上证、恒生、"
    "日经、德国指数、英国富时、标普、纳指、恐慌指数。"
    "如果听到类似底特币、笔特币、必特币，要按比特币转写。"
    "请转写用户真实说的话，不要翻译，不要输出提示词。"
)
STT_HOTWORDS = (
    "手表 语音 测试 打开 返回 首页 聊天 图表 亮度 调亮 调暗 "
    "刷新 行情 比特币 底特币 笔特币 必特币 以太坊 黄金 原油 美元 人民币 "
    "欧元 上证 恒生 日经 德国指数 英国富时 标普 纳指 恐慌指数"
)

MARKET_ALIASES = (
    ("BTC", ("btc", "bitcoin", "比特币", "底特币", "笔特币", "必特币", "币特币", "逼特币")),
    ("ETH", ("eth", "ethereum", "以太坊", "以太", "一太坊", "以太方", "以太房")),
    ("XAU", ("xau", "gold", "黄金", "金价")),
    ("WTI", ("wti", "oil", "原油", "油价", "美油")),
    ("DXY", ("dxy", "美元指数", "美元")),
    ("CNY", ("cny", "人民币", "美元人民币", "离岸人民币")),
    ("EUR", ("eur", "欧元", "欧元美元", "后原", "后元", "欧原", "欧园")),
    ("VIX", ("vix", "恐慌", "恐慌指数", "波动率")),
    ("SHC", ("shc", "上证", "上正", "沪指", "a股", "A股", "大盘")),
    ("HSI", ("hsi", "恒生", "横声", "横生", "恒声", "恒指", "港股")),
    ("N225", ("n225", "日经", "日经指数")),
    ("DAX", ("dax", "德国", "德指", "德国指数")),
    ("UKX", ("ukx", "英国", "富时", "副实", "富时指数")),
    ("SPX", ("spx", "标普", "调普", "标铺", "标普五百", "美股")),
    ("NDX", ("ndx", "纳指", "纳斯达克", "纳斯达克一百", "科技股")),
)

REALTIME_MARKET_SYMBOLS = {
    "BTC": ("BTC-USD", "Bitcoin"),
    "ETH": ("ETH-USD", "Ethereum"),
    "XAU": ("GC=F", "Gold"),
    "WTI": ("CL=F", "Crude Oil"),
    "DXY": ("DX-Y.NYB", "US Dollar Index"),
    "CNY": ("CNH=X", "USD/CNH"),
    "EUR": ("EURUSD=X", "EUR/USD"),
    "VIX": ("^VIX", "VIX"),
    "SHC": ("000001.SS", "Shanghai Composite"),
    "HSI": ("^HSI", "Hang Seng Index"),
    "N225": ("^N225", "Nikkei 225"),
    "DAX": ("^GDAXI", "DAX"),
    "UKX": ("^FTSE", "FTSE 100"),
    "SPX": ("^GSPC", "S&P 500"),
    "NDX": ("^NDX", "Nasdaq 100"),
}

WATCH_HOME_MARKETS = (
    ("BTC", "BTC-USD"),
    ("ETH", "ETH-USD"),
    ("XAU", "GC=F"),
    ("WTI", "CL=F"),
    ("DXY", "DX-Y.NYB"),
    ("CNY", "CNY=X"),
    ("EUR", "EURUSD=X"),
    ("VIX", "^VIX"),
    ("SHC", "000001.SS"),
    ("HSI", "^HSI"),
    ("N225", "^N225"),
    ("DAX", "^GDAXI"),
    ("UKX", "^FTSE"),
    ("SPX", "^GSPC"),
    ("NDX", "^NDX"),
)

REALTIME_TRIGGER_WORDS = (
    "为什么", "为啥", "原因", "怎么回事", "怎么看", "分析", "解释",
    "今天", "最近", "现在", "最新", "新闻", "消息", "联网", "查一下",
    "大跌", "大涨", "暴跌", "暴涨", "后面", "未来", "预测", "会不会",
)

NEWS_TRIGGER_WORDS = (
    "新闻", "消息", "快讯", "热点", "头条", "报道", "爆料", "资讯",
    "动态", "进展", "最新", "最近", "实时", "刚刚", "发生了什么",
)
NEWS_CONTEXT_CACHE_TTL_S = 90
WEB_CONTEXT_CACHE_TTL_S = 90
WEB_FETCH_TIMEOUT_S = 5
MARKET_FETCH_TIMEOUT_S = 8
GENERIC_NEWS_QUERIES = {
    "新闻",
    "查新闻",
    "看看新闻",
    "最新新闻",
    "今天新闻",
    "今天有什么新闻",
    "今天有哪些新闻",
    "新闻头条",
    "头条新闻",
    "热点新闻",
}

TEXT_CORRECTIONS = (
    ("底特币", "比特币"),
    ("笔特币", "比特币"),
    ("必特币", "比特币"),
    ("币特币", "比特币"),
    ("逼特币", "比特币"),
    ("比特必", "比特币"),
    ("比特幣", "比特币"),
    ("一太坊", "以太坊"),
    ("以太方", "以太坊"),
    ("以太房", "以太坊"),
    ("以太放", "以太坊"),
    ("以太幣", "以太坊"),
    ("手标", "手表"),
    ("納指", "纳指"),
    ("那指", "纳指"),
    ("砍纳指", "看纳指"),
    ("砍那只", "看纳指"),
    ("砍那支", "看纳指"),
    ("看那只", "看纳指"),
    ("看那支", "看纳指"),
    ("標普", "标普"),
    ("调普", "标普"),
    ("标铺", "标普"),
    ("恆生", "恒生"),
    ("横声", "恒生"),
    ("横生", "恒生"),
    ("恒声", "恒生"),
    ("上正", "上证"),
    ("黃金", "黄金"),
    ("后原", "欧元"),
    ("后元", "欧元"),
    ("欧原", "欧元"),
    ("欧园", "欧元"),
    ("原油期貨", "原油"),
    ("副实", "富时"),
    ("投表", "图表"),
    ("漂亮屏幕", "调亮屏幕"),
    ("屏幕漂亮", "屏幕调亮"),
    ("调案屏幕", "调暗屏幕"),
    ("调案", "调暗"),
    ("翻语", "返回"),
    ("现在级点", "现在几点"),
    ("级点", "几点"),
    ("让评", "亮屏"),
    ("亮评", "亮屏"),
    ("靓屏", "亮屏"),
    ("圖表", "图表"),
    ("首頁", "首页"),
    ("聊天頁", "聊天"),
    ("語音", "语音"),
    ("幫助", "帮助"),
    ("設置", "设置"),
    ("關屏", "关屏"),
    ("关评", "关屏"),
    ("关平", "关屏"),
    ("熄屏", "息屏"),
)

SAFE_REPLY_TRANSLATION = str.maketrans({
    "\r": "\n",
    "\t": " ",
    "\u00a0": " ",
    "\u3000": " ",
    "\u200b": "",
    "\u200c": "",
    "\u200d": "",
    "\ufe0e": "",
    "\ufe0f": "",
    "，": "，",
    "。": "。",
    "！": "！",
    "？": "？",
    "：": "：",
    "；": "；",
    "（": "（",
    "）": "）",
    "【": "【",
    "】": "】",
    "《": "《",
    "》": "》",
    "「": "“",
    "」": "”",
    "『": "“",
    "』": "”",
    "－": "-",
    "—": "-",
    "–": "-",
    "−": "-",
    "～": "。",
    "〜": "。",
    "…": "...",
    "✅": "",
    "✔": "",
    "☑": "",
    "❌": "",
    "✖": "",
    "👉": "",
    "👍": "",
})

TRADITIONAL_REPLY_TRANSLATION = str.maketrans({
    "這": "这", "個": "个", "裏": "里", "裡": "里", "們": "们", "著": "着",
    "麼": "么", "麽": "么", "讓": "让", "還": "还", "夠": "够", "樣": "样",
    "測": "测", "試": "试", "體": "体", "繁": "繁", "簡": "简",
    "進": "进", "請": "请", "後": "后", "語": "语", "音": "音",
    "關": "关", "閉": "闭", "開": "开", "啟": "启", "顯": "显", "示": "示",
    "圖": "图", "錶": "表", "標": "标", "幣": "币", "價": "价", "格": "格",
    "漲": "涨", "跌": "跌", "資": "资", "訊": "讯", "數": "数", "據": "据",
    "時": "时", "間": "间", "電": "电", "量": "量", "螢": "屏", "幕": "幕",
    "頁": "页", "首": "首", "助": "助", "手": "手", "設": "设", "置": "置",
    "歡": "欢", "迎": "迎", "嗎": "吗", "聽": "听", "說": "说", "話": "话",
    "識": "识", "別": "别", "應": "应", "該": "该", "題": "题", "問": "问",
    "註": "注", "釋": "释", "與": "与", "為": "为", "現": "现", "險": "险",
    "風": "风", "隨": "随", "輸": "输", "入": "入", "輸": "输", "出": "出",
    "網": "网", "絡": "络", "連": "连", "接": "接", "錯": "错", "誤": "误",
    "輕": "轻", "鬆": "松", "準": "准", "確": "确", "認": "认", "錄": "录",
    "廣": "广", "東": "东", "麥": "麦", "克": "克", "風": "风", "喇": "喇",
    "叭": "叭", "黃": "黄", "恆": "恒", "納": "纳", "歐": "欧", "國": "国",
    "際": "际", "萬": "万", "億": "亿", "長": "长", "短": "短", "亮": "亮",
    "暗": "暗", "調": "调", "整": "整", "線": "线", "刷": "刷", "新": "新",
    "讀": "读", "寫": "写", "馬": "马", "達": "达", "發": "发", "傳": "传",
    "統": "统", "錯": "错", "誤": "误", "測": "测", "試": "试",
    "氣": "气", "溫": "温", "度": "度", "雲": "云", "灣": "湾", "內": "内",
    "外": "外", "單": "单", "複": "复", "雜": "杂", "優": "优", "化": "化",
    "級": "级", "檔": "档", "案": "案", "檢": "检", "查": "查", "儲": "储",
    "存": "存", "載": "载", "離": "离", "綫": "线", "線": "线", "歷": "历",
    "史": "史", "區": "区", "塊": "块", "鏈": "链", "錢": "钱", "包": "包",
    "報": "报", "告": "告", "資": "资", "料": "料", "訊": "讯", "息": "息",
    "庫": "库", "務": "务", "務": "务", "總": "总", "結": "结", "論": "论",
})

WATCH_SAFE_PUNCT = set("，。！？、：；（）【】《》￥％＋－")
WATCH_FONT_CODEPOINTS: set[int] | None = None


def watch_font_codepoints() -> set[int]:
    global WATCH_FONT_CODEPOINTS
    if WATCH_FONT_CODEPOINTS is not None:
        return WATCH_FONT_CODEPOINTS

    codepoints: set[int] = set()
    try:
        font_source = WATCH_FONT_PATH.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        WATCH_FONT_CODEPOINTS = codepoints
        return codepoints

    for match in re.finditer(r"/\*\s+U\+([0-9A-Fa-f]{4,6})\s+", font_source):
        codepoints.add(int(match.group(1), 16))
    WATCH_FONT_CODEPOINTS = codepoints
    return codepoints


def sanitize_watch_reply(text: str, max_chars: int = 120) -> str:
    """Keep replies short and inside the watch bitmap-font comfort zone."""
    cleaned = normalize_watch_text(str(text or "")).translate(TRADITIONAL_REPLY_TRANSLATION)
    cleaned = cleaned.translate(SAFE_REPLY_TRANSLATION)
    cleaned = re.sub(r"```.*?```", "", cleaned, flags=re.S)
    cleaned = re.sub(r"`([^`]*)`", r"\1", cleaned)
    cleaned = re.sub(r"[*_#>\[\]{}|~]", "", cleaned)

    font_codepoints = watch_font_codepoints()
    out: list[str] = []
    last_space = False
    last_newline = False
    for ch in cleaned:
        cp = ord(ch)
        if ch == "\n":
            if not last_newline and out:
                out.append("\n")
            last_space = False
            last_newline = True
            continue
        if ch.isspace():
            if not last_space and not last_newline and out:
                out.append(" ")
            last_space = True
            continue
        last_space = False
        last_newline = False

        if 0x20 <= cp <= 0x7E:
            out.append(ch)
        elif ch in WATCH_SAFE_PUNCT and (not font_codepoints or cp in font_codepoints):
            out.append(ch)
        elif 0x4E00 <= cp <= 0x9FFF and (not font_codepoints or cp in font_codepoints):
            out.append(ch)
        else:
            category = unicodedata.category(ch)
            if category.startswith("C") or category[0] in {"P", "S"}:
                continue
            out.append("?")

        if len(out) >= max_chars:
            break

    reply = "".join(out).strip(" \n")
    reply = re.sub(r"\n{3,}", "\n\n", reply)
    return reply or "我听到了。"


@dataclasses.dataclass
class GatewayConfig:
    host: str
    port: int
    token: str
    upstream_api_url: str
    upstream_api_key: str
    upstream_model: str
    default_voice: str
    tts_sample_rate: int
    whisper_model: str
    whisper_device: str
    whisper_compute_type: str
    stt_provider: str
    stt_model: str
    tts_provider: str
    tts_model: str
    request_timeout_s: int
    preload_whisper: bool
    preload_tts: bool


WHISPER: Any | None = None
WHISPER_LOCK = threading.Lock()
VOICE_AUDIO: dict[str, bytes] = {}
VOICE_AUDIO_TASKS: dict[str, asyncio.Task[bytes]] = {}
LAST_VOICE_INFO: dict[str, Any] = {}
TTS_CACHE: dict[tuple[str, str, str, int], bytes] = {}
WEB_CONTEXT_CACHE: dict[tuple[str, str], tuple[float, str]] = {}
WEB_CONTEXT_CACHE_LOCK = threading.Lock()


def load_saved_config() -> dict[str, str]:
    if not CONFIG_PATH.exists():
        return {}
    try:
        data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    if not isinstance(data, dict):
        return {}
    return {str(k): str(v) for k, v in data.items()}


def normalize_base_url(url: str) -> str:
    url = (url or "").strip().rstrip("/")
    if not url:
        return ""
    if url.endswith("/chat/completions"):
        return url[: -len("/chat/completions")]
    return url


def extract_bearer_token(request: web.Request) -> str:
    auth = request.headers.get("Authorization", "").strip()
    if auth.lower().startswith("bearer "):
        return auth[7:].strip()
    return ""


def request_or_config_api_key(cfg: GatewayConfig, request: web.Request) -> str:
    return extract_bearer_token(request) or cfg.upstream_api_key


def auth_ok(request: web.Request) -> bool:
    cfg: GatewayConfig = request.app["cfg"]
    if not cfg.token:
        return True
    return hmac.compare_digest(extract_bearer_token(request), cfg.token)


def json_error(status: int, message: str, error_type: str = "invalid_request_error") -> web.Response:
    return web.json_response({"error": {"message": message, "type": error_type}}, status=status)


def parse_header_params(value: str) -> tuple[str, dict[str, str]]:
    parts = [part.strip() for part in (value or "").split(";") if part.strip()]
    if not parts:
        return "", {}
    params: dict[str, str] = {}
    for part in parts[1:]:
        if "=" not in part:
            continue
        key, raw = part.split("=", 1)
        raw = raw.strip()
        if len(raw) >= 2 and raw[0] == '"' and raw[-1] == '"':
            raw = raw[1:-1].replace('\\"', '"')
        params[key.strip().lower()] = raw
    return parts[0].lower(), params


def save_last_voice_debug(audio_data: bytes, suffix: str, source: str,
                          text: str = "", error: str = "") -> None:
    global LAST_VOICE_INFO
    try:
        DEBUG_DIR.mkdir(parents=True, exist_ok=True)
        LAST_VOICE_PATH.write_bytes(audio_data)
        info = {
            "updated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "source": source,
            "suffix": suffix,
            "bytes": len(audio_data),
            "text": text,
            "error": error,
            "audio_url": "/api/watch/voice/last.wav",
        }
        LAST_VOICE_INFO_PATH.write_text(
            json.dumps(info, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
        LAST_VOICE_INFO = info
    except OSError:
        LOG.exception("Failed to save last voice debug file")


def wav_energy_stats(audio_data: bytes) -> dict[str, float] | None:
    try:
        with wave.open(io.BytesIO(audio_data), "rb") as wav:
            channels = wav.getnchannels()
            sample_width = wav.getsampwidth()
            rate = wav.getframerate()
            frames = wav.getnframes()
            pcm = wav.readframes(frames)
    except (wave.Error, EOFError, OSError):
        return None

    if sample_width != 2 or rate <= 0 or channels <= 0 or not pcm:
        return None

    sample_count = len(pcm) // 2
    if sample_count <= 0:
        return None

    total_abs = 0
    peak = 0
    for i in range(0, sample_count * 2, 2):
        sample = int.from_bytes(pcm[i:i + 2], "little", signed=True)
        value = abs(sample)
        total_abs += value
        if value > peak:
            peak = value

    return {
        "duration_s": frames / rate,
        "avg_abs": total_abs / sample_count,
        "peak": float(peak),
    }


def audio_looks_silent(audio_data: bytes) -> tuple[bool, str]:
    stats = wav_energy_stats(audio_data)
    if not stats:
        return False, ""
    if (
        stats["duration_s"] >= SILENCE_MIN_DURATION_S
        and stats["avg_abs"] < SILENCE_AVG_ABS_THRESHOLD
        and stats["peak"] < SILENCE_PEAK_THRESHOLD
    ):
        return True, (
            "没有检测到有效语音"
            f" avg={stats['avg_abs']:.1f} peak={stats['peak']:.0f}"
        )
    return False, ""


def stt_text_is_prompt_hallucination(text: str) -> bool:
    normalized = "".join(str(text or "").split())
    if not normalized:
        return True
    return any(marker in normalized for marker in PROMPT_HALLUCINATION_MARKERS)


def upstream_chat_url(cfg: GatewayConfig) -> str:
    base = normalize_base_url(cfg.upstream_api_url)
    if base.endswith("/v1"):
        return base + "/chat/completions"
    return base + "/v1/chat/completions"


def upstream_endpoint_url(cfg: GatewayConfig, endpoint_path: str) -> str:
    base = normalize_base_url(cfg.upstream_api_url)
    endpoint_path = endpoint_path.strip("/")
    if base.endswith("/v1"):
        return f"{base}/{endpoint_path}"
    return f"{base}/v1/{endpoint_path}"


def http_json_get(url: str, timeout: int = 8) -> dict[str, Any]:
    req = urllib.request.Request(
        url,
        headers={
            "Accept": "application/json",
            "User-Agent": "Mozilla/5.0 WatchVoiceGateway/1.0",
        },
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8", errors="replace"))


def http_text_get(url: str, timeout: int = 8) -> str:
    req = urllib.request.Request(
        url,
        headers={
            "Accept": "text/html,application/xhtml+xml,text/plain,application/rss+xml,application/xml",
            "User-Agent": "Mozilla/5.0 WatchVoiceGateway/1.0",
        },
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read().decode("utf-8", errors="replace")


def strip_html(text: str) -> str:
    text = re.sub(r"<script.*?</script>", "", text, flags=re.S | re.I)
    text = re.sub(r"<style.*?</style>", "", text, flags=re.S | re.I)
    text = re.sub(r"<[^>]+>", " ", text)
    return re.sub(r"\s+", " ", html.unescape(text)).strip()


def realtime_needed(text: str) -> bool:
    normalized = normalize_watch_text(text).replace(" ", "")
    return any(word in normalized for word in REALTIME_TRIGGER_WORDS)


def web_context_cache_get(kind: str, query: str, ttl_s: int) -> str:
    key = (kind, normalize_watch_text(query).strip().lower())
    now = time.monotonic()
    with WEB_CONTEXT_CACHE_LOCK:
        cached = WEB_CONTEXT_CACHE.get(key)
        if not cached:
            return ""
        created, text = cached
        if now - created <= ttl_s:
            return text
        WEB_CONTEXT_CACHE.pop(key, None)
    return ""


def web_context_cache_set(kind: str, query: str, text: str) -> str:
    key = (kind, normalize_watch_text(query).strip().lower())
    with WEB_CONTEXT_CACHE_LOCK:
        if len(WEB_CONTEXT_CACHE) >= 96:
            oldest = min(WEB_CONTEXT_CACHE, key=lambda cache_key: WEB_CONTEXT_CACHE[cache_key][0])
            WEB_CONTEXT_CACHE.pop(oldest, None)
        WEB_CONTEXT_CACHE[key] = (time.monotonic(), text)
    return text


def news_query_needed(text: str) -> bool:
    normalized = normalize_watch_text(text).replace(" ", "")
    return any(word in normalized for word in NEWS_TRIGGER_WORDS)


def generic_news_query_needed(text: str) -> bool:
    normalized = normalize_watch_text(text).strip(" ，。！？、,.!?;；:").replace(" ", "")
    return normalized in GENERIC_NEWS_QUERIES


def google_news_search_query(text: str) -> str:
    query = normalize_watch_text(text).strip(" ，。！？、,.!?;；:")
    return query or "中国 国际 最新新闻"


def format_rss_pubdate_cn(pubdate: str) -> str:
    if not pubdate:
        return ""
    try:
        parsed = email.utils.parsedate_to_datetime(pubdate)
    except (TypeError, ValueError):
        return ""
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    beijing = parsed.astimezone(dt.timezone(dt.timedelta(hours=8)))
    return beijing.strftime("%m-%d %H:%M")


def google_news_context(query: str, limit: int = 5) -> str:
    cached = web_context_cache_get("google_news", query, NEWS_CONTEXT_CACHE_TTL_S)
    if cached:
        return cached
    url = (
        "https://news.google.com/rss/search?q="
        + urllib.parse.quote(query)
        + "&hl=zh-CN&gl=CN&ceid=CN:zh-Hans"
    )
    body = http_text_get(url, timeout=WEB_FETCH_TIMEOUT_S)
    root = ET.fromstring(body)
    channel = root.find("channel")
    if channel is None:
        return web_context_cache_set("google_news", query, "")
    items: list[str] = []
    for item in channel.findall("item"):
        title = html.unescape((item.findtext("title") or "")).strip()
        if not title:
            continue
        when = format_rss_pubdate_cn(item.findtext("pubDate") or "")
        source = html.unescape((item.findtext("source") or "")).strip()
        if source and f" - {source}" not in title:
            title = f"{title} - {source}"
        prefix = f"{len(items) + 1}. "
        if when:
            prefix += f"{when} "
        items.append((prefix + title)[:240])
        if len(items) >= limit:
            break
    return web_context_cache_set("google_news", query, "\n".join(items))


def google_news_top_context(limit: int = 6) -> str:
    cache_key = "zh-CN/CN/top"
    cached = web_context_cache_get("google_news_top", cache_key, NEWS_CONTEXT_CACHE_TTL_S)
    if cached:
        return cached
    url = "https://news.google.com/rss?hl=zh-CN&gl=CN&ceid=CN:zh-Hans"
    body = http_text_get(url, timeout=WEB_FETCH_TIMEOUT_S)
    root = ET.fromstring(body)
    channel = root.find("channel")
    if channel is None:
        return web_context_cache_set("google_news_top", cache_key, "")
    items: list[str] = []
    for item in channel.findall("item"):
        title = html.unescape((item.findtext("title") or "")).strip()
        if not title or "找不到此 Feed" in title:
            continue
        when = format_rss_pubdate_cn(item.findtext("pubDate") or "")
        source = html.unescape((item.findtext("source") or "")).strip()
        if source and f" - {source}" not in title:
            title = f"{title} - {source}"
        prefix = f"{len(items) + 1}. "
        if when:
            prefix += f"{when} "
        items.append((prefix + title)[:240])
        if len(items) >= limit:
            break
    return web_context_cache_set("google_news_top", cache_key, "\n".join(items))


def duckduckgo_lite_context(query: str, limit: int = 3) -> str:
    cached = web_context_cache_get("duckduckgo", query, WEB_CONTEXT_CACHE_TTL_S)
    if cached:
        return cached
    url = "https://lite.duckduckgo.com/lite/?q=" + urllib.parse.quote(query)
    body = http_text_get(url, timeout=WEB_FETCH_TIMEOUT_S)
    rows = re.findall(r"<tr[^>]*>(.*?)</tr>", body, flags=re.S | re.I)
    items: list[str] = []
    pending_title = ""
    for row in rows:
        if "result-link" in row:
            pending_title = strip_html(row)
            continue
        if pending_title and "result-snippet" in row:
            snippet = strip_html(row)
            if pending_title:
                item = pending_title
                if snippet:
                    item += " - " + snippet
                items.append(item[:220])
                pending_title = ""
            if len(items) >= limit:
                break
    if not items:
        for match in re.finditer(r'class=["\']result-link["\'][^>]*>(.*?)</a>', body, flags=re.S | re.I):
            title = strip_html(match.group(1))
            if title:
                items.append(title[:160])
            if len(items) >= limit:
                break
    return web_context_cache_set("duckduckgo", query, "；".join(items[:limit]))


def network_datetime_context() -> str:
    cached = web_context_cache_get("network_time", "Asia/Shanghai", 15)
    if cached:
        return cached
    urls = (
        "https://worldtimeapi.org/api/timezone/Asia/Shanghai",
        "https://timeapi.io/api/Time/current/zone?timeZone=Asia/Shanghai",
    )
    weekdays = ("一", "二", "三", "四", "五", "六", "日")
    for url in urls:
        try:
            data = http_json_get(url, timeout=WEB_FETCH_TIMEOUT_S)
        except Exception as exc:
            LOG.warning("network time fetch failed %s: %s", url, exc)
            continue
        try:
            if "datetime" in data:
                raw = str(data["datetime"]).replace("Z", "+00:00")
                parsed = dt.datetime.fromisoformat(raw)
                if parsed.tzinfo is None:
                    parsed = parsed.replace(tzinfo=dt.timezone.utc)
                beijing = parsed.astimezone(dt.timezone(dt.timedelta(hours=8)))
            else:
                beijing = dt.datetime(
                    int(data["year"]),
                    int(data["month"]),
                    int(data["day"]),
                    int(data["hour"]),
                    int(data["minute"]),
                    tzinfo=dt.timezone(dt.timedelta(hours=8)),
                )
            text = (
                f"联网校时：北京时间 {beijing.year}年{beijing.month}月{beijing.day}日，"
                f"星期{weekdays[beijing.weekday()]}，{beijing.hour:02d}:{beijing.minute:02d}。"
            )
            return web_context_cache_set("network_time", "Asia/Shanghai", text)
        except (KeyError, TypeError, ValueError) as exc:
            LOG.warning("network time parse failed %s: %s", url, exc)
    return web_context_cache_set("network_time", "Asia/Shanghai", "")


def header_datetime_context() -> str:
    cached = web_context_cache_get("header_time", "Asia/Shanghai", 15)
    if cached:
        return cached
    urls = (
        "https://www.baidu.com/",
        "https://www.bing.com/",
        "https://www.google.com/generate_204",
    )
    weekdays = ("一", "二", "三", "四", "五", "六", "日")
    for url in urls:
        try:
            req = urllib.request.Request(
                url,
                headers={"User-Agent": "Mozilla/5.0 WatchVoiceGateway/1.0"},
                method="HEAD",
            )
            with urllib.request.urlopen(req, timeout=WEB_FETCH_TIMEOUT_S) as resp:
                raw_date = resp.headers.get("Date", "")
        except Exception as exc:
            LOG.warning("header time fetch failed %s: %s", url, exc)
            continue
        try:
            parsed = email.utils.parsedate_to_datetime(raw_date)
            if parsed.tzinfo is None:
                parsed = parsed.replace(tzinfo=dt.timezone.utc)
            beijing = parsed.astimezone(dt.timezone(dt.timedelta(hours=8)))
        except (TypeError, ValueError) as exc:
            LOG.warning("header time parse failed %s: %s", url, exc)
            continue
        text = (
            f"联网校时：北京时间 {beijing.year}年{beijing.month}月{beijing.day}日，"
            f"星期{weekdays[beijing.weekday()]}，{beijing.hour:02d}:{beijing.minute:02d}。"
        )
        return web_context_cache_set("header_time", "Asia/Shanghai", text)
    return web_context_cache_set("header_time", "Asia/Shanghai", "")


def yahoo_chart_context(symbol: str) -> str:
    url = (
        "https://query1.finance.yahoo.com/v8/finance/chart/"
        + urllib.parse.quote(symbol, safe="")
        + "?range=5d&interval=1d"
    )
    data = http_json_get(url)
    result = (((data.get("chart") or {}).get("result") or [None])[0] or {})
    meta = result.get("meta") or {}
    quote = ((((result.get("indicators") or {}).get("quote") or [None])[0]) or {})
    closes = [v for v in quote.get("close", []) if isinstance(v, (int, float)) and v > 0]
    price = meta.get("regularMarketPrice")
    if not isinstance(price, (int, float)) and closes:
        price = closes[-1]
    day_low = meta.get("regularMarketDayLow")
    day_high = meta.get("regularMarketDayHigh")
    parts = []
    if isinstance(price, (int, float)):
        parts.append(f"最新价约 {price:.2f}")
    if len(closes) >= 2:
        prev = closes[-2]
        change = ((closes[-1] - prev) / prev) * 100.0 if prev else 0.0
        parts.append(f"近一日约 {change:+.2f}%")
    if len(closes) >= 5:
        first = closes[0]
        change = ((closes[-1] - first) / first) * 100.0 if first else 0.0
        parts.append(f"近五日约 {change:+.2f}%")
    if isinstance(day_low, (int, float)) and isinstance(day_high, (int, float)):
        parts.append(f"日内 {day_low:.2f}-{day_high:.2f}")
    return "；".join(parts)


def yahoo_home_market_quote(symbol: str) -> tuple[float, float] | None:
    url = (
        "https://query1.finance.yahoo.com/v8/finance/chart/"
        + urllib.parse.quote(symbol, safe="")
        + "?range=1d&interval=1d"
    )
    data = http_json_get(url, timeout=MARKET_FETCH_TIMEOUT_S)
    result = (((data.get("chart") or {}).get("result") or [None])[0] or {})
    meta = result.get("meta") or {}
    quote = ((((result.get("indicators") or {}).get("quote") or [None])[0]) or {})
    closes = [v for v in quote.get("close", []) if isinstance(v, (int, float)) and v > 0]
    price = meta.get("regularMarketPrice")
    if not isinstance(price, (int, float)) and closes:
        price = closes[-1]
    previous = meta.get("chartPreviousClose")
    if (not isinstance(previous, (int, float)) or previous <= 0) and closes:
        previous = closes[0]
    if not isinstance(price, (int, float)) or price <= 0:
        return None
    change = 0.0
    if isinstance(previous, (int, float)) and previous > 0:
        change = ((float(price) - float(previous)) / float(previous)) * 100.0
    return float(price), float(change)


def format_watch_home_price(value: float) -> str:
    if value <= 0:
        return "--"
    if value >= 100000:
        return f"{value / 1000:.1f}k"
    if value >= 1000:
        return f"{value:.0f}"
    if value >= 100:
        return f"{value:.1f}"
    return f"{value:.2f}"


def format_watch_home_change(value: float) -> str:
    return f"{value:+.1f}%"


async def watch_market_full(request: web.Request) -> web.Response:
    if not auth_ok(request):
        return json_error(401, "missing or invalid bearer token", "authentication_error")

    assets: list[dict[str, str]] = []
    ok_count = 0
    for code, symbol in WATCH_HOME_MARKETS:
        try:
            quote = await asyncio.to_thread(yahoo_home_market_quote, symbol)
        except Exception as exc:
            LOG.warning("watch market quote failed %s/%s: %s", code, symbol, exc)
            quote = None
        if quote:
            price, change = quote
            ok_count += 1
            assets.append({
                "code": code,
                "price": format_watch_home_price(price),
                "change": format_watch_home_change(change),
            })
        else:
            assets.append({"code": code, "price": "--", "change": "--"})

    now = dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).strftime("%H:%M")
    return web.json_response({
        "ok": ok_count > 0,
        "status": f"GW {ok_count}",
        "market_time": now,
        "assets": assets,
    })


def yahoo_news_context(symbol: str, name: str, limit: int = 3) -> str:
    query_text = name or symbol
    query = urllib.parse.quote(query_text)
    url = f"https://query1.finance.yahoo.com/v1/finance/search?q={query}&quotesCount=1&newsCount={limit}"
    data = http_json_get(url)
    items = []
    for item in data.get("news", [])[:limit]:
        if not isinstance(item, dict):
            continue
        title = str(item.get("title") or "").strip()
        publisher = str(item.get("publisher") or "").strip()
        ts = item.get("providerPublishTime")
        when = ""
        if isinstance(ts, (int, float)):
            when = time.strftime("%m-%d %H:%M", time.localtime(ts))
        if title:
            prefix = f"{when} " if when else ""
            suffix = f" {publisher}" if publisher else ""
            items.append(prefix + title + suffix)
    if not items and name and name != symbol:
        fallback = urllib.parse.quote(symbol)
        url = f"https://query1.finance.yahoo.com/v1/finance/search?q={fallback}&quotesCount=1&newsCount={limit}"
        data = http_json_get(url)
        for item in data.get("news", [])[:limit]:
            title = str(item.get("title") or "").strip()
            if title:
                items.append(title)
    return "；".join(items)


def build_realtime_context_text(user_text: str, force: bool = False) -> str:
    lines = ["联网资料（每个信息问题都必须以这里为主要依据）："]
    time_context = network_datetime_context() or header_datetime_context()
    if time_context:
        lines.append(time_context)
    else:
        lines.append("联网校时：未能从网络时间接口取得当前时间。")

    code = market_code_from_text(user_text)
    if code:
        symbol, name = REALTIME_MARKET_SYMBOLS.get(code, ("", ""))
        if symbol:
            lines.append(f"行情对象：{code} / {name}。")
            try:
                chart = yahoo_chart_context(symbol)
                if chart:
                    lines.append("行情：" + chart + "。")
            except Exception as exc:
                LOG.warning("realtime chart fetch failed %s: %s", symbol, exc)
            try:
                news = yahoo_news_context(symbol, name)
                if news:
                    lines.append("相关新闻标题：" + news + "。")
            except Exception as exc:
                LOG.warning("realtime news fetch failed %s: %s", symbol, exc)
            if news_query_needed(user_text) or realtime_needed(user_text):
                try:
                    news_query = f"{name} {normalize_watch_text(user_text)}"
                    news = google_news_context(news_query, limit=5)
                    if news:
                        lines.append("Google 新闻标题：\n" + news)
                except Exception as exc:
                    LOG.warning("google news fetch failed for market %r: %s", user_text, exc)
            lines.append("要求：只能基于以上联网行情和新闻回答；资料不足就说没查到可靠资料。回答简短，不构成投资建议。")
            return "\n".join(lines)

    if not force and not realtime_needed(user_text):
        return ""
    if news_query_needed(user_text):
        try:
            if generic_news_query_needed(user_text):
                news = google_news_top_context(limit=6)
            else:
                news = google_news_context(google_news_search_query(user_text), limit=6)
        except Exception as exc:
            LOG.warning("google news fetch failed for %r: %s", user_text, exc)
            news = ""
        if news:
            lines.append("Google 新闻标题：\n" + news)
            lines.append("要求：新闻问题只能基于以上联网新闻标题和时间回答；没有直接相关内容就说没查到可靠新闻，不要编。")
            return "\n".join(lines)

    try:
        search = duckduckgo_lite_context(user_text)
    except Exception as exc:
        LOG.warning("web search failed for %r: %s", user_text, exc)
        search = ""
    if not search:
        lines.append("网页搜索：本次搜索没有拿到可靠摘要。")
        lines.append("要求：必须说明没查到可靠资料，不要编造具体事实。")
        return "\n".join(lines)
    lines.append("网页搜索摘要：\n" + search)
    lines.append("要求：基于以上联网资料回答；如果资料不足或不相关，必须说没查到可靠资料，不要编造。回答保持简短。")
    return "\n".join(lines)


def call_upstream_chat_text(cfg: GatewayConfig, prompt: str, upstream_api_key: str = "") -> str:
    api_key = upstream_api_key or cfg.upstream_api_key
    if not cfg.upstream_api_url or not api_key:
        raise RuntimeError("upstream API URL/key are not configured")
    realtime_context = build_realtime_context_text(prompt, force=True)
    user_content = prompt if not realtime_context else f"{realtime_context}\n用户问题：{prompt}"
    payload = {
        "model": cfg.upstream_model,
        "stream": False,
        "messages": [
            {
                "role": "system",
                "content": (
                    "你是手表里的中文助手，回答要短、直接、自然。通常不超过三句话，"
                    "只用常用简体中文，不用 emoji、繁体字或生僻字。"
                    "所有事实、新闻、日期、行情和知识性内容必须以用户消息里的联网资料为准；"
                    "联网资料不足、失败或不相关时，必须直接说没查到可靠资料，不要凭记忆编造。"
                ),
            },
            {"role": "user", "content": user_content},
        ],
        "temperature": 0.2,
        "max_tokens": 80,
    }
    req = urllib.request.Request(
        upstream_chat_url(cfg),
        data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
            "User-Agent": "WatchVoiceGateway/1.0",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=cfg.request_timeout_s) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    return sanitize_watch_reply(data["choices"][0]["message"]["content"].strip())


def fix_wav_riff_sizes(wav: bytes) -> bytes:
    """Normalize streaming WAV headers that use 0xffffffff placeholders."""
    if len(wav) < 44 or wav[:4] != b"RIFF" or wav[8:12] != b"WAVE":
        return wav
    out = bytearray(wav)
    riff_size = len(out) - 8
    if struct.unpack_from("<I", out, 4)[0] in (0xFFFFFFFF, 0, riff_size):
        struct.pack_into("<I", out, 4, riff_size)
    offset = 12
    while offset + 8 <= len(out):
        chunk_id = bytes(out[offset:offset + 4])
        chunk_size = struct.unpack_from("<I", out, offset + 4)[0]
        chunk_data_start = offset + 8
        if chunk_id == b"data":
            actual_size = max(0, len(out) - chunk_data_start)
            if chunk_size in (0xFFFFFFFF, 0) or chunk_data_start + chunk_size > len(out):
                struct.pack_into("<I", out, offset + 4, actual_size)
            break
        if chunk_size == 0xFFFFFFFF or chunk_size == 0:
            break
        offset = chunk_data_start + chunk_size + (chunk_size & 1)
    return bytes(out)


def upstream_error_message(service: str, status: int, body_text: str) -> str:
    message = body_text.strip()
    try:
        data = json.loads(body_text)
        error = data.get("error") if isinstance(data, dict) else None
        if isinstance(error, dict):
            message = str(error.get("message_cn") or error.get("message") or message)
        elif isinstance(error, str):
            message = error
    except json.JSONDecodeError:
        pass

    compact = "".join(message.split())
    if status == 401 or "InvalidAPIKey" in compact or "无效的APIKEY" in compact:
        return f"{service}: 302 API KEY 无效"
    if status == 429 or "quota" in compact.lower() or "余额" in compact or "额度" in compact:
        return f"{service}: 302 额度或频率限制"
    return f"{service}: 302 HTTP {status} {message[:120]}"


def transcribe_audio_upstream(audio_data: bytes, suffix: str, language: str,
                              initial_prompt: str, cfg: GatewayConfig,
                              upstream_api_key: str = "") -> str:
    api_key = upstream_api_key or cfg.upstream_api_key
    if not cfg.upstream_api_url or not api_key:
        raise RuntimeError("upstream STT API URL/key are not configured")

    boundary = f"----watchstt{int(time.time() * 1000)}"
    filename = f"voice{suffix or '.wav'}"
    fields = {
        "model": cfg.stt_model,
        "language": language or "zh",
        "prompt": initial_prompt or "中文普通话语音转写，只输出用户说的话，不要翻译。",
    }
    chunks: list[bytes] = []
    for key, value in fields.items():
        chunks.append(
            f"--{boundary}\r\n"
            f"Content-Disposition: form-data; name=\"{key}\"\r\n\r\n"
            f"{value}\r\n".encode("utf-8")
        )
    chunks.append(
        f"--{boundary}\r\n"
        f"Content-Disposition: form-data; name=\"file\"; filename=\"{filename}\"\r\n"
        "Content-Type: audio/wav\r\n\r\n".encode("utf-8")
        + audio_data
        + b"\r\n"
    )
    chunks.append(f"--{boundary}--\r\n".encode("utf-8"))
    body = b"".join(chunks)

    req = urllib.request.Request(
        upstream_endpoint_url(cfg, "audio/transcriptions"),
        data=body,
        headers={
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "Authorization": f"Bearer {api_key}",
            "User-Agent": "WatchVoiceGateway/1.0",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=cfg.request_timeout_s) as resp:
            data = json.loads(resp.read().decode("utf-8", errors="replace"))
    except urllib.error.HTTPError as exc:
        body_text = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(upstream_error_message("STT", exc.code, body_text)) from exc

    text = str(data.get("text") or data.get("transcript") or "").strip()
    if not text and isinstance(data.get("data"), dict):
        text = str(data["data"].get("text") or "").strip()
    for src, dst in TEXT_CORRECTIONS:
        text = text.replace(src, dst)
    return text


def synthesize_wav_upstream_sync(text: str, voice: str, speed: Any,
                                 cfg: GatewayConfig, upstream_api_key: str = "") -> bytes:
    api_key = upstream_api_key or cfg.upstream_api_key
    if not cfg.upstream_api_url or not api_key:
        raise RuntimeError("upstream TTS API URL/key are not configured")
    payload = {
        "model": cfg.tts_model,
        "input": sanitize_watch_reply(text, max_chars=90),
        "voice": normalize_voice(voice, cfg.default_voice),
        "response_format": "wav",
        "speed": speed if speed is not None else 1.0,
    }
    req = urllib.request.Request(
        upstream_endpoint_url(cfg, "audio/speech"),
        data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
            "User-Agent": "WatchVoiceGateway/1.0",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=cfg.request_timeout_s) as resp:
            wav = resp.read()
    except urllib.error.HTTPError as exc:
        body_text = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(upstream_error_message("TTS", exc.code, body_text)) from exc
    return fix_wav_riff_sizes(wav)


def sanitize_chat_response_body(body: str) -> str:
    try:
        data = json.loads(body)
    except json.JSONDecodeError:
        return body
    if not isinstance(data, dict):
        return body

    changed = False
    choices = data.get("choices")
    if isinstance(choices, list):
        for choice in choices:
            if not isinstance(choice, dict):
                continue
            for key in ("message", "delta"):
                message = choice.get(key)
                if isinstance(message, dict) and isinstance(message.get("content"), str):
                    message["content"] = sanitize_watch_reply(message["content"])
                    changed = True
    if isinstance(data.get("output_text"), str):
        data["output_text"] = sanitize_watch_reply(data["output_text"])
        changed = True

    return json.dumps(data, ensure_ascii=False) if changed else body


def last_user_message_text(payload: dict[str, Any]) -> str:
    messages = payload.get("messages")
    if not isinstance(messages, list):
        return ""
    for message in reversed(messages):
        if not isinstance(message, dict):
            continue
        if str(message.get("role") or "").lower() != "user":
            continue
        content = message.get("content")
        if isinstance(content, str):
            return content
        if isinstance(content, list):
            parts: list[str] = []
            for item in content:
                if isinstance(item, dict) and item.get("type") in {"text", "input_text"}:
                    text = item.get("text")
                    if isinstance(text, str):
                        parts.append(text)
            return "\n".join(parts).strip()
    return ""


def inject_realtime_context_into_payload(payload: dict[str, Any]) -> dict[str, Any]:
    prompt = last_user_message_text(payload)
    if not prompt:
        return payload
    realtime_context = build_realtime_context_text(prompt, force=True)
    if not realtime_context:
        return payload

    updated = dict(payload)
    messages = updated.get("messages")
    if not isinstance(messages, list):
        return payload
    new_messages: list[Any] = []
    system_inserted = False
    for message in messages:
        if not isinstance(message, dict):
            new_messages.append(message)
            continue
        role = str(message.get("role") or "").lower()
        if role == "system":
            patched = dict(message)
            content = str(patched.get("content") or "")
            rule = (
                "所有事实、新闻、日期、行情和知识性内容必须以联网资料为准；"
                "资料不足、失败或不相关时，必须说没查到可靠资料，不要编造。"
            )
            patched["content"] = (content + "\n" + rule).strip()
            new_messages.append(patched)
            system_inserted = True
        else:
            new_messages.append(message)
    if not system_inserted:
        new_messages.insert(
            0,
            {
                "role": "system",
                "content": (
                    "你是手表里的中文助手，回答简短。所有事实、新闻、日期、行情和知识性内容"
                    "必须以联网资料为准；资料不足、失败或不相关时，必须说没查到可靠资料，不要编造。"
                ),
            },
        )

    for idx in range(len(new_messages) - 1, -1, -1):
        message = new_messages[idx]
        if not isinstance(message, dict):
            continue
        if str(message.get("role") or "").lower() != "user":
            continue
        patched = dict(message)
        patched["content"] = f"{realtime_context}\n用户问题：{prompt}"
        new_messages[idx] = patched
        break

    updated["messages"] = new_messages
    updated["stream"] = False
    try:
        temperature = float(updated.get("temperature", 0.2) or 0.2)
    except (TypeError, ValueError):
        temperature = 0.2
    updated["temperature"] = min(temperature, 0.3)
    return updated


def parse_watch_context(raw: str) -> dict[str, Any]:
    if not raw:
        return {}
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        return {}
    return data if isinstance(data, dict) else {}


def find_market_snapshot(context: dict[str, Any], code: str) -> dict[str, Any] | None:
    markets = context.get("markets")
    if not isinstance(markets, list):
        return None
    for item in markets:
        if not isinstance(item, dict):
            continue
        if str(item.get("code", "")).upper() == code:
            return item
    return None


def market_code_from_text(text: str) -> str:
    normalized = normalize_watch_text(text).lower().replace(" ", "")
    if not normalized:
        return ""
    for code, aliases in MARKET_ALIASES:
        for alias in aliases:
            if str(alias).lower().replace(" ", "") in normalized:
                return code
    return ""


def normalize_watch_text(text: str) -> str:
    normalized = str(text or "")
    for wrong, right in TEXT_CORRECTIONS:
        normalized = normalized.replace(wrong, right)
    return normalized


def watch_action(action: str = "", asset: str = "", brightness_delta: int = 0) -> dict[str, Any]:
    return {
        "action": action,
        "asset": asset,
        "brightness_delta": brightness_delta,
    }


def current_datetime_cn() -> str:
    now = time.localtime()
    weekdays = ("一", "二", "三", "四", "五", "六", "日")
    return (
        f"{now.tm_year}年{now.tm_mon}月{now.tm_mday}日，"
        f"星期{weekdays[now.tm_wday]}，北京时间 {now.tm_hour:02d}:{now.tm_min:02d}"
    )


def build_watch_local_reply(text: str, context: dict[str, Any]) -> tuple[str, bool, bool, dict[str, Any]]:
    normalized = normalize_watch_text(text).replace(" ", "")
    lower = normalized.lower()
    short_command = normalized.strip("，。！？、,.!?;；:")
    no_action = watch_action()
    if not normalized:
        return "", False, False, no_action

    # Whisper sometimes hears the short command "返回" as "翻译".
    if short_command in {"翻译", "反回", "返会", "返 回", "返回来", "回来"}:
        return "已回到首页。", True, False, watch_action("home")

    if any(word in normalized for word in ("回首页", "回到首页", "主页", "主页面", "首页", "返回", "退出", "回去")):
        return "已回到首页。", True, False, watch_action("home")

    if any(word in normalized for word in ("帮助", "功能", "能干什么", "可以做什么", "你会什么", "怎么用", "说明")):
        return "我能打开图表、查价格、刷新行情、调亮调暗、回首页、打开聊天。", True, False, no_action

    if short_command in {"你好", "您好", "在吗", "喂", "小助手"}:
        return "我在，请直接说。", True, False, no_action

    if "测试" in normalized and any(word in normalized for word in ("语音", "声音", "麦克风", "喇叭")):
        return "语音正常。", True, False, no_action

    if any(word in normalized for word in ("息屏", "关屏", "关闭屏幕", "睡眠", "休眠")):
        return "已进入睡眠。", True, False, watch_action("sleep")

    if (
        "聊天" in normalized
        or "对话" in normalized
        or "AI" in normalized
        or "ai" in lower
        or "人工智能" in normalized
        or "助手" in normalized
    ):
        if any(word in normalized for word in ("打开", "进入", "切到", "去", "显示", "开始")):
            return "已打开聊天。", True, False, watch_action("ai")

    if any(word in normalized for word in ("调亮", "亮一点", "太暗", "更亮", "亮度高", "亮屏", "屏幕亮")):
        return "已调亮屏幕。", True, False, watch_action("brightness", brightness_delta=10)

    if any(word in normalized for word in ("调暗", "暗一点", "太亮", "更暗", "亮度低")):
        return "已调暗屏幕。", True, False, watch_action("brightness", brightness_delta=-10)

    if "刷新" in normalized and any(word in normalized for word in ("行情", "市场", "价格")):
        return "正在刷新行情，请稍等。", True, True, watch_action("refresh_market")

    if any(word in normalized for word in (
        "今天几号", "今天日期", "今天星期", "星期几", "礼拜几", "现在日期",
        "当前日期", "现在几号", "几月几号", "今天是几号", "今天是什么日子",
        "日期时间",
    )):
        return "今天是" + current_datetime_cn() + "。", True, False, no_action

    if "几点" in normalized or "时间" in normalized:
        now = time.localtime()
        return f"现在 {now.tm_hour:02d}:{now.tm_min:02d}。", True, False, no_action

    if "电量" in normalized or "电池" in normalized:
        battery = context.get("battery_percent")
        try:
            percent = int(battery)
        except (TypeError, ValueError):
            percent = -1
        if 0 <= percent <= 100:
            return f"电量 {percent}%。", True, False, no_action
        return "暂时读不到电量。", True, False, no_action

    code = market_code_from_text(lower)
    if code:
        wants_analysis = any(word in normalized for word in (
            "为什么", "为啥", "原因", "怎么回事", "怎么看", "分析", "解释",
            "大跌", "大涨", "暴跌", "暴涨", "后面", "未来", "预测", "会不会",
        ))
        wants_chart = any(word in normalized for word in ("图表", "走势图", "走势", "K线", "k线", "曲线"))
        wants_quote = any(word in normalized for word in ("价格", "多少钱", "多少", "报价", "涨跌"))
        if wants_analysis and not wants_chart and not wants_quote:
            return "", False, False, no_action
        if not wants_chart and not wants_quote:
            wants_chart = any(word in normalized for word in ("看", "打开", "显示", "进入", "切到"))
        if wants_chart:
            if "刷新" in normalized:
                return f"正在刷新 {code} 图表。", True, False, watch_action("chart_refresh", asset=code)
            return f"已打开 {code} 图表。", True, False, watch_action("chart", asset=code)
        item = find_market_snapshot(context, code)
        market_time = str(context.get("market_time") or "--:--")
        price = str(item.get("price", "--")) if item else "--"
        change = str(item.get("change", "--")) if item else "--"
        if price and price != "--":
            return f"{code} 现在 {price}，涨跌 {change or '--'}，更新时间 {market_time}。", True, False, no_action
        return f"{code} 暂无行情，我先刷新。", True, True, watch_action("refresh_market")

    return "", False, False, no_action


async def chat_completions(request: web.Request) -> web.Response:
    cfg: GatewayConfig = request.app["cfg"]
    if not auth_ok(request):
        return json_error(401, "missing or invalid bearer token", "authentication_error")
    upstream_api_key = request_or_config_api_key(cfg, request)
    if not cfg.upstream_api_url or not upstream_api_key:
        return json_error(400, "upstream API URL/key are not configured")

    try:
        payload = await request.json()
    except Exception:
        return json_error(400, "request body must be JSON")
    if not isinstance(payload, dict):
        return json_error(400, "request body must be a JSON object")
    if not payload.get("model"):
        payload["model"] = cfg.upstream_model
    try:
        payload = inject_realtime_context_into_payload(payload)
    except Exception as exc:
        LOG.warning("chat realtime context injection failed: %s", exc)

    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        upstream_chat_url(cfg),
        data=data,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {upstream_api_key}",
            "User-Agent": "WatchVoiceGateway/1.0",
        },
        method="POST",
    )

    def run_request() -> tuple[int, str, str]:
        try:
            with urllib.request.urlopen(req, timeout=cfg.request_timeout_s) as resp:
                body = resp.read().decode("utf-8", errors="replace")
                return resp.status, resp.headers.get("content-type", "application/json"), body
        except urllib.error.HTTPError as exc:
            body = exc.read().decode("utf-8", errors="replace")
            return exc.code, exc.headers.get("content-type", "application/json"), body

    status, content_type, body = await asyncio.to_thread(run_request)
    if "json" in content_type.lower():
        body = sanitize_chat_response_body(body)
    return web.Response(status=status, text=body, content_type=content_type.split(";")[0])


def normalize_voice(voice: str, default_voice: str) -> str:
    voice = (voice or "").strip()
    if not voice or voice.lower() in OPENAI_GENERIC_VOICES:
        return default_voice
    return voice


def speed_to_edge_rate(value: Any) -> str:
    try:
        speed = float(value)
    except (TypeError, ValueError):
        speed = 1.0
    if not math.isfinite(speed):
        speed = 1.0
    speed = max(0.5, min(2.0, speed))
    percent = int(round((speed - 1.0) * 100.0))
    return f"+{percent}%" if percent >= 0 else f"{percent}%"


def env_bool(name: str, default: bool) -> bool:
    raw = os.getenv(name)
    if raw is None or raw.strip() == "":
        return default
    return raw.strip().lower() not in {"0", "false", "no", "off"}


def env_text(name: str, default: str) -> str:
    raw = os.getenv(name)
    if raw is None or raw == "":
        return default
    return raw


def env_int(name: str, default: int) -> int:
    raw = os.getenv(name)
    if raw is None or raw.strip() == "":
        return default
    return int(raw)


def mp3_to_wav_bytes(mp3_path: Path, sample_rate: int) -> bytes:
    try:
        import miniaudio
    except ImportError as exc:
        raise RuntimeError("local TTS requires miniaudio; use WATCH_VOICE_TTS_PROVIDER=upstream or install miniaudio") from exc

    decoded = miniaudio.decode_file(
        str(mp3_path),
        output_format=miniaudio.SampleFormat.SIGNED16,
        nchannels=1,
        sample_rate=sample_rate,
    )
    pcm = decoded.samples.tobytes()
    with io.BytesIO() as out:
        with wave.open(out, "wb") as wav:
            wav.setnchannels(decoded.nchannels)
            wav.setsampwidth(2)
            wav.setframerate(decoded.sample_rate)
            wav.writeframes(pcm)
        return out.getvalue()


async def synthesize_wav(text: str, voice: str, speed: Any, cfg: GatewayConfig,
                         upstream_api_key: str = "") -> bytes:
    text = sanitize_watch_reply(text, max_chars=90)
    voice = normalize_voice(voice, cfg.default_voice)
    if cfg.tts_provider == "upstream":
        return await asyncio.to_thread(
            synthesize_wav_upstream_sync,
            text,
            voice,
            speed,
            cfg,
            upstream_api_key,
        )

    rate = speed_to_edge_rate(speed)
    cache_key = (str(text), voice, rate, cfg.tts_sample_rate)
    cached = TTS_CACHE.get(cache_key)
    if cached is not None:
        return cached
    try:
        import edge_tts
    except ImportError as exc:
        raise RuntimeError("local TTS requires edge-tts; use WATCH_VOICE_TTS_PROVIDER=upstream or install edge-tts") from exc

    with tempfile.TemporaryDirectory(prefix="watch-tts-") as tmpdir:
        mp3_path = Path(tmpdir) / "speech.mp3"
        await edge_tts.Communicate(text, voice=voice, rate=rate).save(str(mp3_path))
        wav = mp3_to_wav_bytes(mp3_path, cfg.tts_sample_rate)
    if len(TTS_CACHE) >= TTS_CACHE_MAX_ITEMS:
        TTS_CACHE.pop(next(iter(TTS_CACHE)))
    TTS_CACHE[cache_key] = wav
    return wav


async def audio_speech(request: web.Request) -> web.Response:
    cfg: GatewayConfig = request.app["cfg"]
    if not auth_ok(request):
        return json_error(401, "missing or invalid bearer token", "authentication_error")
    try:
        body = await request.json()
    except Exception:
        return json_error(400, "request body must be JSON")
    if not isinstance(body, dict):
        return json_error(400, "request body must be a JSON object")
    text = sanitize_watch_reply(str(body.get("input", "")).strip(), max_chars=90)
    if not text:
        return json_error(400, "`input` is required")
    response_format = str(body.get("response_format", "wav")).lower()
    if response_format != "wav":
        return json_error(400, "only wav response_format is supported")
    upstream_api_key = request_or_config_api_key(cfg, request)
    try:
        wav = await asyncio.wait_for(
            synthesize_wav(text, str(body.get("voice", "")), body.get("speed", 1.0), cfg, upstream_api_key),
            timeout=cfg.request_timeout_s,
        )
    except asyncio.TimeoutError:
        return json_error(504, "TTS synthesis timed out", "timeout")
    except NoAudioReceived as exc:
        return json_error(502, str(exc), "upstream_error")
    except Exception as exc:
        LOG.exception("TTS failed")
        return json_error(500, str(exc), "server_error")
    return web.Response(body=wav, content_type="audio/wav")


def get_whisper(cfg: GatewayConfig) -> Any:
    global WHISPER
    with WHISPER_LOCK:
        if WHISPER is None:
            try:
                from faster_whisper import WhisperModel
            except ImportError as exc:
                raise RuntimeError("local STT requires faster-whisper; use WATCH_VOICE_STT_PROVIDER=upstream or install faster-whisper") from exc

            LOG.info("Loading Whisper model %s", cfg.whisper_model)
            WHISPER = WhisperModel(
                cfg.whisper_model,
                device=cfg.whisper_device,
                compute_type=cfg.whisper_compute_type,
            )
    return WHISPER


def transcribe_audio_bytes(audio_data: bytes, suffix: str, language: str,
                           initial_prompt: str, cfg: GatewayConfig) -> str:
    with tempfile.TemporaryDirectory(prefix="watch-stt-") as tmpdir:
        audio_path = Path(tmpdir) / f"voice{suffix}"
        audio_path.write_bytes(audio_data)
        model = get_whisper(cfg)
        prompt = STT_INITIAL_PROMPT
        if initial_prompt and initial_prompt not in prompt:
            prompt = f"{initial_prompt} {prompt}"
        segments, _info = model.transcribe(
            str(audio_path),
            language=language,
            beam_size=1,
            best_of=1,
            temperature=0.0,
            condition_on_previous_text=False,
            initial_prompt=prompt,
            hotwords=STT_HOTWORDS,
            vad_filter=False,
        )
        text = "".join(seg.text for seg in segments).strip()
        return text.replace("手标", "手表")


async def transcribe_audio(audio_data: bytes, suffix: str, language: str,
                           initial_prompt: str, cfg: GatewayConfig,
                           upstream_api_key: str = "") -> str:
    if cfg.stt_provider == "upstream":
        return await asyncio.to_thread(
            transcribe_audio_upstream,
            audio_data,
            suffix,
            language,
            initial_prompt,
            cfg,
            upstream_api_key,
        )
    return await asyncio.to_thread(transcribe_audio_bytes, audio_data, suffix, language, initial_prompt, cfg)


async def audio_transcriptions(request: web.Request) -> web.Response:
    cfg: GatewayConfig = request.app["cfg"]
    if not auth_ok(request):
        return json_error(401, "missing or invalid bearer token", "authentication_error")

    reader = await request.multipart()
    audio_data: bytes | None = None
    suffix = ".wav"
    language = "zh"
    initial_prompt = "中文普通话语音转写，只输出用户说的话，不要翻译。"
    async for part in reader:
        if part.name == "file":
            audio_data = await part.read(decode=False)
            _, params = parse_header_params(part.headers.get("content-disposition", ""))
            filename = params.get("filename", "voice.wav")
            suffix = Path(filename).suffix or ".wav"
        elif part.name == "language":
            language = (await part.text()).strip() or "zh"
        elif part.name in {"prompt", "initial_prompt"}:
            initial_prompt = (await part.text()).strip() or initial_prompt
        else:
            await part.release()

    if not audio_data:
        return json_error(400, "`file` is required")

    save_last_voice_debug(audio_data, suffix, "audio_transcriptions")
    silent, silence_error = audio_looks_silent(audio_data)
    if silent:
        save_last_voice_debug(audio_data, suffix, "audio_transcriptions", error=silence_error)
        return json_error(422, "没有识别到有效语音")
    try:
        upstream_api_key = request_or_config_api_key(cfg, request)
        text = await asyncio.wait_for(
            transcribe_audio(audio_data, suffix, language, initial_prompt, cfg, upstream_api_key),
            timeout=max(120, cfg.request_timeout_s),
        )
        if stt_text_is_prompt_hallucination(text):
            save_last_voice_debug(audio_data, suffix, "audio_transcriptions", text=text, error="疑似空音频幻听")
            return json_error(422, "没有识别到有效语音")
        save_last_voice_debug(audio_data, suffix, "audio_transcriptions", text=text)
    except asyncio.TimeoutError:
        save_last_voice_debug(audio_data, suffix, "audio_transcriptions", error="STT transcription timed out")
        return json_error(504, "STT transcription timed out", "timeout")
    except Exception as exc:
        save_last_voice_debug(audio_data, suffix, "audio_transcriptions", error=str(exc))
        LOG.exception("STT failed")
        return json_error(500, str(exc), "server_error")
    return web.json_response({"text": text})


async def watch_voice(request: web.Request) -> web.Response:
    cfg: GatewayConfig = request.app["cfg"]
    if not auth_ok(request):
        return json_error(401, "missing or invalid bearer token", "authentication_error")

    content_type = request.headers.get("content-type", "").lower()
    if "multipart/form-data" in content_type:
        reader = await request.multipart()
        audio_data: bytes | None = None
        suffix = ".wav"
        language = "zh"
        initial_prompt = "中文普通话语音转写，只输出用户说的话，不要翻译。"
        context_raw = ""
        async for part in reader:
            if part.name == "file":
                audio_data = await part.read(decode=False)
                _, params = parse_header_params(part.headers.get("content-disposition", ""))
                suffix = Path(params.get("filename", "voice.wav")).suffix or ".wav"
            elif part.name == "language":
                language = (await part.text()).strip() or "zh"
            elif part.name in {"prompt", "initial_prompt"}:
                initial_prompt = (await part.text()).strip() or initial_prompt
            elif part.name == "context":
                context_raw = (await part.text()).strip()
            else:
                await part.release()
    else:
        audio_data = await request.read()
        suffix = ".wav"
        language = request.query.get("language", "zh")
        initial_prompt = request.query.get(
            "prompt",
            "中文普通话语音转写，只输出用户说的话，不要翻译。",
        )
        context_raw = request.query.get("context", "")

    if not audio_data:
        return json_error(400, "audio body is required")

    save_last_voice_debug(audio_data, suffix, "watch_voice")
    silent, silence_error = audio_looks_silent(audio_data)
    if silent:
        save_last_voice_debug(audio_data, suffix, "watch_voice", error=silence_error)
        return json_error(422, "没有识别到有效语音")
    total_start = time.monotonic()
    try:
        upstream_api_key = request_or_config_api_key(cfg, request)
        stt_start = time.monotonic()
        text = await asyncio.wait_for(
            transcribe_audio(audio_data, suffix, language, initial_prompt, cfg, upstream_api_key),
            timeout=max(120, cfg.request_timeout_s),
        )
        stt_s = time.monotonic() - stt_start
        if stt_text_is_prompt_hallucination(text):
            save_last_voice_debug(audio_data, suffix, "watch_voice", text=text, error="疑似空音频幻听")
            return json_error(422, "没有识别到有效语音")
        save_last_voice_debug(audio_data, suffix, "watch_voice", text=text)
        chat_start = time.monotonic()
        reply = await asyncio.wait_for(
            asyncio.to_thread(call_upstream_chat_text, cfg, text, upstream_api_key),
            timeout=cfg.request_timeout_s,
        )
        chat_s = time.monotonic() - chat_start
        local = False
        refresh_market = False
        action = watch_action()
        reply = sanitize_watch_reply(reply, max_chars=90)
    except asyncio.TimeoutError:
        save_last_voice_debug(audio_data, suffix, "watch_voice", error="voice request timed out")
        return json_error(504, "voice request timed out", "timeout")
    except Exception as exc:
        save_last_voice_debug(audio_data, suffix, "watch_voice", error=str(exc))
        LOG.exception("watch voice request failed")
        return json_error(500, str(exc), "server_error")

    audio_id = str(int(time.time() * 1000))
    tts_task = asyncio.create_task(synthesize_wav(reply, cfg.default_voice, 1.0, cfg, upstream_api_key))
    VOICE_AUDIO.clear()
    VOICE_AUDIO_TASKS.clear()
    VOICE_AUDIO_TASKS[audio_id] = tts_task
    LOG.info(
        "watch voice text ready total=%.3fs stt=%.3fs chat=%.3fs local=%s chars=%d reply=%d audio=%s",
        time.monotonic() - total_start,
        stt_s,
        chat_s,
        local,
        len(text),
        len(reply),
        audio_id,
    )
    return web.json_response({
        "text": text,
        "reply": reply,
        "audio_url": f"/api/watch/voice/audio/{audio_id}.wav",
        "local": local,
        "refresh_market": refresh_market,
        "action": action.get("action", ""),
        "asset": action.get("asset", ""),
        "brightness_delta": action.get("brightness_delta", 0),
    })


async def watch_voice_audio(request: web.Request) -> web.Response:
    cfg: GatewayConfig = request.app["cfg"]
    audio_id = request.match_info.get("audio_id", "")
    wav = VOICE_AUDIO.get(audio_id)
    task = VOICE_AUDIO_TASKS.get(audio_id)
    if wav is None and task is not None:
        tts_start = time.monotonic()
        try:
            wav = await asyncio.wait_for(task, timeout=cfg.request_timeout_s)
        except asyncio.TimeoutError:
            return json_error(504, "TTS synthesis timed out", "timeout")
        except Exception as exc:
            LOG.exception("deferred TTS failed")
            return json_error(500, str(exc), "server_error")
        VOICE_AUDIO[audio_id] = wav
        VOICE_AUDIO_TASKS.pop(audio_id, None)
        LOG.info("watch voice audio ready audio=%s tts=%.3fs bytes=%d",
                 audio_id, time.monotonic() - tts_start, len(wav))
    if wav is None:
        return json_error(404, "audio not found")
    return web.Response(body=wav, content_type="audio/wav")


async def watch_voice_last_audio(request: web.Request) -> web.Response:
    if not LAST_VOICE_PATH.exists():
        return json_error(404, "last voice audio not found")
    return web.Response(body=LAST_VOICE_PATH.read_bytes(), content_type="audio/wav")


async def watch_voice_last_info(request: web.Request) -> web.Response:
    if LAST_VOICE_INFO:
        return web.json_response(LAST_VOICE_INFO)
    if LAST_VOICE_INFO_PATH.exists():
        try:
            return web.json_response(json.loads(LAST_VOICE_INFO_PATH.read_text(encoding="utf-8")))
        except (OSError, json.JSONDecodeError):
            pass
    return web.json_response({"ok": False, "error": "last voice info not found"}, status=404)


async def health(request: web.Request) -> web.Response:
    cfg: GatewayConfig = request.app["cfg"]
    return web.json_response({
        "ok": True,
        "service": "watch-voice-gateway",
        "upstream": normalize_base_url(cfg.upstream_api_url),
        "model": cfg.upstream_model,
        "voice": cfg.default_voice,
        "stt_provider": cfg.stt_provider,
        "stt_model": cfg.stt_model,
        "tts_provider": cfg.tts_provider,
        "tts_model": cfg.tts_model,
        "whisper_model": cfg.whisper_model,
        "whisper_loaded": WHISPER is not None,
        "tts_cache_items": len(TTS_CACHE),
        "last_voice": "/api/watch/voice/last",
    })


def build_app(cfg: GatewayConfig) -> web.Application:
    app = web.Application(client_max_size=8 * 1024 * 1024)
    app["cfg"] = cfg
    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)
    app.router.add_get("/", health)
    app.router.add_post("/", chat_completions)
    app.router.add_get("/health", health)
    app.router.add_post("/v1/chat/completions", chat_completions)
    app.router.add_post("/v1/audio/speech", audio_speech)
    app.router.add_post("/v1/audio/transcriptions", audio_transcriptions)
    app.router.add_get("/api/watch/market/full", watch_market_full)
    app.router.add_post("/api/watch/voice", watch_voice)
    app.router.add_get("/api/watch/voice/last", watch_voice_last_info)
    app.router.add_get("/api/watch/voice/last.wav", watch_voice_last_audio)
    app.router.add_get("/api/watch/voice/audio/{audio_id}.wav", watch_voice_audio)
    return app


async def preload_runtime(cfg: GatewayConfig) -> None:
    if cfg.preload_whisper and cfg.stt_provider == "local":
        try:
            await asyncio.to_thread(get_whisper, cfg)
        except Exception:
            LOG.exception("Whisper preload failed")
    if cfg.preload_tts and cfg.tts_provider == "local":
        for text in COMMON_TTS_PRELOAD_TEXTS:
            try:
                await synthesize_wav(text, cfg.default_voice, 1.0, cfg)
            except Exception:
                LOG.exception("TTS preload failed for %r", text)


async def on_startup(app: web.Application) -> None:
    cfg: GatewayConfig = app["cfg"]
    app["preload_task"] = asyncio.create_task(preload_runtime(cfg))


async def on_cleanup(app: web.Application) -> None:
    task = app.get("preload_task")
    if task:
        task.cancel()


def parse_args() -> argparse.Namespace:
    saved = load_saved_config()
    parser = argparse.ArgumentParser(description="Watch local voice gateway")
    parser.add_argument("--host", default=env_text("WATCH_VOICE_HOST", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=env_int("WATCH_VOICE_PORT", 8790))
    parser.add_argument("--token", default=env_text("WATCH_VOICE_TOKEN", ""))
    parser.add_argument("--upstream-api-url", default=env_text("WATCH_GATEWAY_API_URL", saved.get("api_url", DEFAULT_UPSTREAM_API_URL)))
    parser.add_argument("--upstream-api-key", default=env_text("WATCH_GATEWAY_API_KEY", saved.get("api_key", "")))
    parser.add_argument("--upstream-model", default=env_text("WATCH_GATEWAY_MODEL", saved.get("model", "gpt-5.4")))
    parser.add_argument("--voice", default=env_text("WATCH_VOICE_TTS_VOICE", saved.get("tts_voice", "zh-CN-XiaoxiaoNeural")))
    parser.add_argument("--tts-sample-rate", type=int, default=env_int("WATCH_VOICE_TTS_SAMPLE_RATE", 16000))
    parser.add_argument("--whisper-model", default=env_text("WATCH_VOICE_WHISPER_MODEL", "tiny"))
    parser.add_argument("--whisper-device", default=env_text("WATCH_VOICE_WHISPER_DEVICE", "cpu"))
    parser.add_argument("--whisper-compute-type", default=env_text("WATCH_VOICE_WHISPER_COMPUTE", "int8"))
    parser.add_argument("--stt-provider", choices=("local", "upstream"),
                        default=env_text("WATCH_VOICE_STT_PROVIDER", "local"))
    parser.add_argument("--stt-model", default=env_text("WATCH_VOICE_STT_MODEL", "whisper-1"))
    parser.add_argument("--tts-provider", choices=("local", "upstream"),
                        default=env_text("WATCH_VOICE_TTS_PROVIDER", "local"))
    parser.add_argument("--tts-model", default=env_text("WATCH_VOICE_TTS_MODEL", "tts-1"))
    parser.add_argument("--timeout", type=int, default=env_int("WATCH_VOICE_TIMEOUT", 90))
    parser.add_argument("--preload-whisper", action=argparse.BooleanOptionalAction,
                        default=env_bool("WATCH_VOICE_PRELOAD_WHISPER", True))
    parser.add_argument("--preload-tts", action=argparse.BooleanOptionalAction,
                        default=env_bool("WATCH_VOICE_PRELOAD_TTS", True))
    parser.add_argument("--log-level", default=os.getenv("WATCH_VOICE_LOG_LEVEL", "INFO"))
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    logging.basicConfig(
        level=getattr(logging, str(args.log_level).upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    cfg = GatewayConfig(
        host=args.host,
        port=args.port,
        token=args.token,
        upstream_api_url=args.upstream_api_url,
        upstream_api_key=args.upstream_api_key,
        upstream_model=args.upstream_model,
        default_voice=args.voice,
        tts_sample_rate=args.tts_sample_rate,
        whisper_model=args.whisper_model,
        whisper_device=args.whisper_device,
        whisper_compute_type=args.whisper_compute_type,
        stt_provider=args.stt_provider,
        stt_model=args.stt_model,
        tts_provider=args.tts_provider,
        tts_model=args.tts_model,
        request_timeout_s=args.timeout,
        preload_whisper=args.preload_whisper,
        preload_tts=args.preload_tts,
    )
    LOG.info("Listening on http://%s:%d/v1", cfg.host, cfg.port)
    LOG.info("Chat upstream: %s model=%s", normalize_base_url(cfg.upstream_api_url), cfg.upstream_model)
    LOG.info("Voice providers: stt=%s/%s tts=%s/%s",
             cfg.stt_provider, cfg.stt_model, cfg.tts_provider, cfg.tts_model)
    web.run_app(build_app(cfg), host=cfg.host, port=cfg.port)


if __name__ == "__main__":
    main()
