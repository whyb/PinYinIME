# -*- coding: utf-8 -*-
"""
Convert rime-ice dictionary files to PinyinIME format.

rime-ice format (YAML + TSV):
    8105.dict.yaml:     字\t拼音\t词频        (single chars, real freqs)
    41448.dict.yaml:    字\t拼音              (single chars, no freq)
    base.dict.yaml:     词语\t拼 音\t词频      (words with spaced pinyin, real freqs)
    ext.dict.yaml:      词语\t拼 音\t词频      (words, most freq=100 default)
    tencent.dict.yaml:  词语\t词频             (3+ char words, no pinyin, most freq=100)
    others.dict.yaml:   词语\t拼 音            (special readings, no freq)

PinyinIME format (TSV):
    拼音(no spaces)\t汉字\t词频

Strategy:
    - single_chars: keep all from 8105 + 41448 + single chars in base/ext
    - words: keep all base (real freqs) + ext/tencent with freq > 100 (non-default)
             + limit top 300 per pinyin key
    - short_pinyin: auto-generated from word entries, top 200 per abbreviation
"""

import os
import re
from collections import defaultdict

RIME_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rime-ice', 'cn_dicts')
OUT_DIR = os.path.dirname(os.path.abspath(__file__))

# Limits per pinyin key to keep dictionary size practical
MAX_WORDS_PER_PINYIN = 300
MAX_SHORT_PER_ABBREV = 200


