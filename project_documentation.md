# 极客电子相册项目完整说明文档 (Project Documentation)

本文档旨在从硬件底层接线到后端服务，再到核心的 ASR/LLM/TTS 流式语音交互流水线，对极客电子相册（Digital Album Project）的整体技术架构进行全盘分析和总结。

---

## 一、 硬件接线与外设架构设计

项目的核心主控基于 **ESP32-S3**。考虑到音频全双工传输与外设扩展的需求，系统使用了高度复用的引脚设计与 I2C 扩展芯片。

### 1. I2S 音频管线 (全双工复用)
系统通过 ESP32 的 I2S 外设实现了麦克风录音（输入）和喇叭播报（输出）。为了节省 GPIO，两者**共享了时钟管脚**。

| 模块名称 | 管脚功能 | ESP32-S3 物理引脚 | 说明 |
| :--- | :--- | :--- | :--- |
| **MAX98357A (功放)** | **BCLK** | **GPIO 12** | I2S 位时钟信号线，与麦克风共享 |
| | **LRC / WS** | **GPIO 11** | I2S 左右声道选择信号线，与麦克风共享 |
| | **DIN** | **GPIO 13** | I2S 音频数据输入线，接 ESP32 的 I2S0 DOUT |
| **INMP441 (麦克风)** | **SCK / BCLK**| **GPIO 12** | I2S 位时钟信号线 |
| | **WS / LRCLK**| **GPIO 11** | I2S 左右声道选择信号线 |
| | **SD / DOUT** | **GPIO 6**  | I2S 串行数据输出，接 ESP32 的 I2S DIN |
| | **L/R** | GND | 左声道模式 |

### 2. PCF8574 I2C 扩展与 HE30 香薰控制
由于相册终端加入了香薰雾化等交互功能，ESP32 剩余 GPIO 紧张。因此引入了 PCF8574 进行 I2C 低速引脚扩展。
- **I2C 总线引脚**：SDA (GPIO 8) / SCL (GPIO 9)
- **香薰通道**：扩展板的 P0、P1、P2 引脚分别连接至 HE30 驱动模块的 CH1、CH2、CH3，实现对三种香薰（如薰衣草、茉莉花、檀香木）的微安级隔离驱动。

---

## 二、 后端服务架构

系统的云端/局域网大脑主要由 Flask 框架与 Python 异步服务构成，负责控制流中转、照片云端同步与本地模型运算管理。

### 1. 核心目录与服务
- `backend/server.py`：Flask 后端的主入口，提供了例如照片拉取接口 (`/api/upload/check`)、设备命令轮询 (`/api/device/command`) 等 HTTP RESTful API。
- `backend/services/voice_server.py`：核心的 **WebSocket 语音服务端**，绑定在 `0.0.0.0:8888` 端口，专为超低延迟的情感助理对话场景设计。

### 2. 状态同步与控制数据流
设备端会高频向后端请求数据。例如香薰开启指令，由网页前端发给 Flask 暂存，设备的 `aroma_ctrl.c` 轮询获取到后，通过 I2C 设置 PCF8574 引脚电平开启雾化，从而实现“云-边-端”闭环控制。

---

## 三、 ASR / LLM / TTS 流式全双工流水线分析

为了让用户在相册终端获得如真实对话般的“零等待”流畅体验，整个唤醒与回复链路均基于**高并发、流式（Streaming）**与**分块推送（Chunked Push）**设计。相关代码实现在 `voice_server.py` 中。

整个处理生命周期如下：

### 1. 音频捕获与 VAD 终点检测
- **录音发起**：ESP32 在本地通过唤醒词（WakeNet9）或按键触发 `recording_started`，立刻通过 WebSocket 持续向 Python 端传输 16kHz/16bit 单声道 PCM 裸流。
- **VAD 判断**：Python 端使用 `webrtcvad`（若缺失则回退到 RMS 音量能量检测）实时判断当前传输的帧是否有人声。
- **动态截断**：一旦检测到持续的静音超过设定阈值（默认 `0.6` 秒），证明用户已说完，Python 端无需等待设备发送录音结束事件，便会提前触发 ASR 管线。

### 2. 阶段 1：ASR 语音识别
- **引擎**：使用 `faster-whisper`（默认 `small` 模型，CPU int8 量化推理）。
- **处理**：VAD 截断出的连续有效语音块被送入 Whisper 模型，转录为对应的中文文本（如：“帮我开一下香薰”）。
- **同步**：转录好的文本首先会被直接发还给客户端（`event: asr_final`），方便屏幕上能够立刻显示用户的输入字幕。

