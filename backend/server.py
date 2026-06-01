#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
云端电子相册 Flask 服务器
- 模块化 WebUI 页面（基于 templates）
- 增强的照片管理、多维度检索、自定义标签、快捷编辑
- 设备专属状态面板与极客遥控、局域网轮询协议
- 智能助手交互审计与香薰联动
"""

from __future__ import annotations

from pathlib import Path
from flask import Flask, abort, send_file, Response, request, redirect, render_template, jsonify
import mimetypes
import sqlite3
import json
import html
import uuid
import time
import datetime as dt
from io import BytesIO
from PIL import Image, ImageOps
import config as cfg

ROOT_DIR = Path(__file__).resolve().parent

# --- 配置 ---
DOWNLOAD_KEY = str(getattr(cfg, "DOWNLOAD_KEY", "") or "").strip()
UPLOAD_PASSWORD = str(getattr(cfg, "UPLOAD_PASSWORD", "") or "").strip()

DB_PATH = Path(str(getattr(cfg, "DB_PATH", "./photos.db") or "./photos.db")).expanduser()
if not DB_PATH.is_absolute():
    DB_PATH = (ROOT_DIR / DB_PATH).resolve()

IMAGE_DIR = Path(str(getattr(cfg, "IMAGE_DIR", "") or "")).expanduser()
if not IMAGE_DIR.is_absolute():
    IMAGE_DIR = (ROOT_DIR / IMAGE_DIR).resolve()

BIN_OUTPUT_DIR = Path(str(getattr(cfg, "BIN_OUTPUT_DIR", "./output") or "./output")).expanduser()
if not BIN_OUTPUT_DIR.is_absolute():
    BIN_OUTPUT_DIR = (ROOT_DIR / BIN_OUTPUT_DIR).resolve()
BIN_OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

UPLOAD_DB_PATH = Path(str(getattr(cfg, "UPLOAD_DB_PATH", "./upload.db") or "./upload.db")).expanduser()
if not UPLOAD_DB_PATH.is_absolute():
    UPLOAD_DB_PATH = (ROOT_DIR / UPLOAD_DB_PATH).resolve()

UPLOAD_DIR = Path(str(getattr(cfg, "UPLOAD_DIR", "./upload_photos") or "./upload_photos")).expanduser()
if not UPLOAD_DIR.is_absolute():
    UPLOAD_DIR = (ROOT_DIR / UPLOAD_DIR).resolve()
UPLOAD_DIR.mkdir(parents=True, exist_ok=True)

FLASK_HOST = str(getattr(cfg, "FLASK_HOST", "0.0.0.0") or "0.0.0.0")
FLASK_PORT = int(getattr(cfg, "FLASK_PORT", 8765) or 8765)
ENABLE_REVIEW_WEBUI = bool(getattr(cfg, "ENABLE_REVIEW_WEBUI", True))
DAILY_PHOTO_QUANTITY = int(getattr(cfg, "DAILY_PHOTO_QUANTITY", 5) or 5)

# 强制模板重新加载，便于开发时修改 HTML 即时生效
app = Flask(__name__, template_folder="templates", static_folder="static")
app.config['TEMPLATES_AUTO_RELOAD'] = True

# 设备状态全局缓存机制
DEVICE_STATE = {
    "last_seen": 0,
    "fps": 0.0,
    "free_mem": 0,
    "current_photo_id": "",
    "aroma_channels": [0, 0, 0],
    "pending_command": None
}

# DB 初始化与静默热迁移
def _init_databases():
    # 1. 初始化 upload.db，增加 dialog_history 表用于智能助手审计
    conn = sqlite3.connect(str(UPLOAD_DB_PATH))
    conn.execute("""
        CREATE TABLE IF NOT EXISTS upload_photos (
            id TEXT PRIMARY KEY,
            original_name TEXT,
            rgb565_path TEXT,
            preview_path TEXT,
            message TEXT,
            uploader_name TEXT,
            target_date TEXT,
            created_at TEXT,
            downloaded INTEGER DEFAULT 0
        )
    """)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS dialog_history (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            sender TEXT,
            content TEXT,
            aroma_change TEXT,
            photo_change_id TEXT,
            created_at TEXT
        )
    """)
    conn.commit()
    conn.close()

    # 2. 静默热升级 photos.db，引入 tags 和 hidden
    if DB_PATH.exists():
        conn = sqlite3.connect(str(DB_PATH))
        c = conn.cursor()
        try:
            c.execute("ALTER TABLE photo_scores ADD COLUMN tags TEXT")
        except sqlite3.OperationalError:
            pass
        try:
            c.execute("ALTER TABLE photo_scores ADD COLUMN hidden INTEGER DEFAULT 0")
        except sqlite3.OperationalError:
            pass
        conn.commit()
        conn.close()

