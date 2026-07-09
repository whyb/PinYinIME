# 极简拼音输入法 (PinyinIME)

## 为什么会有这个项目

受够了某狗输入法。

弹窗广告铺天盖地，关都关不掉——关闭按钮小得跟蚂蚁似的，点歪了直接弹出一整个浏览器窗口。这还不算完，用着用着发现电脑上莫名其妙多了一堆根本没装过的软件：某狗桌面壁纸、某狗浏览器、某某垃圾清理工具……我**保证**不是我装的，我**绝对**没有点过任何"安装"按钮。某狗在后台一声不吭就给你塞进来，跟小偷没什么两样。

这种流氓行径必须谴责。一个输入法而已，凭什么监听你的键盘、统计你的词频、扫描你的硬盘、把你的输入习惯上传到它们服务器？凭什么弹广告、装全家桶？

于是就有了这个项目——**一个极简的拼音输入法，开源，不需要联网，隐私完全可控**。没有广告，没有后台，没有全家桶，更没有藏在角落里的"推荐安装"。代码干净透明，每一行都看得见。

功能上也没含糊：支持全拼/简拼、模糊音（z/zh、c/ch、s/sh、n/l、f/h、en/eng、in/ing）、繁简体一键切换（2000+ 字符映射 + 词汇级消歧 + 两岸 IT 术语差异）、用户自定义词库、词频自动学习、DP 拼音分割（输入 `haiyoumeiyou` 自动拆成「还有没有」）、多皮肤配色、候选词数量可调……该有的都有。

一句话：**我的输入法，我做主。**

## 使用实录

