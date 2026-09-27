# 无线写入加速实施计划

## 1. 文档目的

本文为 ESP32-S3 Wireless DAP 项目的无线写入加速实施计划。目标是在保持现有 SWD 烧录可靠性、CMSIS-DAP 工具兼容性和断线恢复能力的前提下，逐步降低无线往返延迟，提高固件写入吞吐。

本计划分为三个阶段：

1. 第一阶段：在当前 CMSIS-DAP v1、64 字节 DAP 包和协议 v1 基础上，优化现有链路。
2. 第二阶段：引入 CMSIS-DAP v2 bulk、大 DAP 包、ESP-NOW v2 和请求窗口，减少无线往返次数。
3. 第三阶段：以同目标、同镜像、同校验口径对标 ST-Link，先拆解现有瓶颈，再依次验证短响应、性能构建、1024 字节包、单板 TCP 和硬件辅助 SWD。

三个阶段可以并行设计，但必须保留稳定基线和可回退路径。第一阶段完成后应形成一个可独立发布的稳定版本，第二阶段在此基础上继续演进；第三阶段的每项优化必须单独 A/B 测试，不能用标称 SWD 频率代替实际 Flash 吞吐。

## 2. 当前问题和基线

### 2.1 原始稳定基线链路

```text
PC / OpenOCD
    -> USB Full-Speed HID，64 字节，1 ms polling interval
    -> transmitter
    -> ESP-NOW 单播，当前协议 v1
    -> receiver
    -> 软件 bit-bang SWD
    -> 目标 MCU Flash
```

### 2.2 原始基线已确认的固定开销

- `common/wireless_link/wireless_link.c` 的发送任务在每个 ESP-NOW 帧后固定延迟 2 ms。
- 一个 DAP 交换至少包含一个请求帧和一个响应帧，因此固定延迟会叠加到一次完整交换中。
- ESP-NOW 默认 PHY 速率为 1 Mbps，当前代码没有配置 peer 的速率。
- DAP 包固定为 64 字节，`DAP_PACKET_QUEUE_COUNT` 为 1，发送端和无线层一次只处理一个请求。
- 丢包后当前等待 45 ms 才进行下一次 DAP 请求重发。
- SWD 默认时钟为 1 MHz，README 中推荐的初始值为 500-1000 kHz。
- 当前 USB 使用 Full-Speed HID，DAP endpoint 的 polling interval 为 1 ms。
- 当前协议限制在经典 ESP-NOW 250 字节范围内，实际自定义帧 payload 上限为 192 字节。

### 2.3 当前基线必须测量的指标

在任何性能修改前，记录以下数据：

- 单次 DAP 请求的端到端延迟：平均值、P95、P99。
- 无线请求发送到 receiver 收到的延迟。
- `dap_protocol_execute()` 的执行时间。
- receiver 响应发送到 transmitter 收到的延迟。
- ESP-NOW 发送失败次数。
- DAP 重试次数和重试间隔。
- SWD WAIT、FAULT、ERROR 和 parity error 次数。
- RAM `DAP_TransferBlock` 的有效吞吐。
- 实际 Flash 烧录时间，分别记录 erase、write、verify 和 reset 阶段。
- UART 高流量同时存在时对 DAP 的影响。

建议使用 `esp_timer_get_time()` 增加临时统计点，并在性能测试固件中提供可关闭的详细日志，避免日志本身影响时序。

### 2.4 已验证的当前基线

截至 2026-09-28，当前真机高性能候选链路为：

```text
PC / OpenOCD 0.12.0+dev-snapshot
    -> USB Full-Speed CMSIS-DAP v2 Bulk，Packet Size=256，Packet Count=2
    -> transmitter
    -> ESP-NOW v2 单播，11 Mbps，固定发送延迟 0 ms，window=2
    -> receiver
    -> dedicated GPIO 软件 SWD，请求时钟 2 MHz
    -> STM32F412 512 KiB Flash
```

该配置已经完成 10/10 次全量 erase/write/verify，最终回读 SHA256 与原始镜像一致。512 KiB
平均写入为 54.160 s（9.453 KiB/s），而同一目标和镜像的 ST-Link/V2 为 13.595 s
（37.661 KiB/s）；当前无线写入仍慢约 3.98 倍。完整结果见第 7.4 节。

### 2.5 已确认的第三阶段瓶颈

- 真机构建的 transmitter 和 receiver 当前均为 `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y`，即 `-Og`；尚未做性能优化构建的独立 A/B。
- 512 KiB 写入详细日志中，写入阶段约 53.095 s，共约 2,858 个 DAP 包和 133,315 次 SWD transaction，平均每包约 46.65 次 transaction。
- 主流满包包含 62 次 AP 写，两个 DAP slot 确实交替工作，说明 window=2 已生效；典型满包从请求到响应仍约需 18 ms。
- 62 次 SWD 写在理想 2 MHz 位时钟下仅约 1.4 ms。即使考虑 turnaround、ACK 和空闲周期，18 ms 仍说明软件执行、任务调度和无线收发占据了大部分时间，不能继续把问题简单归因于无线带宽。
- receiver 已使用 dedicated GPIO，但每个 SWD bit 的两个半周期都会读取 cycle counter 和全局周期值；每个独立 SWD transaction 还会进入和退出一次 FreeRTOS critical section。
- transmitter 的 Bulk 请求目前统一标记为 256 字节；receiver 虽然能得到 `dap_protocol_execute()` 的真实响应长度，却仍按请求长度回传。典型 62-word `DAP_TransferBlock` 写响应实际约 4 字节，但无线和 USB 路径当前都按 256 字节处理。
- 请求的 2 MHz 尚未由逻辑分析仪确认实际 SWCLK 波形。提高到 4/8/10 MHz 前，必须先测真实周期、占空比和 turnaround。