_init_databases()

# 辅助函数
def _send_static_file(p: Path) -> Response:
    if not p.exists() or not p.is_file():
        abort(404)
    if p.suffix.lower() == ".rgb565":
        return send_file(p, mimetype="application/octet-stream", as_attachment=False)
    mt, _ = mimetypes.guess_type(str(p))
    if mt:
        return send_file(p, mimetype=mt, as_attachment=False)
    return send_file(p, as_attachment=False)

def extract_date_from_exif(exif_json: str | None) -> str:
    """
    从 EXIF JSON 字符串中提取拍摄日期。
    
    :param exif_json: 包含 EXIF 信息的 JSON 字符串
    :return: 格式化为 YYYY-MM-DD 的日期字符串，若提取失败则返回空字符串
    """
    if not exif_json:
        return ""
    try:
        data = json.loads(exif_json)
        dtv = data.get("datetime")
        if not dtv:
            return ""
        date_part = str(dtv).split()[0]
        parts = date_part.replace(":", "-").split("-")
        if len(parts) >= 3:
            return f"{parts[0]}-{parts[1]}-{parts[2]}"
    except Exception:
        pass
    return ""

# 照片查询逻辑
def load_rows(page: int = 1, page_size: int = 20, md: str = "", filter_date: str = "", tag: str = "", query: str = "", sort: str = "memory"):
    """
    从数据库加载并筛选照片记录。
    
    :param page: 当前页码
    :param page_size: 每页数量
    :param md: 按月日匹配 (例如 05-28)
    :param filter_date: 按完整日期匹配
    :param tag: 按标签匹配
    :param query: 全文模糊搜索关键字
    :param sort: 排序依据
    :return: 照片行字典列表与总记录数
    """
    if not DB_PATH.exists():
        return [], 0
    if page < 1: page = 1
    offset = (page - 1) * page_size

    conn = sqlite3.connect(str(DB_PATH))
    conn.row_factory = sqlite3.Row
    c = conn.cursor()

    dt_expr = "json_extract(exif_json, '$.datetime')"
    where_clauses = ["IFNULL(hidden, 0) = 0"]
    params = []

    md = (md or "").strip()
    if md and len(md) == 5 and md[2] == "-":
        md_expr = f"(substr({dt_expr}, 6, 2) || '-' || substr({dt_expr}, 9, 2))"
        where_clauses.append(f"{dt_expr} IS NOT NULL AND {md_expr} = ?")
        params.append(md)
        
    filter_date = (filter_date or "").strip()
    if filter_date:
        search_date = filter_date.replace("-", ":")
        where_clauses.append(f"({dt_expr} LIKE ? OR exif_datetime LIKE ?)")
        params.append(search_date + "%")
        params.append(search_date + "%")
        
    tag = (tag or "").strip()
    if tag:
        where_clauses.append(f"IFNULL(tags, '') LIKE ?")
        params.append(f"%{tag}%")
        
    query = (query or "").strip()
    if query:
        where_clauses.append(f"(path LIKE ? OR caption LIKE ? OR side_caption LIKE ? OR reason LIKE ? OR IFNULL(tags, '') LIKE ?)")
        for _ in range(5):
            params.append(f"%{query}%")

    where_sql = "WHERE " + " AND ".join(where_clauses)
    total = c.execute(f"SELECT COUNT(1) FROM photo_scores {where_sql}", params).fetchone()[0]

    sort_map = {
        "beauty": "ORDER BY COALESCE(beauty_score, -1) DESC, path",
        "time_new": f"ORDER BY ({dt_expr} IS NULL) ASC, {dt_expr} DESC, path",
        "time_old": f"ORDER BY ({dt_expr} IS NULL) ASC, {dt_expr} ASC, path",
    }
    order_sql = sort_map.get(sort, "ORDER BY COALESCE(memory_score, -1) DESC, path")

    rows = c.execute(f"""
        SELECT path, caption, type, memory_score, beauty_score, reason,
               exif_json, width, height, orientation, used_at, side_caption,
               exif_datetime, exif_make, exif_model, exif_city, exif_gps_lat, exif_gps_lon, tags
        FROM photo_scores {where_sql} {order_sql} LIMIT ? OFFSET ?
    """, list(params) + [page_size, offset]).fetchall()
    
    result = [dict(r) for r in rows]
    conn.close()
    return result, int(total)

