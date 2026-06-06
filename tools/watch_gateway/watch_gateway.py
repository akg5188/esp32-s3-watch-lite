#!/usr/bin/env python3
import argparse
import html
import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


ROOT = Path(__file__).resolve().parent
CONFIG_PATH = ROOT / "watch_gateway_config.json"

DEFAULT_CONFIG = {
    "market_provider": "auto",
    "api_url": "",
    "api_key": "",
    "model": "gpt-5.4",
    "system_prompt": "You are a concise Chinese AI assistant on a tiny watch.",
    "starter_prompt": "用中文简短回复：手表 AI 聊天已经启动，请问我可以帮你什么？",
    "tts_url": "",
    "tts_key": "",
    "tts_voice": "zh-CN-XiaoxiaoNeural",
}

STATE = {
    "state": "Ready",
    "hint": "Tap AI on the watch to start.",
    "reply": "Gateway is running.",
    "updated": int(time.time()),
}

MARKET_CACHE = {
    "data": None,
    "updated": 0,
}

MARKET_CACHE_TTL_S = 60


def load_config():
    cfg = dict(DEFAULT_CONFIG)
    for key, env_key in {
        "api_url": "WATCH_GATEWAY_API_URL",
        "api_key": "WATCH_GATEWAY_API_KEY",
        "model": "WATCH_GATEWAY_MODEL",
        "market_provider": "WATCH_GATEWAY_MARKET_PROVIDER",
    }.items():
        value = os.environ.get(env_key)
        if value:
            cfg[key] = value
    if CONFIG_PATH.exists():
        try:
            saved = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
            if isinstance(saved, dict):
                cfg.update({k: str(v) for k, v in saved.items() if k in cfg})
        except (OSError, json.JSONDecodeError):
            pass
    return cfg


