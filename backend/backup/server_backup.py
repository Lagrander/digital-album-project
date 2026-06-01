#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
云端电子相册 Flask 服务器
- 照片浏览 WebUI
- ESP32 下载 API（RGB565 二进制）
- 上传照片上传 + 查询
"""

from __future__ import annotations

from pathlib import Path
from flask import Flask, abort, send_file, Response, request, redirect, render_template_string
import mimetypes
import sqlite3
import json
import html
import uuid
import datetime as dt
from io import BytesIO
from PIL import Image, ImageOps
import config as cfg

ROOT_DIR = Path(__file__).resolve().parent

# --- 配置 ---
DOWNLOAD_KEY = str(getattr(cfg, "DOWNLOAD_KEY", "") or "").strip()
if not DOWNLOAD_KEY:
    raise SystemExit("config.py 里没有配置 DOWNLOAD_KEY")

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

REVIEW_PAGE_SIZE = 100

app = Flask(__name__)


# 辅助函数

def _safe_join(base: Path, rel: str) -> Path:
    p = (base / rel).resolve()
    if not str(p).startswith(str(base.resolve())):
        raise ValueError("path traversal blocked")
    return p


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
    if not exif_json:
        return ""
    try:
        data = json.loads(exif_json)
    except Exception:
        return ""
    dtv = data.get("datetime")
    if not dtv:
        return ""
    try:
        date_part = str(dtv).split()[0]
        parts = date_part.replace(":", "-").split("-")
        if len(parts) >= 3:
            return f"{parts[0]}-{parts[1]}-{parts[2]}"
    except Exception:
        return ""
    return ""


# 上传照片 DB 初始化

def _init_upload_db():
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
    conn.commit()
    conn.close()


_init_upload_db()


# 照片分析 DB 加载

def load_rows(page: int = 1, page_size: int = REVIEW_PAGE_SIZE, md: str = "", sort: str = "memory"):
    if not DB_PATH.exists():
        return [], 0
    if page < 1:
        page = 1
    offset = (page - 1) * page_size

    conn = sqlite3.connect(str(DB_PATH))
    c = conn.cursor()

    dt_expr = "json_extract(exif_json, '$.datetime')"
    where_sql = ""
    params = []

    md = (md or "").strip()
    if md and len(md) == 5 and md[2] == "-":
        md_expr = f"(substr({dt_expr}, 6, 2) || '-' || substr({dt_expr}, 9, 2))"
        where_sql = f"WHERE {dt_expr} IS NOT NULL AND {md_expr} = ?"
        params.append(md)

    if where_sql:
        total = c.execute(f"SELECT COUNT(1) FROM photo_scores {where_sql}", params).fetchone()[0]
    else:
        total = c.execute("SELECT COUNT(1) FROM photo_scores").fetchone()[0]

    sort_map = {
        "beauty": "ORDER BY COALESCE(beauty_score, -1) DESC, path",
        "time_new": f"ORDER BY ({dt_expr} IS NULL) ASC, {dt_expr} DESC, path",
        "time_old": f"ORDER BY ({dt_expr} IS NULL) ASC, {dt_expr} ASC, path",
    }
    order_sql = sort_map.get(sort, "ORDER BY COALESCE(memory_score, -1) DESC, path")

    rows = c.execute(f"""
        SELECT path, caption, type, memory_score, beauty_score, reason,
               exif_json, width, height, orientation, used_at, side_caption
        FROM photo_scores {where_sql} {order_sql} LIMIT ? OFFSET ?
    """, list(params) + [page_size, offset]).fetchall()
    conn.close()
    return rows, int(total)


# /api/today — ESP32 获取当日照片列表

@app.get("/api/today")
def api_today():
    """返回当日 N 张照片的元数据 JSON"""
    today = dt.date.today()
    target_md = f"{today.month:02d}-{today.day:02d}"

    if not DB_PATH.exists():
        return {"photos": [], "date": str(today), "target_md": target_md}

    conn = sqlite3.connect(str(DB_PATH))
    c = conn.cursor()
    rows = c.execute("""
        SELECT path, exif_json, side_caption, memory_score,
               exif_gps_lat, exif_gps_lon, exif_city, caption
        FROM photo_scores WHERE exif_json IS NOT NULL
    """).fetchall()
    conn.close()

    # 按 MM-DD 分组并选片（与 render_daily.py 相同逻辑）
    items = []
    for path, exif_json, side, mem, lat, lon, city, caption in rows:
        date_str = extract_date_from_exif(exif_json)
        if not date_str:
            continue
        try:
            y, m, d = map(int, date_str.split("-"))
        except Exception:
            continue
        md = f"{m:02d}-{d:02d}"
        items.append({
            "id": Path(path).stem,
            "path": str(path),
            "date": date_str,
            "md": md,
            "side": side or "",
            "caption": side or caption or "",
            "memory": float(mem) if mem else 0,
            "lat": lat,
            "lon": lon,
            "city": city or "",
        })

    # 按 md 分组
    by_md = {}
    for it in items:
        by_md.setdefault(it["md"], []).append(it)
    for arr in by_md.values():
        arr.sort(key=lambda x: x["memory"], reverse=True)

    # 回溯选片
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
        if doy <= 0:
            doy += 365
        md = _doy_to_md(doy)
        arr = by_md.get(md, [])
        if not arr:
            continue
        candidates = [p for p in arr if p["memory"] > threshold]
        if not candidates:
            continue
        if len(candidates) >= count:
            chosen = random.sample(candidates, count)
        else:
            chosen = list(candidates)
        break

    if not chosen:
        sorted_all = sorted(items, key=lambda x: x["memory"], reverse=True)
        chosen = sorted_all[:count]

    return {
        "date": str(today),
        "target_md": target_md,
        "count": len(chosen),
        "photos": [{
            "id": p["id"],
            "date": p["date"],
            "side": p["side"],
            "caption": p["caption"],
            "memory": p["memory"],
            "city": p["city"],
            "lat": p["lat"],
            "lon": p["lon"],
        } for p in chosen],
    }


# /api/photo/<id>.rgb565 — ESP32 下载 RGB565

@app.get("/api/photo/<photo_id>.rgb565")
def api_photo_rgb565(photo_id: str):
    """ESP32 下载照片 RGB565 二进制数据"""
    safe_id = Path(photo_id).name
    p = BIN_OUTPUT_DIR / f"photo_{safe_id}.rgb565"
    if not p.exists():
        p = BIN_OUTPUT_DIR / f"{safe_id}.rgb565"
    return _send_static_file(p)


@app.get("/api/photo/<photo_id>.jpg")
def api_photo_jpg(photo_id: str):
    """ESP32 / Web 下载照片 JPEG 预览"""
    safe_id = Path(photo_id).name
    p = BIN_OUTPUT_DIR / f"preview_{safe_id}.jpg"
    if not p.exists():
        p = BIN_OUTPUT_DIR / f"{safe_id}.jpg"
    return _send_static_file(p)


# /api/upload/check — ESP32 查询上传照片

@app.get("/api/upload/check")
def api_upload_check():
    """ESP32 查询当日是否有上传照片"""
    today_str = dt.date.today().isoformat()

    conn = sqlite3.connect(str(UPLOAD_DB_PATH))
    row = conn.execute("""
        SELECT id, message, uploader_name, rgb565_path, downloaded
        FROM upload_photos
        WHERE target_date = ?
        ORDER BY created_at DESC LIMIT 1
    """, (today_str,)).fetchone()
    conn.close()

    if row is None:
        return {"has_upload": False}

    sid, message, uploader, rgb565_path, downloaded = row
    return {
        "has_upload": True,
        "id": sid,
        "message": message or "",
        "uploader_name": uploader or "神秘人",
        "downloaded": bool(downloaded),
    }


@app.get("/api/upload/<upload_id>.rgb565")
def api_upload_rgb565(upload_id: str):
    """ESP32 下载上传照片 RGB565"""
    safe_id = Path(upload_id).name
    conn = sqlite3.connect(str(UPLOAD_DB_PATH))
    row = conn.execute(
        "SELECT rgb565_path FROM upload_photos WHERE id = ?", (safe_id,)
    ).fetchone()
    if row is None or not row[0]:
        conn.close()
        abort(404)

    p = Path(row[0])
    conn.execute("UPDATE upload_photos SET downloaded = 1 WHERE id = ?", (safe_id,))
    conn.commit()
    conn.close()

    if not p.exists():
        abort(404)
    return send_file(p, mimetype="application/octet-stream", as_attachment=False)


# /upload — 手机/电脑上传照片页面

UPLOAD_PAGE_HTML = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>上传照片 - 云端电子相册</title>
<style>
:root {
    --bg: #f5f0eb;
    --card: #ffffff;
    --text: #2c2416;
    --muted: #8c7b6b;
    --accent: #d4843a;
    --accent2: #5b8c5a;
    --border: #e0d5c5;
}
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
    font-family: -apple-system, "PingFang SC", "Noto Sans SC", sans-serif;
    background: var(--bg);
    color: var(--text);
    display: flex; justify-content: center; align-items: center;
    min-height: 100vh; padding: 20px;
}
.card {
    background: var(--card);
    border-radius: 20px;
    padding: 40px;
    max-width: 480px;
    width: 100%;
    box-shadow: 0 4px 24px rgba(0,0,0,0.06);
}
h1 { font-size: 1.6em; margin-bottom: 4px; }
.subtitle { color: var(--muted); font-size: 0.9em; margin-bottom: 28px; }
.upload-zone {
    border: 2px dashed var(--border);
    border-radius: 14px;
    padding: 40px 20px;
    text-align: center;
    cursor: pointer;
    transition: border-color 0.2s;
    margin-bottom: 20px;
    position: relative;
    min-height: 120px;
    display: flex; flex-direction: column; align-items: center; justify-content: center;
}
.upload-zone:hover, .upload-zone.drag-over { border-color: var(--accent); }
.upload-zone.has-file { border-style: solid; border-color: var(--accent2); }
.upload-icon { font-size: 2.5em; margin-bottom: 8px; }
.upload-hint { color: var(--muted); font-size: 0.9em; }
.upload-hint strong { color: var(--accent); }
#preview-img { max-width: 100%; max-height: 200px; border-radius: 8px; margin-top: 12px; display: none; }
input, textarea {
    width: 100%; padding: 12px 16px; border: 1px solid var(--border);
    border-radius: 10px; font-size: 1em; font-family: inherit;
    margin-bottom: 14px; background: #fafaf8; transition: border-color 0.2s;
}
input:focus, textarea:focus { outline: none; border-color: var(--accent); }
textarea { resize: vertical; min-height: 70px; }
label { font-size: 0.85em; color: var(--muted); margin-bottom: 4px; display: block; }
.btn {
    width: 100%; padding: 14px; border: none; border-radius: 12px;
    font-size: 1.05em; font-weight: 600; cursor: pointer; transition: all 0.2s;
    color: #fff; background: var(--accent);
}
.btn:hover { opacity: 0.9; transform: translateY(-1px); }
.btn:disabled { background: #ccc; cursor: not-allowed; transform: none; }
.success-msg {
    text-align: center; padding: 40px 0; display: none;
}
.success-msg .icon { font-size: 4em; }
.success-msg h2 { margin: 16px 0 8px; }
.success-msg p { color: var(--muted); }
#file-input { display: none; }
.error { color: #d44; font-size: 0.85em; margin-bottom: 12px; display: none; }
</style>
</head>
<body>
<div class="card" id="main-card">
    <h1>上传照片</h1>
    <p class="subtitle">为远方的电子相册，传递一张属于今天的特别回忆</p>

    <div id="error-msg" class="error"></div>

    <div class="upload-zone" id="upload-zone" onclick="document.getElementById('file-input').click()">
        <div class="upload-icon">📷</div>
        <div class="upload-hint">点击选择照片 或 拖拽到此处</div>
        <div class="upload-hint" style="font-size:0.8em">支持 JPG / PNG / HEIC</div>
        <img id="preview-img" alt="预览">
    </div>
    <input type="file" id="file-input" accept="image/*">

    <label>留言（选填，最多 50 字）</label>
    <textarea id="message" maxlength="50" placeholder="想对 TA 说点什么..."></textarea>

    <label>你的昵称（选填）</label>
    <input type="text" id="name" maxlength="20" placeholder="例如：远方的妈妈">

    <label>目标日期</label>
    <input type="date" id="target-date">

    <button class="btn" id="submit-btn" onclick="uploadPhoto()">发送上传</button>
</div>

<div class="card success-msg" id="success-msg">
    <div class="icon">✨</div>
    <h2>照片已送达！</h2>
    <p id="success-text"></p>
    <button class="btn" style="margin-top:24px" onclick="location.reload()">再发一张</button>
</div>

<script>
var selectedFile = null;
var dz = document.getElementById('upload-zone');
var fi = document.getElementById('file-input');
var preview = document.getElementById('preview-img');
var submitBtn = document.getElementById('submit-btn');
var errorEl = document.getElementById('error-msg');

// 设置默认日期为今天
document.getElementById('target-date').value = new Date().toISOString().split('T')[0];

fi.addEventListener('change', function(e) {
    if (e.target.files.length > 0) handleFile(e.target.files[0]);
});

dz.addEventListener('dragover', function(e) { e.preventDefault(); dz.classList.add('drag-over'); });
dz.addEventListener('dragleave', function() { dz.classList.remove('drag-over'); });
dz.addEventListener('drop', function(e) {
    e.preventDefault();
    dz.classList.remove('drag-over');
    if (e.dataTransfer.files.length > 0) handleFile(e.dataTransfer.files[0]);
});

function handleFile(file) {
    if (!file.type.match(/^image\/(jpeg|png|heic|heif|webp)$/i)) {
        showError('不支持的文件格式，请选择图片文件');
        return;
    }
    if (file.size > 20 * 1024 * 1024) {
        showError('图片大小不能超过 20MB');
        return;
    }
    selectedFile = file;
    dz.classList.add('has-file');
    dz.querySelector('.upload-icon').textContent = '✅';
    dz.querySelector('.upload-hint').textContent = file.name;
    submitBtn.disabled = false;

    var reader = new FileReader();
    reader.onload = function(ev) {
        preview.src = ev.target.result;
        preview.style.display = 'block';
    };
    reader.readAsDataURL(file);
}

function showError(msg) {
    errorEl.textContent = msg;
    errorEl.style.display = 'block';
    setTimeout(function() { errorEl.style.display = 'none'; }, 3000);
}

function uploadPhoto() {
    if (!selectedFile) { showError('请先选择一张照片'); return; }

    submitBtn.disabled = true;
    submitBtn.textContent = '上传中...';

    var formData = new FormData();
    formData.append('photo', selectedFile);
    formData.append('message', document.getElementById('message').value);
    formData.append('name', document.getElementById('name').value);
    formData.append('target_date', document.getElementById('target-date').value);
    {% if password_required %}
    formData.append('password', prompt('请输入上传密码：'));
    {% endif %}

    fetch('/upload/send', { method: 'POST', body: formData })
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.ok) {
                document.getElementById('main-card').style.display = 'none';
                var sm = document.getElementById('success-msg');
                sm.style.display = 'block';
                document.getElementById('success-text').textContent =
                    '照片将在 ' + (data.target_date || '今天') + ' 出现在电子相册上';
            } else {
                showError(data.error || '上传失败，请重试');
                submitBtn.disabled = false;
                submitBtn.textContent = '发送上传';
            }
        })
        .catch(function() {
            showError('网络错误，请检查连接后重试');
            submitBtn.disabled = false;
            submitBtn.textContent = '发送上传';
        });
}
</script>
</body>
</html>
"""


