// shared/pinyin_settings.h — PinyinSettings 数据结构
// DLL 和 EXE 共享 — 纯数据 + 文件序列化, 无 UI 依赖
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include "utf_utils.h"

// ==================== 设置数据结构 ====================
struct PinyinSettings {
    // === 语言 ===
    bool useTraditional = false;

    // === 外观 (默认 亮色 墨绿主题) ===
    COLORREF bgColor     = RGB(0xF0, 0xF5, 0xF0);  // 淡绿背景
    COLORREF borderColor = RGB(0x96, 0xC6, 0x96);  // 中绿边框
    COLORREF textColor   = RGB(0x28, 0x3D, 0x28);  // 深绿文字
    COLORREF indexColor  = RGB(0x3C, 0x81, 0x3C);  // 墨绿序号
    COLORREF inputColor  = RGB(0x32, 0x64, 0x32);  // 墨绿输入
    std::wstring fontName = L"Microsoft YaHei"; // 字体名称
    int fontSize         = 20;   // 字体高度 (负值=像素)
    int candidateCount   = 5;    // 候选词数量 5-9
    bool verticalLayout  = false; // 候选框竖排

    // === 模糊音 ===
    bool fuzzyZ_Zh  = false;
    bool fuzzyC_Ch  = false;
    bool fuzzyS_Sh  = false;
    bool fuzzyN_L   = false;
    bool fuzzyF_H   = false;
    bool fuzzyEn_Eng = false;
    bool fuzzyIn_Ing = false;

    // === 智能功能 ===
    bool smartCorrection  = true;
    bool autoWordCreate   = true;
    bool autoFreqAdjust   = true;
    bool chinesePunctuation = true;

    // === 候选翻页 ===
    bool enableMinusEqualsPage = true;   // 启用 -/= 翻页
    bool enableCommaPeriodPage = true;   // 启用 ,/. (</>)翻页
    bool enableTabPage = true;           // 启用 Tab/Shift+Tab 翻页
    bool enableBracketPage = true;       // 启用 [/] 翻页
    // === 候选窗显示 ===
    bool showSettingsGear = true;        // 显示设置齿轮 ⚙
    bool roundedCorner = true;           // 候选框圆角


    // 预设皮肤
    static const struct Skin {
        const wchar_t* name;
        COLORREF bg, border, text, index, input;
    } skins[];

    static const int SKIN_COUNT = 9;

    void applySkin(int idx) {
        if (idx < 0 || idx >= SKIN_COUNT) return;
        bgColor     = skins[idx].bg;
        borderColor = skins[idx].border;
        textColor   = skins[idx].text;
        indexColor  = skins[idx].index;
        inputColor  = skins[idx].input;
    }

    bool loadFromFile(const std::string& path) {
        std::ifstream fin(path);
        if (!fin.is_open()) return false;
        std::string line;
        while (std::getline(fin, line)) {
            if (line.empty() || line[0] == '#') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            val.erase(0, val.find_first_not_of(" \t"));
            val.erase(val.find_last_not_of(" \t") + 1);

            if (key == "useTraditional") useTraditional = (val == "1");
            else if (key == "bgColor") bgColor = (COLORREF)std::stoul(val);
            else if (key == "borderColor") borderColor = (COLORREF)std::stoul(val);
            else if (key == "textColor") textColor = (COLORREF)std::stoul(val);
            else if (key == "indexColor") indexColor = (COLORREF)std::stoul(val);
            else if (key == "inputColor") inputColor = (COLORREF)std::stoul(val);
            else if (key == "fontName") fontName = utf8ToWide(val);
            else if (key == "fontSize") fontSize = std::stoi(val);
            else if (key == "candidateCount") candidateCount = std::stoi(val);
            else if (key == "verticalLayout") verticalLayout = (val == "1");
            else if (key == "fuzzyZ_Zh") fuzzyZ_Zh = (val == "1");
            else if (key == "fuzzyC_Ch") fuzzyC_Ch = (val == "1");
            else if (key == "fuzzyS_Sh") fuzzyS_Sh = (val == "1");
            else if (key == "fuzzyN_L") fuzzyN_L = (val == "1");
            else if (key == "fuzzyF_H") fuzzyF_H = (val == "1");
            else if (key == "fuzzyEn_Eng") fuzzyEn_Eng = (val == "1");
            else if (key == "fuzzyIn_Ing") fuzzyIn_Ing = (val == "1");
            else if (key == "smartCorrection") smartCorrection = (val == "1");
            else if (key == "autoWordCreate") autoWordCreate = (val == "1");
            else if (key == "autoFreqAdjust") autoFreqAdjust = (val == "1");
            else if (key == "chinesePunctuation") chinesePunctuation = (val == "1");
            else if (key == "enableMinusEqualsPage") enableMinusEqualsPage = (val == "1");
            else if (key == "enableCommaPeriodPage") enableCommaPeriodPage = (val == "1");
            else if (key == "enableTabPage") enableTabPage = (val == "1");
            else if (key == "enableBracketPage") enableBracketPage = (val == "1");
            else if (key == "showSettingsGear") showSettingsGear = (val == "1");
            else if (key == "roundedCorner") roundedCorner = (val == "1");
        }
        return true;
    }

