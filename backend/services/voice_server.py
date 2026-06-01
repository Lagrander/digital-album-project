#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
WebSocket voice server — relay between ESP32 and DashScope Qwen Omni Turbo.
Runs alongside Flask (port 8765) on port 8888.
"""

import os
import sys
import json
import base64
import wave
import asyncio
import websockets
from datetime import datetime
from pydub import AudioSegment
import time
import socket

from omni_realtime_client import (
    OmniRealtimeClient,
    TurnDetectionMode,
)

SAMPLE_RATE = 16000
MODEL_SAMPLE_RATE = 24000
CHANNELS = 1
BIT_DEPTH = 16
BYTES_PER_SAMPLE = 2

WS_HOST = "0.0.0.0"
WS_PORT = 8888

ROOT_DIR = os.path.dirname(__file__)


class VoiceServer:

    def __init__(self):
        self.output_dir = os.path.join(ROOT_DIR, "user_records")
        self.response_dir = os.path.join(ROOT_DIR, "response_records")

        os.makedirs(self.output_dir, exist_ok=True)
        os.makedirs(self.response_dir, exist_ok=True)

        self.api_key = os.environ.get("DASHSCOPE_API_KEY")

        if not self.api_key:
            print(
                "⚠️  警告: 未设置DASHSCOPE_API_KEY环境变量\n"
                "   请通过以下方式之一配置API密钥：\n"
                "   1. 设置环境变量: export DASHSCOPE_API_KEY='your-api-key'\n"
                "   2. 在代码中直接设置 self.api_key"
            )
            self.use_model = False
        else:
            self.use_model = True
            print("✅ 已配置大模型API，将使用AI生成响应音频")

    async def handle_client(self, websocket, path):
        client_ip = websocket.remote_address[0]
        print(f"\n🔗 新的客户端连接: {client_ip}")

        client_state = {
            "is_recording": False,
            "realtime_client": None,
            "message_task": None,
            "audio_buffer": bytearray(),
            "audio_tracker": {
                "total_sent": 0,
                "last_time": time.time(),
            },
        }

        try:
            async for message in websocket:
                try:
                    if isinstance(message, bytes):
                        if (
                            client_state["is_recording"]
                            and client_state["realtime_client"]
                        ):
                            client_state["audio_buffer"].extend(message)

                            encoded_data = base64.b64encode(message).decode("utf-8")

                            event = {
                                "event_id": "event_"
                                + str(int(time.time() * 1000)),
                                "type": "input_audio_buffer.append",
                                "audio": encoded_data,
                            }

                            await client_state["realtime_client"].send_event(event)
                            print(f"   📤 实时转发音频块: {len(message)} 字节")
                        continue

                    data = json.loads(message)
                    event = data.get("event")

                    if event == "wake_word_detected":
                        print(f"🎉 [{client_ip}] 检测到唤醒词！")

                    elif event == "recording_started":
                        print(f"🎤 [{client_ip}] 开始录音...")
                        client_state["is_recording"] = True
                        client_state["audio_buffer"] = bytearray()
                        client_state["audio_tracker"] = {
                            "total_sent": 0,
                            "last_time": time.time(),
                        }

                        if self.use_model:
                            try:
                                client_state["realtime_client"] = OmniRealtimeClient(
                                    base_url="wss://dashscope.aliyuncs.com/api-ws/v1/realtime",
                                    api_key=self.api_key,
                                    model="qwen-omni-turbo-realtime-2025-05-08",
                                    voice="Chelsie",
                                    on_audio_delta=lambda audio: asyncio.create_task(
                                        self._on_audio_delta(
                                            websocket,
                                            client_ip,
                                            audio,
                                            client_state["audio_tracker"],
                                        )
                                    ),
                                    turn_detection_mode=TurnDetectionMode.MANUAL,
                                )

                                await client_state["realtime_client"].connect()
                                client_state["message_task"] = asyncio.create_task(
                                    client_state["realtime_client"].handle_messages()
                                )

                                print(f"✅ [{client_ip}] LLM连接成功，准备接收实时音频")

                            except Exception as e:
                                print(f"❌ [{client_ip}] 初始化大模型失败: {e}")
                                client_state["realtime_client"] = None

                    elif event == "recording_ended":
                        print(f"✅ [{client_ip}] 录音结束")
                        client_state["is_recording"] = False

                        if len(client_state["audio_buffer"]) > 0:
                            duration = len(client_state["audio_buffer"]) / BYTES_PER_SAMPLE / SAMPLE_RATE
                            print(f"📊 [{client_ip}] 音频总大小: {len(client_state['audio_buffer'])} 字节 ({duration:.2f}秒)")

                            current_timestamp = datetime.now()
                            saved_file = await self._save_audio(
                                [bytes(client_state["audio_buffer"])], current_timestamp
                            )
                            if saved_file:
                                print(f"✅ [{client_ip}] 音频已保存: {saved_file}")

                        if self.use_model and client_state["realtime_client"]:
                            try:
                                await client_state["realtime_client"].create_response()

                                print(f"🤖 [{client_ip}] 等待模型生成响应...")
                                max_wait_time = 30
                                start_time = time.time()

                                while time.time() - start_time < max_wait_time:
                                    await asyncio.sleep(0.1)

                                    if (
                                        client_state["audio_tracker"]["total_sent"] > 0
                                        and time.time()
                                        - client_state["audio_tracker"]["last_time"]
                                        > 2.0
                                    ):
                                        print(
                                            f"✅ [{client_ip}] 响应音频发送完成，总计: {client_state['audio_tracker']['total_sent']} 字节"
                                        )
                                        break

                                if client_state["audio_tracker"]["total_sent"] == 0:
                                    print(f"⚠️ [{client_ip}] 未收到大模型响应")

                                await websocket.ping()

                            except Exception as e:
                                print(f"❌ [{client_ip}] 模型处理失败: {e}")
                        else:
                            print(f"⚠️ [{client_ip}] 未启用AI模型，无法生成响应")

                    elif event == "recording_cancelled":
                        print(f"⚠️ [{client_ip}] 录音取消")
                        client_state["is_recording"] = False
                        client_state["audio_buffer"] = bytearray()

                except json.JSONDecodeError as e:
                    print(f"❌ [{client_ip}] JSON解析错误: {e}")
                except Exception as e:
                    print(f"❌ [{client_ip}] 处理消息错误: {e}")

        except websockets.exceptions.ConnectionClosed:
            print(f"🔌 [{client_ip}] 客户端断开连接")
        except Exception as e:
            print(f"❌ [{client_ip}] 连接错误: {e}")
        finally:
            if client_state["realtime_client"]:
                try:
                    if client_state["message_task"]:
                        client_state["message_task"].cancel()
                    await client_state["realtime_client"].close()
                except:
                    pass

    async def _on_audio_delta(self, websocket, client_ip, audio_data, audio_tracker):
        try:
            resampled = self._resample_audio(audio_data, MODEL_SAMPLE_RATE, SAMPLE_RATE)
            await websocket.send(resampled)
            print(f"   → 流式发送音频块: {len(resampled)} 字节")

            audio_tracker["total_sent"] += len(resampled)
            audio_tracker["last_time"] = time.time()

        except Exception as e:
            print(f"❌ [{client_ip}] 发送音频块失败: {e}")

    async def _save_audio(self, audio_buffer, timestamp):
        if not audio_buffer:
            print("⚠️  没有音频数据可保存")
            return None

        try:
            audio_data = b"".join(audio_buffer)

            timestamp_str = (
                timestamp.strftime("%Y%m%d_%H%M%S")
                if timestamp
                else datetime.now().strftime("%Y%m%d_%H%M%S")
            )
            wav_filename = os.path.join(
                self.output_dir, f"recording_{timestamp_str}.wav"
            )
            mp3_filename = os.path.join(
                self.output_dir, f"recording_{timestamp_str}.mp3"
            )

            with wave.open(wav_filename, "wb") as wav_file:
                wav_file.setnchannels(CHANNELS)
                wav_file.setsampwidth(BIT_DEPTH // 8)
                wav_file.setframerate(SAMPLE_RATE)
                wav_file.writeframes(audio_data)

            audio = AudioSegment.from_wav(wav_filename)
            audio.export(mp3_filename, format="mp3", bitrate="128k")
            os.remove(wav_filename)

            duration = len(audio_data) / BYTES_PER_SAMPLE / SAMPLE_RATE
            print(f"\n✅ 音频信息:")
            print(f"   时长: {duration:.2f} 秒")
            print(f"   大小: {len(audio_data) / 1024:.1f} KB")

            return mp3_filename

        except Exception as e:
            print(f"\n❌ 保存音频失败: {e}")
            return None

    def _resample_audio(self, audio_data, from_rate, to_rate):
        if from_rate == to_rate:
            return audio_data

        try:
            import numpy as np
            from scipy import signal

            audio_array = np.frombuffer(audio_data, dtype=np.int16)
            num_samples = int(len(audio_array) * to_rate / from_rate)
            resampled = signal.resample(audio_array, num_samples)
            resampled = np.clip(resampled, -32768, 32767).astype(np.int16)
            return resampled.tobytes()

        except ImportError:
            print("⚠️  未安装scipy，使用简单重采样方法")

            audio_array = []
            for i in range(0, len(audio_data), 2):
                if i + 1 < len(audio_data):
                    sample = int.from_bytes(
                        audio_data[i : i + 2], byteorder="little", signed=True
                    )
                    audio_array.append(sample)

            ratio = to_rate / from_rate
            resampled = []
            for i in range(int(len(audio_array) * ratio)):
                src_idx = i / ratio
                src_idx_int = int(src_idx)
                src_idx_frac = src_idx - src_idx_int

                if src_idx_int + 1 < len(audio_array):
                    sample = int(
                        audio_array[src_idx_int] * (1 - src_idx_frac)
                        + audio_array[src_idx_int + 1] * src_idx_frac
                    )
                else:
                    sample = audio_array[min(src_idx_int, len(audio_array) - 1)]

                resampled.append(sample)

            result = bytearray()
            for sample in resampled:
                result.extend(sample.to_bytes(2, byteorder="little", signed=True))

            return bytes(result)

    def _get_local_ips(self):
        ips = []
        try:
            hostname = socket.gethostname()
            for info in socket.getaddrinfo(hostname, None):
                if info[0] == socket.AF_INET:
                    ip = info[4][0]
                    if ip not in ips and not ip.startswith("127."):
                        ips.append(ip)

            if not ips:
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                try:
                    s.connect(("8.8.8.8", 80))
                    ip = s.getsockname()[0]
                    if ip not in ips and not ip.startswith("127."):
                        ips.append(ip)
                except:
                    pass
                finally:
                    s.close()

            ips.append("127.0.0.1")

        except Exception as e:
            print(f"⚠️  获取本机IP地址失败: {e}")
            ips = ["127.0.0.1"]

        return ips

    async def start(self):
        print("=" * 60)
        print("ESP32语音助手 WebSocket服务器")
        print("=" * 60)

        local_ips = self._get_local_ips()
        print("可用的连接地址:")
        for ip in local_ips:
            print(f"  - ws://{ip}:{WS_PORT}")

        if self.use_model:
            print(f"\n响应模式: AI大模型生成响应")
            print(f"模型: qwen-omni-turbo-realtime")
        else:
            print(f"\n响应模式: 未启用（需要设置 DASHSCOPE_API_KEY）")
            print(f"提示: 设置环境变量 DASHSCOPE_API_KEY 以启用AI响应")
        print("=" * 60)
        print("\n等待ESP32连接...\n")

        async with websockets.serve(self.handle_client, WS_HOST, WS_PORT):
            await asyncio.Future()


def main():
    server = VoiceServer()
    try:
        asyncio.run(server.start())
    except KeyboardInterrupt:
        print("\n\n⚠️  服务器已停止")


if __name__ == "__main__":
    main()
