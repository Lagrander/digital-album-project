#!/usr/bin/env python3
"""
Generate LVGL 8.x compatible font C file from a TTF/TTC font.

Generates a font covering ASCII + most common CJK characters
suitable for an ESP32 digital photo album display.

Usage:
    python generate_lvgl_font.py --font C:/Windows/Fonts/msyh.ttc --size 16
        --bpp 2 --output ../main/lv_ui/src/lv_font_cjk_16.c --name lv_font_cjk_16

Requirements:
    pip install Pillow
"""

import argparse
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("Error: Pillow is required. Install with: pip install Pillow")
    sys.exit(1)


def make_charset(cjk_count=3755):
    """
    Build the character set ordered by Unicode codepoint.

    Includes:
    - ASCII printable (U+0020 - U+007E)
    - CJK Unified Ideographs first N chars (U+4E00 - U+4E00+N-1)
    - Common fullwidth punctuation

    Characters are returned sorted by codepoint.
    """
    chars = set()

    # ASCII printable
    for cp in range(0x20, 0x7F):
        chars.add(chr(cp))

    # GB2312 Level 1 and 2 characters
    # If cjk_count <= 3755, only Level 1 (0xB0 to 0xD7) is included to save space.
    max_b1 = 0xD8 if cjk_count <= 3755 else 0xF8
    for b1 in range(0xB0, max_b1):
        for b2 in range(0xA1, 0xFF):
            try:
                ch = bytes([b1, b2]).decode('gb2312')
                chars.add(ch)
            except:
                pass

    # Fullwidth punctuation and symbols
    extra = [
        0x3001, 0x3002, 0xFF01, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B,
        0xFF1F, 0x2018, 0x2019, 0x201C, 0x201D, 0x2014, 0x2026,
        0x00B0,  # degree sign
        0x00D7,  # multiplication sign
        0x00B7,  # middle dot (·)
        0x2103,  # Celsius sign (℃)
        0x25A0, 0x25A1, 0x25B2, 0x25BC, # Some geometric shapes
    ]
    for cp in extra:
        chars.add(chr(cp))

    # Use actual needed characters only
    codepoints = sorted(list(set(ord(c) for c in chars)))
    return [chr(cp) for cp in codepoints]


def pack_pixels(pixels, bpp):
    """
    Pack 8-bit pixel values (0-255) into LVGL bitmap bytes.

    LVGL format: MSB = leftmost pixel within each byte.
    Row-by-row, left-to-right, top-to-bottom.
    """
    max_val = (1 << bpp) - 1
    ppb = 8 // bpp  # pixels per byte

    result = bytearray()
    for i, p in enumerate(pixels):
        byte_idx = i // ppb
        bit_pos = i % ppb
        shift = (ppb - 1 - bit_pos) * bpp  # MSB first
        q = int(round(p / 255.0 * max_val))
        if byte_idx >= len(result):
            result.append(0)
        result[byte_idx] |= (q << shift)
    return bytes(result)