![gif](https://raw.githubusercontent.com/whyb/PinYinIME/main/imgs/video.gif)

## 特性

### 🚫 反流氓
- **零联网**: 不联网、不上传、不收集隐私
- **零广告**: 没有弹窗，没有推广，永远不会有
- **零捆绑**: 不安装任何第三方软件
- **代码可审计**: 开源，每个人都能看

### 🎯 核心输入
- **全拼 / 简拼**: 支持完整拼音和首字母缩写（`nh` → 你好）
- **DP 拼音分割**: 输入长串拼音自动拆成词组（`haiyoumeiyou` → 还有没有）
- **内置词库**: 基于 [rime-ice](https://github.com/iDvel/rime-ice) 词典，49 万拼音 key，123 万词条，含 46,000+ 单字和大量词组
- **增量查询**: 长词通过引擎自动分割为短音节逐轮查询，无需在词库中存储长词条（`nihao` → 你好 作为一轮，`shijie` → 世界 作为下一轮）
- **用户词库**: 自学习，自动记录选词频率，越用越顺手
- **词频动态调整**: 每次选择自动 +1 频率，下次优先显示
- **Shift 切换中英文**: 中文输入过程中按 Shift 直接切换纯英文模式（当前拼音以英文字母形式上屏），再按 Shift 切回中文

### 🎨 外观
- **预设皮肤**: 9 款配色（5 款暗色：墨黑、炭灰、暖咖、墨绿、藏蓝；4 款亮色：浅灰、纯白、暖米、墨绿）
- **自定义配色**: 通过取色器自由选择主色调，自动生成整套配色
- **候选词数量**: 3-9 个可调
- **字体大小**: 12-36px 可调
- **横排 / 竖排**: 候选框自由切换
- **圆角 / 直角**: 候选框圆角可开关，即时生效
- **候选翻页**: 支持 `-/=`、`,/.`、`[/]`、`Tab` 以及 `PageUp/PageDown` 多组翻页热键

### 🔧 高级功能
- **模糊音**: 支持 7 组模糊音（z/zh、c/ch、s/sh、n/l、f/h、en/eng、in/ing）
- **繁简体转换**: ~2000 字符映射 + ~150 词汇级消歧 + ~40 两岸 IT 术语
  - 一简对多繁智能消歧（如「发」→「發/髮」、「后」→「後/后」）
  - 最长词匹配优先策略
- **中文标点**: 自动转换 `，。！？——「」`（可在设置中开关）
- **设置窗口**: 纯 Win32 手写 UI，无资源文件依赖，圆角窗口 + 自绘标题栏
- **用户词典管理**: 可视化增删改查
- **系统注册**: 可注册为 Windows 系统输入法，集成到语言栏，支持开机自启

### 🖥️ 技术亮点
- **TSF 文本服务框架**: 基于 Windows Text Services Framework (TSF) 实现，告别全局键盘钩子
- **三层分立架构**: DLL (TSF 注入) + Service (后台词库) + EXE (设置/注册)，职责清晰
- **IPC 词库服务**: DLL 通过命名共享内存 + 命名事件与后台服务通信，词库系统级只加载一份
- **排序数组词库**: dict.bin 采用排序数组 + 二分查找（O(log n)），编译秒级完成，文件仅 25MB
- **内存映射加载**: dict.bin 通过 MapViewOfFile 零解析加载，OS 按需调页，物理内存仅占用访问过的页面
- **沙盒穿透**: 命名对象使用 WD + AC 双重 DACL，兼容 Chrome/Edge/UWP 等沙盒应用
- **UIAutomation + TSF GetTextExt 双模光标检测**: 精准定位文本输入光标，候选框绝不挡字
- **自绘候选窗口**: GDI+ 纯手工绘制，圆角 + 渐变边框 + 选中反色，无 Qt/Electron 依赖
- **DisplayAttributeProvider**: TSF 标准组合文本下划线样式，内联拼音显示带虚线标识
- **CompartmentEventSink**: 监听系统 IME 开关状态，系统关闭输入法时自动提交残留文本

---

## 架构

```
┌──────────────────────────────────────────────────────────────────────┐
│                    用户应用程序进程 (Chrome/Edge/Notepad...)           │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │  PinyinIMETSF.dll (TSF COM 文本服务, 注入到每个进程)          │   │
│  │                                                              │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐   │   │
│  │  │ TextService  │  │ PinyinEngine │  │ CandidateWindow  │   │   │
│  │  │ (TSF COM)    │  │ (DictClient) │  │ (GDI+ 自绘)      │   │   │
│  │  └──────────────┘  └──────┬───────┘  └──────────────────┘   │   │
│  │                           │ IPC Client                       │   │
│  └───────────────────────────┼──────────────────────────────────┘   │
│                              │                                      │
└──────────────────────────────┼──────────────────────────────────────┘
                               │
          Named Shared Memory (64KB) + Named Events
          Local\ 命名空间 (优先), Global\ 回退 (沙盒兼容)
                               │
┌──────────────────────────────┼──────────────────────────────────────┐
│  PinyinIMEDictService.exe (后台词库服务, 单实例, 常驻)              │
│                              │                                      │
│  ┌───────────────────────────┼──────────────────────────────────┐   │
│  │  IPC Server Loop          │  BinaryDictReader                │   │
│  │  WaitForMultipleObjects   │  dict.bin (MapViewOfFile)        │   │
│  │  EvtQuery + EvtStop       │  排序数组 + 二分查找              │   │
│  └───────────────────────────┴──────────────────────────────────┘   │
│                                                                      │
│  dict.bin (~25 MB on disk, ~5 MB RSS)                               │
│  · 494,072 拼音 key          · 1,236,485 词条                       │
│  · String Pool (dedup UTF-8) · DictFileEntry[] (word_off, freq)     │
└──────────────────────────────────────────────────────────────────────┘
                               ▲
              Windows Task Scheduler (ONLOGON 触发器)
              ServiceManager (启动/停止/开机自启)
                               │
┌──────────────────────────────┴──────────────────────────────────────┐
│  PinyinIME.exe (配置工具)                                           │
│  ┌──────────────┐  ┌────────────────┐  ┌──────────────────────┐    │
│  │ Settings UI  │  │ TSF Register   │  │ ServiceManager       │    │
│  │ (皮肤/字体)  │  │ (系统注册/注销) │  │ (服务管理/开机自启)  │    │
│  └──────────────┘  └────────────────┘  └──────────────────────┘    │
└─────────────────────────────────────────────────────────────────────┘
```

### 词库编译

dict.bin 由 `dict_compiler.exe` 离线编译生成：

```
dict_compiler.exe cn_dicts dict.bin [--max-chars N]
```

- 解析 rime-ice YAML 词库文件（8105 字表、41448 大字表、base/ext/others/tencent 词组）
- 按汉字字数过滤（默认最多 3 字），长词由输入法引擎的增量查询自动处理
- 生成排序数组格式的二进制词库（v3 格式），编译秒级完成
- 运行时通过 `MapViewOfFile` 零解析加载，二分查找精确匹配（O(log n)），前缀扫描完成联想

### IPC 协议

- **通道**: 64KB 命名共享内存 (page-file-backed) + 2 个命名 Auto-Reset 事件
- **协议**: DLL 写入拼音 → 设置状态 → 触发 EvtQuery → 等待 EvtReply（50ms 超时）
- **安全**: WD (Everyone) + AC (AppContainer) 双重 DACL，兼容沙盒应用
- **命名空间**: Local\ 优先（无需特权的常见场景），Global\ 回退（提升的沙盒兼容）
- **服务生命周期**: Task Scheduler ONLOGON 自启，EXE 可手动启动/停止

---

## 编译

### 环境要求
- Windows 10+
- Visual Studio 2022 (带 C++ 桌面开发工作负载)
- CMake 3.16+

### 构建

```cmd
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

输出：
- `build/Release/PinyinIMETSF.dll` — TSF 文本服务 DLL
- `build/Release/PinyinIMEDictService.exe` — 后台词库服务
- `build/Release/PinyinIME.exe` — 设置/注册程序
- `build/Release/dict_compiler.exe` — 离线词库编译器

所有产物均静态链接 CRT (`/MT`)，无需安装任何运行时。

### 测试

项目附带一个轻量级词库查询测试工具，可直接验证拼音候选词，无需注册 IME 或启动 IPC 服务：

```cmd
# 构建
cmake --build build --config Release --target test_partial

# 查询任意拼音的候选词
build\Release\test\Release\test_partial.exe nihao
build\Release\test\Release\test_partial.exe zhongg
build\Release\test\Release\test_partial.exe xianz
```

工具直接读取 `dict.bin` 进行精确匹配 + 前缀搜索，输出 Top 20 候选词及频率。支持未输完的拼音，例如：

```
> test_partial.exe xianz
══ "xianz" ══
  Exact: none
  Prefix: 294 results: 现在(503182) 限制(500607) 现状(500251) 闲置(89840)...
```

还有完整的 IPC 集成测试（需要两个终端）：

```cmd
cmake --build build --config Release --target test_service --target test_client
:: 终端 1: build\Release\test\Release\test_service.exe
:: 终端 2: build\Release\test\Release\test_client.exe -v
```

测试客户端模拟逐字母打字，逐击键显示候选词，最后以退出码报告通过/失败。

---

## 使用方法

### 注册为系统输入法
1. 运行 `build\Release\PinyinIME.exe`（需**管理员权限**）
2. 打开设置 → 点击「📌 注册输入法到系统」
3. 进入 **Windows 设置 → 时间和语言 → 语言 → 中文 → 键盘 → 添加键盘 → PinyinIME**
4. 使用 **Win+Space** 切换到 PinyinIME

注册过程会自动：
- 注册 COM 组件和 TSF Profile
- 编译 dict.bin（如不存在）
- 创建 Task Scheduler 开机自启任务
- 启动后台词库服务

### 输入操作
- **Shift**: 切换中/英文模式
- **空格**: 确认第一个候选
- **数字 1-9**: 选择对应候选
- **Enter**: 将拼音字母直接上屏
- **- / =**: 上一页 / 下一页
- **, / .**: 上一页 / 下一页
- **[ / ]**: 上一页 / 下一页
- **Tab / Shift+Tab**: 下一页 / 上一页
- **PageUp / PageDown**: 翻页
- **← / →**: 候选词选择导航
- **Backspace**: 删除最后一个字母
- **Escape**: 清空输入
- **'**: 分词符（如 `xi'an` → 西安）
- 点击候选窗上的 **⚙** 图标打开设置

---

## 文件说明

| 文件/目录 | 说明 |
|-----------|------|
| `exe/main.cpp` | EXE 入口：托盘图标、跨进程消息、单实例管理 |
| `exe/settings_window.h` | 设置窗口：纯 Win32 自绘 UI、皮肤预览、用户词典对话框 |
| `exe/registration.h` | TSF 系统注册：COM 注册、TSF Profile 注册、UAC 提权 |
| `exe/service_manager.h` | 服务管理：启动/停止 PinyinIMEDictService、Task Scheduler 开机自启 |
| `exe/register_window.h` | 注册进度窗口：逐步展示 COM/TSF/词库编译/服务启动过程 |
| `dll/dll_main.cpp` | DLL 入口：COM DllMain、DllRegisterServer、DllGetClassObject |
| `dll/text_service.cpp` | **TSF 核心**：ITfTextInputProcessorEx、ITfKeyEventSink、组合管理、按键处理 |
| `dll/text_service.h` | TSF 接口声明 + DisplayAttribute + EditSession |
| `dll/class_factory.h` | COM IClassFactory 实现 |
| `dll/pinyin_engine.h` | 拼音引擎：缓冲区、候选匹配、DP 分词、自学习、翻页 |
| `dll/dict_client.h` | **IPC 词库客户端**：通过命名共享内存与 DictService 通信 |
| `dll/ipc_client.h` | **IPC 协议客户端**：共享内存映射、查询/回复事件、50ms 超时 |
| `dll/candidate_window.h` | GDI+ 候选窗：自绘渲染、齿轮按钮、光标定位 |
| `dll/shm_dict.h/cpp` | 共享内存扁平 Trie（遗留，已被 IPC 模式替代） |
| `dll/trie_dict.h/cpp` | 前缀树内存数据库（遗留） |
| `dll/s2t_data.h` | 简繁转换数据：2000+ 字符映射 + 词汇消歧 + 两岸 IT 术语 |
| `service/dict_service.cpp` | **后台词库服务**：dict.bin mmap 加载、IPC 服务循环、单实例保证 |
| `service/dict_compiler.cpp` | **离线词库编译器**：YAML→dict.bin，排序数组格式 |
| `shared/dict_binary.h` | 二进制词库格式定义 + BinaryDictReader 查询引擎（v2 DAT / v3 排序数组） |
| `shared/ipc_protocol.h` | IPC 协议：共享内存布局、事件名、沙盒安全描述符、命名空间回退策略 |
| `shared/ime_ipc.h` | 跨进程内核对象名：单实例互斥体、服务互斥体、窗口消息 |
| `shared/pinyin_settings.h` | DLL/EXE 共享设置结构：序列化/反序列化、9 款预设皮肤 |
| `shared/tsf_guids.h` | TSF GUID 定义：CLSID、Profile GUID |
| `shared/unique_handle.h` | RAII Windows HANDLE 封装 |
| `shared/utf_utils.h` | UTF-8 / UTF-16 / Wide 转换工具 |
| `rime-ice/cn_dicts/` | rime-ice 词库（git submodule） |
| `pinyin_config.ini` | 用户配置文件（运行时生成） |
| `user.dict` | 用户自学习词库（运行时生成） |

---

## 简繁转换

基于《简化字总表》三表体系：

- **表一** (~350 字)：不作简化偏旁的简化字
- **表二** (~132 字 + 14 简化偏旁)：可作简化偏旁的简化字
- **表三** (类推简化，1500+ 字)：应用表二偏旁类推简化

转换策略：**最长词匹配优先** → 单字映射回退，确保「一简对多繁」正确消歧。

词汇覆盖：
- 一简对多繁词组消歧 ~150 对（如 出发→出發 / 头发→頭髮）
- 两岸 IT 词汇差异 ~40 对（如 软件→軟體、鼠标→滑鼠、服务器→伺服器）

---

## 自学习机制

每次选择候选时：
1. 该候选在用户词库中频率 +1
2. 自动保存到 `user.dict` 文件
3. 下次输入相同拼音时，高频词优先显示
4. 候选排序 = 系统词库基础频率 + 用户学习频率

---

## 许可证

MIT License