# API: 网页端照片管理交互
@app.route("/api/photos/hide", methods=["POST"])
def api_hide_photo():
    data = request.json or {}
    path = data.get("path")
    if not path:
        return jsonify({"ok": False, "error": "缺少 path 参数"}), 400
    conn = sqlite3.connect(str(DB_PATH))
    conn.execute("UPDATE photo_scores SET hidden = 1 WHERE path = ?", (path,))
    conn.commit()
    conn.close()
    return jsonify({"ok": True})

@app.route("/api/photos/update_tags", methods=["POST"])
def api_update_tags():
    data = request.json or {}
    path = data.get("path")
    tags = data.get("tags", "")
    if not path:
        return jsonify({"ok": False, "error": "缺少 path 参数"}), 400
    conn = sqlite3.connect(str(DB_PATH))
    conn.execute("UPDATE photo_scores SET tags = ? WHERE path = ?", (tags, path))
    conn.commit()
    conn.close()
    return jsonify({"ok": True})

@app.route("/api/photos/update_side_caption", methods=["POST"])
def api_update_side_caption():
    data = request.json or {}
    path = data.get("path")
    side_caption = data.get("side_caption", "")
    if not path:
        return jsonify({"ok": False, "error": "缺少 path 参数"}), 400
    conn = sqlite3.connect(str(DB_PATH))
    conn.execute("UPDATE photo_scores SET side_caption = ? WHERE path = ?", (side_caption, path))
    conn.commit()
    conn.close()
    return jsonify({"ok": True})

# API: 设备联络协议与遥控器
@app.route("/api/device/heartbeat", methods=["POST"])
def api_device_heartbeat():
    data = request.json or {}
    DEVICE_STATE["last_seen"] = time.time()
    if "fps" in data: DEVICE_STATE["fps"] = float(data["fps"])
    if "free_mem" in data: DEVICE_STATE["free_mem"] = int(data["free_mem"])
    if "current_photo_id" in data: DEVICE_STATE["current_photo_id"] = data["current_photo_id"]
    if "aroma_channels" in data: DEVICE_STATE["aroma_channels"] = data["aroma_channels"]
    return jsonify({"ok": True})

@app.route("/api/device/status", methods=["GET"])
def api_device_status():
    # 判断是否在线，30秒无心跳即掉线
    is_online = (time.time() - DEVICE_STATE["last_seen"]) < 30
    return jsonify({
        "online": is_online,
        "fps": DEVICE_STATE["fps"],
        "free_mem": DEVICE_STATE["free_mem"],
        "current_photo_id": DEVICE_STATE["current_photo_id"],
        "aroma_channels": DEVICE_STATE["aroma_channels"],
        "pending_command": DEVICE_STATE["pending_command"]
    })

