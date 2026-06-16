# ✨ 极客电子相册主终端项目工程架构说明 (Architecture & Specifications)

> [!NOTE]
> 本文档基于底层硬件接线白皮书 `backend_interface.md` 规范，结合根目录下 `main/` 核心组件的真实模块实现进行编写，旨在为相册的二次开发、硬件调试以及跨平台维护提供权威的系统级架构与集成指南。

---

## 🗺️ 1. 系统宏观架构与模块分工

主项目工程采用多任务、高度并行的 **FreeRTOS + 模块化解耦** 架构进行设计。整个相册终端的“视觉感知”、“数据拉取”、“底层物理外设”与“语音情感助理”分由以下五个核心目录各司其职，保证了整个嵌入式系统的**高内聚与低耦合**：

```mermaid
graph TD
    subgraph 视觉与人机交互 (Core 1)
        UI[lv_ui: LVGL 8 主屏幕渲染]
    end
    subgraph 音频与外设物理底层 (Core 0 / I2C & I2S)
        PER[peripherals: 硬件驱动与外设服务]
        PER_TASK[peripherals_task: 高频采样任务]
    end
    subgraph 网络通信与云端数据链路 (Core 0 / Async WiFi)
        NET[net_mgr: Wi-Fi STA 网络底座]
        CLIENT[photo_client: 惊喜照片拉取]
    end
    subgraph 情感智能助理 (Core 0 / ESP-SR)
        VA[voice_assistant: 离线唤醒与情感引擎]
    end

    NET ==>|提供网络就绪事件| CLIENT
    CLIENT ==>|RGB565数据直接注入| UI
    PER_TASK ==>|温湿度缓存更新| UI
    VA ==>|触发TTS与指示灯动作| PER
```

### 📂 核心子目录功能图鉴

