#!/usr/bin/env python3
"""
PNG to LVGL C Array Converter (For LVGL 8.x) - Multi-Color-Depth Auto-Adaptive Version

Generates multiple precompiled condition segments (16bit Swap=0, 16bit Swap=1, 32bit ARGB)
based on '#if LV_COLOR_DEPTH' to guarantee perfect, non-garbled display results 
across different screens and drivers.

Author: Antigravity AI
"""

import os
import sys
import glob

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow is required. Please run: pip install Pillow")
    sys.exit(1)

# 配置项
ASSETS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "../main/lv_ui/assets"))
OUTPUT_C = os.path.abspath(os.path.join(os.path.dirname(__file__), "../main/lv_ui/src/ui_png_images.c"))
OUTPUT_H = os.path.abspath(os.path.join(os.path.dirname(__file__), "../main/lv_ui/include/ui_png_images.h"))

# 默认缩放大小（适合顶部栏34px高度，也极度节省 ESP32 Flash 空间）
TARGET_SIZE = (20, 20) 

def convert_png_to_lvgl_data(img_path, target_w, target_h):
    """
    读取 PNG，缩放到目标尺寸，并生成三种色彩模式的字节序列：
    1. 16位小端 (Swap=0): [low, high, a]
    2. 16位大端 (Swap=1): [high, low, a]
    3. 32位全彩 (ARGB):   [b, g, r, a]
    """
    img = Image.open(img_path)
    
    # 自动进行高质量缩放以适应嵌入式屏幕顶栏并节省 RAM/Flash
    if img.size != (target_w, target_h):
        try:
            resample_filter = Image.Resampling.LANCZOS
        except AttributeError:
            resample_filter = Image.ANTIALIAS
        img = img.resize((target_w, target_h), resample_filter)
        
    img = img.convert("RGBA")
    width, height = img.size
    
    bytes_16_no_swap = bytearray()
    bytes_16_swap = bytearray()
    bytes_32 = bytearray()
    
    for y in range(height):
        for x in range(width):
            r, g, b, a = img.getpixel((x, y))
            
            # 1. 16位色彩深度计算
            r5 = (r >> 3) & 0x1F
            g6 = (g >> 2) & 0x3F
            b5 = (b >> 3) & 0x1F
            rgb565 = (r5 << 11) | (g6 << 5) | b5
            
            low = rgb565 & 0xFF
            high = (rgb565 >> 8) & 0xFF
            
            # 16位 Swap=0
            bytes_16_no_swap.append(low)
            bytes_16_no_swap.append(high)
            bytes_16_no_swap.append(a)
            
            # 16位 Swap=1
            bytes_16_swap.append(high)
            bytes_16_swap.append(low)
            bytes_16_swap.append(a)
            
            # 2. 32位色彩深度计算 (小端 BGRA)
            bytes_32.append(b)
            bytes_32.append(g)
            bytes_32.append(r)
            bytes_32.append(a)
            
    return bytes_16_no_swap, bytes_16_swap, bytes_32, width, height

def format_hex_lines(data_bytes):
    """把字节数组格式化为优雅的C数组文本，每12个换行"""
    lines = []
    for i in range(0, len(data_bytes), 12):
        chunk = data_bytes[i:i+12]
        hex_str = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append("    " + hex_str + ",")
    return lines