## 3. 总体实施原则

### 3.1 稳定基线

任何大改动前保留当前协议 v1 的可用构建。第一阶段完成后建立新的稳定基线，第二阶段所有性能数据都与该基线比较。

### 3.2 功能开关

建议为以下特性提供编译期或运行期配置：

- 无线 PHY 速率。
- 发送队列节流策略。
- SWD 默认时钟。
- DAP 逻辑包大小。
- CMSIS-DAP v1/v2 USB 后端。
- 无线协议版本。
- packet window 大小。

### 3.3 可靠性优先级

无线速度提高不能以重复执行 Flash 写操作为代价。所有重传、批量和窗口设计都必须保证：

- 请求有唯一序号。
- 响应可以准确匹配请求。
- ACK 丢失时不会重复执行已经完成的 SWD 写操作。
- 队列满、断线和超时时可以清理状态并重新建立会话。

## 4. 第一阶段：现有协议低风险优化

### 4.1 阶段目标

保持以下兼容性不变：

- CMSIS-DAP v1 HID。
- 64 字节 DAP packet。
- 当前协议 v1。
- 现有 OpenOCD 和其他 CMSIS-DAP 主机工具。
- 当前自动发现、心跳和断线重连行为。

第一阶段只优化发送调度、无线 PHY、重试、任务调度和 SWD 时钟。

### 4.2 第一阶段任务拆分

#### A. 性能测量和诊断

1. 在 transmitter 侧记录 USB 收包、DAP 入队、无线请求入队和响应返回时间。
2. 在 receiver 侧记录无线接收、DAP 开始执行、DAP 执行结束和响应入队时间。
3. 为每次 DAP 交换记录序号、总耗时、重试次数和失败原因。
4. 在 `send_cb()` 中统计 `ESP_NOW_SEND_SUCCESS` 和 `ESP_NOW_SEND_FAIL`。
5. 提供一个只用于测试的统计输出命令或周期性摘要，避免默认日志过多。

#### B. 移除固定发送节流

当前发送任务在每次 `esp_now_send()` 后固定等待 2 ms。改进方向：

1. 先将固定延迟改为可配置值，默认仍保持 2 ms 以便对照测试。
2. 测试 2、1、0 ms 三种配置。
3. 确认 `esp_now_send()` 返回 `ESP_ERR_ESPNOW_NO_MEM` 或类似忙状态时的退避策略。
4. 使用发送结果或发送完成信号决定是否继续发送，而不是无条件等待。
5. 确保不会因为移除延迟导致 Wi-Fi 驱动队列溢出。

验收重点：平均延迟下降，同时 ESP-NOW 发送失败率和 DAP 重试率不明显增加。

#### C. 配置 ESP-NOW PHY 速率

1. 在 peer 添加完成后调用 `esp_now_set_peer_rate_config()`。
2. 增加配置项，至少支持默认速率、11 Mbps 和 HT20 速率。
3. 记录不同速率下的 RSSI、发送失败率、重试率和有效吞吐。
4. 在较弱信号环境中保留回退到低速率的能力。
5. 不对广播发现帧和稳定单播帧强制使用同一个速率，必要时分别配置。

建议初始测试顺序：默认速率、11 Mbps、HT20 MCS2/MCS3，再测试更高 MCS。最高速率不一定是最佳速率，最终以稳定吞吐为准。

#### D. 自适应 DAP 重试

1. 将当前固定 45 ms 重试间隔改为可配置参数。
2. 区分“发送调用失败”和“请求或响应丢失”。
3. 优先使用发送回调和已观测的 P95 延迟决定下一次重试时间。
4. 建议初始测试 5、10、20、45 ms。
5. 对连续失败采用有限退避，防止无线拥塞时形成发送风暴。
6. 保留当前 receiver 的 inflight/cache 去重机制。

重试优化不能简单地把超时改成极小值。应以实际链路的 P99 延迟加安全余量为下限。

#### E. DAP/UART 队列隔离

1. 将 DAP 数据和 UART 数据从同一个无线发送队列中分离。
2. 为 DAP 保留高优先级发送路径。
3. UART 发送队列满时允许丢弃或覆盖旧数据，但不应阻塞 DAP。
4. 心跳和发现帧不能长期占用 DAP 发送资源。
5. 在 UART 连续大流量下重复执行 Flash 烧录测试。

#### F. SWD 时钟调优

