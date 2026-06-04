# 后端接口说明

本文档说明当前 UI/API 层应如何调用后端事件接口，并记录当前硬件接线与引脚分配。

## 调用边界

UI 和 API 代码应该通过下面两个接口发送应用层事件：

```c
app_event_send(app_event_id_t id, int param);
app_event_send_text(app_event_id_t id, const char *text_arg);
```

UI/API 层不要直接调用底层驱动接口：

- `i2s_output_*`
- `pcf8574_*`
- 原始 I2S driver API
- 原始 PCF8574 driver API

事件层会统一把事件分发到音频播放服务和香氛控制服务。

## 硬件接线与引脚分配

本项目后端当前涉及三类硬件连接：I2C 总线、I2S 音频输入/输出、HE30 三路香氛/雾化控制。UI/API 层只需要调用事件接口，但硬件调试和接线时需要参考本节。

### 1. I2C 总线接线

IO8 / IO9 是 I2C 总线：

| 功能 | ESP32-S3 引脚 | 说明 |
|---|---|---|
| I2C SDA | GPIO8 | I2C 数据线 |
| I2C SCL | GPIO9 | I2C 时钟线 |

当前 I2C 总线上连接：

- AHT20 / SHT31 温湿度传感器
- PCF8574 / MCP23017 / TCA9555 IO 扩展模块

当前代码实际使用 PCF8574。香氛 1 / 香氛 2 / 香氛 3 通过 PCF8574 的 P0 / P1 / P2 控制。

### 2. PCF8574 I2C 扩展 IO 接线

| PCF8574 引脚 | ESP32-S3 引脚 | 说明 |
|---|---|---|
| VCC | 3.3V | 推荐使用 3.3V，保证 I2C 电平兼容 ESP32 |
| GND | GND | 与 ESP32 共地 |
| SDA | GPIO8 | I2C 数据线 |
| SCL | GPIO9 | I2C 时钟线 |
| A0 / A1 / A2 | 按模块地址配置 | 当前软件默认地址为 `0x27`，实际地址需与模块焊盘/拨码一致 |

注意：PCF8574 只用于低速 IO 控制，不直接驱动雾化器、功放或其他大电流负载。

### 3. INMP441 I2S 数字麦克风接线

IO11 / IO12 / IO13 从 TF 卡槽转接板引出，给 INMP441 使用：

| INMP441 引脚 | ESP32-S3 引脚 | 说明 |
|---|---|---|
| VDD | 3.3V | INMP441 供电 |
| GND | GND | 与 ESP32 共地 |
| SCK / BCLK | GPIO11 | I2S Bit Clock |
| WS / LRCLK | GPIO12 | I2S Word Select / LRCLK |
| SD / DOUT | GPIO13 | I2S 麦克风数据输出，接 ESP32 I2S DIN |
| L/R | GND | 默认选择左声道 |

### 4. MAX98357A I2S 功放接线

MAX98357A 不能只接 DIN，还需要接 I2S 时钟。当前硬件方案中，MAX98357A 与 INMP441 共用 GPIO11 / GPIO12 作为 I2S BCLK / LRCLK，GPIO4 单独作为功放数据输出。

| MAX98357A 引脚 | ESP32-S3 引脚 | 说明 |
|---|---|---|
| VIN | 5V | MAX98357A 供电 |
| GND | GND | 与 ESP32 共地 |
| BCLK | GPIO11 | I2S Bit Clock，与 INMP441 共用 |
| LRC / WS | GPIO12 | I2S Word Select / LRCLK，与 INMP441 共用 |
| DIN | GPIO4 | I2S 数据输入，接 ESP32 I2S SPK DOUT |
| SPK+ | 扬声器 + | 接喇叭正极 |
| SPK- | 扬声器 - | 接喇叭负极 |

注意：当前 `i2s_output` 和 `i2s_mic_input` 仍是独立模块。如果后续需要同时录音和播放，需要进一步统一 I2S 总线初始化，避免 BCLK / WS 被两个模块重复驱动。

### 5. HE30 三路香氛 / 雾化控制接线

当前三路香氛控制链路如下：

```text
ESP32-S3
-> I2C
-> PCF8574
-> P0 / P1 / P2
-> HE30 三路控制输入
-> 三路香氛 / 雾化模块
```

PCF8574 不直接驱动雾化负载，只输出控制信号给 HE30 控制输入。

| 香氛通道 | PCF8574 引脚 | HE30 控制端 | 软件通道 |
|---|---|---|---|
| 香氛 1 | P0 | HE30 香氛 1 控制输入 | `AROMA_CH_1` |
| 香氛 2 | P1 | HE30 香氛 2 控制输入 | `AROMA_CH_2` |
| 香氛 3 | P2 | HE30 香氛 3 控制输入 | `AROMA_CH_3` |

UI/API 层不需要关心 HE30 控制输入是高电平触发还是低电平触发。上层永远只表达“打开”或“关闭”：

```c
aroma_set(AROMA_CH_1, true);   // 打开香氛 1
aroma_set(AROMA_CH_1, false);  // 关闭香氛 1
```