@app.get("/upload")
def upload_page():
    password_required = bool(UPLOAD_PASSWORD)
    return render_template_string(
        UPLOAD_PAGE_HTML,
        password_required=password_required
    )


@app.post("/upload/send")
def phone_upload():
    # 密码验证
    if UPLOAD_PASSWORD:
        pw = request.form.get("password", "")
        if pw != UPLOAD_PASSWORD:
            return {"ok": False, "error": "密码错误"}, 403

    file = request.files.get("photo")
    if not file or not file.filename:
        return {"ok": False, "error": "未选择照片"}, 400

    # 文件类型检查
    allowed = {".jpg", ".jpeg", ".png", ".heic", ".heif", ".webp"}
    ext = Path(file.filename).suffix.lower()
    if ext not in allowed:
        return {"ok": False, "error": f"不支持的格式：{ext}"}, 400

    # 保存原图
    sid = uuid.uuid4().hex[:12]
    safe_name = f"{sid}{ext}"
    orig_path = UPLOAD_DIR / safe_name
    file.save(str(orig_path))

    # 渲染为 800×480 RGB565
    try:
        img = Image.open(orig_path)
        img = ImageOps.exif_transpose(img).convert("RGB")
    except Exception:
        return {"ok": False, "error": "无法解析图片文件"}, 400

    # 高级高斯模糊背景填充（竖屏 480×800）
    from PIL import ImageFilter
    LCD_W = int(getattr(cfg, "LCD_WIDTH", 480) or 480)
    LCD_H = int(getattr(cfg, "LCD_HEIGHT", 800) or 800)
    img_w, img_h = img.size
    
    # 1. 创建画布与生成模糊背景
    canvas_img = Image.new("RGB", (LCD_W, LCD_H), (0, 0, 0))
    bg_scale = max(LCD_W / img_w, LCD_H / img_h)
    bg_w, bg_h = int(img_w * bg_scale), int(img_h * bg_scale)
    img_bg_resized = img.resize((bg_w, bg_h), Image.LANCZOS)
    bg_left = (bg_w - LCD_W) // 2
    bg_top = (bg_h - LCD_H) // 2
    img_bg_cropped = img_bg_resized.crop((bg_left, bg_top, bg_left + LCD_W, bg_top + LCD_H))
    img_blurred = img_bg_cropped.filter(ImageFilter.GaussianBlur(radius=40))
    canvas_img.paste(img_blurred, (0, 0))
    
    # 2. 居中贴上清晰前景原图
    fg_scale = min(LCD_W / img_w, LCD_H / img_h)
    fg_w, fg_h = int(img_w * fg_scale), int(img_h * fg_scale)
    img_fg_resized = img.resize((fg_w, fg_h), Image.LANCZOS)
    fg_left = (LCD_W - fg_w) // 2
    fg_top = (LCD_H - fg_h) // 2
    canvas_img.paste(img_fg_resized, (fg_left, fg_top))
    
    # 后续编码统一使用合成后的 canvas_img
    img_cropped = canvas_img

    # RGB565 编码
    pixels = img_cropped.load()
    rgb565_data = bytearray(LCD_W * LCD_H * 2)
    idx = 0
    for y in range(LCD_H):
        for x in range(LCD_W):
            r, g, b = pixels[x, y]
            r5 = (r >> 3) & 0x1F
            g6 = (g >> 2) & 0x3F
            b5 = (b >> 3) & 0x1F
            val = (r5 << 11) | (g6 << 5) | b5
            rgb565_data[idx] = val & 0xFF
            rgb565_data[idx + 1] = (val >> 8) & 0xFF
            idx += 2

    rgb565_path = UPLOAD_DIR / f"{sid}.rgb565"
    with open(rgb565_path, "wb") as f:
        f.write(rgb565_data)

    # 保存 JPEG 预览
    preview_path = UPLOAD_DIR / f"{sid}_preview.jpg"
    img_cropped.save(preview_path, "JPEG", quality=85)

    # 写入数据库
    message = (request.form.get("message", "") or "").strip()[:50]
    uploader = (request.form.get("name", "") or "").strip()[:20]
    target_date = (request.form.get("target_date", "") or "").strip()
    if not target_date:
        target_date = dt.date.today().isoformat()

    conn = sqlite3.connect(str(UPLOAD_DB_PATH))
    conn.execute("""
        INSERT INTO upload_photos (id, original_name, rgb565_path, preview_path,
            message, uploader_name, target_date, created_at, downloaded)
        VALUES (?, ?, ?, ?, ?, ?, ?, datetime('now', 'localtime'), 0)
    """, (sid, file.filename, str(rgb565_path), str(preview_path),
          message, uploader, target_date))
    conn.commit()
    conn.close()

    return {"ok": True, "id": sid, "target_date": target_date}


