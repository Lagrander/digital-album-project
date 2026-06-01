import os
import re
from pathlib import Path

def clean_file(filepath):
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            content = f.read()
    except Exception as e:
        print(f"Error reading {filepath}: {e}")
        return

    original_content = content

    # 1. Simplify Python # ========= separators
    # Find blocks like:
    # # ============================================================
    # # 历史记录
    # # ============================================================
    # And replace with:
    # # 历史记录
    pattern_py_block = re.compile(r'# ={10,}\n# (.*?)\n# ={10,}', re.MULTILINE)
    content = pattern_py_block.sub(r'# \1', content)

    # 2. Simplify C /* ========= separators
    # /* ============================================================
    #  * 历史记录
    #  * ============================================================ */
    pattern_c_block = re.compile(r'/\* ={10,}\n \* (.*?)\n \* ={10,} \*/', re.MULTILINE)
    content = pattern_c_block.sub(r'/* \1 */', content)

    # 3. Strip history remarks
    content = re.sub(r'', '', content)
    content = re.sub(r'\', '', content)
    content = re.sub(r'', '', content)
    
    # 4. Remove exaggerated phrases
    exaggerated_phrases = [
        "", "", "", 
        "", "", "",
        "", "", "",
        "", "", "", "", "", ""
    ]
    for phrase in exaggerated_phrases:
        content = content.replace(phrase, "")

    if content != original_content:
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f"Cleaned {filepath}")

def main():
    root_dir = Path(r"W:\Desktop\digital_album_project")
    for ext in ['*.c', '*.h', '*.py']:
        for filepath in root_dir.rglob(ext):
            if "venv" in str(filepath) or ".git" in str(filepath):
                continue
            clean_file(filepath)

if __name__ == "__main__":
    main()
