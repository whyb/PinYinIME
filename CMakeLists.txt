cmake_minimum_required(VERSION 3.16)
project(PinyinIME VERSION 2.0 LANGUAGES CXX)

# MSVC 静态 CRT (/MT)
if(MSVC)
    cmake_policy(SET CMP0091 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8)
    add_compile_definitions(_UNICODE UNICODE)
endif()

# ==================== Shared Headers ====================
set(SHARED_HEADERS
    shared/pinyin_settings.h
    shared/tsf_guids.h
    shared/utf_utils.h
    shared/ime_ipc.h
)

# ==================== DLL: PinyinIMETSF.dll ====================
set(DLL_SOURCES
    dll/dll_main.cpp
    dll/text_service.cpp
    dll/trie_dict.cpp
    dll/shm_dict.cpp
)

set(DLL_HEADERS
    ${SHARED_HEADERS}
    dll/class_factory.h
    dll/text_service.h
    dll/pinyin_engine.h
    dll/candidate_window.h
    dll/dictionary.h
    dll/trie_dict.h
    dll/shm_dict.h
    dll/s2t_data.h
)

add_library(PinyinIMETSF SHARED ${DLL_SOURCES} ${DLL_HEADERS} dll/PinyinIMETSF.def)

target_include_directories(PinyinIMETSF PRIVATE
    ${CMAKE_SOURCE_DIR}/dll
    ${CMAKE_SOURCE_DIR}/shared
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(PinyinIMETSF PRIVATE
    user32 gdi32 kernel32 imm32 comctl32 shell32
    oleacc ole32 oleaut32 UIAutomationCore
    gdiplus advapi32
)

set_target_properties(PinyinIMETSF PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# 复制词库到 DLL 输出目录
add_custom_command(TARGET PinyinIMETSF POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:PinyinIMETSF>/cn_dicts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/rime-ice/cn_dicts"
        "$<TARGET_FILE_DIR:PinyinIMETSF>/cn_dicts"
    COMMENT "Copying rime-ice dictionaries to DLL output..."
)

# ==================== EXE: PinyinIME.exe ====================
set(EXE_SOURCES
    exe/main.cpp
)

set(EXE_HEADERS
    ${SHARED_HEADERS}
    exe/settings_window.h
    exe/registration.h
)

add_executable(PinyinIME WIN32 ${EXE_SOURCES} ${EXE_HEADERS})

target_include_directories(PinyinIME PRIVATE
    ${CMAKE_SOURCE_DIR}/exe
    ${CMAKE_SOURCE_DIR}/shared
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(PinyinIME PRIVATE
    user32 gdi32 kernel32 comctl32 shell32
    ole32 oleaut32 gdiplus advapi32
)

set_target_properties(PinyinIME PROPERTIES
    WIN32_EXECUTABLE TRUE
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# 复制词库和批处理文件到 EXE 输出目录 (设置文件需要)
add_custom_command(TARGET PinyinIME POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:PinyinIME>/cn_dicts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/rime-ice/cn_dicts"
        "$<TARGET_FILE_DIR:PinyinIME>/cn_dicts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/register.bat"
        "$<TARGET_FILE_DIR:PinyinIME>/register.bat"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/unregister.bat"
        "$<TARGET_FILE_DIR:PinyinIME>/unregister.bat"
    COMMENT "Copying rime-ice dictionaries and batch files to EXE output..."
)

# ==================== 安装 ====================
install(TARGETS PinyinIMETSF PinyinIME
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/rime-ice/cn_dicts/
    DESTINATION cn_dicts
    FILES_MATCHING PATTERN "*.dict.yaml"
)

install(FILES ${CMAKE_SOURCE_DIR}/register.bat ${CMAKE_SOURCE_DIR}/unregister.bat
    DESTINATION .
)

# ==================== 输出信息 ====================
message(STATUS "PinyinIME v2.0 — TSF 输入法 (DLL + EXE)")
message(STATUS "  DLL: PinyinIMETSF.dll  (TSF COM 文本服务)")
message(STATUS "  EXE: PinyinIME.exe     (设置 + 托盘 + 注册)")
message(STATUS "  C++17, 静态 CRT (/MT)")
message(STATUS "  cmake -B build && cmake --build build --config Release")
