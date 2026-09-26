# Receiver

接收端是独立 ESP-IDF 工程，自动连接发送端并在目标侧执行 SWD 命令，同时桥接
UART1。默认引脚：

| ESP32-S3 | 目标信号 |
|---|---|
| GPIO9 | SWCLK |
| GPIO10 | SWDIO |
| GPIO4 | nRESET |
| GPIO17 | UART RX（目标侧） |
| GPIO18 | UART TX（目标侧） |
| GND | GND |

表中的 GPIO17 是接收端 TX，应连接目标 RX；GPIO18 是接收端 RX，应连接目标 TX。
GPIO48 驱动板载 WS2812。默认 UART 为 115200 8N1，连接后会跟随电脑 CDC 的线编码。

```bash
. "$IDF_PATH/export.sh"
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyACM1 flash monitor
```

配置项位于 `Wireless DAP receiver` 菜单。默认 ESP-NOW 信道 6、`PAIR_ID`
`0x57444150`，必须与发送端相同。

目标板必须独立供电并与接收端共地。本工程只适用于 3.3 V 逻辑，不检测 VTref，也
不提供电平转换。连接 1.8 V 或 5 V 目标前必须增加合适的电平转换电路。

主要实现：

- `main/main.c`：SWD、UART 和状态任务；
- `components/dap_engine/`：CMSIS-DAP 命令解析与软件 SWD；
- `../common/wireless_link/`：ESP-NOW 链路。

完整限制、OpenOCD 示例和真机测试步骤见仓库根目录 `README.md`。
