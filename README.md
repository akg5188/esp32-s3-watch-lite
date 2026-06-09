# ESP32-S3 Watch Lite

这是给 `Waveshare ESP32-S3 Touch AMOLED 2.06` 做的轻量手表固件。目标是把手表端做稳、做省电：首页看行情，AI 页面做语音/文本问答，配置通过网页完成，API Key 不写死在固件里。

## 当前状态

- 首页显示 `BTC / ETH / XAU / WTI / DXY / CNY / EUR / VIX / SHC / HSI / N225 / DAX / UKX / SPX / NDX`，支持联网刷新和本地缓存。
- 国内网络优先行情：`BTC / ETH` 先走 Gate/Huobi/Binance 公开 ticker，行情请求直连，不再依赖 Clash 共享。
- AI 页面支持中文显示、语音输入、TTS 播放、多 AI Profile 切换；文本聊天可直接配置 GPT、Gemini、豆包、DeepSeek、千问等兼容接口。
- 支持最多 5 个 Wi-Fi 配置，每个 Wi-Fi 可保存独立静态 IP、网关、DNS。
- 配置热点平时关闭省电；短按 BOOT 唤醒并刷新，长按 BOOT 约 2 秒打开配置热点。
- 蓝牙关闭，Wi-Fi AP 不常驻，功放默认关闭，摇晃唤醒 IMU 默认不启动。
- 息屏后约 15 秒进入 deep sleep，只保留 BOOT 按键唤醒。
- LVGL 使用系统堆分配，避免 64KB 小内存池导致显示初始化卡死/白屏。

## 目录

```text
main/minimal_watch_main.c      当前主固件
main/watch_ai_cn_24.c          AI 页面中文字体
components/                    Waveshare AMOLED BSP
tools/watch_gateway/           简单 Ubuntu 网关
tools/voice_gateway.py         语音/联网资料网关
docker-compose.voice-gateway.yml
FLASHING_GUIDE.md              编译和刷机
WATCH_LITE_GUIDE.md            手表使用说明
POWER_SAVING.md                省电策略说明
VOICE_GATEWAY_DOCKER.md        Docker 语音网关说明
```

## 快速编译

需要 ESP-IDF 5.5.x：

```bash
cd "/path/to/ESP32-S3 Watch"
. /home/ak/esp-idf-v5.5.4/export.sh
idf.py -B build_lite build
```

## 快速刷机

```bash
cd "/path/to/ESP32-S3 Watch"
. /home/ak/esp-idf-v5.5.4/export.sh
idf.py -B build_lite -p /dev/ttyACM0 flash monitor
```

如果是从 GitHub Release 下载固件，优先使用发布附件里的合并固件：

```bash
python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 write_flash \
  0x0 watch_ai-esp32s3-merged.bin
```

如果端口不是 `/dev/ttyACM0`，先运行：

```bash
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

更详细步骤见 [FLASHING_GUIDE.md](FLASHING_GUIDE.md)。

## 配置方式

如果已经保存过家里 Wi-Fi，手表默认不会打开配置热点。需要配置时长按 BOOT 约 2 秒，屏幕会显示 `CFG AP`，然后：

```text
Wi-Fi: WatchLite-xxxx
密码: 12345678
网页: http://192.168.4.1
```

网页里可以配置 Wi-Fi、静态 IP、API URL、API Key、模型、行情、亮度、睡眠等。

## 安全说明

`.env*`、真实 API Key、网关配置、构建产物、原厂备份固件都不会提交到 GitHub。首次使用 Docker 语音网关时，请复制 `.env.voice-gateway.example` 生成自己的 `.env.voice-gateway`。

## 许可证

本仓库包含第三方 BSP 和 ESP-IDF 组件声明。实际使用时请同时遵守 ESP-IDF、LVGL、Waveshare BSP 及相关依赖的许可证。