    bool saveToFile(const std::string& path) {
        std::ofstream fout(path);
        if (!fout.is_open()) return false;
        fout << "# PinyinIME Settings\n";
        fout << "useTraditional=" << useTraditional << "\n";
        fout << "bgColor=" << (unsigned long)bgColor << "\n";
        fout << "borderColor=" << (unsigned long)borderColor << "\n";
        fout << "textColor=" << (unsigned long)textColor << "\n";
        fout << "indexColor=" << (unsigned long)indexColor << "\n";
        fout << "inputColor=" << (unsigned long)inputColor << "\n";
        fout << "fontName=" << wideToUtf8(fontName) << "\n";
        fout << "fontSize=" << fontSize << "\n";
        fout << "candidateCount=" << candidateCount << "\n";
        fout << "verticalLayout=" << verticalLayout << "\n";
        fout << "fuzzyZ_Zh=" << fuzzyZ_Zh << "\n";
        fout << "fuzzyC_Ch=" << fuzzyC_Ch << "\n";
        fout << "fuzzyS_Sh=" << fuzzyS_Sh << "\n";
        fout << "fuzzyN_L=" << fuzzyN_L << "\n";
        fout << "fuzzyF_H=" << fuzzyF_H << "\n";
        fout << "fuzzyEn_Eng=" << fuzzyEn_Eng << "\n";
        fout << "fuzzyIn_Ing=" << fuzzyIn_Ing << "\n";
        fout << "smartCorrection=" << smartCorrection << "\n";
        fout << "autoWordCreate=" << autoWordCreate << "\n";
        fout << "autoFreqAdjust=" << autoFreqAdjust << "\n";
        fout << "chinesePunctuation=" << chinesePunctuation << "\n";
        fout << "enableMinusEqualsPage=" << enableMinusEqualsPage << "\n";
        fout << "enableCommaPeriodPage=" << enableCommaPeriodPage << "\n";
        fout << "enableTabPage=" << enableTabPage << "\n";
        fout << "enableBracketPage=" << enableBracketPage << "\n";
        fout << "showSettingsGear=" << showSettingsGear << "\n";
        fout << "roundedCorner=" << roundedCorner << "\n";
        return true;
    }
};

// 预设皮肤定义 (5 款暗色 + 4 款亮色)
inline const PinyinSettings::Skin PinyinSettings::skins[] = {
    // ── 暗色 系列 ──
    {L"暗色 墨黑",   RGB(0x15,0x15,0x15), RGB(0x33,0x33,0x33), RGB(0xBD,0xBD,0xBD), RGB(0x76,0x76,0x76), RGB(0x5E,0x97,0xF6)},
    {L"暗色 炭灰",   RGB(0x21,0x21,0x21), RGB(0x3C,0x3C,0x3C), RGB(0xC8,0xC8,0xC8), RGB(0x82,0x82,0x82), RGB(0x6D,0xA0,0xF8)},
    {L"暗色 暖咖",   RGB(0x1C,0x18,0x16), RGB(0x3D,0x34,0x30), RGB(0xC8,0xB8,0xA0), RGB(0x88,0x78,0x60), RGB(0xD4,0xA0,0x50)},
    {L"暗色 墨绿",   RGB(0x14,0x1A,0x14), RGB(0x2A,0x3A,0x2A), RGB(0xA0,0xC0,0xA0), RGB(0x60,0x80,0x60), RGB(0x50,0xB8,0x70)},
    {L"暗色 藏蓝",   RGB(0x14,0x16,0x20), RGB(0x2A,0x30,0x40), RGB(0xA0,0xB0,0xD0), RGB(0x68,0x78,0xA0), RGB(0x60,0x78,0xE0)},
    // ── 亮色系列 ──
    {L"亮色 浅灰",   RGB(0xF0,0xF0,0xF0), RGB(0xC0,0xC0,0xC0), RGB(0x33,0x33,0x33), RGB(0x70,0x70,0xC0), RGB(0x00,0x60,0xC0)},
    {L"亮色 纯白",   RGB(0xFF,0xFF,0xFF), RGB(0xD0,0xD0,0xD0), RGB(0x1A,0x1A,0x1A), RGB(0x58,0x58,0xB8), RGB(0x00,0x48,0xB8)},
    {L"亮色 暖米",   RGB(0xFF,0xF8,0xF0), RGB(0xD8,0xC8,0xB0), RGB(0x4A,0x3A,0x2A), RGB(0xB0,0x70,0x50), RGB(0xA0,0x50,0x30)},
    {L"亮色 墨绿",   RGB(0xF0,0xF5,0xF0), RGB(0x96,0xC6,0x96), RGB(0x28,0x3D,0x28), RGB(0x3C,0x81,0x3C), RGB(0x32,0x64,0x32)},
};
