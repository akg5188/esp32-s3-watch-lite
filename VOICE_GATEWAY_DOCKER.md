# Watch Voice Gateway Docker

这个方案只把 `voice_gateway.py` 放进容器，不把整台电脑目录挂进去，也不要在路由器上把电脑端口直接映射到公网。

## 本机启动

如果这台电脑还没安装 Docker，先执行：

```bash
cd "/path/to/ESP32-S3 Watch"
sudo tools/install_docker_ubuntu.sh
```

官方 Docker 安装文档：<https://docs.docker.com/engine/install/ubuntu/>

```bash
cd "/path/to/ESP32-S3 Watch"
tools/run_voice_gateway_docker.sh
curl http://127.0.0.1:8790/health
```

默认只监听电脑本机 `127.0.0.1:8790`，适合配合 Cloudflare Tunnel、frp 或 nginx 反代。
首次使用先复制配置示例：

```bash
cp .env.voice-gateway.example .env.voice-gateway
```

然后按需填写 `.env.voice-gateway`。这个文件包含 API Key 或 tunnel token 时不要提交到 GitHub。

如果容器显示 started 但 `curl` 连不上，通常是旧容器/旧端口映射残留，执行：

```bash
sudo tools/recreate_voice_gateway_docker.sh
```

如果手表要在家里 Wi-Fi 里直接访问这台电脑，把 `.env.voice-gateway` 改成：

```env
WATCH_VOICE_BIND=0.0.0.0
```

然后手表 API URL 填：

```text
http://电脑局域网IP:8790
```

## 公网访问

推荐用 Cloudflare Tunnel，不要做路由器端口映射。

1. 在 Cloudflare Zero Trust 创建 Tunnel。
2. Public Hostname 的服务地址填 `http://voice-gateway:8790`。
3. 把 tunnel token 填进 `.env.voice-gateway` 的 `CLOUDFLARE_TUNNEL_TOKEN`。
4. 启动：

```bash
docker compose --env-file .env.voice-gateway -f docker-compose.voice-gateway.yml --profile tunnel up -d --build
```

手表 API URL 填你的公网域名，例如：

```text
https://watch-ai.example.com
```

## 配置说明

- 默认 `STT` 走上游中转站，容器不跑本地 Whisper，构建和运行都轻很多。
- 默认 `TTS` 用本地 Edge TTS，voice 是 `zh-CN-XiaoxiaoNeural`，这个更接近之前已经跑通的声音方案。
- 如果要 TTS 也走 302/OpenAI 兼容接口，把 `.env.voice-gateway` 里 `WATCH_VOICE_TTS_PROVIDER=upstream`，voice 改成 `alloy`。
- 如果 `.env.voice-gateway` 没填 API Key，容器会读取 `tools/watch_gateway/watch_gateway_config.json` 里已经保存的配置。
- 容器只挂载 `tools/watch_gateway` 这个小目录，用来保存配置、日志和最近一次录音调试文件。
- 不要挂载 `/var/run/docker.sock`，不要用 `network_mode: host`，不要把 `8790` 直接映射到公网。

## 常用命令

```bash
docker compose --env-file .env.voice-gateway -f docker-compose.voice-gateway.yml ps
docker compose --env-file .env.voice-gateway -f docker-compose.voice-gateway.yml logs -f voice-gateway
docker compose --env-file .env.voice-gateway -f docker-compose.voice-gateway.yml restart voice-gateway
docker compose --env-file .env.voice-gateway -f docker-compose.voice-gateway.yml down
```
