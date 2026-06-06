# Watch Lite Ubuntu Gateway

这个小网关跑在 Ubuntu 上，手表只访问这个网关，不保存 API Key。

## 启动

```bash
cd "/path/to/ESP32-S3 Watch/tools/watch_gateway"
python3 watch_gateway.py --host 0.0.0.0 --port 8787
```

然后在手机或电脑打开：

```text
http://<ubuntu-ip>:8787
```

在网页里填写中转 API URL、API Key、模型。手表设置页里的 `Ubuntu Gateway URL` 填：

```text
http://<ubuntu-ip>:8787
```

## 手表使用的接口

```text
GET  /api/watch/market
GET  /api/watch/ai
POST /api/watch/ai/start
```

## 安全提醒

只建议放在家里局域网或 Tailscale 内网里，不要直接暴露到公网。
