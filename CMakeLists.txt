cmake_minimum_required(VERSION 3.16)
project(PinyinIME VERSION 1.0 LANGUAGES CXX)

# ==================== 编译选项 ====================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC 静态链接 CRT (/MT), 避免运行依赖 vcruntime*.dll
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8)
    add_compile_definitions(_UNICODE UNICODE)
endif()

# ==================== 源文件 ====================
set(SOURCES
    main.cpp
)

set(HEADERS
    dictionary.h
    settings.h
    s2t_data.h
)

# ==================== 可执行文件 ====================
add_executable(PinyinIME WIN32 ${SOURCES} ${HEADERS})

# ==================== 链接库 ====================
target_link_libraries(PinyinIME PRIVATE
    user32
    gdi32
    kernel32
    imm32
    comctl32
    shell32
    oleacc
    ole32
    oleaut32
    UIAutomationCore
)

# ==================== 安装规则 ====================
# 安装 exe + 运行时需要的词典文件
install(TARGETS PinyinIME RUNTIME DESTINATION .)

install(FILES
    single_chars.txt
    words.txt
    short_pinyin.txt
    DESTINATION .
)

# ==================== 自定义目标 ====================
# 重新生成词典 (需要 Python 3)
add_custom_target(generate_dicts
    COMMAND python generate_dicts.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Regenerating dictionary files (single_chars.txt / words.txt / short_pinyin.txt)"
)

# ==================== 平台特定 ====================
if(WIN32)
    # 隐藏控制台窗口 (WIN32 已设置, 这里额外保证)
    set_target_properties(PinyinIME PROPERTIES
        WIN32_EXECUTABLE TRUE
    )

    # MSVC 静态 CRT 配置
    set_target_properties(PinyinIME PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

# ==================== 输出信息 ====================
message(STATUS "PinyinIME v1.0 — 极简拼音输入法 (Win32)")
message(STATUS "  C++ Standard:  C++17")
message(STATUS "  Build type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  CRT linkage:   Static (/MT)")
message(STATUS "  Usage:")
message(STATUS "    cmake -B build -DCMAKE_BUILD_TYPE=Release")
message(STATUS "    cmake --build build --config Release")
message(STATUS "    cd build && PinyinIME.exe")