1. 保持主机下发的 `DAP_SWJ_Clock` 能够动态生效。
2. 逐级测试 1、2、4、8 MHz。
3. 使用逻辑分析仪测量真实 SWCLK 周期、占空比和 SWDIO turnaround。
4. 记录不同目标芯片和不同连线长度下的 WAIT、FAULT 和 parity error。
5. 为不稳定目标保留低速回退配置。

建议将 2-4 MHz 作为第一阶段的候选默认范围，不应在没有逻辑分析和目标验证的情况下直接启用 8 MHz。

#### G. 低收益优化

在完成上述任务后再评估：

- 使用 `esp_crc32_le()` 替代逐位 CRC 实现。
- 减少不必要的帧清零和 memcpy。
- 优化任务栈、优先级和 CPU 核绑定。
- 检查 USB HID ready 等待是否产生额外 1 ms 延迟。

这些优化预计低于调度、PHY 和协议窗口优化的收益，不应优先于前面的任务。

### 4.3 第一阶段验收标准

第一阶段版本必须满足：

1. 现有 HID v1 主机工具仍可识别并完成连接、读 ID、擦除、写入、校验和复位。
2. 与原始基线相比，RAM `TransferBlock` 端到端吞吐有明确提升。
3. 实际 Flash 烧录时间下降，且不能仅靠关闭 verify 得到虚假的提升。
4. 连续烧录 10 次以上无偶发写入错误。
5. 无线弱信号下能正确重试并恢复。
6. UART 无流量和高流量两种情况下 DAP 都可以正常工作。
7. 失败率、重试率和 SWD 错误率在可接受范围内。

### 4.4 第一阶段交付物

- 第一阶段稳定固件。
- 性能统计和测试日志格式。
- 不同 PHY 速率、SWD 时钟和重试参数的测试结果。
- 推荐默认配置。
- 可回退到原始协议 v1 的构建配置。

## 5. 第二阶段：CMSIS-DAP v2 和大包传输

### 5.1 阶段目标

通过减少 DAP 无线往返次数，解决 64 字节单包串行协议的结构性限制。

建议目标配置：

```text
CMSIS-DAP v2 bulk USB
    -> 256/512/1024 byte logical DAP packet
    -> ESP-NOW v2
    -> receiver ordered execution
    -> 2-4 packet window
```

第二阶段第一版建议先实现 256 字节，而不是直接启用 1024 字节。256 字节更容易验证内存、主机兼容性和错误恢复；稳定后再评估 512/1024 字节。

### 5.2 第二阶段任务拆分

#### A. 协议 v2 设计

重新定义无线帧：

- 协议版本。
- 消息类型。
- 请求序号。
- packet window 序号或窗口确认号。
- payload 长度。
- DAP 包大小。
- CRC。
- 可选能力位。

协议 v2 必须支持能力协商，至少协商：

- ESP-NOW v2 是否可用。
- 最大无线 payload。
- 最大 DAP packet size。
- packet window 大小。
- 是否支持兼容聚合模式。

协议 v1 和 v2 应能通过配置或版本协商明确区分，不能让旧固件误解析新帧。

#### B. ESP-NOW v2 大帧

1. 将当前 192 字节 payload 限制扩展到协议允许的范围。
2. 先实现 256 字节 DAP payload。
3. 测试 512 和 1024 字节 payload。
4. 保证总帧长度不超过设备和 IDF 版本支持的限制。
5. 为大帧增加长度边界检查和内存分配失败处理。
6. 记录大帧的丢包率和发送耗时。

大包并不代表一定要使用动态堆分配。对于固定的 256/512/1024 模式，优先使用静态对齐缓冲区，避免运行时碎片。

#### C. Receiver 大包解析和执行

1. 将 `DAP_PACKET_SIZE` 从固定 64 改为可协商大小。
2. 调整 `dap_protocol_execute()`、请求缓冲区和响应缓冲区。
3. 检查 `DAP_TransferBlock` 在写入、读取和混合命令下的边界行为。
4. 保持 SWD 目标访问顺序不变。
5. 一个大包执行期间继续禁止其他任务打断 SWD 时序。
6. 对异常长度、非法命令和执行中断返回明确错误。

#### D. CMSIS-DAP v2 bulk USB

1. 增加 CMSIS-DAP v2 bulk endpoint。
2. 保留 CDC ACM UART 功能。
3. 提供 Windows WCID/WinUSB 描述符。
4. 在 Linux、macOS 和 Windows 上分别验证枚举和工具兼容性。
5. 对主机不支持大包的情况提供兼容模式。
6. 评估是否保留 HID v1 作为独立兼容接口。

USB v2 迁移应先用 64 字节逻辑 DAP 包验证，再启用 256/512/1024 字节，避免同时引入两个变量。

#### E. 64 字节兼容聚合模式

兼容模式的目标是：主机仍以 64 字节 USB 包工作，transmitter 在本地聚合多个 DAP 分片，再通过无线发送一个 256/1024 字节逻辑包。

需要处理：

- 分片边界。
- 最后一包不足 64 字节的判断。
- 主机填充字节。
- 响应拆分。
- 超时和中途中断。
- `DAP_TransferAbort`。

该模式可以降低对 OpenOCD 和其他主机工具的要求，是第二阶段的重要兼容层。

