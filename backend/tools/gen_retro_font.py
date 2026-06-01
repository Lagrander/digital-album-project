#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_retro_font.py — 霞鹜文楷复古字体子集生成器
================================================

用途：为 ESP32 数字电子相册生成 LVGL 复古字体 C 文件。
采用「选项 A」策略：
  - 仅提取固定 UI 文字 + 常见城市名约 260 字
  - Flash 占用约 50~80 KB（远小于全字库的 2~3 MB）
  - AI 生成的 side_caption 仍使用已有的 lv_font_cjk_16（全字库）

依赖工具：
  - lv_font_conv（推荐）：Node.js 生态，官方 LVGL 字体转换器
      npm install -g @lvgl/lv_font_conv
  - 字体文件：霞鹜文楷 LXGWWenKai-Regular.ttf
      下载：https://github.com/lxgw/LxgwWenKai/releases

用法：
  python gen_retro_font.py [--font 字体路径] [--size 像素大小] [--bpp 位数]

输出：
  lv_font_retro_20.c → 复制到 main/lv_ui/src/
  然后在 CMakeLists.txt 添加该文件，并在 ui_main.c 启用对应注释行
"""

import subprocess
import shutil
import argparse
import sys
from pathlib import Path

# 字符集定义

# 固定 UI 文字（历史弹窗、惊喜横幅、状态提示等）
UI_FIXED_CHARS = (
    "历史记录暂无关闭今日惊喜给你发来一张照片有人神秘人"
    "温湿度摄氏度百分比上一张下一张前后设置确认取消"
    "年月日时分秒星期一二三四五六七"
    "云端电子相册正在加载请稍候连接失败离线模式"
)

# 常见中国城市名（覆盖拍摄地显示所需）
CITY_CHARS = (
    "北京上海广州深圳成都重庆武汉西安杭州南京苏州天津"
    "长沙郑州青岛沈阳大连哈尔滨厦门福州济南昆明太原"
    "合肥南昌长春贵阳南宁兰州银川西宁乌鲁木齐拉萨"
    "宁波无锡温州佛山东莞珠海惠州中山汕头潮州梅州"
    "三亚海口桂林柳州南宁玉林北海钦州防城港"
    "呼和浩特包头鄂尔多斯赤峰通辽"
    "石家庄唐山保定邯郸秦皇岛廊坊张家口承德"
    "泉州漳州龙岩三明南平宁德莆田"
    "烟台潍坊淄博济宁泰安临沂德州聊城滨州东营日照"
    "洛阳开封新乡安阳焦作许昌平顶山南阳商丘"
    "常州镇江扬州南通连云港盐城淮安徐州"
    "芜湖马鞍山安庆蚌埠淮南铜陵黄山宣城滁州"
    "赣州景德镇萍乡九江上饶抚州吉安宜春新余鹰潭"
    "衡阳株洲湘潭常德岳阳益阳娄底张家界怀化邵阳"
    "宜昌荆州荆门孝感黄冈黄石十堰襄阳随州"
    "绵阳德阳宜宾泸州自贡南充遂宁内江达州广元"
    "遵义六盘水安顺毕节铜仁凯里都匀"
    "曲靖玉溪丽江大理保山昭通楚雄临沧"
    "延安宝鸡咸阳渭南汉中安康商洛铜川"
    "白银天水平凉庆阳张掖武威酒泉嘉峪关"
    "香港澳门台北高雄台中台南新竹嘉义桃园"
    "首尔东京大阪京都名古屋横滨福冈新加坡吉隆坡曼谷"
    "巴黎伦敦纽约洛杉矶悉尼墨尔本多伦多温哥华"
    # 常见省份简称
    "粤苏浙闽赣湘皖鄂豫晋冀鲁川渝黔滇桂琼陕甘宁新藏蒙"
    "省市区县乡镇街道路号"
)

# 合并去重
def build_char_set() -> str:
    chars = set()
    # ASCII 可打印字符
    for c in range(0x20, 0x7F):
        chars.add(chr(c))
    # 中文字符
    for s in [UI_FIXED_CHARS, CITY_CHARS]:
        for c in s:
            chars.add(c)
    return "".join(sorted(chars))


# lv_font_conv 命令构建

def find_lv_font_conv() -> str | None:
    """查找 lv_font_conv 可执行文件"""
    candidates = ["lv_font_conv", "lv_font_conv.cmd"]
    for name in candidates:
        path = shutil.which(name)
        if path:
            return path
    return None


def build_command(font_path: str, size: int, bpp: int,
                  char_set: str, output: str) -> list[str]:
    conv = find_lv_font_conv()
    if not conv:
        raise SystemExit(
            "[ERROR] 未找到 lv_font_conv，请先安装：\n"
            "  npm install -g @lvgl/lv_font_conv\n"
            "  （需要 Node.js v14+ 环境）"
        )

    return [
        conv,
        "--font", font_path,
        "--size", str(size),
        "--format", "lvgl",
        "--bpp", str(bpp),
        "--symbols", char_set,
        "-o", output,
    ]


# 主流程

def main():
    parser = argparse.ArgumentParser(description="生成 LVGL 复古字体子集")
    parser.add_argument(
        "--font",
        default="LXGWWenKai-Regular.ttf",
        help="字体文件路径（默认：LXGWWenKai-Regular.ttf）",
    )
    parser.add_argument(
        "--size", type=int, default=20,
        help="字体像素大小（默认：20）",
    )
    parser.add_argument(
        "--bpp", type=int, default=4, choices=[1, 2, 4, 8],
        help="抗锯齿位数，4=细腻（默认），1=最省空间",
    )
    parser.add_argument(
        "--output",
        default=None,
        help="输出文件路径（默认：../main/lv_ui/src/lv_font_retro_<size>.c）",
    )
    args = parser.parse_args()

    font_path = Path(args.font)
    if not font_path.exists():
        print(f"[ERROR] 字体文件不存在：{font_path}")
        print()
        print("请下载霞鹜文楷字体：")
        print("  https://github.com/lxgw/LxgwWenKai/releases")
        print("  下载 LXGWWenKai-Regular.ttf 放到 backend/ 目录")
        sys.exit(1)

    output = args.output
    if output is None:
        script_dir = Path(__file__).resolve().parent
        out_dir = script_dir.parent / "main" / "lv_ui" / "src"
        out_dir.mkdir(parents=True, exist_ok=True)
        output = str(out_dir / f"lv_font_retro_{args.size}.c")

    char_set = build_char_set()
    print(f"[INFO] 字体文件：{font_path}")
    print(f"[INFO] 字体大小：{args.size}px，bpp={args.bpp}")
    print(f"[INFO] 字符集大小：{len(char_set)} 个字符")
    print(f"[INFO] 预估 Flash 占用：~{len(char_set) * args.size * args.size * args.bpp // 8 // 1024} KB")
    print(f"[INFO] 输出文件：{output}")
    print()

    cmd = build_command(str(font_path), args.size, args.bpp, char_set, output)
    print(f"[CMD] {' '.join(cmd[:3])} ...")

    try:
        result = subprocess.run(cmd, check=True, capture_output=True, text=True)
        print("[OK] 字体生成成功！")
        if result.stdout:
            print(result.stdout)
    except subprocess.CalledProcessError as e:
        print(f"[ERROR] 字体生成失败：\n{e.stderr}")
        sys.exit(1)

    print()
    print("=" * 60)
    print("后续步骤：")
    print(f"  1. 检查生成文件：{output}")
    print(f"  2. 在 main/CMakeLists.txt 添加：")
    print(f"       \"./lv_ui/src/lv_font_retro_{args.size}.c\"")
    print(f"  3. 在 main/lv_ui/src/ui_main.c 中：")
    print(f"       取消注释 extern const lv_font_t lv_font_retro_{args.size};")
    print(f"       将 UI_FONT_PRIMARY 改为 (&lv_font_retro_{args.size})")
    print("=" * 60)


if __name__ == "__main__":
    main()