def save_config(cfg):
    CONFIG_PATH.write_text(json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8")


CONFIG = load_config()


def normalize_chat_url(api_url):
    url = api_url.strip()
    if not url:
        return ""
    url = url.rstrip("/")
    if url.endswith("/chat/completions"):
        return url
    if url.endswith("/v1"):
        return url + "/chat/completions"
    return url + "/v1/chat/completions"


def format_price(symbol, value):
    if symbol == "SOL":
        return f"${value:.2f}"
    return f"${value:,.0f}"


def format_change(value):
    return f"{value:+.2f}%"


def mock_market(status="mock market"):
    tick = int(time.time() / 60) % 100
    trend = [24, 38, 31, 52, 46, 68, 74]
    trend = [max(6, min(96, v + ((tick + i * 7) % 13) - 6)) for i, v in enumerate(trend)]
    return {
        "status": status,
        "assets": [
            {"symbol": "BTC", "price": "$--", "change": "--"},
            {"symbol": "ETH", "price": "$--", "change": "--"},
            {"symbol": "SOL", "price": "$--", "change": "--"},
        ],
        "trend": trend,
    }


def binance_market():
    symbols = urllib.parse.quote(json.dumps(["BTCUSDT", "ETHUSDT", "SOLUSDT"]))
    url = f"https://api.binance.com/api/v3/ticker/24hr?symbols={symbols}"
    req = urllib.request.Request(url, headers={"User-Agent": "WatchLiteGateway/1.0"})
    with urllib.request.urlopen(req, timeout=5) as resp:
        data = json.loads(resp.read().decode("utf-8"))

    mapping = {}
    for item in data:
        pair = item.get("symbol", "")
        symbol = pair.replace("USDT", "")
        if symbol in {"BTC", "ETH", "SOL"}:
            mapping[symbol] = {
                "price": float(item.get("lastPrice", "0")),
                "change": float(item.get("priceChangePercent", "0")),
            }

    assets = []
    for symbol in ["BTC", "ETH", "SOL"]:
        item = mapping.get(symbol, {"price": 0.0, "change": 0.0})
        assets.append({
            "symbol": symbol,
            "price": format_price(symbol, item["price"]),
            "change": format_change(item["change"]),
        })

    base = mapping.get("BTC", {}).get("change", 0.0)
    trend = [int(max(8, min(96, 52 + base * 4 + i * 4 - 12))) for i in range(7)]
    return {"status": "Binance 24h", "assets": assets, "trend": trend}


def coingecko_market():
    url = (
        "https://api.coingecko.com/api/v3/simple/price"
        "?ids=bitcoin,ethereum,solana"
        "&vs_currencies=usd"
        "&include_24hr_change=true"
    )
    req = urllib.request.Request(url, headers={"User-Agent": "WatchLiteGateway/1.0"})
    with urllib.request.urlopen(req, timeout=5) as resp:
        data = json.loads(resp.read().decode("utf-8"))

    mapping = {
        "BTC": data.get("bitcoin", {}),
        "ETH": data.get("ethereum", {}),
        "SOL": data.get("solana", {}),
    }
    assets = []
    for symbol in ["BTC", "ETH", "SOL"]:
        item = mapping.get(symbol, {})
        price = float(item.get("usd", 0) or 0)
        change = float(item.get("usd_24h_change", 0) or 0)
        assets.append({
            "symbol": symbol,
            "price": format_price(symbol, price),
            "change": format_change(change),
        })

    base = float(mapping.get("BTC", {}).get("usd_24h_change", 0) or 0)
    trend = [int(max(8, min(96, 52 + base * 4 + i * 4 - 12))) for i in range(7)]
    return {"status": "CoinGecko 24h", "assets": assets, "trend": trend}


def coinbase_stats(symbol):
    url = f"https://api.exchange.coinbase.com/products/{symbol}-USD/stats"
    req = urllib.request.Request(url, headers={"User-Agent": "WatchLiteGateway/1.0"})
    with urllib.request.urlopen(req, timeout=5) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    last = float(data.get("last", "0") or 0)
    open_price = float(data.get("open", "0") or 0)
    change = ((last - open_price) / open_price * 100.0) if open_price else 0.0
    return last, change


def coinbase_market():
    assets = []
    btc_change = 0.0
    for symbol in ["BTC", "ETH", "SOL"]:
        price, change = coinbase_stats(symbol)
        if symbol == "BTC":
            btc_change = change
        assets.append({
            "symbol": symbol,
            "price": format_price(symbol, price),
            "change": format_change(change),
        })
    trend = [int(max(8, min(96, 52 + btc_change * 4 + i * 4 - 12))) for i in range(7)]
    return {"status": "Coinbase 24h", "assets": assets, "trend": trend}


def get_market():
    now = time.time()
    cached = MARKET_CACHE.get("data")
    if cached and now - MARKET_CACHE.get("updated", 0) < MARKET_CACHE_TTL_S:
        return cached

    provider = CONFIG.get("market_provider", "binance").lower()
    if provider == "mock":
        data = mock_market()
        MARKET_CACHE.update({"data": data, "updated": now})
        return data

    providers = {
        "coingecko": [coingecko_market],
        "coinbase": [coinbase_market],
        "binance": [binance_market, coingecko_market, coinbase_market],
        "auto": [coingecko_market, coinbase_market, binance_market],
    }.get(provider, [coingecko_market, coinbase_market, binance_market])

    last_error = "unknown"
    for fn in providers:
        try:
            data = fn()
            MARKET_CACHE.update({"data": data, "updated": now})
            return data
        except Exception as exc:
            last_error = f"{fn.__name__}: {type(exc).__name__}"
            print(f"market provider failed: {last_error}")

    data = mock_market(f"market fallback: {last_error}")
    MARKET_CACHE.update({"data": data, "updated": now})
    return data


def call_chat(prompt):
    api_url = normalize_chat_url(CONFIG.get("api_url", ""))
    api_key = CONFIG.get("api_key", "")
    model = CONFIG.get("model", "gpt-5.4")
    if not api_url or not api_key:
        return {
            "state": "Need API",
            "hint": "Open gateway page and set API URL/Key/model.",
            "reply": "Gateway is running, but no chat API is configured yet.",
        }

    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": CONFIG.get("system_prompt", "")},
            {"role": "user", "content": prompt},
        ],
        "temperature": 0.7,
        "max_tokens": 180,
    }
    req = urllib.request.Request(
        api_url,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
            "User-Agent": "WatchLiteGateway/1.0",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=35) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    reply = data["choices"][0]["message"]["content"].strip()
    return {
        "state": "Done",
        "hint": f"Model {model}",
        "reply": reply[:180],
    }


