# ESP32-S3 Wireless DAP

基于两片 ESP32-S3 的无线 SWD 烧录/调试器原型。发送端通过 USB 向电脑提供
CMSIS-DAP v1 HID 和 CDC ACM 串口；接收端连接目标板的 SWD 与 UART。两端使用
ESP-NOW 自动发现并通信，应用任务基于 ESP-IDF/FreeRTOS。

> 当前版本是可编译的工程原型，尚未完成真实开发板与目标芯片联调。首次接线时请
> 从低 SWD 频率开始，并先确认目标板电平和地线。

## 功能与限制

- CMSIS-DAP v1 HID，64 字节 DAP 包，无需自定义 Windows WinUSB 驱动。
- 支持 SWD 烧录和调试，不支持 JTAG。
- USB CDC ACM 与目标 UART 双向桥接，打开串口时会同步波特率、数据位、校验位和停止位。
- ESP-NOW 自动发现、心跳、断线重连、CRC32 校验，以及 DAP 请求重试和接收端去重。
- GPIO48 上的一颗 WS2812 显示启动、等待、连接、活动和错误状态。
- 当前无线串口是 best-effort 通道：没有逐帧确认或流控，链路拥塞时可能丢数据，
  不适合传输固件文件或要求零丢包的数据流。
- `PAIR_ID` 只隔离附近的设备组，不提供身份认证或加密。当前 ESP-NOW 单播也未加密。
- 接收端没有目标电压检测、目标供电和电平转换功能。

## 架构

```text
PC / OpenOCD / serial terminal
       | USB: CMSIS-DAP v1 HID + CDC ACM
       v
ESP32-S3 transmitter
       | ESP-NOW, channel 6 by default
       v
ESP32-S3 receiver
       | SWCLK / SWDIO / nRESET + UART TX/RX
       v
Target MCU
```

仓库包含两个独立的 ESP-IDF 工程：

```text
.
|-- transmitter/             # 连接电脑，提供复合 USB 设备
|-- receiver/                # 连接目标 MCU，执行 SWD 并桥接 UART
|   `-- components/dap_engine/
|-- common/
|   |-- wireless_link/       # ESP-NOW 配对、可靠 DAP 通道、UART 通道
|   `-- status_led/          # WS2812 FreeRTOS 状态任务
`-- docs/protocol.md         # 无线帧格式与行为
```

## 硬件连接

发送端只需连接电脑 USB。ESP32-S3 原生 USB 使用 GPIO19 (D-) 和 GPIO20 (D+)，通常已
连接到开发板的 USB 接口；不要再把这两个引脚分配给其他外设。

接收端与目标板连接如下：

| 接收端 ESP32-S3 | 目标板 | 默认用途 |
|---|---|---|
| GPIO9 | SWCLK | SWD 时钟 |
| GPIO10 | SWDIO | SWD 双向数据 |
| GPIO4 | nRESET | 目标复位，可选但建议连接 |
| GPIO17 (TX) | UART RX | 无线串口发送到目标 |
| GPIO18 (RX) | UART TX | 接收目标串口输出 |
| GND | GND | 必须共地 |

两块 ESP32-S3 的 WS2812 数据脚默认为 GPIO48。

### 电气安全

- 所有信号默认是 **3.3 V 逻辑**，不能直接连接 5 V 信号。
- 目标为 1.8 V 或 5 V I/O 时必须使用合适的双向/单向电平转换器。
- 本项目不从接收端给目标板供电。目标板应独立、稳定供电，并与接收端共地。
- 接收端不采样 VTref，通电前需要人工确认目标 I/O 电压。
- nRESET 通常低有效；若目标复位电路不兼容，可断开并在配置中禁用相关使用。

## 开发环境

推荐 ESP-IDF 5.3 或更新版本。本仓库已用 ESP-IDF 6.1-rc1 编译验证。依赖由 IDF
Component Manager 按各工程的 `dependencies.lock` 下载，包括 `esp_tinyusb`、TinyUSB
和 `led_strip`。

```bash
# 先加载你的 ESP-IDF 环境
. "$IDF_PATH/export.sh"

# 构建发送端
cd transmitter
idf.py set-target esp32s3
idf.py build

# 构建接收端
cd ../receiver
idf.py set-target esp32s3
idf.py build
```

不要从仓库根目录运行 `idf.py build`；根目录只用于组织两个工程。

## 配置与烧录

两个工程都可独立运行 `idf.py menuconfig`。在 `Wireless DAP transmitter` 或
`Wireless DAP receiver` 菜单中设置参数。

两端以下配置必须相同：

- `Pair identifier`：默认 `0x57444150`。
- `ESP-NOW Wi-Fi channel`：默认信道 6。

接收端还可配置 LED、SWD、复位和 UART 引脚，以及初始串口波特率。修改公共配置时，
需要分别进入两个工程修改并重新烧录。

```bash
cd transmitter
idf.py menuconfig
idf.py -p /dev/ttyACM0 flash monitor