实现状态（2026-09-27）：已完成首版，默认关闭。协议 v2/256 字节配置下可公布三个 HID
packet slot，发送端使用标准 `DAP_ExecuteCommands` 聚合最多三个请求，默认等待 1 ms，
并按原顺序拆分、补零响应。实现已覆盖 Transfer、TransferBlock、SWJ Sequence 和 SWD
Sequence 的变长边界，排除本地、未知和嵌套批量命令；批量失败会逐请求返回错误。
`DAP_TransferAbort` 已使用高优先级独立无线帧和接收端中止任务，不等待普通请求队列。
软件构建验证完成后仍需按第 7 节在真实硬件上验证吞吐、中断时延和主机兼容性。

#### F. Packet window

1. 第一版仍然保持 receiver 顺序执行。
2. 先支持 window=2。
3. 稳定后测试 window=4。
4. 请求和响应使用独立序号，不能仅依靠“最近一个请求”匹配。
5. receiver 维护已接收、执行中、已完成和可重发状态。
6. Flash 写入相关请求必须保证顺序，不能因为无线重排而改变目标访问顺序。

不建议第一版直接让多个任务并行调用 SWD。SWD 本身是串行总线，真正收益主要来自提前传输和减少空闲，而不是并行 GPIO 操作。

实现状态（2026-09-27）：已完成 window=2 软件首版，默认仍为 window=1。协议 v2 复用
帧头 metadata 编码窗口数量和索引，每项使用连续但独立的 sequence。发送端同时发送最多
两个 HID64/Bulk 请求、接收乱序响应并按 USB 原顺序返回，只重发未完成项；接收端分别维护
received、queued/executing、completed/cache 状态，严格按 index 顺序执行并缓存两项响应，
避免重传重复执行 Flash 写入。Abort、断线和本地命令顺序屏障已接入。window=2 与 HID64
聚合首版互斥，已通过默认 v1/64 和 v2/256/window=2 两端软件构建。

真机状态（2026-09-28）：Linux/OpenOCD、Bulk256、window=2、11 Mbps PHY、2 MHz SWD
配置已完成连续 10 次 STM32F412 512 KiB 全量擦除、写入和校验，10/10 通过，未出现
command mismatch、OpenOCD Error、乱序或重复写入。最终独立回读与原始备份逐字节一致。
测试期间发现 OpenOCD 会发送长度恰好为 USB endpoint 整数倍的变长请求，例如包含 25 个
AP 写事务的 `DAP_Transfer` 长度为 128 字节；此类 Bulk OUT 传输没有短包边界。transmitter
现已按 CMSIS-DAP 命令结构解析请求长度，不再依赖 USB 短包或 ZLP，并要求 Vendor RX/TX
FIFO 能容纳公布的整个 packet window。丢帧注入、弱信号、UART 高负载和中止时延仍需按
第 7 节继续验证。

#### G. 大包可靠性和断线恢复

测试以下情况：

- 请求帧丢失。
- 响应帧丢失。
- ACK 丢失。
- Receiver 忙。
- 大帧 CRC 错误。
- 主机在 Flash 写入期间发送 abort。
- 任一 ESP32 复位。
- 配对超时并重新发现。

所有情况都必须保证不会出现“主机认为失败、目标已经写入，但重试再次执行同一写入”的危险状态。

### 5.3 第二阶段验收标准

1. CMSIS-DAP v2 bulk 能被主机工具稳定识别。
2. 256 字节逻辑包可以完成完整烧录、校验和复位。
3. 64 字节兼容模式可用。
4. 512/1024 字节模式在目标、无线和工具支持时可选启用。
5. packet window=2 在连续烧录中不产生乱序或重复写入。
6. 无线重传、断线和重连不会导致协议死锁。
7. 与第一阶段基线相比，RAM TransferBlock 和实际 Flash 烧录都取得可测量提升。
8. 目标 Flash 编程时间成为主要瓶颈时，能够通过统计数据确认，而不是误判为无线链路问题。

### 5.4 第二阶段交付物

- 协议 v2 文档。
- CMSIS-DAP v2 USB 描述符和兼容模式。
- ESP-NOW v2 大包传输实现。
- 256 字节稳定模式。
- 可选 512/1024 字节模式。
- packet window=2 的稳定实现。
- 跨平台枚举和工具兼容性测试记录。
- 与第一阶段的吞吐、延迟和可靠性对比报告。

## 6. 第三阶段：对标 ST-Link 的架构升级

### 6.1 调研结论

本轮调研选取了四个公开 ESP32-S3 无线调试器实现和 OpenOCD 原生 TCP backend。结论是：
公开项目中可以找到值得借鉴的架构和局部实现，但截至调研 commit，没有找到与本项目同一
目标、同一 512 KiB 镜像、包含 erase/write/verify 且可复现的“超过 ST-Link”数据。因此，
不能把 10/40 MHz 标称 SWD 时钟、USB packet size 或产品宣传直接当作烧录吞吐证据。

