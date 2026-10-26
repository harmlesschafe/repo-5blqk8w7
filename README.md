# 第七次课 ESP32 与 MQTT 双向通信练习

> 学号：2022030518　姓名：姚佳

电位器 JSON 上报 → MQTTX 订阅；MQTTX 发布命令 → ESP32 控制共阴极 RGB LED；附 Topic 隔离、故障排查与选做 status 主题。

## 目录

```
esp32_mqtt/
├─ platformio.ini        PlatformIO 配置（esp32dev + PubSubClient）
├─ src/main.cpp          主程序
├─ src/rgb_command.h     RGB 命令解析（按 length 读取、转大写、未知命令拒绝）
├─ test/host_test.cpp    电脑端单元测试
├─ diagram.json          Wokwi 接线（电位器 + 共阴极 RGB LED + 3×220 Ω）
└─ wokwi.toml            Wokwi 固件路径
build_logs/              编译记录与单元测试输出
screenshots/mqttx/       MQTTX 截图（见 screenshots/截图说明.md）
screenshots/wokwi/       Wokwi 截图（见 screenshots/截图说明.md）
```

## 一、概念说明

| 角色 | 本练习中的对象 | 作用 |
| --- | --- | --- |
| Publisher | ESP32（发布 pot）、MQTTX（发布 rgb/set） | 把消息发到某个 Topic |
| Subscriber | MQTTX（订阅 pot）、ESP32（订阅 rgb/set） | 接收所订阅 Topic 上的消息 |
| Broker | broker.emqx.io:1883 | 负责按 Topic 转发，发布者和订阅者互不直连 |
| Topic | `iot/lesson7/esp32/2022030518/pot` 等 | 分层地址，含学号保证不同同学互相隔离 |
| Message | `{"raw":2048,"percent":50}`、`RED` | 实际载荷（Payload） |
| QoS | 0 | “最多一次”，不确认、不重发 |

## 二、第一部分 个人化配置

| 配置项 | 值 |
| --- | --- |
| 电位器 Topic | `iot/lesson7/esp32/2022030518/pot` |
| RGB 控制 Topic | `iot/lesson7/esp32/2022030518/rgb/set` |
| status Topic（选做） | `iot/lesson7/esp32/2022030518/status` |
| ESP32 Client ID | `esp32-2022030518` |
| MQTTX Client ID | `mqttx-2022030518` |

代码中只需修改 `src/main.cpp` 的 `#define STUDENT_ID`，三个 Topic 与 Client ID 由宏自动拼接，避免手抄出错。

接线：电位器 VCC/GND→3V3/GND，SIG→GPIO34；RGB R/G/B→GPIO25/26/27（各串 220 Ω），COM→GND（共阴极，HIGH 点亮）。

## 三、第二部分 MQTTX 链路自测

| 检查项 | 操作 | 结果 |
| --- | --- | --- |
| Broker 连接 | Host broker.emqx.io（手机网页版用 WSS 8084，Path `/mqtt`），用户名/密码留空，Client ID mqttx-2022030518 | Connected（截图中可见 Connected 提示） |
| Topic 订阅 | 订阅 `iot/lesson7/esp32/2022030518/pot`，QoS 0 | 订阅成功，收到 ESP32 发布的 JSON（mqttx/04_pot_mid_received.png） |
| 链路自测 | 向该 Topic 发布 Plaintext `hello-2022030518` | 未截图 |

## 四、第三部分 电位器 JSON 上报

程序每 1 s 执行 `analogRead(34)`，用 `map(raw, 0, 4095, 0, 100)` 换算百分比，以 QoS 0、Retain false 发布 JSON。串口依次出现 `MQTT connected`、`Subscribe ... OK`、`Publish sent (QoS 0)`。

| 测试组 | 电位器位置 | MQTTX 接收结果 |
| --- | --- | --- |
| 1 | 低位 | raw 0，percent 0 |
| 2 | 中位 | raw 2048，percent 50（MQTTX 实收 `{"raw":2050,"percent":50}`） |
| 3 | 高位 | raw 4095，percent 100 |