def parse_rime_dict(filepath):
    """Parse a rime-ice YAML dictionary file.
    Returns list of (word, pinyin, freq) tuples.
    pinyin is '' for files without pinyin (tencent). freq is 0 if not present.
    """
    entries = []
    in_data = False

    with open(filepath, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n\r')
            if not line:
                continue
            if line == '...':
                in_data = True
                continue
            if line == '---':
                in_data = False
                continue
            if not in_data:
                continue
            if line.startswith('#'):
                continue

            parts = line.split('\t')

            if len(parts) == 3:
                word = parts[0].strip()
                pinyin = parts[1].strip()
                try:
                    freq = int(parts[2].strip())
                except ValueError:
                    freq = 0
                if word and pinyin:
                    entries.append((word, pinyin, freq))

            elif len(parts) == 2:
                col1 = parts[0].strip()
                col2 = parts[1].strip()
                if re.match(r'^-?\d+$', col2):
                    # word + freq (tencent style, no pinyin)
                    freq = int(col2)
                    entries.append((col1, '', freq))
                else:
                    # word + pinyin (41448, others style, no freq)
                    entries.append((col1, col2, 0))

    return entries


def build_char_pinyin_map(single_char_entries):
    """Build mapping: char -> most common pinyin (by frequency)."""
    char_pinyins = defaultdict(lambda: defaultdict(int))
    for word, pinyin, freq in single_char_entries:
        if len(word) == 1:
            char_pinyins[word][pinyin] += max(freq, 1)

    char_map = {}
    for char, pinyins in char_pinyins.items():
        char_map[char] = max(pinyins, key=pinyins.get)
    return char_map


def generate_pinyin_for_word(word, char_map):
    """Generate space-separated pinyin for a word using char->pinyin map.
    Returns None if any character is unknown."""
    pinyins = []
    for ch in word:
        if ch in char_map:
            pinyins.append(char_map[ch])
        elif ord(ch) < 128 or ch in '，。！？——「」、；：""''（）【】《》…—·':
            pinyins.append(ch)
        else:
            return None
    return ' '.join(pinyins)


def main():
    print("=" * 60)
    print("Converting rime-ice dictionaries to PinyinIME format")
    print("=" * 60)

    # ── Step 1: Parse all source dictionaries ────────────────────────
    print("\n[1/6] Parsing source dictionaries...")

    sources = {}
    for filename in ['8105.dict.yaml', '41448.dict.yaml', 'base.dict.yaml',
                      'ext.dict.yaml', 'tencent.dict.yaml', 'others.dict.yaml']:
        filepath = os.path.join(RIME_DIR, filename)
        if os.path.exists(filepath):
            entries = parse_rime_dict(filepath)
            sources[filename] = entries
            print(f"  {filename}: {len(entries):,} entries")
        else:
            print(f"  {filename}: NOT FOUND, skipping")
            sources[filename] = []

    # ── Step 2: Build character-to-pinyin map ────────────────────────
    print("\n[2/6] Building character-to-pinyin map...")
    single_char_raw = sources['8105.dict.yaml'] + sources['41448.dict.yaml']
    char_pinyin_map = build_char_pinyin_map(single_char_raw)
    print(f"  {len(char_pinyin_map):,} characters mapped")

    # ── Step 3: Generate single_chars.txt ────────────────────────────
    print("\n[3/6] Generating single_chars.txt...")
    single_chars = {}  # key: (pinyin, char) -> max_freq

    for word, pinyin, freq in sources['8105.dict.yaml']:
        if len(word) == 1 and pinyin:
            key = (pinyin, word)
            single_chars[key] = max(single_chars.get(key, 0), freq)

    for word, pinyin, freq in sources['41448.dict.yaml']:
        if len(word) == 1 and pinyin:
            key = (pinyin, word)
            if key not in single_chars:
                single_chars[key] = 1

    # Also pick up single-char entries from base and ext
    for src_name in ['base.dict.yaml', 'ext.dict.yaml']:
        for word, pinyin, freq in sources[src_name]:
            if len(word) == 1 and pinyin:
                key = (pinyin.replace(' ', ''), word)
                single_chars[key] = max(single_chars.get(key, 0), freq)

    single_list = sorted(single_chars.items(), key=lambda x: (x[0][0], -x[1]))
    single_list = [(k[0], k[1], v) for k, v in single_list]

    single_path = os.path.join(OUT_DIR, 'single_chars.txt')
    with open(single_path, 'w', encoding='utf-8') as f:
        f.write("# PinyinIME Single Characters Dictionary\n")
        f.write(f"# Source: rime-ice (8105 + 41448 + base/ext single chars)\n")
        f.write(f"# Total: {len(single_list):,} entries\n")
        f.write("# Format: pinyin\tchar\tfrequency\n")
        for py, char, freq in single_list:
            f.write(f"{py}\t{char}\t{freq}\n")
    print(f"  {len(single_list):,} single chars written to single_chars.txt")

    # ── Step 4: Generate words.txt ───────────────────────────────────
    print("\n[4/6] Generating words.txt...")
    words_by_pinyin = defaultdict(dict)  # pinyin -> {word: max_freq}

    def add_word(word, pinyin_compact, freq):
        """Add a word entry, keeping highest frequency for duplicates."""
        if freq <= 0:
            return
        if pinyin_compact not in words_by_pinyin or word not in words_by_pinyin[pinyin_compact] \
           or freq > words_by_pinyin[pinyin_compact][word]:
            words_by_pinyin[pinyin_compact][word] = freq

    # 4a. base.dict.yaml — keep all entries (real, varied frequencies)
    n_base = 0
    for word, pinyin, freq in sources['base.dict.yaml']:
        if len(word) >= 2 and pinyin:
            py = pinyin.replace(' ', '').replace('\'', '')
            add_word(word, py, freq)
            n_base += 1
    print(f"  base.dict.yaml words: {n_base:,}")

    # 4b. ext.dict.yaml — keep only entries with freq > 100 (non-default)
    n_ext_kept = 0
    n_ext_skipped = 0
    for word, pinyin, freq in sources['ext.dict.yaml']:
        if len(word) >= 2 and pinyin:
            if freq > 100:  # only keep non-default-frequency entries
                py = pinyin.replace(' ', '').replace('\'', '')
                add_word(word, py, freq)
                n_ext_kept += 1
            else:
                n_ext_skipped += 1
    print(f"  ext.dict.yaml words: {n_ext_kept:,} kept (freq>100), {n_ext_skipped:,} skipped (freq<=100)")

    # 4c. others.dict.yaml — keep all (special readings)
    n_others = 0
    for word, pinyin, freq in sources['others.dict.yaml']:
        if len(word) >= 2 and pinyin:
            py = pinyin.replace(' ', '').replace('\'', '')
            add_word(word, py, freq if freq > 0 else 1000)
            n_others += 1
    print(f"  others.dict.yaml words: {n_others:,}")

    # 4d. tencent.dict.yaml — generate pinyin, keep only freq > 100
    n_tencent_kept = 0
    n_tencent_skipped = 0
    for word, pinyin, freq in sources['tencent.dict.yaml']:
        if len(word) >= 2:
            if freq > 100:
                generated = generate_pinyin_for_word(word, char_pinyin_map)
                if generated:
                    py = generated.replace(' ', '').replace('\'', '')
                    add_word(word, py, freq)
                    n_tencent_kept += 1
                else:
                    n_tencent_skipped += 1
            else:
                n_tencent_skipped += 1
    print(f"  tencent.dict.yaml words: {n_tencent_kept:,} kept (freq>100), {n_tencent_skipped:,} skipped")

    # ── Apply per-pinyin limit ──
    total_before_limit = sum(len(d) for d in words_by_pinyin.values())
    words_by_pinyin_limited = {}
    for py, word_dict in words_by_pinyin.items():
        sorted_words = sorted(word_dict.items(), key=lambda x: -x[1])
        words_by_pinyin_limited[py] = dict(sorted_words[:MAX_WORDS_PER_PINYIN])

    total_after_limit = sum(len(d) for d in words_by_pinyin_limited.values())
    print(f"  Per-pinyin limit ({MAX_WORDS_PER_PINYIN}): {total_before_limit:,} -> {total_after_limit:,}")

    # Flatten and sort
    word_list = []
    for py, word_dict in words_by_pinyin_limited.items():
        for word, freq in word_dict.items():
            word_list.append((py, word, freq))
    word_list.sort(key=lambda x: (x[0], -x[2]))

    words_path = os.path.join(OUT_DIR, 'words.txt')
    with open(words_path, 'w', encoding='utf-8') as f:
        f.write("# PinyinIME Words Dictionary\n")
        f.write(f"# Source: rime-ice (base + ext + tencent + others)\n")
        f.write(f"# Total: {len(word_list):,} entries\n")
        f.write("# Format: pinyin\tword\tfrequency\n")
        for py, word, freq in word_list:
            f.write(f"{py}\t{word}\t{freq}\n")
    print(f"  {len(word_list):,} words written to words.txt")

    # ── Step 5: Generate short_pinyin.txt ────────────────────────────
    print("\n[5/6] Generating short_pinyin.txt...")
    short_by_abbrev = defaultdict(dict)  # abbrev -> {word: max_freq}

    def add_short(word, syllables, freq):
        """Generate abbreviation from space-separated pinyin syllables."""
        if not syllables or freq <= 0:
            return
        abbrev = ''.join(s[0] for s in syllables.split() if s)
        if not abbrev:
            return
        if abbrev not in short_by_abbrev or word not in short_by_abbrev[abbrev] \
           or freq > short_by_abbrev[abbrev][word]:
            short_by_abbrev[abbrev][word] = freq

    # From base (all entries)
    for word, pinyin, freq in sources['base.dict.yaml']:
        if len(word) >= 2 and pinyin and freq > 0:
            add_short(word, pinyin, freq)

    # From ext (only entries we kept)
    for word, pinyin, freq in sources['ext.dict.yaml']:
        if len(word) >= 2 and pinyin and freq > 100:
            add_short(word, pinyin, freq)

    # From others
    for word, pinyin, freq in sources['others.dict.yaml']:
        if len(word) >= 2 and pinyin:
            add_short(word, pinyin, freq if freq > 0 else 1000)

    # From tencent (only entries we kept)
    for word, pinyin, freq in sources['tencent.dict.yaml']:
        if len(word) >= 2 and freq > 100:
            generated = generate_pinyin_for_word(word, char_pinyin_map)
            if generated:
                add_short(word, generated, freq)

    # Also add single chars as single-letter abbreviations
    for (py, char), freq in single_chars.items():
        if py and freq > 0:
            abbrev = py[0]
            if abbrev not in short_by_abbrev or char not in short_by_abbrev[abbrev] \
               or freq > short_by_abbrev[abbrev][char]:
                short_by_abbrev[abbrev][char] = freq

    # Apply per-abbrev limit
    total_short_before = sum(len(d) for d in short_by_abbrev.values())
    short_by_abbrev_limited = {}
    for abbr, word_dict in short_by_abbrev.items():
        sorted_words = sorted(word_dict.items(), key=lambda x: -x[1])
        short_by_abbrev_limited[abbr] = dict(sorted_words[:MAX_SHORT_PER_ABBREV])

    total_short_after = sum(len(d) for d in short_by_abbrev_limited.values())
    print(f"  Per-abbrev limit ({MAX_SHORT_PER_ABBREV}): {total_short_before:,} -> {total_short_after:,}")

    short_list = []
    for abbr, word_dict in short_by_abbrev_limited.items():
        for word, freq in word_dict.items():
            short_list.append((abbr, word, freq))
    short_list.sort(key=lambda x: (x[0], -x[2]))

    short_path = os.path.join(OUT_DIR, 'short_pinyin.txt')
    with open(short_path, 'w', encoding='utf-8') as f:
        f.write("# PinyinIME Short Pinyin Dictionary\n")
        f.write(f"# Source: rime-ice (auto-generated abbreviations)\n")
        f.write(f"# Total: {len(short_list):,} entries\n")
        f.write("# Format: abbreviation\tword\tfrequency\n")
        for abbr, word, freq in short_list:
            f.write(f"{abbr}\t{word}\t{freq}\n")
    print(f"  {len(short_list):,} short pinyin entries written to short_pinyin.txt")

    # ── Step 6: Summary ──────────────────────────────────────────────
    print("\n[6/6] Summary")
    print("=" * 60)
    total_sources = sum(len(v) for v in sources.values())
    print(f"  Total source entries:        {total_sources:,}")
    print(f"  Single chars output:         {len(single_list):,}")
    print(f"  Words output:                {len(word_list):,}")
    print(f"  Short pinyin output:         {len(short_list):,}")

    for name in ['single_chars.txt', 'words.txt', 'short_pinyin.txt']:
        path = os.path.join(OUT_DIR, name)
        size_kb = os.path.getsize(path) / 1024
        size_mb = size_kb / 1024
        if size_mb >= 1:
            print(f"  {name}: {size_mb:.1f} MB")
        else:
            print(f"  {name}: {size_kb:.0f} KB")

    total_mb = sum(os.path.getsize(os.path.join(OUT_DIR, n)) for n in
                   ['single_chars.txt', 'words.txt', 'short_pinyin.txt']) / (1024 * 1024)
    print(f"\n  Total dictionary size: {total_mb:.1f} MB")
    print("\nDone! Backups at: *.txt.bak")


if __name__ == '__main__':
    main()
