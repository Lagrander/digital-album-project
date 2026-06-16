#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
@file voice_server.py
@brief 零延迟并发多轮对话流式引擎 (ASR -> LLM -> TTS)

@architecture
本模块依托 WebSockets 建立 ESP32 与服务器的全双工低延迟通道：
1. ASR 阶段：引入 WebRTC VAD 侦测与 faster-whisper 实现不等截断直接转录。
2. LLM 阶段：通过 Ollama stream=True 实现 Token 级别逐字下发。
3. TTS 阶段：边缘计算与云端(edge-tts -> piper-tts)双模回退，分块 (Chunked) 发送 PCM 结合自然反压，避免设备 OOM 且支持随时的硬核打断。
"""

import sys
from unittest.mock import MagicMock
# Python 3.13 移除了 audioop 模块，pydub 依赖它，mock 掉避免崩溃
sys.modules['audioop'] = MagicMock()
sys.modules['pyaudioop'] = MagicMock()
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='ignore')
if hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8', errors='ignore')

import os
import json
import wave
import asyncio
import re
import shutil
import concurrent.futures
import subprocess
import websockets
import time
import socket
import random
import threading

# 调试开关：True 表示开启 Echo 回环测试，跳过 ASR/LLM/TTS 物理计算
ECHO_TEST_MODE = False
from datetime import datetime
from pathlib import Path

SAMPLE_RATE = 16000
CHANNELS = 1
BIT_DEPTH = 16
BYTES_PER_SAMPLE = 2

WS_HOST = "0.0.0.0"
WS_PORT = 8888
ROOT_DIR = os.path.dirname(__file__)

# ── 本地疗愈曲库映射 ──────────────────────────────────────────────
AUDIO_ROUTING_MAP = {
    "insomnia":   [f"rain_{i}.mp3" for i in range(1, 8)],
    "anxiety":    [f"handpan_relax_{i}.mp3" for i in range(1, 6)],
    "meditation": [f"singing_bowl_{i}.mp3" for i in range(1, 6)],
    "sad":        [f"ambient_piano_{i}.mp3" for i in range(1, 6)],
    "tired":      ["campfire_1.mp3", "campfire_2.mp3"],
    "relax":      [f"forest_stream_{i}.mp3" for i in range(1, 5)],
    "happy":      [f"happy_pop_{i}.mp3" for i in range(1, 4)],
    "ocean":      [f"ocean_waves_{i}.mp3" for i in range(1, 4)],
    "winter":     ["winter_wind_1.mp3", "winter_wind_2.mp3"],
}

SCENE_MAP = {
    "海边": {"audio_keyword": "ocean", "mist_level": 1, "mist_channel": "none"},
    "大海": {"audio_keyword": "ocean", "mist_level": 1, "mist_channel": "none"},
    "雪景": {"audio_keyword": "winter", "mist_level": 1, "mist_channel": "mint"},
    "森林": {"audio_keyword": "relax",  "mist_level": 2, "mist_channel": "mint"},
    "篝火": {"audio_keyword": "tired",  "mist_level": 1, "mist_channel": "rose"},
}

# ── Ollama / 模型配置 ────────────────────────────────────────────
OLLAMA_HOST = os.environ.get("OLLAMA_HOST", "http://127.0.0.1:11434")
OLLAMA_MODEL = os.environ.get("OLLAMA_MODEL", "qwen2.5:7b")
WHISPER_MODEL = os.environ.get("WHISPER_MODEL", "small")
PIPER_MODEL = os.environ.get("PIPER_MODEL", "zh_CN-huayan-medium.onnx")
# 静音阈值 (秒) — VAD 检测到持续静音即触发 ASR
SILENCE_TRIGGER_SEC = float(os.environ.get("SILENCE_TRIGGER", "0.6"))

# Flask server 内部回调地址（用于 voice_server 将 LLM 回复推送给 Web UI）
_FLASK_CALLBACK_URL = os.environ.get("FLASK_CALLBACK_URL", "http://127.0.0.1:8765/api/internal/llm_reply")

def _post_llm_reply_to_flask(reply_text: str):
    """通过 HTTP 将 LLM 回复内容回传给 Flask，再由 Flask 通过 SSE 推送给前端。"""
    if not reply_text:
        return
    try:
        import urllib.request
        payload = json.dumps({"reply": reply_text}, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(
            _FLASK_CALLBACK_URL,
            data=payload,
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        urllib.request.urlopen(req, timeout=5)
    except Exception as e:
        print(f"⚠️  [voice_server] 回调 Flask 失败: {e}")

def _load_system_prompt():
    prompt_path = os.path.join(ROOT_DIR, "voice_system_prompt.md")
    if os.path.exists(prompt_path):
        with open(prompt_path, "r", encoding="utf-8") as f:
            return f.read()
    return "You are a helpful assistant."


class VoiceServer:

    def __init__(self):
        self.output_dir = os.path.join(ROOT_DIR, "user_records")
        self.response_dir = os.path.join(ROOT_DIR, "response_records")
        os.makedirs(self.output_dir, exist_ok=True)
        os.makedirs(self.response_dir, exist_ok=True)

        self._executor = concurrent.futures.ThreadPoolExecutor(max_workers=3)
        self._model_lock = threading.Lock()

        # ── 多轮对话会话历史 ──
        self.sessions = {}          # {client_ip: [{"role":"user"|"assistant","content":...}, ...]}
        self.MAX_HISTORY_TURNS = 50 # 最多保留 50 轮对话（100 条消息）
        self.active_sessions = []   # 用于存储所有活跃连接的信息，以便内部 RPC 调用

        # ── Ollama ──
        self.ollama_ok = self._check_ollama()
        if self.ollama_ok:
            print(f"✅ Ollama: {OLLAMA_MODEL} @ {OLLAMA_HOST}")
        else:
            print(f"⚠️  Ollama 不可用 — 请确保已启动且 {OLLAMA_MODEL} 已拉取")

        # ── faster-whisper ──
        self._whisper_model = None
        try:
            from faster_whisper import WhisperModel
            self._whisper_model = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8", cpu_threads=2)
            self.whisper_ok = True
            print(f"✅ faster-whisper: {WHISPER_MODEL} (CPU threads: 2)")
        except ImportError:
            self.whisper_ok = False
            print("⚠️  faster-whisper 未安装")
        except Exception as e:
            self.whisper_ok = False
            print(f"⚠️  whisper 加载失败: {e}")

        # ── VAD ──
        self._vad = None
        self._vad_aggressiveness = int(os.environ.get("VAD_AGGRESSIVENESS", "2"))
        try:
            import webrtcvad
            self._vad = webrtcvad.Vad(self._vad_aggressiveness)
            self.vad_ok = True
            print(f"✅ VAD: webrtcvad (aggressiveness={self._vad_aggressiveness})")
        except ImportError:
            self.vad_ok = False
            print("⚠️  webrtcvad 未安装，使用能量检测回退")

        # ── TTS 优先级: edge-tts (首选, 中文质量高) → piper (离线兜底) ──
        self.edge_tts_ok = self._check_edge_tts()
        self.piper_ok = self._check_piper()
        if self.edge_tts_ok:
            print(f"✅ edge-tts: zh-CN-XiaoxiaoNeural (首选)")
        if self.piper_ok:
            print(f"✅ piper-tts: {PIPER_MODEL} (兜底)")
        if not self.edge_tts_ok and not self.piper_ok:
            print("⚠️  没有可用的 TTS 方案")

    def _check_ollama(self):
        try:
            import urllib.request
            req = urllib.request.urlopen(f"{OLLAMA_HOST}/api/tags", timeout=5)
            req.read()
            return True
        except Exception:
            return False

    def _check_edge_tts(self):
        try:
            # 用 python -m edge_tts 比直接敲 edge-tts 更可靠（Scripts 目录可能不在 PATH）
            result = subprocess.run(
                ["python", "-m", "edge_tts", "--help"],
                capture_output=True, timeout=10
            )
            return result.returncode == 0
        except FileNotFoundError:
            return False

    def _check_piper(self):
        try:
            result = subprocess.run(
                ["piper", "--help"], capture_output=True, timeout=5
            )
            if result.returncode != 0:
                return False
            model_path = os.path.join(ROOT_DIR, PIPER_MODEL)
            if os.path.exists(model_path):
                return True
            print(f"⚠️  piper 模型未找到: {model_path}")
            return False
        except FileNotFoundError:
            return False

    # ════════════════════════════════════════════════════════════════
    # WebSocket 客户端处理（每个连接一个协程）
    # ════════════════════════════════════════════════════════════════

    async def handle_client(self, websocket, path=None):
        client_ip = websocket.remote_address[0]
        print(f"\n🔗 连接: {client_ip}")

        # 增大 Windows TCP 接收缓冲区，防止反压到 ESP32 的 SND_BUF
        try:
            sock = websocket.transport.get_extra_info('socket')
            if sock is not None:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 131072)
        except Exception:
            pass

        st = {
            "is_recording": False,
            "audio_buffer": bytearray(),
            "telemetry_data": {},
            "last_speech_time": 0.0,      # 上次检测到语音的时间
            "asr_running": False,          # ASR 是否正在后台运行
            "pipeline_running": False,     # 完整管线(ASR+LLM+TTS)是否在运行
            "llm_streaming": False,        # LLM 流式生成中
            "cancel_requested": False,     # 打断标志
        }

        session_info = {"websocket": websocket, "st": st, "client_ip": client_ip}
        if client_ip != "127.0.0.1":
            self.active_sessions.append(session_info)

        try:
            async for message in websocket:
                try:
                    if isinstance(message, bytes):
                        if st["is_recording"]:
                            st["audio_buffer"].extend(message)
                            # 如果是 Echo 模式，无需在 Python 端做 VAD，直接更新时间戳，隔离 C 库 webrtcvad 潜在的崩溃隐患
                            if ECHO_TEST_MODE:
                                st["last_speech_time"] = time.time()
                            else:
                                if self._has_speech(message):
                                    st["last_speech_time"] = time.time()
                            # 静音超过阈值 + 缓冲够长 + 管线空闲 → 触发流式识别
                            buf_sec = len(st["audio_buffer"]) / BYTES_PER_SAMPLE / SAMPLE_RATE
                            silence_dur = time.time() - st["last_speech_time"]
                            if (not st["pipeline_running"] and buf_sec > 1.5
                                    and silence_dur > SILENCE_TRIGGER_SEC):
                                st["pipeline_running"] = True
                                st["asr_running"] = True
                                audio_copy = bytes(st["audio_buffer"])
                                # 重置状态，防止紧接着的静音切片引发重复触发
                                st["audio_buffer"] = bytearray()
                                st["last_speech_time"] = time.time()
                                asyncio.create_task(
                                    self._streaming_pipeline(audio_copy, websocket, st, client_ip)
                                )
                        await asyncio.sleep(0)  # 每帧后让出事件循环，确保 Windows select() 立即注册下一轮 I/O
                        continue

                    data = json.loads(message)
                    event = data.get("event")

                    if event == "flask_rpc":
                        rpc_cmd = data.get("cmd")
                        rpc_text = data.get("text", "")
                        print(f"🤖 [RPC] 收到 Flask 内部调用: {rpc_cmd} -> {rpc_text}")
                        for s in self.active_sessions:
                            target_st = s["st"]
                            target_ws = s["websocket"]
                            target_ip = s["client_ip"]
                            if rpc_cmd == "simulate_text":
                                target_st["is_recording"] = False
                                target_st["audio_buffer"] = bytearray()
                                target_st["cancel_requested"] = True
                                target_st["pipeline_running"] = True
                                target_st["asr_running"] = False
                                if rpc_text == "[CANCEL]":
                                    target_st["pipeline_running"] = False
                                    continue
                                asyncio.create_task(
                                    self._streaming_pipeline(None, target_ws, target_st, target_ip, text_override=rpc_text)
                                )
                        continue

                    if event == "telemetry":
                        st["telemetry_data"] = data

                    elif event == "privacy_mode":
                        await websocket.send(json.dumps({
                            "action": {"mist": False, "brightness": 0}
                        }))

                    elif event == "privacy_mode_exit":
                        await websocket.send(json.dumps({
                            "action": {"brightness": 100}
                        }))
                        
                    elif event == "simulate_text":
                        sim_text = data.get("text", "")
                        print(f"🤖 [{client_ip}] 收到模拟文本输入: {sim_text}")
                        st["is_recording"] = False
                        st["audio_buffer"] = bytearray()
                        st["cancel_requested"] = True # 打断当前
                        st["pipeline_running"] = True
                        st["asr_running"] = False
                        st["is_web_simulated"] = True
                        asyncio.create_task(
                            self._streaming_pipeline(None, websocket, st, client_ip, text_override=sim_text)
                        )

                    elif event == "wake_word_detected":
                        print(f"🎉 [{client_ip}] 唤醒词")
                        st["cancel_requested"] = True # 💡 唤醒时立即标记打断上一轮
                        st["is_web_simulated"] = False
                        self.sessions.pop(client_ip, None)  # 新会话：清除历史

                    elif event == "recording_started":
                        print(f"🎤 [{client_ip}] 录音开始")
                        st["cancel_requested"] = True # 💡 强唤醒/录音开始双重打断保障
                        st["is_recording"] = True
                        st["audio_buffer"] = bytearray()
                        st["last_speech_time"] = time.time()
                        st["asr_running"] = False
                        st["pipeline_running"] = False

                    elif event == "recording_ended":
                        print(f"✅ [{client_ip}] 录音结束")
                        st["is_recording"] = False
                        if len(st["audio_buffer"]) == 0:
                            continue

                        buf_sec = len(st["audio_buffer"]) / BYTES_PER_SAMPLE / SAMPLE_RATE
                        print(f"📊 [{client_ip}] {len(st['audio_buffer'])} 字节 ({buf_sec:.2f}s)")

                        # 保存音频（后台线程）
                        audio_copy = bytes(st["audio_buffer"])
                        self._executor.submit(self._save_audio, audio_copy)

                        # 若管线已在运行（流式 ASR 已触发），不再重复处理
                        if st.get("pipeline_running") or st.get("llm_streaming") or st.get("tts_generating"):
                            continue

                        # 最终一次性处理（流式未触发时的回退路径）
                        await self._streaming_pipeline(audio_copy, websocket, st, client_ip)

                    elif event == "recording_cancelled":
                        print(f"⚠️ [{client_ip}] 录音取消")
                        st["is_recording"] = False
                        st["audio_buffer"] = bytearray()
                        st["cancel_requested"] = True

                    elif event == "session_end":
                        print(f"🔇 [{client_ip}] 会话结束")
                        self.sessions.pop(client_ip, None)

                except json.JSONDecodeError:
                    pass
                except Exception as e:
                    print(f"❌ [{client_ip}] 错误: {e}")
                    import traceback
                    traceback.print_exc()

        except websockets.exceptions.ConnectionClosed:
            print(f"🔌 [{client_ip}] 连接断开 (code={websocket.close_code}, reason={websocket.close_reason})")
        except Exception as e:
            print(f"❌ [{client_ip}] 异常: {e}")
        finally:
            if session_info in self.active_sessions:
                self.active_sessions.remove(session_info)
            st["is_recording"] = False
            st["pipeline_running"] = False
            st["asr_running"] = False
            st["llm_streaming"] = False
            st["tts_generating"] = False
            try:
                await websocket.close()
            except Exception:
                pass
            print(f"🔌 [{client_ip}] 资源已清理")

    def _append_history(self, client_ip, role, content):
        """追加一条消息到会话历史，限制最多 MAX_HISTORY_TURNS 轮。"""
        if not content:
            return
        sess = self.sessions.setdefault(client_ip, [])
        sess.append({"role": role, "content": content})
        if len(sess) > self.MAX_HISTORY_TURNS * 2:
            self.sessions[client_ip] = sess[-(self.MAX_HISTORY_TURNS * 2):]

    # ════════════════════════════════════════════════════════════════
    # 流式管线: ASR → LLM (stream) → TTS (piper) → ESP32
    # ════════════════════════════════════════════════════════════════

    async def _streaming_pipeline(self, pcm_data, websocket, st, client_ip, text_override=None):
        """在后台线程执行重计算，结果通过 WebSocket 流式推送。"""
        st["pipeline_running"] = True
        st["asr_running"] = True
        st["cancel_requested"] = False # 💡 开启新一轮 Pipeline，在此重置打断状态
        try:
            loop = asyncio.get_running_loop()

            if ECHO_TEST_MODE:
                print(f"🤖 [{client_ip}] Echo 模式触发：跳过 ASR/LLM/TTS，直接返回测试回复")
                # 1. 发送 ASR 文本
                await websocket.send(json.dumps({"event": "asr_final", "text": "Echo OK"}))
                
                # 2. 发送大模型 dialogue 控制帧
                dialogue_msg = {
                    "dialogue": {
                        "tts_text": "Echo OK",
                        "emotion": "neutral"
                    },
                    "action": {
                        "mist": {"command": "keep"},
                        "audio": {"command": "keep"},
                        "screen": {"command": "keep"}
                    }
                }
                await websocket.send(json.dumps(dialogue_msg, ensure_ascii=False))
                
                # 3. 发送 tts_start 音频头（16000Hz）
                await websocket.send(json.dumps({
                    "type": "tts_start",
                    "rate": 16000,
                    "format": "pcm"
                }))
                
                # 4. 读取并流式推送 test_16k_16b.pcm 音频文件
                pcm_path = os.path.join(ROOT_DIR, "test_16k_16b.pcm")
                
                # 若测试 pcm 文件缺失，自动生成一个 3 秒的 1kHz 纯正弦波测试 PCM 文件
                if not os.path.exists(pcm_path):
                    print(f"Creating fake test pcm: {pcm_path}")
                    import math, struct
                    with open(pcm_path, "wb") as pf:
                        for t in range(int(16000 * 3)): # 3秒长度
                            # 1000Hz正弦波，30%最大振幅
                            val = int(32767 * 0.3 * math.sin(2.0 * math.pi * 1000.0 * t / 16000))
                            pf.write(struct.pack("<h", val))
                            
                if os.path.exists(pcm_path):
                    with open(pcm_path, "rb") as f:
                        while chunk := f.read(3200):
                            if st.get("cancel_requested"):
                                print(f"⚠️ [{client_ip}] Echo 发送被打断，退出。")
                                break
                            await websocket.send(chunk)
                            await asyncio.sleep(0.09) # 稍微比 100ms 快一点点，防止 buffer 饿死
                else:
                    # 回退防挂起方案：若生成写入依旧受限，回退流式发送 1 秒的静音包
                    for _ in range(10):
                        await websocket.send(b'\x00' * 3200)
                        await asyncio.sleep(0.09)
                    
                # 5. 发送 ping 结束包
                await websocket.send("ping")
                print(f"✅ [{client_ip}] Echo 回复包推送完毕。")
                return

            # ── 阶段 1: ASR（线程池）──
            if text_override:
                transcribed = text_override
                st["asr_running"] = False
            else:
                transcribed = await asyncio.to_thread(self._transcribe, pcm_data)

            if not transcribed or st.get("cancel_requested"):
                print(f"⚠️ [{client_ip}] ASR 无结果")
                await websocket.send(json.dumps({"event": "asr_failed"}))
                return
            print(f"🗣️ [{client_ip}] ASR/文本: {transcribed}")
            await websocket.send(json.dumps({"event": "asr_final", "text": transcribed}))

            # ── 多轮对话：记录用户说的话 ──
            self._append_history(client_ip, "user", transcribed)

            # ── 阶段 2: LLM streaming（线程池）与 TTS 并发 ──
            st["llm_streaming"] = True
            
            tts_queue = asyncio.Queue()
            pcm_queue = asyncio.Queue()
            
            async def tts_consumer():
                st["tts_generating"] = True
                while True:
                    sentence = await tts_queue.get()
                    if sentence is None:
                        await pcm_queue.put(None)
                        break
                    if sentence.strip() and not st.get("cancel_requested"):
                        pcm_res = await asyncio.to_thread(
                            self._generate_tts_audio_only, sentence, client_ip
                        )
                        if pcm_res:
                            await pcm_queue.put(pcm_res)
                    tts_queue.task_done()
                st["tts_generating"] = False

            async def pcm_sender():
                while True:
                    pcm_res = await pcm_queue.get()
                    if pcm_res is None:
                        break
                    if not st.get("cancel_requested"):
                        await self._stream_pcm_data_async(
                            pcm_res, websocket, client_ip, st
                        )
                    pcm_queue.task_done()

            tts_task = asyncio.create_task(tts_consumer())
            pcm_task = asyncio.create_task(pcm_sender())

            full_text = await asyncio.to_thread(
                self._call_ollama_stream, transcribed, st, websocket, loop, client_ip, tts_queue
            )
            st["llm_streaming"] = False
            
            await tts_queue.put(None)
            await tts_task
            await pcm_task
            
            # 所有 TTS 断句全部发送完毕后，才发送 ping 通知设备可以结束播音并重启 VAD
            if not st.get("cancel_requested"):
                await websocket.send("ping")
            # ── 多轮对话：记录 AI 的回复 ──
            self._append_history(client_ip, "assistant", full_text)

            # ── 如果来自 simulate_text，将 LLM 回复内容回传给 Flask/SSE ──
            if text_override and full_text:
                # 尝试从 LLM JSON 中提取可读的 tts_text
                import urllib.request as _ureq
                display_text = full_text
                try:
                    match = re.search(r'"tts_text"\s*:\s*"((?:[^"\\]|\\.)*?)"', full_text)
                    if match:
                        display_text = json.loads('"' + match.group(1) + '"')
                except Exception:
                    pass
                # 在当前事件循环中异步调用回调（不阻塞音频流）
                asyncio.create_task(
                    asyncio.to_thread(_post_llm_reply_to_flask, display_text)
                )

            if st.get("cancel_requested"):
                return

            # ── 阶段 3: 解析 CJSON ──
            cjson = self._parse_llm_json(full_text or transcribed, client_ip)
            if cjson:
                print(f"   📤 CJSON: {json.dumps(cjson, ensure_ascii=False)}")
                await websocket.send(json.dumps(cjson, ensure_ascii=False))
            else:
                control = self._parse_intent(full_text or transcribed, client_ip)
                if control:
                    await websocket.send(json.dumps(control))

        except Exception as e:
            print(f"❌ [{client_ip}] _streaming_pipeline 运行崩溃: {e}")
            import traceback
            traceback.print_exc()
        finally:
            st["pipeline_running"] = False
            st["asr_running"] = False
            st["llm_streaming"] = False
            st["tts_generating"] = False

    # ════════════════════════════════════════════════════════════════
    # VAD 语音活动检测
    # ════════════════════════════════════════════════════════════════

    def _has_speech(self, pcm_chunk):
        """检测 PCM 块是否含语音活动（webrtcvad 或能量回退）。"""
        has_voice = False
        if self._vad is not None:
            try:
                # webrtcvad 要求 16kHz 16bit mono PCM，帧长 10/20/30ms
                frame_ms = 30
                frame_bytes = int(SAMPLE_RATE * frame_ms / 1000) * BYTES_PER_SAMPLE
                for i in range(0, len(pcm_chunk) - frame_bytes + 1, frame_bytes):
                    if self._vad.is_speech(pcm_chunk[i:i + frame_bytes], SAMPLE_RATE):
                        has_voice = True
                        break
            except Exception:
                pass
        
        # 💡 逻辑规整：若 webrtcvad 未安装或报错，则走能量检测回退
        if not has_voice and len(pcm_chunk) >= 2:
            try:
                import struct
                samples = struct.unpack(f"<{len(pcm_chunk)//2}h", pcm_chunk)
                rms = (sum(s * s for s in samples) / len(samples)) ** 0.5
                has_voice = rms > 500  # 静音阈值
            except Exception:
                pass

        # 💡 可视化控制台打印（同一行输出）
        if has_voice:
            print("█", end="", flush=True) # 有声音
        else:
            print("_", end="", flush=True) # 静音
            
        return has_voice

    # ════════════════════════════════════════════════════════════════
    # 阶段 1: faster-whisper ASR（同步 → 线程池调用）
    # ════════════════════════════════════════════════════════════════

    def _transcribe(self, pcm_data):
        if not self.whisper_ok or not self._whisper_model:
            return None
        try:
            import numpy as np
            audio_array = np.frombuffer(pcm_data, dtype=np.int16)
            if len(audio_array) > 0:
                max_val = np.max(np.abs(audio_array))
                print(f"📊 ASR 接收到音频大小: {len(pcm_data)} 字节, 最大振幅 (底噪/说话强度): {max_val}")
            else:
                print("📊 ASR 接收到音频大小为 0 字节")

            wav_path = os.path.join(self.output_dir, f"_asr_tmp_{time.time()}.wav")
            with wave.open(wav_path, "wb") as wf:
                wf.setnchannels(CHANNELS)
                wf.setsampwidth(BIT_DEPTH // 8)
                wf.setframerate(SAMPLE_RATE)
                wf.writeframes(pcm_data)

            with self._model_lock:  # whisper 模型非线程安全
                segments, _ = self._whisper_model.transcribe(
                    wav_path, language="zh", beam_size=5,
                    vad_filter=True,
                    vad_parameters=dict(
                        threshold=0.5,
                        min_speech_duration_ms=250,
                        min_silence_duration_ms=500,
                    )
                )
            text = " ".join(s.text for s in segments).strip()
            os.remove(wav_path)
            return text if text else None
        except Exception as e:
            print(f"❌ ASR: {e}")
            return None

    # ════════════════════════════════════════════════════════════════
    # 阶段 2: Ollama 流式生成（同步 → 线程池调用）
    # ════════════════════════════════════════════════════════════════

    def _call_ollama_stream(self, user_text, st, websocket, loop, client_ip, tts_queue=None):
        """调用 Ollama stream API，token 通过 asyncio 跨线程推送到 ESP32。"""
        if not self.ollama_ok:
            return None

        system_prompt = _load_system_prompt()
        telemetry_str = ""
        if st.get("telemetry_data"):
            t = st["telemetry_data"]
            telemetry_str = f"温度: {t.get('temp')}℃, 湿度: {t.get('hum')}%"
        full_prompt = system_prompt + ("\n\n[传感器]\n" + telemetry_str if telemetry_str else "")

        # ── 多轮对话：注入历史 ──
        history = self.sessions.get(client_ip, [])
        messages = [{"role": "system", "content": full_prompt}]
        messages += history[-self.MAX_HISTORY_TURNS * 2:]  # 最近 N 轮（每轮 user+assistant）
        messages.append({"role": "user", "content": user_text})

        try:
            import urllib.request
            payload = json.dumps({
                "model": OLLAMA_MODEL,
                "messages": messages,
                "stream": True,
                "format": "json",
                "options": {"temperature": 0.7, "num_predict": 1024},
            }).encode("utf-8")

            req = urllib.request.Request(
                f"{OLLAMA_HOST}/api/chat",
                data=payload,
                headers={"Content-Type": "application/json"}
            )

            full_text = []
            json_buffer = ""
            total_processed_text = ""
            current_sentence = ""
            tts_extraction_active = True

            with urllib.request.urlopen(req, timeout=120) as resp:
                for line in resp:
                    if st.get("cancel_requested"):
                        break
                    line = line.decode("utf-8", errors="ignore").strip()
                    if not line:
                        continue
                    try:
                        chunk = json.loads(line)
                        token = chunk.get("message", {}).get("content", "")
                        if token:
                            full_text.append(token)
                            json_buffer += token
                            # 跨线程推送 token 到 ESP32
                            asyncio.run_coroutine_threadsafe(
                                websocket.send(json.dumps(
                                    {"event": "llm_delta", "text": token}
                                )),
                                loop
                            )
                            
                            # 提取 TTS 文本并按标点推入队列
                            if tts_queue and tts_extraction_active:
                                match = re.search(r'"tts_text"\s*:\s*"((?:[^"\\]|\\.)*)', json_buffer)
                                if match:
                                    raw_val = match.group(1)
                                    try:
                                        # 正确的反转义方法：利用 json.loads 处理标准的 JSON 字符串
                                        # 这样既能保留原生的中文，又能正确解析 \n 和 \uXXXX
                                        decoded = json.loads('"' + raw_val + '"')
                                        new_chars = decoded[len(total_processed_text):]
                                        if new_chars:
                                            current_sentence += new_chars
                                            total_processed_text += new_chars
                                        
                                        puncs = ['。', '！', '？', '；', '!', '?', ';']
                                        last_punc_idx = -1
                                        for p in puncs:
                                            idx = current_sentence.rfind(p)
                                            if idx > last_punc_idx:
                                                last_punc_idx = idx
                                                
                                        if last_punc_idx != -1:
                                            to_send = current_sentence[:last_punc_idx+1].strip()
                                            current_sentence = current_sentence[last_punc_idx+1:]
                                            if to_send:
                                                asyncio.run_coroutine_threadsafe(tts_queue.put(to_send), loop)
                                    except Exception:
                                        pass

                                # 检查 "tts_text" 是否已经闭合结束
                                match_closed = re.search(r'"tts_text"\s*:\s*"((?:[^"\\]|\\.)*)"', json_buffer)
                                if match_closed:
                                    if current_sentence.strip():
                                        asyncio.run_coroutine_threadsafe(tts_queue.put(current_sentence.strip()), loop)
                                        current_sentence = ""
                                    tts_extraction_active = False

                    except json.JSONDecodeError:
                        continue

            result = "".join(full_text).strip()
            print(f"🤖 [{client_ip}] LLM: {result[:80]}...")
            return result

        except Exception as e:
            print(f"❌ Ollama: {e}")
            return None

    # ════════════════════════════════════════════════════════════════
    # 阶段 3: piper-tts 流式生成 PCM（同步 → 线程池调用）
    # ════════════════════════════════════════════════════════════════

    def _generate_tts_audio_only(self, text, client_ip):
        """TTS：仅生成音频 PCM 数据，与网络流式发送解耦，避免阻塞下一句生成。返回 (引擎名, pcm_data)。"""
        if not text:
            return None

        ffmpeg_bin = shutil.which("ffmpeg") or r"C:\Users\kotamen\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-8.1.1-full_build\bin\ffmpeg.exe"
        import uuid
        uid = uuid.uuid4().hex[:8]
        tmp_pcm = os.path.join(self.response_dir, f"_tts_16k_{uid}.pcm")

        # ═════ 1) 首选 edge-tts ═════
        if self.edge_tts_ok:
            try:
                mp3_path = os.path.join(self.response_dir, f"_tts_edge_{uid}.mp3")
                voice = os.environ.get("TTS_VOICE", "zh-CN-XiaoxiaoNeural")
                subprocess.run(
                    ["python", "-m", "edge_tts", "--text", text, "--voice", voice, "--write-media", mp3_path],
                    capture_output=True, timeout=30, check=True
                )
                subprocess.run(
                    [ffmpeg_bin, "-y", "-i", mp3_path, "-f", "s16le", "-ar", "16000", "-ac", "1",
                     "-filter:a", "aresample=resampler=swr:precision=28,volume=1.0", tmp_pcm],
                    capture_output=True, timeout=30, check=True
                )
                try: os.remove(mp3_path)
                except: pass

                with open(tmp_pcm, "rb") as f:
                    pcm_data = f.read()
                try: os.remove(tmp_pcm)
                except: pass

                if len(pcm_data) > 0:
                    return ("edge-tts", pcm_data)
            except Exception as e:
                print(f"⚠️  edge-tts 失败 (本次跳过): {type(e).__name__}: {e}")
                # 不永久禁用 edge-tts，下次请求仍会重试

        # ═════ 2) 兜底 piper-tts ═════
        if not self.piper_ok:
            return None

        model_path = os.path.join(ROOT_DIR, PIPER_MODEL)
        tmp_in = os.path.join(self.response_dir, f"_tts_in_{uid}.txt")
        tmp_raw = os.path.join(self.response_dir, f"_tts_piper_{uid}.raw")

        try:
            piper_bin = shutil.which("piper") or "piper"
            piper_env = os.environ.copy()
            espeak_path = r"C:\Program Files (x86)\eSpeak\command_line"
            if os.path.isdir(espeak_path):
                piper_env["PATH"] = espeak_path + os.pathsep + piper_env.get("PATH", "")

            with open(tmp_in, "w", encoding="utf-8") as f:
                f.write(text)

            with open(tmp_raw, "wb") as f_raw:
                result = subprocess.run(
                    [piper_bin, "--model", model_path, "--output-raw", "-i", tmp_in],
                    stdout=f_raw, stderr=subprocess.PIPE, timeout=30, env=piper_env
                )
            if result.returncode != 0:
                return None

            subprocess.run(
                [ffmpeg_bin, "-y", "-f", "s16le", "-ar", "22050", "-ac", "1",
                 "-i", tmp_raw, "-f", "s16le", "-ar", "16000", "-ac", "1",
                 "-filter:a", "aresample=resampler=swr:precision=28,volume=1.0", tmp_pcm],
                capture_output=True, timeout=30
            )

            with open(tmp_pcm, "rb") as f:
                pcm_data = f.read()

            for f_path in [tmp_in, tmp_raw, tmp_pcm]:
                try: os.remove(f_path)
                except: pass

            if len(pcm_data) > 0:
                return ("piper-tts", pcm_data)
            return None
        except Exception:
            return None

    async def _stream_pcm_data_async(self, source_and_pcm, websocket, client_ip, st):
        """将生成的 PCM 数据慢速流式发送给设备（全异步实现）。"""
        source, pcm_data = source_and_pcm
        total = len(pcm_data)
        if total == 0:
            return

        await websocket.send(json.dumps({
            "type": "tts_start", "format": "pcm",
            "rate": 16000, "channels": 1, "bits": 16,
            "simulated": st.get("is_web_simulated", False) if st else False
        }))

        CHUNK = 2048
        start_time = time.time()
        try:
            for offset in range(0, total, CHUNK):
                if st and st.get("cancel_requested"):
                    print(f"⚠️ [{client_ip}] TTS 播放被打断")
                    break
                await websocket.send(pcm_data[offset:offset + CHUNK])
                # 放宽速率，允许 ESP32 的环形缓冲区囤积数据，由硬件背压控制流速
                # 使用较小的 CHUNK 和匹配的 sleep，防止 ESP32 接收线程长时间阻塞锁死 websocket mutex
                await asyncio.sleep(0.05)
                
            # 等待实际音频播放完毕，再结束 pcm_task（防止 cjson 提前下发导致 MP3 与 TTS 重叠重放卡顿）
            audio_duration = total / 32000.0
            elapsed = time.time() - start_time
            if not (st and st.get("cancel_requested")) and elapsed < audio_duration:
                # 留 0.1 秒的提前量，让客户端有时间去请求网络 MP3，避免两句话之间停顿太久
                remaining = audio_duration - elapsed - 0.1
                if remaining > 0:
                    await asyncio.sleep(remaining)

            print(f"🔊 [{client_ip}] TTS ({source}): {total} 字节 (16kHz)")
        except websockets.exceptions.ConnectionClosed:
            print(f"⚠️ [{client_ip}] 发送音频途中设备断开连接。")
        except Exception as e:
            print(f"⚠️ [{client_ip}] 发送音频时出错: {e}")


    async def _generate_tts_fallback(self, text, websocket, loop, client_ip):
        """edge-tts 回退方案（piper 失败时调用，一般走不到）。"""
        try:
            mp3_path = os.path.join(self.response_dir, "_tts_fb.mp3")
            voice = os.environ.get("TTS_VOICE", "zh-CN-XiaoxiaoNeural")
            # edge-tts: 文本 → mp3（用 python -m 避免 PATH 问题）
            subprocess.run(
                ["python", "-m", "edge_tts", "--text", text,
                 "--voice", voice, "--write-media", mp3_path],
                capture_output=True, timeout=30, check=True
            )
            pcm_path = mp3_path.replace(".mp3", ".pcm")
            ffmpeg_bin = shutil.which("ffmpeg") or r"C:\Users\kotamen\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-8.1.1-full_build\bin\ffmpeg.exe"
            subprocess.run(
                [ffmpeg_bin, "-y", "-i", mp3_path,
                 "-f", "s16le", "-ar", str(SAMPLE_RATE), "-ac", "1",
                 "-filter:a", "aresample=resampler=swr:precision=28,volume=0.4", pcm_path],
                capture_output=True, timeout=30, check=True
            )
            with open(pcm_path, "rb") as f:
                pcm = f.read()
            os.remove(mp3_path)
            os.remove(pcm_path)
            await websocket.send(pcm)
        except Exception as e:
            print(f"❌ TTS 回退失败: {e}")

    # ════════════════════════════════════════════════════════════════
    # 音频保存
    # ════════════════════════════════════════════════════════════════

    def _save_audio(self, audio_data):
        if not audio_data:
            return
        try:
            ts = datetime.now().strftime("%Y%m%d_%H%M%S")
            wav_path = os.path.join(self.output_dir, f"rec_{ts}.wav")
            with wave.open(wav_path, "wb") as wf:
                wf.setnchannels(CHANNELS)
                wf.setsampwidth(BIT_DEPTH // 8)
                wf.setframerate(SAMPLE_RATE)
                wf.writeframes(audio_data)
            print(f"💾 已保存: {wav_path}")
        except Exception as e:
            print(f"❌ 保存失败: {e}")

    # ════════════════════════════════════════════════════════════════
    # CJSON 解析 + 宏指令解析（不变）
    # ════════════════════════════════════════════════════════════════

    def _parse_llm_json(self, text, client_ip):
        if not text: return None
        cleaned = text.strip()
        
        if cleaned.startswith("```"):
            lines = cleaned.split("\n")
            if lines[0].startswith("```"): lines = lines[1:]
            if lines and lines[-1].strip() == "```": lines = lines[:-1]
            cleaned = "\n".join(lines).strip()
            
        # 移除 LLM 生成内容中非法的真实换行符（将其替换为空格），避免 json.loads 直接崩溃
        cleaned = cleaned.replace("\n", " ").replace("\r", " ")
        
        try:
            raw = json.loads(cleaned)
        except json.JSONDecodeError:
            b1, b2 = cleaned.find("{"), cleaned.rfind("}")
            if b1 >= 0 and b2 > b1:
                try: raw = json.loads(cleaned[b1:b2 + 1])
                except json.JSONDecodeError: return None
            else: return None
        if not isinstance(raw, dict) or "dialogue" not in raw:
            return None
        return self._resolve_cjson(raw, client_ip)

    def _resolve_cjson(self, raw, client_ip):
        local_ip = self._get_best_local_ip(client_ip)
        action = raw.get("action", {})

        audio = action.get("audio", {})
        if audio.get("url", "").startswith("<search:"):
            kw = audio["url"][len("<search:"):].rstrip(">").strip()
            if kw in AUDIO_ROUTING_MAP:
                audio["url"] = f"http://{local_ip}:8765/music/{random.choice(AUDIO_ROUTING_MAP[kw])}"
            else:
                audio["url"] = f"http://{local_ip}:8765/music/test.mp3"

        screen = action.get("screen", {})
        scene_hit = None
        if screen.get("url", "").startswith("<search:"):
            kw = screen["url"][len("<search:"):].rstrip(">").strip()
            resolved = self._search_photo(kw, local_ip)
            if resolved: screen["url"] = resolved
            for sk, sc in SCENE_MAP.items():
                if sk in kw: scene_hit = sc; break

        if scene_hit:
            if audio.get("command", "keep") == "keep":
                ak = scene_hit["audio_keyword"]
                if ak in AUDIO_ROUTING_MAP:
                    audio["command"] = "play"
                    audio["url"] = f"http://{local_ip}:8765/music/{random.choice(AUDIO_ROUTING_MAP[ak])}"
                    audio["loop"] = True; audio["volume"] = 40
            mist = action.get("mist", {})
            if mist.get("command", "keep") == "keep":
                mist["command"] = "on"
                mist["channel"] = scene_hit.get("mist_channel", "none")
                mist["level"] = scene_hit.get("mist_level", 1)
        return raw

    def _search_photo(self, keyword, local_ip):
        try:
            import sqlite3
            db = os.path.join(os.path.dirname(__file__), "../photos.db")
            if not os.path.exists(db): return None
            conn = sqlite3.connect(db); conn.row_factory = sqlite3.Row
            rows = conn.execute(
                "SELECT path FROM photo_scores WHERE caption LIKE ? OR side_caption LIKE ? "
                "ORDER BY memory_score DESC LIMIT 1",
                (f"%{keyword}%", f"%{keyword}%")
            ).fetchall()
            conn.close()
            if rows: return f"http://{local_ip}:8765/api/photo/{Path(rows[0]['path']).stem}.jpg"
        except Exception: pass
        return None

    def _parse_intent(self, text, client_ip):
        """旧版关键词回退。"""
        action, media = {}, {}
        tl = text.lower()
        if any(w in tl for w in ["加湿","香薰","喷雾","雾化"]):
            action["mist"] = not any(w in tl for w in ["关","停"])
            if action.get("mist"):
                ch = 0
                if "茉莉" in tl or "jasmine" in tl: ch = 1
                elif "蔷薇" in tl or "rose" in tl: ch = 2
                action["mist_channel"] = ch
                lv = 2
                if any(w in tl for w in ["低","弱","1"]): lv = 1
                elif any(w in tl for w in ["高","强","3"]): lv = 3
                action["mist_level"] = lv
        if any(w in tl for w in ["黑屏","关屏","睡觉"]): action["brightness"] = 0
        elif any(w in tl for w in ["亮屏","开屏"]): action["brightness"] = 100
        if any(w in tl for w in ["音乐","播放","听歌"]) and not any(w in tl for w in ["关","停"]):
            best_ip = self._get_best_local_ip(client_ip)
            media["url"] = f"http://{best_ip}:8765/music/test.mp3"
        r = {}
        if action: r["action"] = action
        if media: r["media"] = media
        return r if r else None

    def _get_best_local_ip(self, client_ip):
        local_ips = self._get_local_ips()
        best_ip = local_ips[0] if local_ips else "127.0.0.1"
        if client_ip:
            ipv4 = client_ip.split(":")[-1]
            if "." in ipv4:
                prefix = ".".join(ipv4.split(".")[:3]) + "."
                for ip in local_ips:
                    if ip.startswith(prefix):
                        best_ip = ip
                        break
        return best_ip

    def _get_local_ips(self):
        ips = []
        try:
            # 优先使用 UDP 连接外部地址来获取真实的默认路由出网 IP（规避虚拟网卡和 TUN 接口）
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                s.connect(("8.8.8.8", 80))
                ip = s.getsockname()[0]
                if not ip.startswith("198.18.") and not ip.startswith("127."):
                    ips.append(ip)
            except: pass
            finally: s.close()

            for info in socket.getaddrinfo(socket.gethostname(), None):
                if info[0] == socket.AF_INET:
                    ip = info[4][0]
                    if ip not in ips and not ip.startswith("127.") and not ip.startswith("198.18."):
                        ips.append(ip)
            ips.append("127.0.0.1")
        except: ips = ["127.0.0.1"]
        return ips

    async def start(self):
        print("=" * 60)
        print("ESP32 语音助手 WebSocket (流式管线)")
        print(f"ASR: faster-whisper {WHISPER_MODEL} + VAD")
        print(f"LLM: Ollama {OLLAMA_MODEL} (stream)")
        print(f"TTS: piper-tts {PIPER_MODEL}")
        for ip in self._get_local_ips():
            print(f"  ws://{ip}:{WS_PORT}")
        print("=" * 60)
        # close_timeout=2s: 旧连接快速释放，减少 opening handshake failed
        async with websockets.serve(self.handle_client, WS_HOST, WS_PORT,
                                   ping_interval=None,
                                   ping_timeout=None,
                                   close_timeout=2):
            await asyncio.Future()


def main():
    if sys.platform == "win32":
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    
    while True:
        try:
            asyncio.run(VoiceServer().start())
        except (KeyboardInterrupt, SystemExit):
            print("\n⚠️  已手动停止")
            break
        except BaseException as e:
            print(f"\n⚠️  服务器异常崩溃 ({e})，正在 3 秒后自动重启以保障高可用...")
            import time
            time.sleep(3)


if __name__ == "__main__":
    main()