数据来自 wokwi-cli 实际仿真（`screenshots/wokwi/serial_logs/run1_startup_pot_low_mid_high.txt`），电位器由仿真脚本依次设到 0、0.5、1.0；对应截图 `screenshots/wokwi/03~05`，MQTTX 端截图 `screenshots/mqttx/04_pot_mid_received.png`。

**QoS 0 的影响：** 消息“最多送达一次”，Broker 和客户端都不确认、不重发，开销最小；网络抖动时可能丢掉个别读数，但每秒都有新数据，丢一条影响不大，适合周期性传感器数据。

**Retain false 的影响：** Broker 不保存最后一条消息。MQTTX 订阅后要等 ESP32 下一次发布（≤1 s）才能看到数据；设备离线后，新订阅者不会收到旧的、可能已过期的读数。

## 五、第四部分 RGB LED 远程控制

向 `iot/lesson7/esp32/2022030518/rgb/set` 发布 Plaintext，QoS 0，Retain 关闭。回调按 `length` 读取 Payload（Payload 不以 `\0` 结尾），去掉首尾空白并转换为大写后比较；未知命令保持原状态并输出 `Command rejected`。

| 测试内容 | 预期结果 | 实际结果 |
| --- | --- | --- |
| RED | 仅红色通道点亮 | **仿真实测**：仅 R 点亮（LED 像素 255,0,0），串口 `Command accepted: RED -> R=1 G=0 B=0` |
| GREEN | 仅绿色通道点亮 | **仿真实测**：仅 G 点亮，串口 `Command accepted: GREEN -> R=0 G=1 B=0` |
| BLUE | 仅蓝色通道点亮 | 未完成仿真实测（MQTTX 被挤下线，见第六节）；单元测试 PASS，`R=0 G=0 B=1` |
| WHITE | 三个通道同时点亮 | 未完成仿真实测；单元测试 PASS，`R=1 G=1 B=1` |
| OFF | 三个通道全部熄灭 | 未完成仿真实测；单元测试 PASS，`R=0 G=0 B=0` |
| YELLOW 或中文 | 拒绝未知命令并输出日志 | 未完成仿真实测；单元测试 PASS（YELLOW、红色均被拒绝，状态不变） |

实测中手机 MQTTX 发出的 Payload 带有换行/空格（RED 为 7 字节、GREEN 为 8 字节），程序去掉首尾空白后仍正确识别，说明“按 length 读取并规范化”是必要的。日志见 `screenshots/wokwi/serial_logs/run2_rgb_RED_GREEN.txt`，截图 `screenshots/wokwi/06_led_red.png`、`07_led_green.png`。

其余命令的逻辑已在电脑端单元测试中逐条验证（含小写 `red`、带空格 `  Blue  `、中文、空消息、按 length 截取），见 `build_logs/host_unit_test.txt`，全部 PASS。

## 六、第五部分 Topic 隔离与故障排查

**Topic 隔离（未完成实测）：** 按设计，向 `iot/lesson7/esp32/20260102/rgb/set`（另一学号）发布 `RED` 时本组 LED 不应变化，因为 ESP32 只订阅了本人 Topic，Broker 不会转发其他 Topic 的消息。旁证：三次仿真全程（约 15 分钟）串口只出现过发往本人 Topic 的两条消息，没有收到任何其他 Topic 的消息。

**故障记录一（实际遇到：Client ID 重复）**

| 项目 | 记录 |
| --- | --- |
| 现象 | 手机 MQTTX 反复掉线，发送时提示 `Client not connected`；第三次仿真 5 分钟内 ESP32 一条命令都没收到 |
| 证据 | MQTTX 弹出 `The Broker has actively disconnected, Reason: Session taken over (Code: 142)`（`screenshots/mqttx/fault_clientid_session_taken_over_1/2.png`）；ESP32 串口同期一直在线且无 `Message arrived`（`screenshots/wokwi/serial_logs/run3_no_command_mqttx_kicked.txt`） |
| 原因 | 另一个客户端（另一标签页 / 设备上的 MQTTX）使用了同一个 Client ID `mqttx-2022030518`。MQTT 规定同一 Client ID 只能有一个会话，新连接会接管会话并踢掉旧连接，两端自动重连后互相挤掉。ESP32 使用 `esp32-2022030518`，未受影响 |
| 修复 | 关闭重复的客户端，并把手机端 Client ID 改为唯一的 `mqttx-2022030518-m` 后重新连接 |