@app.route("/api/device/control", methods=["POST"])
def api_device_control():
    data = request.json or {}
    cmd = data.get("cmd")
    if cmd == "switch_photo":
        DEVICE_STATE["pending_command"] = {"cmd": "switch_photo", "target_id": data.get("target_id", "next")}
    elif cmd == "toggle_aroma":
        DEVICE_STATE["pending_command"] = {"cmd": "toggle_aroma", "channel": data.get("channel", 0), "state": data.get("state", 1)}
    return jsonify({"ok": True})

@app.route("/api/device/command", methods=["GET"])
def api_device_command():
    # 给 ESP32 用的接口，获取后自动清空
    cmd = DEVICE_STATE["pending_command"]
    DEVICE_STATE["pending_command"] = None
    return jsonify({"command": cmd})

# API: 智能交互审计与情感联络
@app.route("/api/dialogs/send", methods=["POST"])
def api_dialogs_send():
    data = request.json or {}
    user_msg = data.get("message", "").strip()
    if not user_msg:
        return jsonify({"ok": False, "error": "空消息"})
    
    # 记录用户发言
    conn = sqlite3.connect(str(UPLOAD_DB_PATH))
    conn.execute("INSERT INTO dialog_history (sender, content, created_at) VALUES (?, ?, datetime('now', 'localtime'))", ("user", user_msg))
    
    # 简易情感规则引擎与香薰联动
    ai_reply = "我听到了。为您准备了一张温暖的照片，希望您能喜欢。"
    aroma_change = None
    photo_change = None

    if "累" in user_msg or "疲惫" in user_msg or "压力" in user_msg or "薰衣草" in user_msg:
        ai_reply = "抱歉听到您今天这么疲惫。已自动为您开启 1 号「薰衣草」香薰阀门，这抹花香具有很好的舒缓助眠功效。同时，相册已为您切换到一张宁静悠闲的旅行风景照，愿这片晚霞带给您片刻的温柔。"
        aroma_change = "通道1(薰衣草)开启"
        DEVICE_STATE["pending_command"] = {"cmd": "toggle_aroma", "channel": 0, "state": 1}
    elif "开心" in user_msg or "兴奋" in user_msg or "茉莉" in user_msg:
        ai_reply = "感受到您的愉悦！已为您开启 2 号「茉莉」香薰，清新的香气与您的好心情绝配。"
        aroma_change = "通道2(茉莉)开启"
        DEVICE_STATE["pending_command"] = {"cmd": "toggle_aroma", "channel": 1, "state": 1}
    elif "平静" in user_msg or "禅" in user_msg or "檀香" in user_msg:
        ai_reply = "已为您开启 3 号「檀香」香薰。凝神静气，享受这一刻的静谧。"
        aroma_change = "通道3(檀香)开启"
        DEVICE_STATE["pending_command"] = {"cmd": "toggle_aroma", "channel": 2, "state": 1}
    
    # 记录 AI 回复与事件联动
    conn.execute("INSERT INTO dialog_history (sender, content, aroma_change, photo_change_id, created_at) VALUES (?, ?, ?, ?, datetime('now', 'localtime'))", 
                 ("ai", ai_reply, aroma_change, photo_change))
    conn.commit()
    conn.close()

    return jsonify({"ok": True, "reply": ai_reply, "aroma_change": aroma_change})

@app.route("/api/dialogs/history", methods=["GET"])
def api_dialogs_history():
    conn = sqlite3.connect(str(UPLOAD_DB_PATH))
    conn.row_factory = sqlite3.Row
    rows = conn.execute("SELECT id, sender, content, aroma_change, photo_change_id, created_at FROM dialog_history ORDER BY id ASC").fetchall()
    result = [dict(r) for r in rows]
    conn.close()
    return jsonify({"history": result})