| 方案 | 主机到目标链路 | 包和并发策略 | SWD 实现 | 可借鉴点和限制 |
|---|---|---|---|---|
| AmphiLink (`2d40f0a`) | 单块 ESP32-S3，Wi-Fi TCP 直连 OpenOCD | 64 字节，Packet Count=8，支持 CMSIS-DAP 队列命令 | GPIO/IO_MUX 寄存器直访，默认 10 MHz | 双核分工、`TCP_NODELAY`、性能优化构建值得借鉴；未提供同口径 Flash 基准 |
| windowsair/wireless-esp8266-dap (`4c56d0d`) | 单板 Wi-Fi，USB/IP、ElaphureLink、WebSocket 等主机路径 | WinUSB 逻辑值 512/255，内部约 20 包队列 | ESP32-S3 高频路径用 SPI 外设实现 3-wire SWD，标称 40 MHz | 硬件 SWD 最激进，但 README 也指出 TCP 是主要瓶颈；主机路径和测试口径不同 |
| ESP32S3_Wireless_CMSIS_DAP (`a8fba96`) | 双 ESP32-S3 + ESP-NOW v2 | 支持 64/256/1024 和兼容聚合；每包先等 ACK，再等 DAP 响应 | receiver 热路径使用通用 GPIO API | 大包、CRC32 和重复请求保护可参考；stop-and-wait 调度及 GPIO 热路径不优于本项目，无可信基准 |
| NexLink (`4b9f902`) | 双 ESP32-S3 + ESP-NOW | 64 字节，Packet Count=8；每个请求等待响应并最多重试三次 | dedicated GPIO，双核分工 | GPIO 和任务划分可参考；无线仍是逐请求响应，仓库 `ISSUES.md` 仍列有队列和稳定性问题 |
| 本项目当前实测 | 双 ESP32-S3，USB Bulk + ESP-NOW v2 | 256 字节，window=2 | dedicated GPIO 软件 SWD，请求 2 MHz | 已有同口径 10/10 基线；512 KiB 写入 54.160 s，仍比 ST-Link/V2 慢约 3.98 倍 |

市场方案真正共同采用的有效方向是：性能优化构建、主机请求流水、减少协议中间层、直接
GPIO 或外设辅助 SWD。它们不能证明某一个参数单独就能超过 ST-Link。本项目应先消除已经
量化的软件和协议浪费，再决定是否增加新传输模式。

### 6.2 P0：测量与低风险热路径优化

P0 是第三阶段的下一项工作，必须先于 1024 字节大包实施。

1. 增加可关闭的聚合统计，按测试阶段输出摘要而不是逐包刷日志：USB 入队等待、TX 无线发送、RX 收包、`dap_protocol_execute()`、响应无线发送和 USB IN 完成时间，以及 P50/P95/P99、失败和重试计数。
2. 用逻辑分析仪分别测 2/4/8/10 MHz 请求值下的真实 SWCLK、占空比、SWDIO turnaround 和连续 62-write 包的总线占用时间。
3. 将 transmitter 和 receiver 分别切换为 `CONFIG_COMPILER_OPTIMIZATION_PERF=y`，保持所有运行参数不变做 A/B；先测 receiver，再测双端，避免无法归因。
4. 协议帧携带真实 request/response length。receiver 使用 `dap_protocol_execute()` 返回值发送短响应，transmitter 只在 USB 边界按 CMSIS-DAP 主机要求补齐；缓存、CRC、重传和 response matching 全部使用真实长度。
5. 检查 SWD 延时函数的调用开销：内联固定热路径、将半周期值缓存到局部或寄存器，并补偿读取 cycle counter 和循环本身的固定成本。
6. 测量每 transaction 一次 critical section 的成本；若占比明显，将互斥范围提升到完整 `DAP_Transfer`/`DAP_TransferBlock`，同时验证 Abort 响应时间和 Wi-Fi 看门狗。
7. 每个单项修改都执行同一 512 KiB erase/write/verify 10 次，并做独立回读 SHA256；任何失败都不得用平均速度掩盖。

P0 的第一个决策点是把典型 62-write 包约 18 ms 拆成可解释的时间组成。若统计显示
`dap_protocol_execute()` 占主导，则优先 SWD 热路径；若无线/队列占主导，则优先短响应和
调度。P0 完成后，512 KiB 平均写入必须至少降低 20%，否则应停止主观调参并重新检查测量。

### 6.3 P1：Bulk1024 与 window=4

P1 必须在短响应完成后按单变量顺序推进：

1. 扩展能力位、静态缓冲、TinyUSB FIFO、帧长度校验和 receiver slot，使逻辑 DAP 包支持 1024 字节。
2. 先测 Bulk1024/window=2；稳定并取得明确收益后再单独启用 window=4。
3. 1024 字节 `DAP_TransferBlock` 可容纳约 254 个 word，256 字节只能容纳约 62 个，单包数据事务约提升 4.1 倍；但响应必须保持真实短长度，否则每个约 4 字节写响应会被放大到 1024 字节。
4. 验证 ESP-NOW v2 大帧在近距离、中距离、弱信号和 2.4 GHz 干扰下的丢包率、重传成本与重复执行保护。
5. 保持 receiver 严格顺序执行；window=4 只用于覆盖 USB/无线等待，不并行访问 SWD。