**故障记录二（代码演示：Topic 写错一个层级）**

制造方法：把 `src/main.cpp` 中的 `#define FAULT_DEMO 0` 改为 `1`，订阅 Topic 变为 `iot/lesson7/2022030518/rgb/set`（缺少 `esp32` 层级）。该版本同样可以编译通过，见 `build_logs/build_fault_demo_topic.txt`。

| 项目 | 记录 |
| --- | --- |
| 现象 | MQTTX 发送记录中能看到 RED/BLUE 已发出，但 RGB LED 不变化；电位器数据仍正常上报 |
| 日志 | 串口显示 `Subscribe iot/lesson7/2022030518/rgb/set (QoS 0): OK`，发送命令后没有任何 `Message arrived` 输出 |
| 原因 | 订阅时 SUBACK 只表示 Broker 接受了订阅请求，并不校验 Topic 是否“正确”。逐层比较发布 Topic `iot/lesson7/esp32/2022030518/rgb/set` 与订阅 Topic `iot/lesson7/2022030518/rgb/set`，第 3 层 `esp32` 缺失，两者不匹配，Broker 不转发 |
| 修复 | 改回 `FAULT_DEMO 0`（订阅 `TOPIC_RGB`），重新 Build 并启动仿真；串口订阅 Topic 与 MQTTX 发布 Topic 完全一致，发送 RED 后立即出现 `Command accepted: RED`，LED 变红。已恢复正确配置 |

其他故障的预防措施也已写在代码里：

- **Client ID 重复**：ESP32 用 `esp32-学号`，MQTTX 用 `mqttx-学号`，两者不同。
- **重连后没有重新订阅**：`subscribe()` 放在 `connectMqtt()` 中每次连接成功之后，掉线重连会自动重新订阅。
- **ADC 接线**：GPIO34 只能作输入且属于 ADC1，WiFi 工作时也能正常读取；raw 恒为 0 或 4095 时检查 SIG/3V3/GND。

## 七、选做任务 status Topic

`connect()` 时设置遗嘱：Topic `.../status`，QoS 0，Retain true，内容 `offline`；连接成功后立刻发布 Retain=true 的 `online`。

- 仿真运行时，新订阅 `.../status` 的客户端立即收到保留消息 `online`。
- 停止仿真后，ESP32 没有发送 DISCONNECT，Broker 在 keepalive 超时（PubSubClient 默认 15 s，约 1.5 倍后判定）后代发遗嘱 `offline`。
- 再新建订阅者订阅 `.../status`，会立即收到最后一条保留消息 `offline`。

## 八、本地调试与验收

| 检查项 | 结果 |
| --- | --- |
| 个人化配置 | Topic 含学号，两个 Client ID 不同 |
| 工程编译 | `pio run` 输出 `[SUCCESS]`，生成 `firmware.bin`（见 `build_logs/build_success.txt`；RAM 14.4%，Flash 73.9%） |
| 命令逻辑 | 电脑端单元测试全部通过（`build_logs/host_unit_test.txt`） |
| Wokwi 仿真 | wokwi-cli v0.29.1 实际运行 3 次：启动、MQTT 连接、电位器三组数据、RED/GREEN 控制已实测 |
| 未完成项 | BLUE/WHITE/OFF/非法命令的仿真实测、跨学号隔离实测（免费版单次仿真限 5 分钟 + MQTTX Client ID 冲突） |

## 运行步骤

1. VS Code 打开 `esp32_mqtt/`，PlatformIO: Build。
2. `F1 → Wokwi: Start Simulator`（WiFi 为 `Wokwi-GUEST`，代码中已配置）。
3. MQTTX 按第二部分连接并订阅，按第三到五部分测试。
