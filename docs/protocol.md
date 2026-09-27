# Wireless Link Protocol

本文记录发送端与接收端之间的内部 ESP-NOW 协议。协议不是稳定的公共 API。

## 协议版本

- `protocol v1`：当前稳定基线，64 字节 DAP、经典 ESP-NOW payload 和 window=1。
- `protocol v2`：第二阶段开发版本。帧版本字段为 2，发现阶段使用 ESP-NOW v2 大帧并
  携带能力协商信息；可在发送端启用实验性的 CMSIS-DAP v2 Bulk USB 接口，并选择
  64 或 256 字节逻辑 DAP 包。可选择 packet window=2，或在 256 字节模式启用 HID64
  请求聚合；这两个模式当前互斥。

两端必须配置相同协议版本。v1 固件会拒绝 v2 帧，v2 固件也会拒绝没有能力信息的旧
发现帧，不会猜测或混用帧格式。

## 传输约束

- ESP-NOW 固定 Wi-Fi 信道，默认为 6。
- 协议 v1 的单帧保持在经典 ESP-NOW 250 字节限制内。协议 v2 的发现帧固定为 251
  字节，用于在建立连接前验证两端都能收发 ESP-NOW v2 帧。
- 多字节整数直接使用 ESP32-S3 小端表示；当前协议只面向相同架构的两端固件。
- 单播未启用 ESP-NOW 加密。

## 帧格式

所有帧以 packed 结构发送，实际长度是 20 字节头部加 `payload_length`：

| 偏移 | 大小 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | `magic` | `0x50414457` (`WDAP`) |
| 4 | 4 | `pair_id` | 两端配置一致才接受 |
| 8 | 2 | `sequence` | DAP/UART 序号 |
| 10 | 2 | `payload_length` | v1 为 0-192 字节；v2 为 0-231 或 0-256 字节 |
| 12 | 1 | `version` | 协议 v1 为 1，协议 v2 为 2 |
| 13 | 1 | `type` | 帧类型 |
| 14 | 1 | `source_role` | 1=发送端，2=接收端 |
| 15 | 1 | `window_meta` | v2 DAP 帧的窗口数量和索引；其他帧及 v1 置零 |
| 16 | 4 | `crc32` | CRC 字段置零后覆盖头部和有效载荷 |
| 20 | 可变 | `payload` | 类型相关数据 |

CRC 使用反射多项式 `0xEDB88320`。接收端检查 magic、版本、`PAIR_ID`、角色、长度和
CRC 后才处理帧。

协议 v2 的 DAP 请求和响应将 byte 15 高 4 位编码为 `window_count - 1`，低 4 位编码为
`window_index`。当前有效值为 `0x00`（单包）、`0x10`（两包窗口 index 0）和 `0x11`
（两包窗口 index 1）。同一窗口使用连续 sequence，响应复制对应请求的 metadata。

## 帧类型

| 值 | 名称 | 方向 | 用途 |
|---:|---|---|---|
| 1 | `HELLO` | 双向广播 | 自动发现 |
| 2 | `HELLO_ACK` | 双向单播 | 确认连接 |
| 3 | `HEARTBEAT` | 双向单播 | 保活 |
| 4 | `DAP_REQUEST` | 发送端到接收端 | 64 或 256 字节 CMSIS-DAP 请求 |
| 5 | `DAP_RESPONSE` | 接收端到发送端 | 与对应请求相同长度的 CMSIS-DAP 响应 |
| 6 | `UART_DATA` | 双向 | 最多 192 字节串口数据 |
| 7 | `UART_CONFIG` | 发送端到接收端 | 波特率、数据位、校验、停止位、DTR、RTS |
| 8 | `DAP_ABORT` | 发送端到接收端 | 无 payload 的高优先级传输中止通知 |

## 连接状态

未连接时每 500 ms 广播 `HELLO`。收到角色相反且配置匹配的 `HELLO` 或
`HELLO_ACK` 后记录对端 MAC；已有连接时不接受另一个 MAC 抢占。连接状态下每秒发送
心跳，3.5 秒没有收到对端有效帧时清除会话并重新发现。

协议 v2 的 `HELLO` 和 `HELLO_ACK` payload 固定为 231 字节，因此包括 20 字节头部的
ESP-NOW 数据总长为 251 字节，刚好越过 v1 上限。前 8 字节是能力结构，其余字节必须
置零：

| 字段 | 大小 | 说明 |
|---|---:|---|
| `max_dap_packet_size` | 2 | 配置的逻辑 DAP 包大小，64 或 256 |
| `max_payload_size` | 2 | 64 模式为 231，256 模式为 256 |
| `packet_window` | 1 | 支持的 DAP 窗口，当前为 1 或 2 |
| `flags` | 1 | bit0=DAP64；bit1=Bulk USB；bit2=window>1；bit3=ESP-NOW v2；bit4=DAP256；bit5=HID64 聚合 |
| `reserved` | 2 | 置零 |