P0+P1 的阶段目标是把 512 KiB 平均写入降到 20 s 以内。若 1024/window2 没有显著收益，
不得直接继续扩大窗口，应根据统计确认限制来自目标 Flash 算法还是 SWD 执行。

### 6.4 P2：单板 Wi-Fi TCP 模式

在 receiver 上增加可选的 OpenOCD CMSIS-DAP TCP 服务，使 PC 通过 Wi-Fi 直接连接目标侧
ESP32-S3，绕过 USB transmitter、ESP-NOW 第二跳和两端协议转换。现有双板 ESP-NOW 模式
继续保留，用于无需接入局域网、低配置和兼容场景。

OpenOCD 当前开发版 `cmsis_dap_tcp` backend 使用 8 字节 `DAP\0` 帧头、最大 1024 字节
payload 和 `TCP_NODELAY`，CMSIS-DAP 核心会查询 Packet Size/Count 并流水发送。建议首版
公布 Packet Size=1024、Packet Count=4，再测试 8。该模式依赖较新的 OpenOCD；稳定版
0.12.0 不具备同等 backend，因此必须明确最低主机版本和连接命令。

TCP 模式不能预设一定快于 ESP-NOW。普通 Wi-Fi TCP 有重传、拥塞和调度抖动，必须使用同一
STM32F412、同一镜像、同一 SWCLK 和完整 write+verify 流程与双板模式 A/B。

### 6.5 P3：SPI/GDMA 辅助 SWD

若 P0/P1 后统计仍显示 `dap_protocol_execute()` 和 GPIO bit-bang 占主要时间，则参考
windowsair 的 ESP32-S3 GPSPI2 3-wire SWD 路径做原型：

- 先加速长连续数据阶段，连接、line reset、turnaround、WAIT/FAULT 和异常恢复保留 GPIO 路径。
- 建立 GPIO 与 SPI 路径的自动回退和逐 transaction 结果一致性测试。
- 测试 4/8/10/20 MHz 的真实波形、ACK 采样边沿、线长容限和目标兼容性。
- 优先复用成熟实现思路，不直接从零实现完整 RMT SWD 状态机。

这是高收益但高风险的工作。只有测量证明软件 SWD 是主要瓶颈后才投入，不能因竞品标称
40 MHz 就跳过 P0/P1。

### 6.6 P4：可选专用 Flash 高速模式

若标准 CMSIS-DAP 的逐 transaction 交互仍限制吞吐，可增加目标相关的高层 Flash block/
loader 协议：在 receiver 或目标 RAM loader 一次处理更大的编程块，减少 OpenOCD 与探针之间
的命令往返。该方案潜力最大，但需要 OpenOCD 插件或自定义 backend，并且依赖目标芯片族。
它只能作为显式可选高速模式，标准 CMSIS-DAP 路径必须继续保留。

### 6.7 第三阶段验收门槛

| 门槛 | 要求 |
|---|---|
| 测量门槛 | 典型 62-write 包约 18 ms 已拆解，真实 SWCLK 已由逻辑分析仪记录 |
| P0 最低收益 | 512 KiB 平均写入相对 54.160 s 至少降低 20% |
| P0+P1 阶段目标 | 512 KiB 平均写入不超过 20 s |
| 进阶目标 | 不超过 ST-Link/V2 基线的 1.3 倍，即约 17.7 s |
| 最终目标 | 同口径达到或优于 13.595 s |
| 可靠性 | 每个候选配置连续 10/10 erase/write/verify，独立回读 SHA256 一致 |

每份结果必须注明目标型号、镜像大小、计时是否包含 erase/write/verify、实际测得 SWCLK、
DAP 包大小、窗口、无线模式、信号条件、构建优化级别和 OpenOCD 版本。未满足这些字段的
结果只能作为线索，不能作为默认配置或“超过 ST-Link”的依据。

## 7. 测试矩阵

### 7.1 传输参数

| 维度 | 测试值 |
|---|---|
| 发送节流 | 2 ms / 1 ms / 0 ms |
| ESP-NOW 速率 | 默认 / 11 Mbps / HT20 MCS2 / HT20 MCS3 |
| SWD 时钟 | 1 / 2 / 4 / 8 / 10 MHz；硬件辅助原型另测 20 MHz |
| DAP 包大小 | 64 / 256 / 512 / 1024 字节 |
| packet window | 1 / 2 / 4 |
| DAP 响应长度 | 固定包长 / 真实有效长度 |
| 主机后端 | HID v1 / USB bulk v2 / TCP |
| 构建优化 | Debug `-Og` / Performance `-O2` |
| UART 负载 | 无 / 中等 / 连续高流量 |
| 无线环境 | 近距离 / 中距离 / 有明显 2.4 GHz 干扰 |

### 7.2 功能测试

- DAP_Info、Connect、Disconnect。
- SWD IDCODE 读取。
- DP/AP 单次读写。
- DAP_TransferBlock 连续读。
- DAP_TransferBlock 连续写。
- Flash erase、program、verify、reset。
- 主机中止传输。
- 目标复位和重新连接。
- UART 双向传输。
- 任一端断电后的自动恢复。

### 7.3 性能测试

至少记录：