@app.route("/api/dialogs/clear", methods=["POST"])
def api_dialogs_clear():
    conn = sqlite3.connect(str(UPLOAD_DB_PATH))
    conn.execute("DELETE FROM dialog_history")
    conn.commit()
    conn.close()
    return jsonify({"ok": True})

# API: 惊喜队列与历史记录 (供前端请求)
@app.route("/api/upload/history", methods=["GET"])
def api_upload_history():
    conn = sqlite3.connect(str(UPLOAD_DB_PATH))
    conn.row_factory = sqlite3.Row
    rows = conn.execute("SELECT id, original_name, preview_path, message, uploader_name, target_date, created_at, downloaded FROM upload_photos ORDER BY created_at DESC").fetchall()
    result = [dict(r) for r in rows]
    conn.close()
    return jsonify({"history": result})

@app.route("/api/upload/delete", methods=["POST"])
def api_upload_delete():
    data = request.json or {}
    uid = data.get("id")
    if not uid: return jsonify({"ok": False})
    conn = sqlite3.connect(str(UPLOAD_DB_PATH))
    conn.execute("DELETE FROM upload_photos WHERE id = ?", (uid,))
    conn.commit()
    conn.close()
    return jsonify({"ok": True})

@app.route("/api/upload/clear", methods=["POST"])
def api_upload_clear():
    conn = sqlite3.connect(str(UPLOAD_DB_PATH))
    conn.execute("DELETE FROM upload_photos")
    conn.commit()
    conn.close()
    return jsonify({"ok": True})

@app.route("/api/baidu/sync", methods=["POST"])
def api_baidu_sync():
    """手动触发百度网盘增量同步任务"""
    import subprocess
    try:
        # 异步非阻塞执行同步脚本
        subprocess.Popen(["python", str(ROOT_DIR / "tasks" / "baidu_sync_task.py")])
        return jsonify({"ok": True, "message": "百度云端同步任务已在后台启动"})
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)})

# 页面路由: 使用 Template 引擎渲染模块化 UI
@app.get("/")
def index():
    if ENABLE_REVIEW_WEBUI:
        return redirect("/review")
    return "InkTime Album Server — /review disabled, /api/* active"

@app.get("/review")
def review():
    if not ENABLE_REVIEW_WEBUI: abort(404)
    page = int(request.args.get("page", "1") or 1)
    md = (request.args.get("md", "") or "").strip()
    filter_date = (request.args.get("filter_date", "") or "").strip()
    tag = (request.args.get("tag", "") or "").strip()
    query = (request.args.get("query", "") or "").strip()
    sort = (request.args.get("sort", "") or "memory").strip()
    
    rows, total = load_rows(page=page, page_size=20, md=md, filter_date=filter_date, tag=tag, query=query, sort=sort)
    total_pages = (total + 19) // 20

    # 获取系统已有所有不重复的 年月 和 标签 供下拉筛选
    conn = sqlite3.connect(str(DB_PATH))
    c = conn.cursor()
    c.execute("SELECT DISTINCT substr(json_extract(exif_json, '$.datetime'), 1, 7) FROM photo_scores WHERE IFNULL(hidden, 0) = 0 AND json_extract(exif_json, '$.datetime') IS NOT NULL")
    date_options = sorted([r[0].replace(':', '-') for r in c.fetchall() if r[0]], reverse=True)
    c.execute("SELECT tags FROM photo_scores WHERE IFNULL(hidden, 0) = 0 AND tags IS NOT NULL AND tags != ''")
    all_tags = set()
    for (t_str,) in c.fetchall():
        for t in t_str.split(","):
            if t.strip(): all_tags.add(t.strip())
    conn.close()

    # 处理照片格式以适配前端渲染
    photos = []
    for r in rows:
        r_dict = dict(r)
        r_dict["date_str"] = extract_date_from_exif(r_dict.get("exif_json"))
        try:
            # 尝试计算出相对于 IMAGE_DIR 的子路径（例如 baidu_cloud/xxx.jpg）
            rel_path = Path(r_dict["path"]).relative_to(IMAGE_DIR)
            r_dict["img_uri"] = "/images/" + str(rel_path).replace("\\", "/")
        except ValueError:
            # 如果不在 IMAGE_DIR 内，降级使用 basename
            r_dict["img_uri"] = "/images/" + Path(r_dict["path"]).name
            
        photos.append(r_dict)

    return render_template("review.html", photos=photos, page=page, total_pages=total_pages, 
                           md=md, filter_date=filter_date, tag=tag, query=query, sort=sort,
                           date_options=date_options, tag_options=sorted(list(all_tags)))

