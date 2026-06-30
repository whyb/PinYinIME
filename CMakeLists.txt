cmake_minimum_required(VERSION 3.16)
project(PinyinIME VERSION 1.0 LANGUAGES CXX)

# MSVC 静态 CRT (/MT) — 需要 CMake 3.15+ 策略
if(MSVC)
    cmake_policy(SET CMP0091 NEW)
endif()

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
    trie_dict.cpp
)

set(HEADERS
    dictionary.h
    settings.h
    s2t_data.h
    trie_dict.h
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
# 安装 exe + rime-ice 词库
install(TARGETS PinyinIME RUNTIME DESTINATION .)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/rime-ice/cn_dicts/
    DESTINATION cn_dicts
    FILES_MATCHING PATTERN "*.dict.yaml"
)

# ==================== 自定义目标 ====================
# 复制 rime-ice 词库到构建输出目录 (供调试运行)
add_custom_command(TARGET PinyinIME POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:PinyinIME>/cn_dicts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/rime-ice/cn_dicts"
        "$<TARGET_FILE_DIR:PinyinIME>/cn_dicts"
    COMMENT "Copying rime-ice dictionaries to build output..."
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
