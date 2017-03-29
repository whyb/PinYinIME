# -*- coding: utf-8 -*-
"""Final cleanup: remove remaining rare chars and words, add more common entries."""
import re

# Known extremely rare/archaic characters to remove (by pinyin)
RARE_SINGLE_CHARS = {
    # Chemical elements nobody types
    ("a", "砹"), ("a", "锿"), ("ai", "锿"), ("an", "闇"), ("an", "媕"),
    # Archaic variants
    ("ang", "骯"),  # traditional of 肮
    # More rare chars
    ("ao", "岙"), ("ao", "廒"),
    ("ba", "钯"), ("ba", "鲅"), ("ba", "灞"),
    ("ban", "钣"), ("ban", "舨"),
    ("bei", "邶"),
    ("bi", "吡"), ("bi", "荜"), ("bi", "萆"),
    ("bian", "砭"),
    ("bin", "邠"), ("bin", "豳"),
    ("bo", "僰"), ("bo", "亳"), ("bo", "饽"),
    ("bu", "钚"),
    # ... more can be added
}

def clean_single_chars(filepath):
    """Remove rare chars from single_chars.txt"""
    with open(filepath, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    removed = 0
    new_lines = []
    for line in lines:
        if line.startswith('#'):
            new_lines.append(line)
            continue
        parts = line.strip().split('\t')
        if len(parts) == 3:
            py, char, freq = parts
            if (py, char) in RARE_SINGLE_CHARS and int(freq) < 7000:
                removed += 1
                continue
        new_lines.append(line)

    with open(filepath, 'w', encoding='utf-8') as f:
        f.writelines(new_lines)
    print(f"single_chars.txt: removed {removed} rare chars, {len(new_lines)} lines remaining")

if __name__ == '__main__':
    base = r'D:\codes\github\InputMethod'
    clean_single_chars(f'{base}/single_chars.txt')
    print("Cleanup complete")
