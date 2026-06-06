# edge-tts 本地代理

这个目录放的是一个给手表用的本地 TTS 代理。它把 `edge-tts` 包一层，伪装成 OpenAI 兼容的 `POST /v1/audio/speech`，这样手表固件不用改，就能继续走现有的 TTS 逻辑。

## 它做了什么

- 接收 OpenAI 风格的 TTS 请求
- 用社区项目 `edge-tts` 合成语音
- 通过纯 Python 的 `miniaudio` 转成 16-bit mono WAV
- 返回给手表直接播放
- 支持可选的 `Bearer` token，方便你在局域网或 Tailscale 里做访问控制

## 安装

先安装 Python 依赖：

```bash
cd "/path/to/ESP32-S3 Watch/tools/edge_tts_proxy"
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
```

## 启动

```bash
cd "/path/to/ESP32-S3 Watch/tools/edge_tts_proxy"
. .venv/bin/activate
export EDGE_TTS_TOKEN='your-secret-token'
python edge_tts_proxy.py --host 0.0.0.0 --port 3000
```

如果只在本机跑，也可以把 `--host` 留成默认的 `127.0.0.1`。

## 手表里怎么填

- `TTS URL`:
  - `http://<ubuntu的局域网IP>:3000/v1/audio/speech`
  - 最稳妥的是让 Ubuntu 和手表在同一个 Wi-Fi 里，直接填局域网 IP
  - 如果你想让外网也能访问，就需要再配公开入口，例如反代或 Funnel；那种情况下务必保留 token
- `TTS Key`:
  - 如果你在代理里设置了 `EDGE_TTS_TOKEN`，这里也填同一个值
  - 如果只是局域网测试，可以留空
- `TTS Voice`:
  - 推荐先填 `zh-CN-XiaoxiaoNeural`
  - 如果你填 `alloy / nova / echo` 这些 OpenAI 风格名字，代理会自动回退到默认中文音色

## 测试

```bash
curl -X POST "http://127.0.0.1:3000/v1/audio/speech" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer your-secret-token" \
  -d '{"model":"gpt-4o-mini-tts","input":"你好，测试一下边缘语音。","voice":"zh-CN-XiaoxiaoNeural","response_format":"wav","speed":1.0}' \
  --output test.wav
```

如果命令成功，会得到一个可播放的 `test.wav`。

## 备注

- 这个代理只返回 WAV，不返回 MP3。
- `edge-tts` 本身不需要微软 Azure Key。
- 这套方案不依赖系统级 `ffmpeg`，只要 Python 依赖装好就能跑。
- 如果你想把服务暴露到公网，务必保留 token，并优先考虑 Tailscale 这类内网方案，而不是直接裸奔到公网。
