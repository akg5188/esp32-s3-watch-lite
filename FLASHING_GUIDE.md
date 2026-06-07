# Watch Lite 刷机指南

适用硬件：`Waveshare ESP32-S3 Touch AMOLED 2.06`

## 准备

1. 安装 ESP-IDF 5.5.x。
2. 用 USB 连接手表。
3. 找到串口。

Linux 查看串口：

```bash
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

也可以用脚本辅助识别：

```bash
python3 tools/find_watch_port.py
```

## 编译

```bash
cd "/path/to/ESP32-S3 Watch"
. /home/ak/esp-idf-v5.5.4/export.sh
idf.py -B build_lite build
```

编译成功后会生成：

```text
build_lite/watch_ai.bin
```

## 从 GitHub Release 下载固件

发布版会附带自动构建的固件包：

```text
watch_ai-esp32s3-merged.bin       合并固件，推荐直接刷 0x0
watch_ai-esp32s3-firmware.zip     完整固件包，包含 bootloader / 分区表 / app / 刷机说明
bootloader.bin                    单独 bootloader
partition-table.bin               单独分区表
watch_ai.bin                      单独 app 固件
FLASHING.txt                      本次 release 对应的刷机命令
```

优先使用合并固件：

```bash
python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  0x0 watch_ai-esp32s3-merged.bin
```

## 刷机

```bash
cd "/path/to/ESP32-S3 Watch"
. /home/ak/esp-idf-v5.5.4/export.sh
idf.py -B build_lite -p /dev/ttyACM0 flash monitor
```

如果实际端口不是 `/dev/ttyACM0`，替换成实际端口。

退出 monitor：

```text
Ctrl+]
```

## 手动 esptool 刷机

```bash
cd "/path/to/ESP32-S3 Watch"
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 32MB \
  0x0 build_lite/bootloader/bootloader.bin \
  0x8000 build_lite/partition_table/partition-table.bin \
  0x10000 build_lite/watch_ai.bin
```

## 刷机后验证

启动日志里应该能看到类似内容：

```text
Watch Lite direct booting
wifi start: configured=1 slots=...
wifi mode: STA config_ap=0 sta_cfg=1
alive sta=1 cfg=1 ...
sleep: screen off, WiFi STA disconnected, config AP=0
deep sleep: entering, button wake only
```

如果进入 deep sleep，串口可能断开，这是正常的。短按 BOOT 可以唤醒。

## 首次配置

如果手表没有 Wi-Fi 配置，会启动配置 AP：

```text
Wi-Fi: WatchLite-xxxx
密码: 12345678
网页: http://192.168.4.1
```

如果已经保存过 Wi-Fi，配置 AP 平时不会打开。需要修改配置时长按 BOOT 约 2 秒。

## 恢复原厂固件

本仓库不会上传原厂备份固件。如果本机有完整 32MB 原厂备份，可以用：

```bash
python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash 0x0 factory_backup_32mb.bin
```