# WebUI — 照片浏览（简化版，从 InkTime 移植）

@app.get("/")
def index():
    if ENABLE_REVIEW_WEBUI:
        return redirect("/review")
    return "InkTime Album Server — /review disabled, /api/* active"


@app.get("/review")
def review():
    if not ENABLE_REVIEW_WEBUI:
        abort(404)

    page = int(request.args.get("page", "1") or 1)
    md = (request.args.get("md", "") or "").strip()
    sort = (request.args.get("sort", "") or "memory").strip()
    rows, total = load_rows(page=page, md=md, sort=sort)

    items_html_parts = []
    for row in rows:
        path, caption, ptype, m_score, b_score, reason, exif_json, w, h, orient, used, side = row
        date_str = extract_date_from_exif(exif_json)
        img_uri = ""
        try:
            filename = Path(path).name
            img_uri = "/images/" + filename
        except Exception:
            pass
        if not img_uri:
            continue

        items_html_parts.append(f"""
        <div class="item">
            <div class="img-wrap">
                <img src="{html.escape(img_uri)}" loading="lazy">
            </div>
            <div class="meta">
                <div class="side">{html.escape(side or '')}</div>
                <div class="scores">回忆度: {m_score or '-'} / 美观度: {b_score or '-'}</div>
                <div class="date">{html.escape(date_str or '')}  ·  {html.escape(orient or '')}</div>
                <div class="caption">{html.escape(caption or '')}</div>
            </div>
        </div>""")

    total_pages = (total + REVIEW_PAGE_SIZE - 1) // REVIEW_PAGE_SIZE
    page_links = "".join(
        f'<a href="?page={p}&md={html.escape(md)}&sort={html.escape(sort)}"'
        f'{" class=active" if p == page else ""}>{p}</a> '
        for p in range(1, total_pages + 1)
    ) if total_pages > 1 else ""

    html_str = f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>电子相册 - 照片库</title>