def main():
    print("🚀 开始将 PNG 批量转化为自适应 LVGL C 数组...")
    
    if not os.path.exists(ASSETS_DIR):
        os.makedirs(ASSETS_DIR)
        print(f"📁 已自动为您创建资产文件夹: {ASSETS_DIR}")
        print("💡 请将您的精美 PNG 图标放入该文件夹，然后重新运行本脚本！")
        return

    png_files = glob.glob(os.path.join(ASSETS_DIR, "*.png"))
    if not png_files:
        print(f"⚠️  未在 {ASSETS_DIR} 找到任何 .png 图标文件！")
        print("💡 请在 assets 文件夹中放入 temp.png, hum.png 等图标后再运行。")
        return

    # 生成 C 文件头部
    c_content = [
        "/*",
        " * LVGL 8.x PNG Images C Array",
        " * Generated automatically by png_to_lvgl.py",
        " * Do NOT modify this file manually.",
        " */",
        "",
        '#include "lvgl.h"',
        ""
    ]
    
    # 生成 H 文件头部
    h_content = [
        "/*",
        " * LVGL 8.x PNG Images Declaring Header",
        " * Generated automatically by png_to_lvgl.py",
        " * Do NOT modify this file manually.",
        " */",
        "",
        "#ifndef UI_PNG_IMAGES_H",
        "#define UI_PNG_IMAGES_H",
        "",
        '#include "lvgl.h"',
        "",
        "#ifdef __cplusplus",
        "extern \"C\" {",
        "#endif",
        ""
    ]

    for png_path in png_files:
        filename = os.path.basename(png_path)
        var_name = "ui_img_" + os.path.splitext(filename)[0].lower()
        
        print(f"🎨 正在自适应处理: {filename} -> {var_name}")
        
        # 转换并自动下采样
        b16_no_swap, b16_swap, b32, w, h = convert_png_to_lvgl_data(png_path, TARGET_SIZE[0], TARGET_SIZE[1])
        
        c_content.append(f"/* ── 图片资源: {filename} ({w}x{h}) ── */")
        
        # 1. 16位 Swap=0 数组
        c_content.append("#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0")
        c_content.append(f"static const uint8_t {var_name}_map_16_no_swap[] = {{")
        c_content.extend(format_hex_lines(b16_no_swap))
        c_content.append("};")
        c_content.append("#endif")
        c_content.append("")
        
        # 2. 16位 Swap=1 数组
        c_content.append("#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP != 0")
        c_content.append(f"static const uint8_t {var_name}_map_16_swap[] = {{")
        c_content.extend(format_hex_lines(b16_swap))
        c_content.append("};")
        c_content.append("#endif")
        c_content.append("")
        
        # 3. 32位全彩 数组
        c_content.append("#if LV_COLOR_DEPTH == 32")
        c_content.append(f"static const uint8_t {var_name}_map_32[] = {{")
        c_content.extend(format_hex_lines(b32))
        c_content.append("};")
        c_content.append("#endif")
        c_content.append("")
        
        # 4. 统一的自适应图像结构体声明
        c_content.append(f"const lv_img_dsc_t {var_name} = {{")
        c_content.append("    .header.always_zero = 0,")
        c_content.append(f"    .header.w = {w},")
        c_content.append(f"    .header.h = {h},")
        c_content.append("    .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,")
        
        # 数据字节大小
        c_content.append("#if LV_COLOR_DEPTH == 16")
        c_content.append(f"    .data_size = {len(b16_no_swap)},")
        c_content.append("#elif LV_COLOR_DEPTH == 32")
        c_content.append(f"    .data_size = {len(b32)},")
        c_content.append("#endif")
        
        # 数据指针绑定
        c_content.append("#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0")
        c_content.append(f"    .data = {var_name}_map_16_no_swap,")
        c_content.append("#elif LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP != 0")
        c_content.append(f"    .data = {var_name}_map_16_swap,")
        c_content.append("#elif LV_COLOR_DEPTH == 32")
        c_content.append(f"    .data = {var_name}_map_32,")
        c_content.append("#endif")
        
        c_content.append("};")
        c_content.append("")
        
        # 写入 H 文件外部声明
        h_content.append(f"extern const lv_img_dsc_t {var_name}; /* 源自 {filename} */")

    # 封尾
    h_content.extend([
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
        "",
        "#endif /* UI_PNG_IMAGES_H */"
    ])

    # 写入 C 和 H 文件
    with open(OUTPUT_C, "w", encoding="utf-8") as f:
        f.write("\n".join(c_content))
    with open(OUTPUT_H, "w", encoding="utf-8") as f:
        f.write("\n".join(h_content))
        
    print(f"\n🎉 自适应转换成功！")
    print(f"💾 自适应 C 像素数组已保存至: {OUTPUT_C}")
    print(f"💾 外部变量声明已保存至: {OUTPUT_H}")

if __name__ == "__main__":
    main()
