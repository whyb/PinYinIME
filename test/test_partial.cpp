// test/test_partial.cpp — Quick partial-pinyin verification
// Run: test_partial.exe [dict.bin path]
// Tests a few key partial-pinyin queries and prints the top candidates.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdarg>

#include "../shared/dict_binary.h"
#include "test_common.h"

static void LOG(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    vprintf(fmt, args); va_end(args); fflush(stdout);
}

int main(int argc, char* argv[]) {
    // Usage: test_partial.exe [query] [path\to\dict.bin]
    //   With query: test just that one pinyin string (exact + prefix)
    //   Without query: run built-in test suite
    const char* singleQuery = nullptr;
    int dictArg = 1;
    if (argc >= 2 && argv[1][0] != '\0' &&
        !(argv[1][0] >= 'A' && argv[1][0] <= 'Z') &&
        !(argv[1][0] >= 'a' && argv[1][0] <= 'z') == false) {
        // Check if arg looks like a path (contains \ or /) vs a pinyin query
        bool looksLikePath = false;
        for (const char* p = argv[1]; *p; p++) {
            if (*p == '\\' || *p == '/') { looksLikePath = true; break; }
        }
        if (!looksLikePath && strlen(argv[1]) <= 32) {
            singleQuery = argv[1];
            dictArg = 2;
        }
    }

    std::wstring dp;
    if (argc > dictArg) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, argv[dictArg], -1, nullptr, 0);
        wchar_t* buf = new wchar_t[wlen];
        MultiByteToWideChar(CP_UTF8, 0, argv[dictArg], -1, buf, wlen);
        dp = buf;
        delete[] buf;
    } else {
        dp = defaultDictPath();
    }
    LOG("Loading: %ls\n\n", dp.c_str());

    HANDLE hFile = CreateFileW(dp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) { LOG("FAIL: open\n"); return 1; }

    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(hFile);

    BinaryDictReader reader;
    if (!reader.attach(view)) { LOG("FAIL: attach\n"); return 1; }
    LOG("Loaded: %zu keys, %zu entries\n\n", reader.stateCount(), reader.entryCount());

    auto showResults = [](const char* key, const std::vector<std::pair<std::string, int>>& results) {
        LOG("══ \"%s\" → %zu results ══\n", key, results.size());
        size_t show = (std::min)(results.size(), (size_t)20);
        for (size_t i = 0; i < show; i++)
            LOG("  %s(%d)", results[i].first.c_str(), results[i].second);
        LOG("\n");
    };

    if (singleQuery) {
        auto results = reader.query(singleQuery);
        showResults(singleQuery, results);
        UnmapViewOfFile(view); CloseHandle(hMap);
        return 0;
    }

    const char* tests[] = {
        "zhey", "beij", "xiangy", "zhongy", "weish", "nengl",
        "jint", "dians", "meiy", "shij", "haom", "kesh",
        "yij", "douc", "haib", "xuya",
        "xianz", "xianzh",
        nullptr
    };

    for (int t = 0; tests[t]; t++) {
        auto results = reader.query(tests[t]);
        showResults(tests[t], results);
    }

    UnmapViewOfFile(view);
    CloseHandle(hMap);
    return 0;
}