@app.get("/upload")
def upload_page():
    password_required = bool(UPLOAD_PASSWORD)
    return render_template("upload.html", password_required=password_required)

@app.get("/device")
def device_page():
    return render_template("device.html")

@app.get("/dialogs")
def dialogs_page():
    return render_template("dialogs.html")

@app.get("/images/<path:subpath>")
def images(subpath: str):
    if not ENABLE_REVIEW_WEBUI: abort(404)
    # 直接使用传过来的带有文件夹层级的路径
    filepath = IMAGE_DIR / subpath
    if not filepath.is_file(): abort(404)
    return send_file(str(filepath))

# ESP32 核心依赖接口（保持原样兼容）
@app.get("/api/today")
def api_today():
    """
    设备端调用：获取当日待展示的轮播照片列表。
    自动根据当天月日及照片打分进行智能推荐选取。
    """
    today = dt.date.today()
    target_md = f"{today.month:02d}-{today.day:02d}"
    if not DB_PATH.exists():
        return {"photos": [], "date": str(today), "target_md": target_md}
    conn = sqlite3.connect(str(DB_PATH))
    c = conn.cursor()
    rows = c.execute("""
        SELECT path, exif_json, side_caption, memory_score,
               exif_gps_lat, exif_gps_lon, exif_city, caption
        FROM photo_scores WHERE exif_json IS NOT NULL AND IFNULL(hidden, 0) = 0
    """).fetchall()
    conn.close()

    items = []
    for path, exif_json, side, mem, lat, lon, city, caption in rows:
        date_str = extract_date_from_exif(exif_json)
        if not date_str: continue
        try:
            y, m, d = map(int, date_str.split("-"))
            md = f"{m:02d}-{d:02d}"
            items.append({
                "id": Path(path).stem, "path": str(path), "date": date_str, "md": md,
                "side": side or "", "caption": side or caption or "", "memory": float(mem) if mem else 0,
                "lat": lat, "lon": lon, "city": city or ""
            })
        except Exception: pass

    by_md = {}
    for it in items: by_md.setdefault(it["md"], []).append(it)
    for arr in by_md.values(): arr.sort(key=lambda x: x["memory"], reverse=True)

    def _md_to_doy(md_str):
        m, d = map(int, md_str.split("-"))
        days_before = [0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334]
        return days_before[m] + d
    def _doy_to_md(doy):
        base = dt.date(2001, 1, 1) + dt.timedelta(days=doy - 1)
        return f"{base.month:02d}-{base.day:02d}"

    target_doy = _md_to_doy(target_md)
    threshold = float(getattr(cfg, "MEMORY_THRESHOLD", 70.0) or 70.0)
    count = DAILY_PHOTO_QUANTITY

    import random
    chosen = []
    for offset in range(365):
        doy = target_doy - offset
        if doy <= 0: doy += 365
        md = _doy_to_md(doy)
        arr = by_md.get(md, [])
        if not arr: continue
        candidates = [p for p in arr if p["memory"] > threshold]
        if not candidates: continue
        if len(candidates) >= count: chosen = random.sample(candidates, count)
        else: chosen = list(candidates)
        break

    if not chosen:
        sorted_all = sorted(items, key=lambda x: x["memory"], reverse=True)
        chosen = sorted_all[:count]

    return {
        "date": str(today), "target_md": target_md, "count": len(chosen),
        "photos": [{"id": p["id"], "date": p["date"], "side": p["side"], "caption": p["caption"],
                    "memory": p["memory"], "city": p["city"], "lat": p["lat"], "lon": p["lon"]} for p in chosen],
    }