### 3. 阶段 2：LLM 流式语义生成与多轮对话
- **环境搭建**：服务器根据传感器信息（如温湿度）拼接出 `system_prompt`，连同 `client_ip` 映射的过往历史对话构建完整的上下文（Context）。
- **流式请求**：向本地部署的 **Ollama**（例如 `qwen2.5:7b`）发起对话请求，并开启 `stream=True`。
- **边生成边下发**：
  - 每一个生成的字/Token 都会以 `{"event": "llm_delta", "text": token}` 形式通过 WebSocket 立即推送到 ESP32，屏幕上如同打字机般实时渲染。
  - **TTS 拦截提取**：系统使用正则表达式监控 JSON 字符流中 `"tts_text"` 字段。当缓冲区内的文本遇到标点符号（如 `。`、`！`、`？`）时，即认定这是一句完整的语义，立即切割并跨线程推送到下一个阶段的 TTS 队列中。

### 4. 阶段 3：TTS 语音合成与流式播报
- **引擎调度**：为保证速度与音质，首选云端高拟真 `edge-tts`（zh-CN-XiaoxiaoNeural），如若网络断开等导致失败，无缝回退到基于本地的离线 `piper-tts`（如 `zh_CN-huayan-medium.onnx`）。
- **音频重采样**：通过系统内的 `ffmpeg` 进行重采样，将输出的音频固定为 `16000Hz 16bit s16le PCM` 格式。
- **分块发送与反压控制**：
  - 发送音频头：`{"type": "tts_start", "format": "pcm", "rate": 16000}`。
  - 切片传输：不再使用一次性发送，而是将生成的 PCM 数据切割为 `3200` 字节的块。并在每两块之间利用 `asyncio.sleep` 结合硬件 TCP 窗口，形成自然的反压（Backpressure），让 ESP32 稳定消费不致死机。
- **全双工打断机制**：设备如果在播报过程中检测到新的唤醒词，会发送 `recording_started` 或 `wake_word_detected`，此时服务端会将 `cancel_requested` 置为 `True`，果断抛弃当前所有还未生成的 LLM 句块和 TTS 音频块，实现毫秒级的跨阶段打断闭环。

---

**总结**：
相册项目依靠底层的 **I2S与I2C硬件分离化管理** 提供了极其坚实的物理承载力；而依托于 `voice_server.py` 的这套 ASR->LLM->TTS 全异步流式处理系统，完美填补了云端响应带来的延迟感，造就了高度智能、拟真且实时的软硬件交互产品。


## 四、 跨设备与新环境项目迁移指南 (Migration Guide)

当项目从旧电脑（原开发环境）移植到新电脑或更换局域网网络环境时，由于环境依赖和硬编码的 IP 地址等变更，需执行以下步骤进行全方位配置更新：

### 1. Python 后端依赖与环境搭建
*   **基础环境**：确保新电脑安装了 Python 3.10+ 环境。在 ackend/ 目录下运行 pip install -r requirements.txt 安装所需依赖。
*   **外部二进制组件**：
    *   **FFmpeg**：须在系统中安装 FFmpeg，并将其加入操作系统的 **环境变量 PATH** 中（代码中使用 shutil.which 搜索环境变量）。
    *   **eSpeak**：Piper TTS 的离线引擎需要依赖 eSpeak。请注意在 oice_server.py 代码中硬编码了路径（C:\Program Files (x86)\eSpeak\command_line），安装时建议保持此默认路径，或直接修改代码内该配置。

### 2. 部署本地大模型 (Ollama) 与 TTS 音色文件
*   **Ollama 模型部署**：安装本地的 Ollama 服务，在终端通过 ollama run qwen2.5:7b 拉取并准备好问答生成模型（或根据 OLLAMA_MODEL 环境变量调整）。
*   **本地 TTS 兜底模型**：检查 ackend/services/ 下是否遗漏大文件（如由于 .gitignore 过滤）。确保 zh_CN-huayan-medium.onnx（及相应的 json 文件）在目标位置，用作网络断开时 TTS 合成的兜底方案。

### 3. ESP32 设备端核心配置修改（网络与 IP 同步）
更换开发环境会导致**局域网 IP 地址发生变化**（例如从旧机器的 192.168.74.192 变为新 IP），这要求设备端的连接地址必须硬性更新。
*   在 main/ 目录下通过终端执行命令：
    `ash
    idf.py menuconfig
    `
*   进入自定义或对应配置页面，更新以下配置：
    *   **后端 API 连接地址 (CONFIG_BACKEND_URL)**：修改为新电脑的局域网 IP，如 http://<新IP>:8765
    *   **WebSocket 语音服务 (CONFIG_VA_WS_URL)**：修改为新电脑的局域网 IP，如 ws://<新IP>:8888
    *   *(注：如果更换了开发环境的无线路由器，请同时修改对应菜单中的 Wi-Fi SSID 与 Password)*
*   修改完毕后，保存退出，并执行以下命令重新编译固件并烧录至 ESP32：
    `ash
    idf.py build flash monitor
    `

### 4. 运行服务
配置确认无误后，在新环境的终端分别独立启动后端基础服务与低延迟语音服务：
1.  python backend/main.py
即运行以下文件：
1.  python backend/server.py
2.  python backend/services/voice_server.py