def config_page(message=""):
    cfg = CONFIG
    redacted = "set" if cfg.get("api_key") else "not set"
    return f"""<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Watch Gateway</title>
<style>
body{{margin:0;background:#07111f;color:#eaf1ff;font-family:Arial,sans-serif}}
.wrap{{max-width:640px;margin:0 auto;padding:22px}}
.card{{background:#101d2e;border:1px solid #263a58;border-radius:22px;padding:18px;margin:14px 0}}
label{{display:block;margin:14px 0 6px;color:#91a3bf}}
input,textarea,select{{width:100%;box-sizing:border-box;border:1px solid #314a70;border-radius:14px;padding:12px;background:#07111f;color:#fff;font-size:16px}}
textarea{{min-height:82px}}
button{{width:100%;border:0;border-radius:16px;padding:14px;margin-top:18px;background:#335cff;color:#fff;font-weight:700;font-size:16px}}
.muted{{color:#91a3bf;line-height:1.55}}.ok{{color:#7be0c3}}.warn{{color:#f7c873}}
</style></head><body><div class="wrap">
<h2>Watch Gateway</h2>
<div class="card muted">
<div class="ok">{html.escape(message)}</div>
<div>Watch endpoint: <b>http://&lt;ubuntu-ip&gt;:8787</b></div>
<div>API key: <b>{redacted}</b></div>
</div>
<form class="card" method="post" action="/save">
<label>Market Provider</label>
<select name="market_provider">
  <option value="auto" {"selected" if cfg.get("market_provider") == "auto" else ""}>auto</option>
  <option value="coingecko" {"selected" if cfg.get("market_provider") == "coingecko" else ""}>coingecko</option>
  <option value="coinbase" {"selected" if cfg.get("market_provider") == "coinbase" else ""}>coinbase</option>
  <option value="binance" {"selected" if cfg.get("market_provider") == "binance" else ""}>binance</option>
  <option value="mock" {"selected" if cfg.get("market_provider") == "mock" else ""}>mock</option>
</select>
<label>OpenAI-Compatible API URL</label>
<input name="api_url" value="{html.escape(cfg.get("api_url", ""))}" placeholder="https://example.com/v1">
<label>API Key</label>
<input name="api_key" type="password" placeholder="leave blank to keep current key">
<label>Model</label>
<input name="model" value="{html.escape(cfg.get("model", "gpt-5.4"))}">
<label>System Prompt</label>
<textarea name="system_prompt">{html.escape(cfg.get("system_prompt", ""))}</textarea>
<label>Watch START Prompt</label>
<textarea name="starter_prompt">{html.escape(cfg.get("starter_prompt", ""))}</textarea>
<label>TTS URL (reserved)</label>
<input name="tts_url" value="{html.escape(cfg.get("tts_url", ""))}">
<label>TTS Voice (reserved)</label>
<input name="tts_voice" value="{html.escape(cfg.get("tts_voice", ""))}">
<button type="submit">Save Gateway Config</button>
</form>
<div class="card muted">
<div>Market: <a class="ok" href="/api/watch/market">/api/watch/market</a></div>
<div>AI status: <a class="ok" href="/api/watch/ai">/api/watch/ai</a></div>
<div class="warn">Keep this gateway on LAN/Tailscale. Do not expose API keys to public internet.</div>
</div>
</div></body></html>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(f"{self.address_string()} - {fmt % args}")

    def send_json(self, obj, status=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_html(self, body, status=200):
        data = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def read_body(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            return b""
        return self.rfile.read(min(length, 65536))

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path == "/":
            self.send_html(config_page())
        elif path == "/api/watch/market":
            self.send_json(get_market())
        elif path == "/api/watch/ai":
            self.send_json(STATE)
        elif path == "/api/watch/config":
            redacted = dict(CONFIG)
            if redacted.get("api_key"):
                redacted["api_key"] = "set"
            if redacted.get("tts_key"):
                redacted["tts_key"] = "set"
            self.send_json(redacted)
        else:
            self.send_json({"error": "not found"}, 404)

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        if path == "/save":
            values = urllib.parse.parse_qs(self.read_body().decode("utf-8"), keep_blank_values=True)
            for key in DEFAULT_CONFIG:
                if key == "api_key":
                    value = values.get(key, [""])[0].strip()
                    if value:
                        CONFIG[key] = value
                elif key == "tts_key":
                    value = values.get(key, [""])[0].strip()
                    if value:
                        CONFIG[key] = value
                else:
                    CONFIG[key] = values.get(key, [CONFIG.get(key, "")])[0].strip()
            save_config(CONFIG)
            self.send_html(config_page("Saved. Update the watch Gateway URL if Ubuntu IP changed."))
        elif path == "/api/watch/ai/start":
            prompt = CONFIG.get("starter_prompt", DEFAULT_CONFIG["starter_prompt"])
            raw = self.read_body()
            if raw:
                try:
                    body = json.loads(raw.decode("utf-8"))
                    if isinstance(body, dict) and body.get("prompt"):
                        prompt = str(body["prompt"])
                except json.JSONDecodeError:
                    pass
            STATE.update({"state": "Thinking", "hint": "Calling chat API...", "updated": int(time.time())})
            try:
                result = call_chat(prompt)
            except (urllib.error.URLError, urllib.error.HTTPError, KeyError, TimeoutError, json.JSONDecodeError) as exc:
                result = {
                    "state": "AI error",
                    "hint": type(exc).__name__,
                    "reply": "Chat API failed. Check API URL, key, model and network.",
                }
            STATE.update(result)
            STATE["updated"] = int(time.time())
            self.send_json(STATE)
        else:
            self.send_json({"error": "not found"}, 404)


def main():
    parser = argparse.ArgumentParser(description="Watch Lite Ubuntu gateway")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8787)
    args = parser.parse_args()
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Watch gateway listening on http://{args.host}:{args.port}")
    print(f"Config file: {CONFIG_PATH}")
    server.serve_forever()


if __name__ == "__main__":
    main()
