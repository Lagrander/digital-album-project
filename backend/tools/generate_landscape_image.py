# generate_landscape_image.py
# 保存在 W:\Desktop\digital_album_project\backend\tools\generate_landscape_image.py

from PIL import Image, ImageDraw, ImageFont
import numpy as np
import os
import webbrowser

def generate_landscape_placeholder(filename, width=800, height=480):
    # 1. 创建底图 (800x480 横屏)
    img = Image.new('RGB', (width, height))
    draw = ImageDraw.Draw(img)

    # 2. 绘制渐变背景（深空蓝）
    for y in range(height):
        r = int(30 + (y / height) * 10)
        g = int(40 + (y / height) * 15)
        b = int(60 + (y / height) * 20)
        draw.rectangle([(0, y), (width, y+1)], fill=(r, g, b))

    # 3. 加载并粘贴“重叠相册”图标
    icon_path = r"W:\Desktop\digital_album_project\backend\tools\icon.png"
    if os.path.exists(icon_path):
        icon = Image.open(icon_path).convert("RGBA")
        # 调整图标大小（由于高度变小，设定高度为 150 像素，保持比例）
        target_height = 150
        aspect_ratio = icon.width / icon.height
        target_width = int(target_height * aspect_ratio)
        icon = icon.resize((target_width, target_height), Image.Resampling.LANCZOS)
        
        # 居中计算
        x_pos = (width - target_width) // 2
        y_pos = (height - target_height) // 2 - 40  # 整体偏上，为下方文字留出空间
        img.paste(icon, (x_pos, y_pos), icon)
        print(f"Loaded icon: {icon_path}")
    else:
        print(f"Warning: icon file {icon_path} not found")

    # 4. 绘制文字（确保使用支持中文的字体）
    try:
        font_main = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 32)
        font_sub = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 20)
    except:
        print("Warning: msyh.ttc not found, using default font")
        font_main = ImageFont.load_default()
        font_sub = ImageFont.load_default()

    # 主标题
    text1 = '欢迎使用 · 电子相册'
    b1 = draw.textbbox((0,0), text1, font=font_main)
    draw.text(((width - (b1[2]-b1[0]))//2, height//2 + 50), text1, fill=(230,230,240), font=font_main)

    # 副标题
    text2 = '请通过 Web 界面上传您的第一张照片'
    b2 = draw.textbbox((0,0), text2, font=font_sub)
    draw.text(((width - (b2[2]-b2[0]))//2, height//2 + 100), text2, fill=(160,160,170), font=font_sub)

    # 5. 转换为 RGB565
    arr = np.array(img)
    data = bytearray(width * height * 2)
    idx = 0
    for row in arr:
        for p in row:
            r, g, b = p[0], p[1], p[2]
            r5 = (r >> 3) & 0x1F
            g6 = (g >> 2) & 0x3F
            b5 = (b >> 3) & 0x1F
            val = (r5 << 11) | (g6 << 5) | b5
            data[idx] = val & 0xFF
            data[idx+1] = (val >> 8) & 0xFF
            idx += 2

    # 6. 写入 RGB565 文件
    with open(filename, 'wb') as f:
        f.write(data)
    print(f'RGB565 file generated: {filename} ({len(data)} bytes)')

    # 7. 生成并打开预览图（方便查看效果）
    preview_filename = os.path.splitext(filename)[0] + "_preview.png"
    img.save(preview_filename)
    print(f'Preview saved: {preview_filename}')
    
    # 自动打开预览图
    if os.name == 'nt':
        try:
            os.startfile(preview_filename)
            print('Preview opened automatically')
        except:
            pass
    else:
        webbrowser.open(preview_filename)
        print('Preview opened automatically')

if __name__ == "__main__":
    # 确保目标文件夹存在
    target_dir = r"W:\Desktop\digital_album_project\main\data"
    target_file = os.path.join(target_dir, "placeholder_landscape.rgb565")
    
    os.makedirs(target_dir, exist_ok=True)
    generate_landscape_placeholder(target_file)