@app.get("/api/photo/<photo_id>.rgb565")
def api_photo_rgb565(photo_id: str):
    """
    设备端调用：获取指定照片转换后的 RGB565 原始像素点阵文件。
    """
    safe_id = Path(photo_id).name
    p = BIN_OUTPUT_DIR / f"photo_{safe_id}.rgb565"
    if not p.exists(): p = BIN_OUTPUT_DIR / f"{safe_id}.rgb565"
    return _send_static_file(p)

@app.get("/api/photo/<photo_id>.jpg")
def api_photo_jpg(photo_id: str):
    safe_id = Path(photo_id).name
    p = BIN_OUTPUT_DIR / f"preview_{safe_id}.jpg"
    if not p.exists(): p = BIN_OUTPUT_DIR / f"{safe_id}.jpg"
    return _send_static_file(p)

@app.get("/api/upload/check")
def api_upload_check():
    today_str = dt.date.today().isoformat()
    conn = sqlite3.connect(str(UPLOAD_DB_PATH))
    row = conn.execute("""
        SELECT id, message, uploader_name, rgb565_path, downloaded
        FROM upload_photos WHERE target_date = ? ORDER BY created_at DESC LIMIT 1
    """, (today_str,)).fetchone()
    conn.close()
    if row is None: return {"has_upload": False}
    sid, message, uploader, rgb565_path, downloaded = row
    return {"has_upload": True, "id": sid, "message": message or "", "uploader_name": uploader or "神秘人", "downloaded": bool(downloaded)}

@app.get("/api/upload/<upload_id>.rgb565")
def api_upload_rgb565(upload_id: str):
    safe_id = Path(upload_id).name
    conn = sqlite3.connect(str(UPLOAD_DB_PATH))
    row = conn.execute("SELECT rgb565_path FROM upload_photos WHERE id = ?", (safe_id,)).fetchone()
    if row is None or not row[0]:
        conn.close()
        abort(404)
    p = Path(row[0])
    conn.execute("UPDATE upload_photos SET downloaded = 1 WHERE id = ?", (safe_id,))
    conn.commit()
    conn.close()
    if not p.exists(): abort(404)
    return send_file(p, mimetype="application/octet-stream", as_attachment=False)