缺少 ESP-NOW v2 能力位、最大 payload 小于本地帧 payload，或两端 DAP 包大小不一致
时不会建立连接。64 模式的 DAP 帧总长为 84 字节；256 模式为 276 字节，后者要求
ESP-NOW v2。HELLO/HELLO_ACK 无论 DAP 包大小如何都保持 251 字节。

发送端的 `Experimental CMSIS-DAP v2 bulk USB interface` 默认关闭，并且只允许在协议
v2 且 TinyUSB Vendor interface count 为 1 时启用。启用后，USB 设备同时保留 HID64 和
CDC ACM，新增接口字符串为 `CMSIS-DAP v2` 的 Bulk IN/OUT 接口。Bulk 可配置为 64 或
256 字节逻辑包，HID 始终为 64 字节。Bulk 与 HID 共用同一条串行 DAP 执行队列，接收端
按请求实际长度执行并返回相同长度。

HID64 聚合默认关闭，只能在协议 v2/256 字节模式启用，并要求接收端在发现能力中设置
bit5，且 packet window 必须保持为 1。发送端此时将 HID `DAP_Info(Packet Count)` 报告为
3。第一个可聚合请求到达后，
发送端默认等待 1 ms 收集第二个请求，随后只收集已经排队的第三个请求；不足两包时仍按
原来的 64 字节交换发送。聚合包使用标准 CMSIS-DAP `DAP_ExecuteCommands (0x7F)`：

```text
0x7F | command_count | command 1 有效字节 | command 2 有效字节 | command 3 有效字节 | 零填充
```

发送端按命令格式去掉每个 HID 报告末尾的填充，并验证每个子响应都能放入 64 字节。当前
支持 Connect/Disconnect、TransferConfigure、Transfer、TransferBlock、WriteABORT、
Delay、ResetTarget、SWJ Pins/Clock/Sequence、SWD Configure/Sequence。Info、HostStatus、
TransferAbort、未知命令和嵌套 QueueCommands/ExecuteCommands 不聚合。接收端响应中的
command count 是实际完成数；发送端按原请求拆分并补零为独立 HID64 响应。批量无线交换
失败、响应格式非法或提前停止时，未取得有效响应的每个子请求分别返回错误。

## DAP 可靠性

第一阶段将发送资源分为两条队列：DAP 请求/响应使用高优先级队列，UART、发现和心跳
使用低优先级队列。UART 队列满时允许丢弃数据，不会阻塞 DAP。发送任务优先检查 DAP
队列；`ESP_ERR_ESPNOW_NO_MEM` 会采用有限指数退避。成功提交无线帧后才应用
`ESP-NOW TX pacing delay`，因此将其设为 0 不会额外引入固定 2 ms 等待。

window=1 时，发送端使用同一序号按 `DAP response retry interval`（默认 45 ms）重发请求，
直到收到匹配响应或主机侧超时。window=2 时，发送端连续预留两个 sequence，连续发送
两项并独立记录响应；响应可以乱序到达，但 USB 响应始终按请求顺序返回。每轮只重发尚未
完成的项，一项失败不会覆盖另一项已经取得的响应。

接收端为窗口内每项分别维护 received、queued/executing 和 completed 状态。index 1 即使
先到达也只会暂存；只有前一项 completed 后才进入唯一的 SWD 执行队列。每个 completed
项保留独立响应缓存，重复请求直接重发缓存，不重新执行 SWD 或 Flash 写入。新窗口只在
旧窗口全部完成或被取消后接收；断线会清空整个窗口状态。

DAP 交换由互斥锁串行化。默认配置公布一个 CMSIS-DAP packet slot；window=2 时 HID 和
Bulk 都公布两个 slot，发送端默认等待 1 ms 收集第二项。本地 Info/HostStatus 不会被前一
个无线请求跨越，也不会和无线请求组成窗口。

HID 聚合开启时，对 USB 主机公布三个 HID packet slot，但无线层仍保持 window=1 和接收端
顺序执行。`DAP_ABORT` 插入 DAP 无线发送队列头部；接收端通过独立高优先级任务置位中止
代数，使当前 Transfer/TransferBlock 尽快退出，并停止 `DAP_ExecuteCommands` 中剩余命令。
window=2 时还会取消尚未执行的后续窗口项；发送端最多再等待两个重试周期，以接收当前
执行项的部分完成响应和已完成项的缓存响应。CMSIS-DAP 的 TransferAbort 没有 USB 响应。

当 `ESP-NOW peer PHY rate` 非 0 时，仅对已配对的单播 peer 设置指定速率；广播发现帧仍
使用驱动默认速率。可选值为 11 Mbps、HT20 MCS2 和 HT20 MCS3，弱信号环境应回退到 0。

## UART 语义

UART 数据帧不确认、不重传，也不保证在队列拥塞时送达。它适合终端日志和低到中等
流量的交互串口。配置帧连续发送三次，接收端使用长度为 1 的覆盖队列保留最新配置。

若后续要求可靠高速串口，应增加窗口、ACK、重传、拥塞控制和端到端流控，并升级
协议版本，而不是把 UART 数据复用到 DAP 的单请求通道。