实际输出高电平还是低电平，由 `aroma_ctrl` 内部的 `AROMA_ACTIVE_LEVEL` 统一处理。当前默认 `AROMA_ACTIVE_LEVEL = true`，即高电平触发。如果实测 HE30 是低电平触发，只需要修改该宏，不需要改 UI/API 层代码。

### 6. 供电与共地要求

所有相关模块必须共地：

```text
ESP32 GND
PCF8574 GND
MAX98357A GND
INMP441 GND
HE30 GND
雾化/香氛模块电源 GND
```

HE30 和雾化模块的供电应按 HE30 模块规格连接。ESP32 和 PCF8574 只负责提供控制信号，不直接给雾化负载供电。

### 7. 当前代码默认引脚汇总

| 功能 | 默认引脚 / 地址 | 代码模块 |
|---|---|---|
| I2C SDA | GPIO8 | `pcf8574_io` / `aht20_sensor` |
| I2C SCL | GPIO9 | `pcf8574_io` / `aht20_sensor` |
| PCF8574 地址 | `0x27` | `pcf8574_io` |
| INMP441 BCLK / SCK | GPIO11 | `i2s_mic_input` |
| INMP441 WS / LRCLK | GPIO12 | `i2s_mic_input` |
| INMP441 SD / DOUT | GPIO13 | `i2s_mic_input` |
| MAX98357A BCLK | GPIO11 | `i2s_output` |
| MAX98357A LRC / WS | GPIO12 | `i2s_output` |
| MAX98357A DIN | GPIO4 | `i2s_output` |
| 香氛 CH1 | PCF8574 P0 -> HE30 香氛 1 控制输入 | `aroma_ctrl` |
| 香氛 CH2 | PCF8574 P1 -> HE30 香氛 2 控制输入 | `aroma_ctrl` |
| 香氛 CH3 | PCF8574 P2 -> HE30 香氛 3 控制输入 | `aroma_ctrl` |

## 音频提示音事件

播放内置提示音时使用下面这些事件：

```c
app_event_send(APP_EVENT_PLAY_STARTUP, 0);
app_event_send(APP_EVENT_PLAY_CLICK, 0);
app_event_send(APP_EVENT_PLAY_AROMA_ON, 0);
app_event_send(APP_EVENT_PLAY_AROMA_OFF, 0);
app_event_send(APP_EVENT_PLAY_ERROR, 0);
app_event_send(APP_EVENT_STOP_AUDIO, 0);
```

## TTS WAV 文件播放

播放 TTS 生成的 WAV 文件时，使用 `app_event_send_text()` 发送文件路径：

```c
app_event_send_text(APP_EVENT_PLAY_TTS_FILE, "/spiffs/tts_reply.wav");
```

路径格式说明：

- 传入一个已经挂载的文件系统路径字符串。
- 当前示例路径是 `/spiffs/tts_reply.wav`。
- 后续如果使用 SD 卡，也可以继续用同一个事件，例如 `/sdcard/tts_reply.wav`，前提是对应文件系统已经完成挂载。

当前音频文件支持范围：

- PCM WAV
- 16-bit
- 单声道
- 16000 Hz 或 24000 Hz
- little-endian

## 香氛三路事件

三路香氛控制使用下面这些事件：

```c
app_event_send(APP_EVENT_AROMA_CH1_ON, 0);
app_event_send(APP_EVENT_AROMA_CH1_OFF, 0);

app_event_send(APP_EVENT_AROMA_CH2_ON, 0);
app_event_send(APP_EVENT_AROMA_CH2_OFF, 0);

app_event_send(APP_EVENT_AROMA_CH3_ON, 0);
app_event_send(APP_EVENT_AROMA_CH3_OFF, 0);

app_event_send(APP_EVENT_AROMA_ALL_OFF, 0);
```

通道映射：

- CH1 对应 PCF8574 P0，再接 HE30 香氛 1 控制输入。
- CH2 对应 PCF8574 P1，再接 HE30 香氛 2 控制输入。
- CH3 对应 PCF8574 P2，再接 HE30 香氛 3 控制输入。

UI/API 层不需要关心 HE30 控制输入是高电平触发还是低电平触发，这部分由 `aroma_ctrl` 内部统一处理。

## 场景事件

当 UI/API 层需要触发预设场景时，使用下面这些事件：

```c
app_event_send(APP_EVENT_SCENE_RELAX, 0);
app_event_send(APP_EVENT_SCENE_SLEEP, 0);
app_event_send(APP_EVENT_SCENE_FOCUS, 0);
```

当前场景行为：

- `APP_EVENT_SCENE_RELAX`：播放 aroma-on 提示音，打开 CH1，关闭 CH2/CH3。
- `APP_EVENT_SCENE_SLEEP`：播放 aroma-on 提示音，打开 CH2，关闭 CH1/CH3。
- `APP_EVENT_SCENE_FOCUS`：播放 click 提示音，打开 CH3，关闭 CH1/CH2。

## 当前限制

- 当前事件层不负责文件系统挂载。
- 当前还没有实现网络 TTS 请求。
- TTS 播放要求指定路径下已经存在 WAV 文件。
- 目前还没有完成硬件实测。