@app.post("/upload/send")
def phone_upload():
    """
    接收手机端或 Web 端的照片上传，进行缩放裁剪和边缘模糊等处理，
    最终生成设备支持的 RGB565 格式供云端下发。
    """
    if UPLOAD_PASSWORD:
        pw = request.form.get("password", "")
        if pw != UPLOAD_PASSWORD: return {"ok": False, "error": "密码错误"}, 403
    file = request.files.get("photo")
    if not file or not file.filename: return {"ok": False, "error": "未选择照片"}, 400
    ext = Path(file.filename).suffix.lower()
    if ext not in {".jpg", ".jpeg", ".png", ".heic", ".heif", ".webp"}:
        return {"ok": False, "error": f"不支持的格式：{ext}"}, 400

    sid = uuid.uuid4().hex[:12]
    safe_name = f"{sid}{ext}"
    orig_path = UPLOAD_DIR / safe_name
    file.save(str(orig_path))

    try:
        img = Image.open(orig_path)
        img = ImageOps.exif_transpose(img).convert("RGB")
    except Exception: return {"ok": False, "error": "无法解析图片文件"}, 400

    from PIL import ImageFilter
    LCD_W = int(getattr(cfg, "LCD_WIDTH", 480) or 480)
    LCD_H = int(getattr(cfg, "LCD_HEIGHT", 800) or 800)
    img_w, img_h = img.size
    
    canvas_img = Image.new("RGB", (LCD_W, LCD_H), (0, 0, 0))
    bg_scale = max(LCD_W / img_w, LCD_H / img_h)
    bg_w, bg_h = int(img_w * bg_scale), int(img_h * bg_scale)
    img_bg_resized = img.resize((bg_w, bg_h), Image.LANCZOS)
    bg_left = (bg_w - LCD_W) // 2
    bg_top = (bg_h - LCD_H) // 2
    img_bg_cropped = img_bg_resized.crop((bg_left, bg_top, bg_left + LCD_W, bg_top + LCD_H))
    img_blurred = img_bg_cropped.filter(ImageFilter.GaussianBlur(radius=40))
    canvas_img.paste(img_blurred, (0, 0))
    
    # Width-Cover: 强制让照片宽度撑满屏幕（避免手机长图左右两侧出现窄边毛玻璃）。
    # 对于横屏宽图，上下会留出大块极具美感的高斯模糊；对于超细长图，上下多出部分会被平滑居中裁剪。
    fg_scale = LCD_W / img_w
    fg_w, fg_h = int(img_w * fg_scale), int(img_h * fg_scale)
    img_fg_resized = img.resize((fg_w, fg_h), Image.LANCZOS)
    fg_left = (LCD_W - fg_w) // 2
    fg_top = (LCD_H - fg_h) // 2
    canvas_img.paste(img_fg_resized, (fg_left, fg_top))
    
    pixels = canvas_img.load()
    rgb565_data = bytearray(LCD_W * LCD_H * 2)
    idx = 0
    for y in range(LCD_H):
        for x in range(LCD_W):
            r, g, b = pixels[x, y]
            val = (((r >> 3) & 0x1F) << 11) | (((g >> 2) & 0x3F) << 5) | ((b >> 3) & 0x1F)
            rgb565_data[idx] = val & 0xFF
            rgb565_data[idx + 1] = (val >> 8) & 0xFF
            idx += 2

    rgb565_path = UPLOAD_DIR / f"{sid}.rgb565"
    with open(rgb565_path, "wb") as f: f.write(rgb565_data)
    preview_path = UPLOAD_DIR / f"{sid}_preview.jpg"
    canvas_img.save(preview_path, "JPEG", quality=85)

    message = (request.form.get("message", "") or "").strip()[:50]
    uploader = (request.form.get("name", "") or "").strip()[:20]
    target_date = (request.form.get("target_date", "") or "").strip()
    if not target_date: target_date = dt.date.today().isoformat()

    conn = sqlite3.connect(str(UPLOAD_DB_PATH))
    conn.execute("""
        INSERT INTO upload_photos (id, original_name, rgb565_path, preview_path,
            message, uploader_name, target_date, created_at, downloaded)
        VALUES (?, ?, ?, ?, ?, ?, ?, datetime('now', 'localtime'), 0)
    """, (sid, file.filename, str(rgb565_path), f"/api/upload/preview/{sid}_preview.jpg", message, uploader, target_date))
    conn.commit()
    conn.close()

    return {"ok": True, "id": sid, "target_date": target_date}

@app.get("/api/upload/preview/<filename>")
def api_upload_preview(filename: str):
    safe_name = Path(filename).name
    p = UPLOAD_DIR / safe_name
    if not p.exists(): abort(404)
    return _send_static_file(p)

# 启动
if __name__ == "__main__":
    print(f"=========================================")
    print(f" 云端电子相册 服务器启动")
    print(f" - WebUI 端口: {FLASK_PORT}")
    print(f" - 设备状态: DEVICE_STATE 已初始化")
    print(f" - DB 挂载: tags, hidden 字段以及 dialog_history 表已更新")
    print(f"=========================================")
    app.run(host=FLASK_HOST, port=FLASK_PORT, debug=True)
