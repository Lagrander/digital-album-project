#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
百度网盘云端照片同步与 AI 分析任务
- 定期从百度网盘指定目录拉取增量照片
- 保存原图至本地主图库 (IMAGE_DIR/baidu_cloud)
- 使用本地 Ollama 进行 AI 分析并入库 (photo_scores)
"""

import os
import sys

# 强制将标准输出切换为 utf-8，防止在 Windows cmd/powershell 下打印 Emoji 报错退出
sys.stdout.reconfigure(encoding='utf-8')

# === 核心：设置 NO_PROXY 使本地 Ollama 绕过代理，解决 502 Error ===
os.environ['NO_PROXY'] = 'localhost,127.0.0.1,::1'
# 刻意不清除全局 http_proxy，让 requests 依然能通过您的代理环境稳定下载百度照片，避免直连带来的 SSL EOF 断连问题。

import time
import json
import sqlite3
import requests
import urllib3
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
import ollama
from pathlib import Path
import sys

ROOT_DIR = Path(__file__).resolve().parent.parent
sys.path.append(str(ROOT_DIR))

import config as cfg
import tasks.cloud_analyze_photos as analyzer


# === 配置提取 ===
BAIDU_ACCESS_TOKEN = getattr(cfg, "BAIDU_ACCESS_TOKEN", "")
BAIDU_REMOTE_DIR = getattr(cfg, "BAIDU_REMOTE_DIR", "/apps/digital_album")
OLLAMA_MODEL = getattr(cfg, "OLLAMA_MODEL", "llava:7b")

IMAGE_DIR = Path(str(getattr(cfg, "IMAGE_DIR", ""))).expanduser()
if not IMAGE_DIR.is_absolute():
    IMAGE_DIR = (ROOT_DIR / IMAGE_DIR).resolve()

BAIDU_LOCAL_DIR = IMAGE_DIR / "baidu_cloud"
BAIDU_LOCAL_DIR.mkdir(parents=True, exist_ok=True)

DB_PATH = Path(str(getattr(cfg, "DB_PATH", "photos.db"))).expanduser()
if not DB_PATH.is_absolute():
    DB_PATH = (ROOT_DIR / DB_PATH).resolve()

AI_PROMPT_CAPTION = "请客观、详细地描述这张照片的内容。包含场景、主体、色彩和光线。请直接用中文描述，不要说“这张照片里有”之类的废话，直接描述画面。"
AI_PROMPT_SIDE_CAPTION = (
    "你是一位为「电子相框」撰写旁白短句的中文文案助手。\n"
    "你的目标不是描述画面，而是为画面补上一点“画外之意”。\n\n"
    "创作原则：\n"
    "1. 避免使用以下词语：世界、梦、时光、岁月、温柔、治愈、刚刚好、悄悄、慢慢 等（但不是禁止）。\n"
    "2. 严禁使用如下句式：……里……着整个世界；……里……着整个夏天；……得像……（简单的比喻）; ……比……还……； ……得比……更……。\n"
    "3. 只基于图片中能确定的信息进行联想，不要虚构时间、人物关系、事件背景。\n"
    "4. 文案应自然、有趣，带一点幽默或者诗意，但请避免煽情、鸡汤。\n"
    "5. 不要复述画面内容本身，而是写“看完画面后，心里多出来的一句话”。\n"
    "6. 可以偏向以下风格之一：\n"
    "   - 日常中的微妙情绪\n"
    "   - 轻微自嘲或冷幽默\n"
    "   - 对时间、记忆、瞬间的含蓄感受\n"
    "   - 看似平淡但有余味的一句判断\n"
    "7. 避免小学生作文式的、套路式的模板化表达\n" 
    "8. 尽量传递积极向上的内容，但不要过于明显或者刻意；适当的矛盾、反转、黑色幽默也是允许的。\n"
    "9. 尽量不要出现“比”这个字\n"
    "格式要求：\n"
    "1. 只输出一句中文短句，不要换行，不要引号，不要任何解释。\n"
    "2. 建议长度 8～20 个汉字，最多不超过 30 个汉字。\n"
    "3. 不要出现“这张照片”“这一刻”“那天”等指代照片本身的词。\n"
    "请基于这张照片，生成一句符合上述所有规则的中文文案。"
)
HEADERS = {'User-Agent': 'pan.baidu.com', 'Connection': 'close'}

def fetch_with_retry(url, headers=None, timeout=10):
    for i in range(3):
        try:
            res = requests.get(url, headers=headers, timeout=timeout)
            return res
        except Exception as e:
            if i == 2:
                raise e
            time.sleep(2)

def fetch_and_analyze():
    if not BAIDU_ACCESS_TOKEN:
        print("❌ 错误：请在 config.py 中配置 BAIDU_ACCESS_TOKEN")
        return

    print(f"1. 正在读取百度网盘专属文件夹: {BAIDU_REMOTE_DIR}")
    list_url = f"https://pan.baidu.com/rest/2.0/xpan/file?method=list&access_token={BAIDU_ACCESS_TOKEN}&dir={BAIDU_REMOTE_DIR}"
    
    try:
        res = fetch_with_retry(list_url, headers=HEADERS)
        res_data = res.json()
        if 'error_code' in res_data:
            print(f"❌ 百度 API 返回错误: {res_data}")
            return
        file_list = res_data.get('list', [])
    except Exception as e:
        print(f"❌ 链接网盘失败 (可能是网络或代理导致): {e}")
        return

    if not file_list:
        print("💡 网盘文件夹里空空如也。")
        return

    # 提取 fs_ids 来获取真实 dlink 和 thumbs
    fs_ids = [str(f['fs_id']) for f in file_list if f.get('isdir') != 1]
    if not fs_ids:
        print("💡 网盘文件夹里没有文件。")
        return

    print("2. 正在获取文件真实下载链接(dlink)...")
    fs_ids_str = "[" + ",".join(fs_ids) + "]"
    meta_url = f"https://pan.baidu.com/rest/2.0/xpan/multimedia?method=filemetas&dlink=1&fsids={fs_ids_str}&access_token={BAIDU_ACCESS_TOKEN}"
    try:
        meta_res = fetch_with_retry(meta_url, headers=HEADERS)
        meta_data = meta_res.json()
        file_list = meta_data.get('list', [])
    except Exception as e:
        print(f"❌ 获取文件元数据失败: {e}")
        return

    # 连接数据库，确保表存在
    conn = sqlite3.connect(DB_PATH)
    analyzer.ensure_table(conn)

    # 查出现有库中的文件，实现增量同步
    cur = conn.cursor()
    cur.execute("SELECT path FROM photo_scores")
    existing_paths = {row[0] for row in cur.fetchall()}

    print(f"🎉 发现 {len(file_list)} 张照片。开始同步...")

    processed_count = 0
    for index, file_info in enumerate(file_list, 1):
        if file_info.get('isdir') == 1:
            continue

        file_name = file_info.get('server_filename') or file_info.get('filename')
        local_path = BAIDU_LOCAL_DIR / file_name

        if str(local_path) in existing_paths:
            print(f"[{index}] ⏩ 已存在且已分析过，跳过: {file_name}")
            continue

        # 优先使用 dlink (原图) 下载保存到图库
        download_url = file_info.get('dlink')
        if not download_url:
            # 降级尝试缩略图
            thumbs = file_info.get('thumbs', {})
            download_url = thumbs.get('url3') or thumbs.get('url2') or thumbs.get('url1')
            
        if not download_url:
            print(f"[{index}] ⚠️ 无法获取文件 {file_name} 的下载链接，跳过。")
            continue
            
        full_download_url = f"{download_url}&access_token={BAIDU_ACCESS_TOKEN}"

        print(f"[{index}] 正在处理: {file_name}")
        print("   ⬇️ 正在从云端拉取图像...")
        
        try:
            print("   ⬇️ 正在从云端流式拉取原图...")
            # 增加基于 chunk 的流式下载和 verify=False 避免彻底断连
            download_success = False
            for attempt in range(3):
                try:
                    img_res = requests.get(full_download_url, headers=HEADERS, timeout=60, stream=True, verify=False)
                    img_res.raise_for_status()
                    
                    with open(local_path, 'wb') as f:
                        for chunk in img_res.iter_content(chunk_size=8192):
                            if chunk:
                                f.write(chunk)
                    download_success = True
                    break
                except Exception as e:
                    print(f"   ⚠️ 下载出错 (尝试 {attempt+1}/3): {e}")
                    time.sleep(2)
                    
            if not download_success:
                print(f"   ❌ 该张照片经过3次尝试依然下载失败，跳过。")
                if local_path.exists():
                    local_path.unlink()
                continue
            
            print(f"   🤖 正在调用本地显卡 ({OLLAMA_MODEL}) 生成长段客观描述...")
            try:
                # 显式指定本地地址，避免 httpx 被全局代理拦截
                client = ollama.Client(host='http://127.0.0.1:11434')
                
                # 第一步：生成客观描述 (caption)
                resp_caption = client.chat(
                    model=OLLAMA_MODEL,
                    messages=[{
                        'role': 'user',
                        'content': AI_PROMPT_CAPTION,
                        'images': [str(local_path)]
                    }]
                )
                ai_caption = resp_caption['message']['content'].strip()
                print(f"   📝 AI 描述结果: {ai_caption[:20]}...")
                
                print(f"   🤖 正在调用本地显卡 ({OLLAMA_MODEL}) 撰写文艺旁白...")
                # 第二步：生成文艺文案 (side_caption)
                resp_side = client.chat(
                    model=OLLAMA_MODEL,
                    messages=[{
                        'role': 'user',
                        'content': AI_PROMPT_SIDE_CAPTION,
                        'images': [str(local_path)]
                    }]
                )
                ai_side_caption = resp_side['message']['content'].strip()
                print(f"   ✨ AI 旁白结果: {ai_side_caption}")
                
            except Exception as e:
                print(f"   ❌ 该张照片处理出错: {e}")
                ai_caption = "暂无描述"
                ai_side_caption = "暂无旁白"

            # 尝试提取基本 EXIF
            exif_info = analyzer.read_exif(local_path)
            
            # 入库 photo_scores
            cur.execute("""
                INSERT OR REPLACE INTO photo_scores (
                    path, caption, side_caption, memory_score, beauty_score,
                    width, height, orientation, exif_datetime, 
                    exif_make, exif_model, exif_gps_lat, exif_gps_lon, exif_json
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """, (
                str(local_path),
                ai_caption,
                ai_side_caption,
                85.0,  # 默认给一个较高的记忆分
                80.0,  # 默认审美分
                exif_info.get('width'),
                exif_info.get('height'),
                exif_info.get('orientation'),
                exif_info.get('datetime'),
                exif_info.get('make'),
                exif_info.get('model'),
                exif_info.get('gps_lat'),
                exif_info.get('gps_lon'),
                json.dumps(exif_info, ensure_ascii=False) if exif_info else "{}"
            ))
            conn.commit()
            processed_count += 1
            print("   ✅ 已存入主图库。")

        except Exception as e:
            print(f"   ❌ 该张照片处理出错: {e}")
            if local_path.exists():
                local_path.unlink() # 出错则删除残留，下次重试
                
        print("-" * 50)

    conn.close()
    print(f"\n🎉 同步完成！本次新增 {processed_count} 张云端照片至本地主图库。")

if __name__ == "__main__":
    fetch_and_analyze()
