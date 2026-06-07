# 省电策略

这版固件把“超时后直接关机”作为主要省电方案。到达配置的空闲超时后，手表会先关屏、断开 Wi-Fi，然后向 AXP2101 PMIC 发送软件关机请求。

## 已启用

- `CONFIG_BT_ENABLED` 关闭，蓝牙不启动。
- Wi-Fi/蓝牙共存关闭，因为蓝牙不用。
- 以太网组件关闭。
- 有 Wi-Fi 配置时默认只启用 `WIFI_MODE_STA`，不常驻 `APSTA`。
- 配置热点只在没有 Wi-Fi 配置、长按 BOOT、Wi-Fi 扫描/切换时开启。
- Wi-Fi 使用 `WIFI_PS_MAX_MODEM`，并降低最大发射功率。
- 空闲超时时断开 STA，并保持配置 AP 关闭。
- 空闲超时时发送 AXP2101 软件关机请求，让 PMIC 切断系统供电。
- 音频功放开机默认拉低，录音/播放结束后拉低。
- 录音/播放前才打开音频模拟电源，结束后关闭 AXP2101 `ALDO2/A3V3`。
- 行情、图表、语音/TTS 后台任务如果卡住，会超时清理 busy 状态，避免整晚阻止自动关机。
- 摇晃唤醒默认关闭，IMU 任务不启动。
- LVGL 改用系统堆分配，降低显示初始化卡死/白屏风险。

## 关机规则

- 配置项 `auto_sleep_s` 现在等同于自动关机超时时间。
- 当前默认超时时间是 45 秒。
- 超时后日志应出现 `poweroff: idle timeout, AXP2101 soft shutdown`。
- PMIC 关机后 `BOOT` 不再负责唤醒。
- 按电源键或插入 USB 可以重新开机。

## 没有继续使用的方案

- 不使用 ESP32 true deep sleep，因为这块板暂时没有确认可靠的硬件唤醒 GPIO。
- 不启用 light sleep，因为 BOOT 虽然可以唤醒，但实测唤醒后触摸 I2C 驱动会 abort 重启。
- 不再保留 BOOT 轮询待机任务，因为 PMIC 直接关机更省电，而且电源键冷启动速度很快。

## 验证日志

刷入后串口在切电前应出现：

```text
wifi mode: STA config_ap=0 sta_cfg=1
sleep: screen off, WiFi STA disconnected, config AP=0
poweroff: idle timeout, AXP2101 soft shutdown
```