- 单次请求延迟。
- 每秒 DAP packet 数量。
- 每秒有效 SWD 写入字节数。
- Flash 实际写入 KB/s。
- 完整烧录耗时。
- 重试率。
- 错误率。
- 目标 Flash 等待时间占比。

### 7.4 2026-09-28 真机测试结果

测试链路：Linux OpenOCD、CMSIS-DAP v2 Bulk、256 字节 DAP 包、packet window=2、
ESP-NOW 11 Mbps、TX 固定延迟 0 ms、SWD 2 MHz，目标为 STM32F412 512 KiB Flash。

| 项目 | 结果 |
|---|---|
| Bulk 协商 | Packet Size=256，Packet Count=2 |
| 无复位重连 | 连续两次成功 |
| 64 KiB 写入（含自动擦除） | 7.863 s，8.139 KiB/s |
| 64 KiB 校验 | 0.816 s，78.431 KiB/s |
| 512 KiB 连续写入，10 次平均 | 54.160 s，9.453 KiB/s |
| 512 KiB 连续写入范围 | 53.765-54.624 s |
| 512 KiB 连续校验，10 次平均 | 4.065 s |
| 512 KiB HID 同条件写入 | 193.548 s，2.645 KiB/s |
| Bulk/HID 写入吞吐比 | 约 3.57 倍，写入时间减少约 72.0% |
| 完整 write+verify 流程 | HID 201.25 s，Bulk 平均约 61.4 s，约 3.28 倍 |
| 512 KiB Bulk 回读 | 约 41.4 s，约 12.4 KiB/s |
| 512 KiB HID 回读基线 | 94.629 s，5.411 KiB/s |
| Bulk/HID 回读吞吐比 | 约 2.28 倍 |
| ST-Link/V2 512 KiB 写入参考 | 13.595 s，37.661 KiB/s |
| ST-Link/V2 512 KiB 校验参考 | 3.751 s，136.497 KiB/s |
| ST-Link/V2 512 KiB 回读参考 | 4.830 s，106.004 KiB/s |
| ST-Link/Bulk 写入吞吐比 | ST-Link 约为 Bulk 的 3.98 倍 |
| 连续可靠性 | 10/10 全量 erase/write/verify 通过 |
| 数据完整性 | 最终回读 SHA256 与备份一致 |
| 备份 SHA256 | `6c3a7863ae558124c58f10ac8863ebe5193bbab6ca8d76febb93507f74f47ce5` |

以上结果确认 Bulk256 已消除 HID64 的主要 USB/无线往返瓶颈。同一 2 MHz SWD 配置下，
Bulk 将 512 KiB 实际写入从约 3 分 14 秒缩短到约 54 秒；verify 仅从 4.232 秒缩短到约
4.065 秒，说明 verify 主要由目标端算法执行，不受逐包往返限制。实际 Flash 写入吞吐仍低于
纯回读吞吐，后续提速应分别评估目标 Flash 算法、window 收集策略和更大逻辑包，不能仅用
SWD 时钟变化推断收益。

ST-Link 对照使用同一目标、同一备份文件和相同的 2 MHz 请求值；ST-Link/V2 实际选择其
最接近的 1.8 MHz 档位。即使实际 SWD 时钟略低，有线写入仍约为无线 Bulk 的 3.98 倍，
说明后续性能空间主要仍在无线请求往返、DAP 执行调度和数据搬运，而不是单纯提高 SWCLK。

## 8. 版本和回滚策略

建议采用以下版本标识：

- `protocol v1 + HID64`：当前稳定基线。
- `protocol v1 + low-latency`：第一阶段版本。
- `protocol v2 + bulk64`：第二阶段 USB/协议验证版本。
- `protocol v2 + bulk256`：第二阶段首个可用大包版本。
- `protocol v2 + bulk1024 + window2/4`：第三阶段双板高性能候选版本。
- `CMSIS-DAP TCP + packet1024`：第三阶段单板 Wi-Fi 候选版本。

回滚时必须保证 transmitter 和 receiver 使用匹配的协议版本。协议协商失败时，应明确报错或回退到 v1，不应尝试猜测帧格式。

## 9. 风险和应对措施

| 风险 | 影响 | 应对措施 |
|---|---|---|
| 提高 PHY 后丢包增加 | 吞吐反而下降 | 测试失败率，支持速率回退 |
| 去掉 2 ms 延迟导致 Wi-Fi 队列拥塞 | 随机超时 | 发送回调、有限队列和退避 |
| SWD 频率过高 | 目标读写错误 | 逻辑分析仪验证，提供低速配置 |
| 大包超过设备兼容范围 | 帧丢失或截断 | 明确协商版本和最大 payload |
| USB v2 Windows 驱动问题 | 主机无法识别 | 使用 WCID/WinUSB，保留兼容接口 |
| packet window 导致重复写入 | Flash 数据损坏 | 序号、状态机和 receiver 去重 |
| UART 队列抢占 DAP | 烧录速度下降 | 独立队列和 DAP 优先级 |
| Flash 编程时间本身占主导 | 无线优化收益有限 | 分离测量 erase/write/verify 时间 |
| 固定长度响应抵消大包收益 | 反向链路传输大量填充字节 | 先实现真实响应长度，再启用 1024 字节 |
| `-Og` 与性能构建行为不同 | 提速同时引入时序或内存问题 | 单端、双端分步 A/B，并执行 10/10 完整验证 |
| TCP 模式依赖开发版 OpenOCD | 用户环境无法连接 | 明确最低版本，保留 USB + ESP-NOW 模式 |
| SPI/GDMA SWD 边沿或 turnaround 错误 | 隐蔽读写错误或目标不兼容 | GPIO 回退、逻辑分析和多目标一致性测试 |