cd ../receiver
idf.py menuconfig
idf.py -p /dev/ttyACM1 flash monitor
```

端口名以本机枚举结果为准。若发送端固件运行后占用原生 USB 为 DAP/CDC 复合设备，
再次下载时可按住 BOOT、点按 RESET 进入下载模式。接收端保留 USB Serial/JTAG 日志。

## 自动配对

启动后，两端每 500 ms 广播发现帧。信道和 `PAIR_ID` 相同的发送端与接收端会采用
首先建立的连接，之后交换心跳；约 3.5 秒收不到对端数据即断开并恢复发现。无需预先
填写 MAC 地址。

同一区域有多套设备时，为每一对设置不同的随机 `PAIR_ID`。这只能减少误配，不是
安全机制；攻击者仍可监听或构造数据帧。不要在不可信无线环境中烧录敏感目标。

## 状态灯

| 显示 | 状态 |
|---|---|
| 蓝色 | 发送端正在启动 |
| 紫色 | 接收端正在启动 |
| 黄色呼吸 | 发送端等待配对或已断线 |
| 紫色呼吸 | 接收端等待配对或已断线 |
| 青色 | 发送端无线连接正常 |
| 绿色 | 接收端无线连接正常 |
| 蓝色短闪 | DAP 或 UART 数据活动 |
| 红色闪烁 | 初始化或任务创建失败 |

若开发板的 WS2812 不是 GPIO48，可分别在两个工程的 `menuconfig` 中修改。

## 使用 OpenOCD

发送端接入电脑并显示绿色后，先确认系统识别到 CMSIS-DAP HID。OpenOCD 的目标配置
需要替换成实际 MCU，例如：

如果不能安装 udev 规则，可以只对本次 OpenOCD 进程使用 root 权限。该方式不写入
`/etc/udev`、不修改用户组，拔插设备后也不会留下权限状态：

```bash
cd /path/to/wireless_DAP
sudo -v                         # 可选：提前缓存一次 sudo 凭据
./tools/openocd-no-udev.sh \
  -s /usr/share/openocd/scripts \
  -f /path/to/stm32f4discovery.cfg \
  -c "cmsis-dap backend hid" \
  -c "adapter usb vid_pid 0x303a 0x4012" \
  -c "adapter speed 500" \
  -c "program /path/to/test_dap.elf verify reset exit"
```

`cmsis-dap backend hid` 应放在目标配置加载之后；如果目标配置已经选择了
CMSIS-DAP 驱动，不要重复加载 `interface/cmsis-dap.cfg`。OpenOCD 被 IDE 自动启动时，
将 OpenOCD 可执行文件设置为仓库内的 `tools/openocd-no-udev.sh`，并先在终端执行
一次 `sudo -v`。

Linux 首次使用时安装仓库提供的 udev 规则，然后重新插拔发送端：

```bash
sudo install -m 0644 tools/60-wireless-dap.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

```bash
openocd \
  -f interface/cmsis-dap.cfg \
  -c "cmsis-dap backend hid" \
  -c "transport select swd" \
  -c "adapter speed 1000" \
  -f target/stm32f1x.cfg
```

烧录示例：

```bash
openocd \
  -f interface/cmsis-dap.cfg \
  -c "cmsis-dap backend hid" \
  -c "transport select swd" \
  -c "adapter speed 500" \
  -f target/stm32f1x.cfg \
  -c "program firmware.elf verify reset exit"
```

无线链路增加了延迟，建议先用 500-1000 kHz。主机下发的 `DAP_SWJ_Clock` 会调整接收端
软件 SWD 时序，但高频性能受 ESP32-S3 GPIO 位操作、无线往返和目标布线影响。

## 使用无线串口

发送端会同时枚举一个名为 `Wireless Target UART` 的 CDC ACM 串口。用任意串口工具
打开它即可，例如：

```bash
picocom -b 115200 /dev/ttyACM1
```

实际设备节点可能因枚举顺序不同而变化。串口工具修改线编码后，发送端会将配置同步
给接收端 UART1。DTR/RTS 当前会被传输但没有映射到目标板的 BOOT/RESET 引脚。

## 首次真机测试

1. 不连接目标板，分别烧录两端，确认 LED 从琥珀色变为绿色，日志显示 `paired with`。
2. 只交叉连接接收端 GPIO17/18，打开发送端 CDC 串口，验证回环收发。
3. 给目标板独立供电并共地，再连接 SWCLK、SWDIO 和可选 nRESET。
4. 用低速 OpenOCD 执行识别或 `halt`，确认目标型号和供电稳定后再尝试烧录。
5. 断电、遮挡或复位任一端，确认约 3.5 秒后进入等待状态并能自动重连。

### 常见问题

- 一直琥珀色：检查两端 Wi-Fi 信道和 `PAIR_ID` 是否一致，并确认 GPIO48 没有与板级功能冲突。
- OpenOCD 找不到探针：确认使用发送端的原生 USB 口，检查 HID 与 CDC 是否同时枚举。
- 能配对但找不到目标：检查共地、SWCLK/SWDIO 是否接反、目标供电和逻辑电压；降低 adapter speed。
- 串口乱码：确认目标 UART 电平、TX/RX 方向和波特率；USB 串口工具应选择目标实际线编码。
- 高流量串口丢字节：这是当前 best-effort UART 通道的已知限制，降低波特率或上层增加校验/重传。

## 参考资料与来源

- [Espressif ESP-NOW Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp_now.html)
- [Espressif USB Device Stack / TinyUSB](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/usb_device.html)
- [Arm CMSIS-DAP](https://arm-software.github.io/CMSIS_5/DAP/html/index.html)
- [SiYue-ZO/ESP32S3_Wireless_CMSIS_DAP](https://github.com/SiYue-ZO/ESP32S3_Wireless_CMSIS_DAP)
- [masbc666/ESP32S3_Wireless_CMSIS_DAP](https://github.com/masbc666/ESP32S3_Wireless_CMSIS_DAP)

接收端 `dap_engine` 从上述 fork 的 CMSIS-DAP/SWD 实现改编。上游仓库在本项目创建时
未声明仓库级许可证，详情见 `receiver/components/dap_engine/NOTICE`。因此本仓库当前未
附加统一的开源许可证；对外再分发源码或固件前应先确认相关代码的授权。