<style>
:root {{ --bg: #f5f0eb; --card: #fff; --text: #2c2416; --muted: #8c7b6b; }}
* {{ box-sizing: border-box; margin: 0; padding: 0; }}
body {{ font-family: -apple-system,"PingFang SC",sans-serif; background: var(--bg); color: var(--text); padding: 20px; }}
h1 {{ text-align: center; margin-bottom: 20px; }}
.filters {{ text-align: center; margin-bottom: 20px; }}
.filters a {{ padding: 6px 14px; margin: 0 4px; border-radius: 8px; text-decoration: none; color: var(--text); background: var(--card); }}
.filters a.active {{ background: #d4843a; color: #fff; }}
.gallery {{ display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: 16px; }}
.item {{ background: var(--card); border-radius: 12px; overflow: hidden; box-shadow: 0 2px 8px rgba(0,0,0,0.04); }}
.img-wrap img {{ width: 100%; display: block; aspect-ratio: 4/3; object-fit: cover; }}
.meta {{ padding: 12px 16px; }}
.side {{ font-weight: 600; font-size: 1.05em; }}
.scores {{ color: var(--muted); font-size: 0.85em; margin: 4px 0; }}
.date {{ color: var(--muted); font-size: 0.8em; }}
.caption {{ font-size: 0.9em; margin-top: 6px; line-height: 1.5; }}
.pages {{ text-align: center; margin-top: 24px; }}
.pages a {{ padding: 4px 10px; text-decoration: none; color: var(--text); }}
.pages a.active {{ font-weight: 700; color: #d4843a; }}
.surprise-link {{ text-align: center; margin-top: 30px; padding-top: 20px; border-top: 1px solid #e0d5c5; }}
.surprise-link a {{ color: #d4843a; font-weight: 600; text-decoration: none; font-size: 1.1em; }}
</style>
</head>
<body>
<h1>云端电子相册</h1>
<div class="filters">
    排序：
    <a href="?sort=memory" class="{'active' if sort == 'memory' else ''}">回忆度</a>
    <a href="?sort=beauty" class="{'active' if sort == 'beauty' else ''}">美观度</a>
    <a href="?sort=time_new" class="{'active' if sort == 'time_new' else ''}">最新</a>
    <a href="?sort=time_old" class="{'active' if sort == 'time_old' else ''}">最早</a>
</div>
<div class="gallery">{''.join(items_html_parts)}</div>
<div class="pages">{page_links}</div>
<div class="surprise-link">
    <a href="/upload">✨ 上传今日照片</a>
</div>
</body>
</html>"""

    return Response(html_str, mimetype="text/html; charset=utf-8")


@app.get("/images/<path:subpath>")
def images(subpath: str):
    if not ENABLE_REVIEW_WEBUI:
        abort(404)
    safe_name = Path(subpath).name
    filepath = IMAGE_DIR / safe_name
    if not filepath.is_file():
        abort(404)
    return send_file(str(filepath))


# 启动

if __name__ == "__main__":
    mimetypes.add_type("application/octet-stream", ".rgb565")
    print(f"[Album] DB: {DB_PATH}")
    print(f"[Album] IMAGE_DIR: {IMAGE_DIR}")
    print(f"[Album] OUT: {BIN_OUTPUT_DIR}")
    print(f"[Album] DOWNLOAD_KEY: {DOWNLOAD_KEY}")
    print(f"[Album] Upload password: {'set' if UPLOAD_PASSWORD else 'none (public)'}")
    print(f"[Album] listen: {FLASK_HOST}:{FLASK_PORT}")
    app.run(host=FLASK_HOST, port=FLASK_PORT, debug=False)