| 目录名称 | 核心物理职能 | 内部关键源文件 | 设计核心理念 |
| :--- | :--- | :--- | :--- |
| **[lv_ui](file:///w:/Desktop/digital_album_project/main/lv_ui)** | **主屏幕视觉渲染与交互** | `ui_main.c`, `lvgl_ui_task.c`, `lv_voice_assistant.c` | 搭载 LVGL 8 渲染引擎，驱动 480x800 RGB 竖屏，支持双缓冲、Crossfade 渐切屏及实时状态 LED 渲染。 |
| **[peripherals](file:///w:/Desktop/digital_album_project/main/peripherals)** | **物理外设底盘与音频/香薰控制** | `pcf8574_io.c`, `audio_player.c`, `he30.c`, `aht20_sensor.c` | 负责硬件原子操作、音频流解码、雾化阀门动作控制。内部加入多核并发互斥锁保护，保障硬件操作绝对安全。 |
| **[net_mgr](file:///w:/Desktop/digital_album_project/main/net_mgr)** | **网络生命线连接** | `net_mgr.c` | 封装 Wi-Fi STA 模式，完成与外部 AP 握手，负责 IP 获取广播和自动掉线重连机制。 |
| **[photo_client](file:///w:/Desktop/digital_album_project/main/photo_client)** | **惊喜照片云端同步** | `photo_client.c` | 异步网络拉取任务，通过心跳轮询云端是否有新投递的惊喜，自动抓取 `.rgb565` 二进制流直接推入显存。 |
| **[voice_assistant](file:///w:/Desktop/digital_album_project/main/voice_assistant)** | **情感唤醒语音助理** | `voice_assistant_task.c`, `ws_voice.c` | 搭载 Espressif 官方语音模型（WakeNet9 唤醒词 + MultiNet7 命令词），提供多线程音频录制与情感对话交互。 |

---

## 🔌 2. 底层硬件接线与引脚定义 (PINOUT Specifications)

根据 `backend_interface.md` 规范，硬件共包含 I2S 音频功放（MAX98357A）、I2C 控制扩展板（PCF8574）以及 HE30 三路香薰雾化控制三大物理部分，所有管脚硬件调试和接线参数规定如下：

### 1. I2S 功放与音频接口接线
相册采用 **MAX98357A** I2S 无滤波 D类功放模块，用于高品质启动音效及 TTS 语音的高声级播放；配合 **INMP441** 数字 MEMS 麦克风实现语音唤醒与录音功能：

| MAX98357A 功放引脚 | ESP32-S3 物理引脚 | 说明 |
| :--- | :--- | :--- |
| **VIN** | 5V | 功放驱动主电源供电 |
| **GND** | GND | 与 ESP32-S3 芯片主板共地 |
| **BCLK** | **GPIO 12** | I2S 位时钟信号线 (Bit Clock)，与麦克风共享 |
| **LRC / WS** | **GPIO 11** | I2S 左右声道选择信号线 (Word Select)，与麦克风共享 |
| **DIN** | **GPIO 13** | I2S 音频数据输入线，接芯片 I2S0 DOUT |
| **SPK+ / SPK-** | 扬声器正负极 | 物理输出对接 `8欧/1.5W` 的腔体小喇叭 |

| INMP441 麦克风引脚 | ESP32-S3 物理引脚 | 说明 |
| :--- | :--- | :--- |
| **VDD** | 3.3V | INMP441 供电 |
| **GND** | GND | 与 ESP32-S3 芯片主板共地 |
| **WS / LRCLK** | **GPIO 11** | I2S Word Select / LRCLK |
| **SCK / BCLK** | **GPIO 12** | I2S Bit Clock |
| **SD / DOUT** | **GPIO 6** | I2S 串行数据输出，接 ESP32 I2S DIN |
| **L/R** | GND | 左声道模式 |

### 2. PCF8574 I2C 扩展低速 IO 接线
相册的外设控制板通过 **PCF8574** 低速扩展模块级联，将极度紧张的 ESP32 GPIO 引脚通过 I2C 扩展成 8 路可用 IO：

| PCF8574 扩展引脚 | ESP32-S3 物理引脚 | 说明 |
| :--- | :--- | :--- |
| **VCC** | 3.3V | 推荐 3.3V 供电，保证 I2C 电平与 ESP32 芯片兼容 |
| **GND** | GND | 与主控板共地 |
| **SDA** | **GPIO 8** | I2C 数据总线线 (带有上拉电阻) |
| **SCL** | **GPIO 9** | I2C 时钟总线线 (带有上拉电阻) |
| **A0 / A1 / A2** | 拨码地址配置 | 默认物理及软件寻址地址为 **`0x27`** |

### 3. HE30 三路香薰雾化通道接线
相册的香薰喷雾交互完全基于 **HE30 驱动芯片**控制，通过 PCF8574 扩展引脚实现微安级逻辑信号隔离驱动：

| 物理香薰通道 | PCF8574 控制管脚 | HE30 雾化板通道 | 软件层通道代号 |
| :--- | :--- | :--- | :--- |
| 🌸 **1 号通道 (薰衣草)** | **P0** | HE30 CH1 控制输入 | `AROMA_CH_1` |
| 🌼 **2 号通道 (茉莉花)** | **P1** | HE30 CH2 控制输入 | `AROMA_CH_2` |
| 🪵 **3 号通道 (檀香木)** | **P2** | HE30 CH3 控制输入 | `AROMA_CH_3` |

> [!IMPORTANT]
> **共地与电源要求**：
> 音频功放、电磁雾化负载（HE30）的工作电流通常在 500mA - 1.5A 级别，**必须保证所有模块的地线（GND）物理共地**。ESP32-S3 与 PCF8574 扩展芯片只提供弱电平控制信号，绝对不允许用单片机的 IO 脚直接给大负载设备供电！

---

## 📡 3. 异步网络数据同步与控制链路 (Command Dataflow)

整个主项目终端通过 Wi-Fi 接入后，在后台自动孵化出两个非常高雅的异步云端联络线程：

### 1. 照片传送下载数据链 (基于 `photo_client.c`)
*   **轮询监听**：在独立的网络任务中，每一秒异步向 Flask 后端发送 `GET /api/upload/check` 心跳包，检测今天是否有用户通过手机页面上传了新的传送照片。
*   **流式解码**：一旦有新照片，网络客户端立刻提取它的 `upload_id`，发起 `GET /api/upload/<id>.rgb565` 图像拉取请求。
*   **无感屏渲染**：二进制数据流（RGB565 裸点阵格式）拉取完毕后直接注入 PSRAM，并调用 `ui_set_photo_data()` 推给主屏幕显示。底部的 AI 卡片立刻展现温情的照片悄悄话，全程**零刷新白屏，完美体现 SPA 级过度视觉**！

### 2. 遥控控制与心跳链路 (基于 `main.c` / `aroma_ctrl.c`)
*   当用户在网页遥控台或智能助手中遥控香薰时，主项目在每 3 秒发起一次 `GET /api/device/command` 拉取指令。
*   解析出控制指令（如 `toggle_aroma`）后，底层直接调用 `aroma_set()` 转换电平并将其写入 PCF8574 扩展引脚，控制 HE30 物理通道完成雾化。
*   **状态同步**：操作成功后，ESP32 将在下一次心跳中将状态回传，网页控制卡片和屏幕状态灯（`aroma_leds`）在毫秒内从红灯（关闭）转换为绿灯（开启），实现了完美的物理-云端三维同步！

---

## 🛡️ 4. 工业级多任务安全与缓存调优机制 (Safety & Optimizations)

为了保证主项目在运行中展现极其丝滑、绝无死锁的 Premium 质感，我们在底层引脚读写与数据缓存中植入了两项非常卓越的安全设计：

### 1. PCF8574 总线多线程互斥锁保护 (`s_mutex`)
在 FreeRTOS 环境下，温湿度采样任务与香薰/显示任务在多核芯片下并发运行，如果同时对 I2C 进行读写会导致硬件时序冲突。
*   **机制**：我们在 **[pcf8574_io.c](file:///w:/Desktop/digital_album_project/main/peripherals/src/pcf8574_io.c)** 底层引入了互斥量保护信号量。每次读写引脚前，系统自动申请 `pcf8574_lock()` 并在写入完成后立刻释放。彻底消除了高并发总线竞争卡死的硬件硬伤！

### 2. 上电端口物理状态防抖
PCF8574 属于准双向口扩展芯片，如果初始化时直接强制写全低 `0x00`，在部分电路设计下会引发开机误导通。
*   **机制**：我们在初始化时将默认缓存电平设为安全状态 `0xFF` (`PCF8574_SAFE_PORT_STATE`，即释放引脚开启弱上拉）。这对 Active-Low（低电平有效）的 HE30 通道提供了最完美的保护，**彻底根成了设备冷启动瞬间，香薰雾化器产生短暂剧烈喷射的物理隐患**！

### 3. 温湿度数据物理读取高速缓存保护
因为 LVGL 屏幕顶栏的数字时钟和温湿度图标需要每秒更新一次，如果每次都发起慢速的 I2C 物理读取，会导致显示屏发生明显卡顿。
*   **机制**：我们在 **[aht20_sensor.c](file:///w:/Desktop/digital_album_project/main/peripherals/src/aht20_sensor.c)** 中设计了全局温湿度缓存变量 `g_latest_temp` 和 `g_latest_hum`。温湿度物理读取仅由定时器在高频后台任务中刷新，主 UI 线程在刷新界面时，通过 `aht20_get_latest()` 直接极其快速地从内存读取，**完美保证了 3D UI 刷新率的绝对稳定和丝滑质感**！