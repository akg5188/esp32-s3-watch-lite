# 省电策略

这版固件优先做“安全省电”，不牺牲亮屏体验，不缩短当前亮屏等待时间。

## 已启用

- `CONFIG_BT_ENABLED` 关闭，蓝牙不启动。
- Wi-Fi/蓝牙共存关闭，因为蓝牙不用。
- 以太网组件关闭。
- 有 Wi-Fi 配置时默认只启用 `WIFI_MODE_STA`，不常驻 `APSTA`。
- 配置热点只在没有 Wi-Fi 配置、长按 BOOT、Wi-Fi 扫描/切换时开启。
- Wi-Fi 使用 `WIFI_PS_MAX_MODEM`，并降低最大发射功率。
- 息屏时断开 STA，保持配置 AP 关闭。
- 息屏后约 15 秒进入 deep sleep。
- deep sleep 只保留 BOOT 按键唤醒。
- 音频功放开机默认拉低，录音/播放结束后拉低。
- 摇晃唤醒默认关闭，IMU 任务不启动。
- LVGL 改用系统堆分配，降低显示初始化卡死/白屏风险。

## 唤醒规则

- 短按 BOOT：唤醒并刷新首页。
- 长按 BOOT 约 2 秒：打开配置 AP。
- 不配置定时自动唤醒，避免无意义耗电。

## 没有继续压的部分

- 不再缩短亮屏时间。
- 不再降低默认亮度。
- 暂时不启用复杂的动态 CPU 降频/light sleep 策略，避免影响 Wi-Fi、触摸和音频稳定性。

## 验证日志

刷入后串口应出现：

```text
wifi mode: STA config_ap=0 sta_cfg=1
sleep: screen off, WiFi STA disconnected, config AP=0
deep sleep: entering, button wake only
```
