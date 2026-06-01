#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
每日相册渲染脚本（彩色 LCD 版）：
- 从 photos.db 选出"历史上的今天"照片
- 渲染到 800×480 横屏布局
- 输出 RGB565 二进制（ESP32 可直接载入 PSRAM 显示）
- 同时生成 JPEG 预览图

与 InkTime 的区别：
- 800×480 横屏（非 480×800 竖屏）
- RGB565 输出（非 4 色调色板抖动）
- 无 Floyd-Steinberg 抖动
"""

from __future__ import annotations

import sys
from pathlib import Path
sys.path.append(str(Path(__file__).resolve().parent.parent))

import sqlite3
import json
import datetime as dt
import struct
from typing import List, Dict, Any, Tuple, Optional
from PIL import Image, ImageDraw, ImageFont, ImageOps
import config as cfg

TODAY = dt.date.today()

# === 路径配置 ===
ROOT_DIR = Path(__file__).resolve().parent.parent

DB_PATH = Path(str(getattr(cfg, "DB_PATH", "photos.db") or "photos.db")).expanduser()
if not DB_PATH.is_absolute():
    DB_PATH = (ROOT_DIR / DB_PATH).resolve()

BIN_OUTPUT_DIR = Path(str(getattr(cfg, "BIN_OUTPUT_DIR", "output") or "output")).expanduser()
if not BIN_OUTPUT_DIR.is_absolute():
    BIN_OUTPUT_DIR = (ROOT_DIR / BIN_OUTPUT_DIR).resolve()
BIN_OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

FONT_PATH = Path(str(getattr(cfg, "FONT_PATH", "") or "")).expanduser()
if str(FONT_PATH) and not FONT_PATH.is_absolute():
    FONT_PATH = (ROOT_DIR / FONT_PATH).resolve()

MEMORY_THRESHOLD = float(getattr(cfg, "MEMORY_THRESHOLD", 70.0) or 70.0)
DAILY_PHOTO_QUANTITY = int(getattr(cfg, "DAILY_PHOTO_QUANTITY", 5) or 5)

# 横屏尺寸
CANVAS_WIDTH = int(getattr(cfg, "LCD_WIDTH", 800) or 800)
CANVAS_HEIGHT = int(getattr(cfg, "LCD_HEIGHT", 480) or 480)
# TEXT_AREA_HEIGHT = 0：纯净图片模式（推荐），文字由 ESP32 LVGL 叠加层显示
# TEXT_AREA_HEIGHT = 80：兼容旧横屏模式，底部文字烘焙进图片
TEXT_AREA_HEIGHT = int(getattr(cfg, "TEXT_AREA_HEIGHT", 0) or 0)


# ========== DB 与 EXIF 处理 ==========

def extract_date_from_exif(exif_json: Optional[str]) -> str:
    if not exif_json:
        return ""
    try:
        data = json.loads(exif_json)
    except Exception:
        return ""
    dt_str = data.get("datetime")
    if not dt_str:
        return ""
    try:
        date_part = str(dt_str).split()[0]
        parts = date_part.replace(":", "-").split("-")
        if len(parts) >= 3:
            return f"{parts[0]}-{parts[1]}-{parts[2]}"
    except Exception:
        return ""
    return ""


def load_sim_rows() -> List[Dict[str, Any]]:
    if not DB_PATH.exists():
        raise SystemExit(f"找不到数据库文件: {DB_PATH}")

    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    rows = c.execute(
        """
        SELECT path, exif_json, side_caption, memory_score,
               exif_gps_lat, exif_gps_lon, exif_city
        FROM photo_scores
        WHERE exif_json IS NOT NULL
        """
    ).fetchall()
    conn.close()

    items: List[Dict[str, Any]] = []
    for path, exif_json, side_caption, memory_score, gps_lat, gps_lon, exif_city in rows:
        date_str = extract_date_from_exif(exif_json)
        if not date_str:
            continue
        if "screenshot" in str(path).lower():
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
            "side": side_caption or "",
            "memory": float(memory_score) if memory_score is not None else -1.0,
            "lat": gps_lat,
            "lon": gps_lon,
            "city": exif_city or "",
        })
    return items


# ========== "历史上的今天"选片（与 InkTime 完全一致） ==========

def md_to_day_of_year(md: str) -> Optional[int]:
    try:
        m, d = map(int, md.split("-"))
        days_before = [0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334]
        if m < 1 or m > 12:
            return None
        return days_before[m] + d
    except Exception:
        return None


def day_of_year_to_md(day: int) -> str:
    base = dt.date(2001, 1, 1) + dt.timedelta(days=day - 1)
    return f"{base.month:02d}-{base.day:02d}"


def choose_photos_for_today(items: List[Dict[str, Any]], today: dt.date, count: int = 5) -> Tuple[List[Dict[str, Any]], Dict[str, Any]]:
    if not items:
        raise RuntimeError("没有任何可用照片")

    by_md: Dict[str, List[Dict[str, Any]]] = {}
    for it in items:
        md = it["md"]
        by_md.setdefault(md, []).append(it)

    for arr in by_md.values():
        arr.sort(key=lambda x: x.get("memory", -1.0), reverse=True)

    target_md = f"{today.month:02d}-{today.day:02d}"
    target_doy = md_to_day_of_year(target_md)
    if target_doy is None:
        raise RuntimeError(f"无法解析今天的月日: {target_md}")

    import random

    for offset in range(0, 365):
        doy = target_doy - offset
        if doy <= 0:
            doy += 365
        md = day_of_year_to_md(doy)

        arr = by_md.get(md, [])
        if not arr:
            continue
        candidates = [p for p in arr if p.get("memory", -1.0) > MEMORY_THRESHOLD]
        if not candidates:
            continue

        if len(candidates) >= count:
            chosen_list = random.sample(candidates, count)
        else:
            chosen_list = list(candidates)
            for extra in arr:
                if extra in chosen_list:
                    continue
                chosen_list.append(extra)
                if len(chosen_list) >= count:
                    break

        info = {
            "target_md": target_md,
            "used_md": md,
            "day_offset": -offset,
            "candidate_count": len(candidates),
            "total_count_md": len(arr),
            "threshold": MEMORY_THRESHOLD,
            "fallback_global_max": False,
        }
        return chosen_list, info

    sorted_all = sorted(items, key=lambda x: x.get("memory", -1.0), reverse=True)
    chosen_list = sorted_all[:count]
    info = {
        "target_md": target_md,
        "used_md": chosen_list[0]["md"] if chosen_list else "",
        "day_offset": None,
        "candidate_count": len(chosen_list),
        "total_count_md": len(items),
        "threshold": MEMORY_THRESHOLD,
        "fallback_global_max": True,
    }
    return chosen_list, info


# ========== 文字排版辅助 ==========

def wrap_text_chinese(draw: ImageDraw.ImageDraw, text: str, font: ImageFont.FreeTypeFont,
                      max_width: int, max_lines: int) -> List[str]:
    if not text:
        return []
    lines: List[str] = []
    line = ""
    for ch in text:
        test = line + ch
        w = draw.textlength(test, font=font)
        if w <= max_width:
            line = test
        else:
            if line:
                lines.append(line)
            line = ch
            if len(lines) >= max_lines:
                break
    if line and len(lines) < max_lines:
        lines.append(line)
    return lines


def format_date_display(date_str: str) -> str:
    if not date_str:
        return ""
    parts = date_str.split("-")
    if len(parts) < 3:
        return date_str
    try:
        m = str(int(parts[1]))
        d = str(int(parts[2]))
    except Exception:
        return date_str
    return f"{parts[0]}.{m}.{d}"


def format_location(lat, lon, city: str) -> str:
    if city and str(city).strip():
        return str(city).strip()
    if lat is None or lon is None:
        return ""
    try:
        return f"{float(lat):.5f}, {float(lon):.5f}"
    except Exception:
        return ""


# ========== 渲染（横屏 800×480） ==========

def render_image(item: Dict[str, Any]) -> Image.Image:
    """
    渲染图片到画布（默认竖屏 480×800，纯净图片模式）：
    采用高斯模糊背景填充方案 (Blurred Padding)
    """
    from PIL import ImageFilter
    
    canvas = Image.new("RGB", (CANVAS_WIDTH, CANVAS_HEIGHT), (0, 0, 0))
    draw = ImageDraw.Draw(canvas)

    # ---------- 加载原图 ----------
    img_path = Path(item["path"])
    if not img_path.exists():
        raise RuntimeError(f"图片不存在: {img_path}")
    img = Image.open(img_path)
    img = ImageOps.exif_transpose(img).convert("RGB")

    img_w, img_h = img.size
    if img_w == 0 or img_h == 0:
        raise RuntimeError(f"图片尺寸非法: {img.size}")

    # ---------- 照片区域尺寸 ----------
    img_area_w = CANVAS_WIDTH
    img_area_h = CANVAS_HEIGHT - TEXT_AREA_HEIGHT  # TEXT_AREA_HEIGHT=0 时即全屏

    # 1. 高斯模糊背景生成 (等比放大覆盖，大半径虚化)
    bg_scale = max(img_area_w / img_w, img_area_h / img_h)
    bg_draw_w = int(img_w * bg_scale)
    bg_draw_h = int(img_h * bg_scale)
    img_bg_resized = img.resize((bg_draw_w, bg_draw_h), Image.LANCZOS)
    bg_left = (bg_draw_w - img_area_w) // 2
    bg_top = (bg_draw_h - img_area_h) // 2
    img_bg_cropped = img_bg_resized.crop((bg_left, bg_top, bg_left + img_area_w, bg_top + img_area_h))
    
    # 虚化大半径 40
    img_blurred = img_bg_cropped.filter(ImageFilter.GaussianBlur(radius=30))
    canvas.paste(img_blurred, (0, 0))

    # 2. 清晰原图居中叠放 (等比缩小完全装入，居中)
    # Width-Cover: 强制让照片的宽度撑满屏幕（消灭左右极窄的毛玻璃）。
    # 超出屏幕高度的部分会被居中裁减，以保证视觉上完全贴合屏幕左右边界。
    fg_scale = img_area_w / img_w
    fg_draw_w = int(img_w * fg_scale)
    fg_draw_h = int(img_h * fg_scale)
    img_fg_resized = img.resize((fg_draw_w, fg_draw_h), Image.LANCZOS)
    
    fg_left = (img_area_w - fg_draw_w) // 2
    fg_top = (img_area_h - fg_draw_h) // 2
    
    canvas.paste(img_fg_resized, (fg_left, fg_top))

    # ---------- 底部文字区域（仅 TEXT_AREA_HEIGHT > 0 时绘制，当前已设为 0）----------
    if TEXT_AREA_HEIGHT > 0:
        padding_x = 20
        text_area_top = CANVAS_HEIGHT - TEXT_AREA_HEIGHT + 8
        text_width = CANVAS_WIDTH - 2 * padding_x

        try:
            font_big   = ImageFont.truetype(str(FONT_PATH), 20) if str(FONT_PATH) else ImageFont.load_default()
            font_small = ImageFont.truetype(str(FONT_PATH), 18) if str(FONT_PATH) else ImageFont.load_default()
        except Exception:
            font_big   = ImageFont.load_default()
            font_small = ImageFont.load_default()

        # 左侧：日期 + 城市
        date_display = format_date_display(item["date"])
        loc_display  = format_location(item.get("lat"), item.get("lon"), item.get("city") or "")
        left_text    = f"{date_display}  {loc_display}" if loc_display else date_display
        draw.text((padding_x, text_area_top), left_text, font=font_small, fill=(60, 60, 60))

        # 右侧：文案
        side_text = item.get("side") or ""
        if side_text:
            lines  = wrap_text_chinese(draw, side_text, font_big, text_width - 180, max_lines=2)
            line_y = text_area_top
            for line in lines:
                line_w = draw.textlength(line, font=font_big)
                draw.text((CANVAS_WIDTH - padding_x - line_w, line_y), line, font=font_big, fill=(0, 0, 0))
                line_y += 22

    return canvas


# ========== RGB565 编码 ==========

def image_to_rgb565(img: Image.Image) -> bytes:
    """
    将 RGB PIL Image 转换为 RGB565 字节流（小端序，每像素 2 字节）。
    RGB565: RRRRRGGG GGGBBBBB
    """
    img = img.convert("RGB")
    if img.size != (CANVAS_WIDTH, CANVAS_HEIGHT):
        raise RuntimeError(f"图像尺寸错误：{img.size}，应为 ({CANVAS_WIDTH}, {CANVAS_HEIGHT})")

    pixels = img.load()
    data = bytearray(CANVAS_WIDTH * CANVAS_HEIGHT * 2)
    idx = 0
    for y in range(CANVAS_HEIGHT):
        for x in range(CANVAS_WIDTH):
            r, g, b = pixels[x, y]
            r5 = (r >> 3) & 0x1F
            g6 = (g >> 2) & 0x3F
            b5 = (b >> 3) & 0x1F
            val = (r5 << 11) | (g6 << 5) | b5
            # 小端序
            data[idx] = val & 0xFF
            data[idx + 1] = (val >> 8) & 0xFF
            idx += 2
    return bytes(data)


# ========== 主流程 ==========

def main():
    items = load_sim_rows()
    if not items:
        raise SystemExit("没有可用照片（exif_json 为空或解析失败）。")

    photos = items

    print(f"[INFO] 准备渲染全部 {len(photos)} 张照片...")
    
    if not photos:
        raise SystemExit("选片结果为空。")

    import shutil

    for idx, chosen in enumerate(photos):
        print(f"[INFO] 第 {idx} 张: {chosen['path']}")
        print(f"[INFO]   日期: {chosen['date']}  回忆度: {chosen['memory']}")

        img = render_image(chosen)

        # 保存 JPEG 预览
        preview_path = BIN_OUTPUT_DIR / f"preview_{chosen['id']}.jpg"
        img.save(preview_path, "JPEG", quality=85)
        print(f"[OK] 预览: {preview_path}")

        # 输出 RGB565
        rgb565_data = image_to_rgb565(img)
        rgb565_path = BIN_OUTPUT_DIR / f"photo_{chosen['id']}.rgb565"
        with open(rgb565_path, "wb") as f:
            f.write(rgb565_data)
        print(f"[OK] RGB565: {rgb565_path} ({len(rgb565_data)} bytes)")

    # 兼容 latest.*
    first_rgb565 = BIN_OUTPUT_DIR / "photo_0.rgb565"
    first_preview = BIN_OUTPUT_DIR / "preview_0.jpg"
    if first_rgb565.exists():
        shutil.copyfile(first_rgb565, BIN_OUTPUT_DIR / "latest.rgb565")
    if first_preview.exists():
        shutil.copyfile(first_preview, BIN_OUTPUT_DIR / "preview.jpg")

    print("[OK] 全部渲染完成")


if __name__ == "__main__":
    main()