def render_glyph(font, char, size):
    """
    Render a single character.
    Returns (bitmap_bytes, box_w, box_h, adv_w, ofs_x, ofs_y) or None for empty.
    """
    bbox = font.getbbox(char)
    if bbox is None:
        return None

    left, top, right, bottom = bbox
    w = right - left
    h = bottom - top

    if w <= 0 or h <= 0:
        # Empty glyph (e.g., space) — still needs metrics
        try:
            adv = int(font.getlength(char))
        except Exception:
            adv = size // 2
        return (b"", 0, 0, adv if adv > 0 else size // 2, 0, 0)

    # Add 1px padding on each side for anti-aliasing
    pad = 1
    img_w = w + pad * 2
    img_h = h + pad * 2

    img = Image.new("L", (img_w, img_h), 0)
    draw = ImageDraw.Draw(img)
    draw.text((pad - left, pad - top), char, font=font, fill=255)

    raw = list(img.getdata())
    bitmap = pack_pixels(raw, 2)  # Using 2bpp internally

    try:
        adv = int(font.getlength(char))
    except Exception:
        adv = size
    if adv <= 0:
        adv = size

    ofs_x = left - pad
    ofs_y = top - pad

    return (bitmap, img_w, img_h, adv, ofs_x, ofs_y)


def build_cmaps(chars, start_glyph_id=0):
    """
    Build a single sparse cmap table for all characters.
    """
    if not chars:
        return []

    codepoints = [ord(c) for c in chars]
    
    # We will use one SPARSE_TINY cmap.
    # range_start will be the first codepoint.
    range_start = codepoints[0]
    
    # unicode_list will contain offsets from range_start
    unicode_list = [cp - range_start for cp in codepoints]
    
    # Check if max offset exceeds uint16_t (it shouldn't for GB2312)
    if unicode_list[-1] > 65535:
        print("Warning: offset exceeds 16-bit! Need multiple cmaps or SPARSE_FULL.")

    return [{
        "range_start": range_start,
        "range_length": codepoints[-1] - range_start + 1,
        "glyph_id_start": start_glyph_id,
        "type": "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY",
        "unicode_list": unicode_list,
    }]


def generate_font_c(chars, font_path, font_size, bpp, font_name, output_path, ttc_index=0):
    print(f"Font: {font_path}  size={font_size}  bpp={bpp}")
    print(f"Characters: {len(chars)}")

    font = ImageFont.truetype(font_path, font_size, index=ttc_index)
    ascent, descent = font.getmetrics()

    glyphs = []          # list of dicts, indexed by glyph_id (same order as chars)
    bitmap_buf = bytearray()

    print("Rendering glyphs...")
    for idx, char in enumerate(chars):
        if idx % 500 == 0:
            print(f"  {idx}/{len(chars)}")

        cp = ord(char)
        result = render_glyph(font, char, font_size)
        if result is None:
            bitmap, bw, bh, adv, ox, oy = b"", 0, 0, size // 2, 0, 0
        else:
            bitmap, bw, bh, adv, ox, oy = result

        bm_index = len(bitmap_buf)
        bitmap_buf.extend(bitmap)

        # LVGL stores adv_w in fixed-point: 8.4 or 28.4 format (real * 16)
        glyphs.append({
            "cp": cp,
            "bitmap_index": bm_index,
            "adv_w": adv * 16,
            "box_w": bw,
            "box_h": bh,
            "ofs_x": ox,
            "ofs_y": oy,
        })

    # Build cmaps from the sorted chars
    cmaps = build_cmaps(chars, start_glyph_id=0)

    print(f"Glyphs: {len(glyphs)}  Bitmap bytes: {len(bitmap_buf)}")

    # ── Write C file ──
    guard = f"LV_FONT_{font_name.upper()}"

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("/*******************************************************************************\n")
        f.write(f" * LVGL Font: {font_name}\n")
        f.write(f" * Size: {font_size} px  Bpp: {bpp}\n")
        f.write(f" * Glyphs: {len(glyphs)}\n")
        f.write(f" * Generated by generate_lvgl_font.py\n")
        f.write(" ******************************************************************************/\n\n")

        f.write("#include \"lvgl.h\"\n\n")

        f.write(f"#ifndef {guard}\n")
        f.write(f"    #define {guard} 1\n")
        f.write("#endif\n\n")

        f.write(f"#if {guard}\n\n")

        # ── Bitmap ──
        f.write("/*Store the image of the glyphs*/\n")
        f.write(f"static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {{\n")
        line = "    "
        for i, byte_val in enumerate(bitmap_buf):
            line += f"0x{byte_val:02x}, "
            if (i + 1) % 16 == 0:
                f.write(line.rstrip() + "\n")
                line = "    "
        if line.strip():
            f.write(line.rstrip() + "\n")
        f.write("};\n\n")

        # ── Glyph descriptors ──
        f.write("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n")
        for i, g in enumerate(glyphs):
            f.write(f"    {{.bitmap_index = {g['bitmap_index']}, "
                    f".adv_w = {g['adv_w']}, "
                    f".box_w = {g['box_w']}, "
                    f".box_h = {g['box_h']}, "
                    f".ofs_x = {g['ofs_x']}, "
                    f".ofs_y = {g['ofs_y']}}}, /* {i}: U+{g['cp']:04X} */\n")
        f.write("};\n\n")

        # ── CMAPs ──
        f.write("static const uint16_t unicode_list_0[] = {\n")
        ul = cmaps[0]["unicode_list"]
        line = "    "
        for i, ofs in enumerate(ul):
            line += f"{ofs}, "
            if (i + 1) % 16 == 0:
                f.write(line.rstrip() + "\n")
                line = "    "
        if line.strip():
            f.write(line.rstrip() + "\n")
        f.write("};\n\n")

        f.write("static const lv_font_fmt_txt_cmap_t cmaps[] = {\n")
        for i, cmap in enumerate(cmaps):
            f.write("    {\n")
            f.write(f"        .range_start = {cmap['range_start']},\n")
            f.write(f"        .range_length = {cmap['range_length']},\n")
            f.write(f"        .glyph_id_start = {cmap['glyph_id_start']},\n")
            f.write(f"        .unicode_list = unicode_list_{i},\n")
            f.write(f"        .glyph_id_ofs_list = NULL,\n")
            f.write(f"        .list_length = {len(cmap['unicode_list'])},\n")
            f.write(f"        .type = {cmap['type']},\n")
            f.write("    },\n")
        f.write("};\n\n")

        # ── Kerning (none) ──
        f.write("static const lv_font_fmt_txt_kern_classes_t kern_classes = {\n")
        f.write("    .class_pair_values = NULL,\n")
        f.write("    .left_class_mapping = NULL,\n")
        f.write("    .right_class_mapping = NULL,\n")
        f.write("    .left_class_cnt = 0,\n")
        f.write("    .right_class_cnt = 0,\n")
        f.write("};\n\n")

        # ── Font descriptor ──
        f.write("static const lv_font_fmt_txt_dsc_t font_dsc = {\n")
        f.write("    .glyph_bitmap = glyph_bitmap,\n")
        f.write("    .glyph_dsc = glyph_dsc,\n")
        f.write("    .cmaps = cmaps,\n")
        f.write("    .kern_dsc = &kern_classes,\n")
        f.write(f"    .kern_scale = 16,\n")
        f.write(f"    .cmap_num = {len(cmaps)},\n")
        f.write(f"    .bpp = {bpp},\n")
        f.write("    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,\n")
        f.write("};\n\n")

        # ── Public font struct ──
        line_height = ascent + descent
        base_line = descent
        f.write(f"const lv_font_t {font_name} = {{\n")
        f.write("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n")
        f.write("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n")
        f.write(f"    .line_height = {line_height},\n")
        f.write(f"    .base_line = {base_line},\n")
        f.write("    .subpx = LV_FONT_SUBPX_NONE,\n")
        f.write("    .underline_position = -1,\n")
        f.write("    .underline_thickness = 1,\n")
        f.write("    .dsc = &font_dsc,\n")
        f.write("};\n\n")

        f.write(f"#endif /*{guard}*/\n")

    print(f"Done → {output_path}")
    print(f"  Glyphs: {len(glyphs)}  Bitmap: {len(bitmap_buf)} bytes  "
          f"~{(len(bitmap_buf)+len(glyphs)*12)/1024:.1f} KB total")


def main():
    parser = argparse.ArgumentParser(description="Generate LVGL 8.x CJK font C file")
    parser.add_argument("--font", required=True, help="Path to TTF/TTC font")
    parser.add_argument("--ttc-index", type=int, default=0)
    parser.add_argument("--size", type=int, default=16, help="Font pixel size")
    parser.add_argument("--bpp", type=int, default=2, choices=[1, 2, 4])
    parser.add_argument("--cjk-count", type=int, default=3755,
                        help="Number of CJK characters to include (default: 3755 ≈ GB2312 Level 1)")
    parser.add_argument("--output", required=True, help="Output .c file path")
    parser.add_argument("--name", required=True, help="Font variable name")

    args = parser.parse_args()

    if not os.path.exists(args.font):
        print(f"Error: Font not found: {args.font}")
        sys.exit(1)

    chars = make_charset(cjk_count=args.cjk_count)
    print(f"Charset: {len(chars)} glyphs (ASCII + {args.cjk_count} CJK + punctuation)")

    generate_font_c(
        chars=chars,
        font_path=args.font,
        font_size=args.size,
        bpp=args.bpp,
        font_name=args.name,
        output_path=args.output,
        ttc_index=args.ttc_index,
    )


if __name__ == "__main__":
    main()
