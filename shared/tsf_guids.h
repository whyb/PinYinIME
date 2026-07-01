// shared/tsf_guids.h — TSF 文本服务 GUID 定义
// DLL 和 EXE 共享 — 用于 COM 注册和 TSF Profile 注册
#pragma once
#include <windows.h>

// PinyinIME 文本服务 CLSID (COM 类标识符)
// {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
static const GUID CLSID_PinyinIME =
    { 0xA1B2C3D4, 0xE5F6, 0x7890, { 0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x90 } };

// PinyinIME TSF Profile GUID (输入法配置文件标识符)
// {B2C3D4E5-F6A1-7890-BCDE-F1234567890A}
static const GUID GUID_PinyinProfile =
    { 0xB2C3D4E5, 0xF6A1, 0x7890, { 0xBC, 0xDE, 0xF1, 0x23, 0x45, 0x67, 0x89, 0x0A } };