## 10. 推荐执行顺序

### 第一阶段执行顺序

1. 建立基线和统计。
2. 移除固定 2 ms 延迟并测试发送拥塞。
3. 配置 ESP-NOW PHY 速率。
4. 调整重试策略。
5. 分离 DAP/UART 队列。
6. 调整 SWD 时钟。
7. 完成连续烧录和断线恢复测试。
8. 冻结第一阶段稳定版本。

### 第二阶段执行顺序

1. 设计并冻结协议 v2 帧格式。
2. 先实现 bulk USB + 64 字节逻辑包。
3. 迁移 ESP-NOW v2，但保持 packet window=1。
4. 实现 256 字节大包。
5. 增加 64 字节兼容聚合模式。
6. 验证 512/1024 字节。
7. 增加 window=2。
8. 完成断线、丢包、重复执行和跨平台测试。
9. 根据数据决定是否启用 window=4 和 1024 字节默认模式。

### 第三阶段执行顺序

1. 为约 18 ms 的典型满包建立聚合分段统计，并用逻辑分析仪确认实际 SWCLK。
2. receiver 性能优化构建 A/B，再做双端性能构建 A/B。
3. 实现无线真实 request/response length 和 USB 边界补齐，完成 10/10 真机验证。
4. 优化 SWD delay 与 critical section，每项单独 A/B。
5. 实现 Bulk1024/window2，确认收益后再测试 window=4。
6. 根据瓶颈占比决定进入单板 TCP 或 SPI/GDMA SWD；两者不同时首发。
7. 只有标准 CMSIS-DAP 仍无法接近目标时，才评估专用 Flash 高速模式。

下一项明确工作为第 1-3 步，即“测量、性能构建、短响应”。在得到这三项的独立结果前，
不应直接把默认配置改为 1024/window4。

## 11. 最终推荐目标

推荐的最终高性能配置为：

```text
CMSIS-DAP v2 bulk
    + 1024 字节逻辑 DAP 包（256 字节稳定回退）
    + ESP-NOW v2
    + 2 或 4 个请求窗口
    + 真实 request/response length
    + 稳定的 11 Mbps 或 HT20 PHY 速率
    + 经逻辑分析仪确认的 4-10 MHz SWD
    + performance 构建和低开销 SWD 热路径
    + 自适应重试
    + DAP/UART 独立队列
    + 64 字节兼容聚合模式
```

单板产品形态另提供 `CMSIS-DAP TCP + Packet Size=1024 + Packet Count=4/8` 模式，是否成为
最终默认方案由同口径 A/B 决定。若目标 Flash 的 erase/program 时间占比已经很高，继续增加
无线包大小或 packet window 的收益会降低；此时应优先优化目标端 Flash 算法和主机端 verify
策略，而不是继续提高无线速率。

## 12. 参考资料

- [Espressif ESP-NOW Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp_now.html)
- [CMSIS-DAP 官方文档](https://arm-software.github.io/CMSIS-DAP/latest/)
- [CMSIS-DAP 官方 DAP_config.h](https://github.com/ARM-software/CMSIS-DAP/blob/main/Firmware/Config/DAP_config.h)
- [同源 ESP32-S3 Wireless CMSIS-DAP 上游实现](https://github.com/SiYue-ZO/ESP32S3_Wireless_CMSIS_DAP)
- [AmphiLink，调研 commit `2d40f0a`](https://github.com/danjinghaoeggggg/AmphiLink--A-Dual-Mode-Wired-Wireless-Debugger/tree/2d40f0a2666bf589ebb59df10086a7d2ac071153)
- [windowsair/wireless-esp8266-dap，调研 commit `4c56d0d`](https://github.com/windowsair/wireless-esp8266-dap/tree/4c56d0d4e853e9785b4e6cbbb55d3da20d278e13)
- [masbc666/ESP32S3_Wireless_CMSIS_DAP，调研 commit `a8fba96`](https://github.com/masbc666/ESP32S3_Wireless_CMSIS_DAP/tree/a8fba964f8dad21b0a97390c19204130c924cf64)
- [Miraitowa-la/NexLink，调研 commit `4b9f902`](https://github.com/Miraitowa-la/NexLink/tree/4b9f902ad1aaf0d6d391dd87fff48f9936fd6403)
- [OpenOCD `cmsis_dap_tcp` backend，调研 commit `21f88d7`](https://github.com/openocd-org/openocd/blob/21f88d7815edfb7632548090934c51017604f450/src/jtag/drivers/cmsis_dap_tcp.c)
- [当前项目无线协议说明](./docs/protocol.md)
