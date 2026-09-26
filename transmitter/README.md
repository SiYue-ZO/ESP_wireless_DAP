# Transmitter

发送端是独立 ESP-IDF 工程，连接电脑后提供一个 USB 复合设备：

- CMSIS-DAP v1 HID，64 字节包；
- CDC ACM 无线目标串口；
- ESP-NOW 自动发现接收端；
- GPIO48 WS2812 状态显示。

ESP32-S3 原生 USB 使用 GPIO19 (D-) / GPIO20 (D+)。默认 ESP-NOW 信道为 6，默认
`PAIR_ID` 为 `0x57444150`，必须与接收端相同。

```bash
. "$IDF_PATH/export.sh"
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

配置项位于 `Wireless DAP transmitter` 菜单。固件启动后原生 USB 会切换为
CMSIS-DAP + CDC 设备；若无法再次下载，可按住 BOOT、点按 RESET 进入下载模式。

主要实现：

- `main/usb_descriptors.c`：USB 复合设备描述符；
- `main/usb_bridge.c`：DAP 与 CDC 的 FreeRTOS 桥接任务；
- `../common/wireless_link/`：ESP-NOW 链路。

完整接线、安全说明与测试流程见仓库根目录 `README.md`。
