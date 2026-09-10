# ORM 系列插件 — 架构说明

面向改动代码的人。构建与使用说明见根目录 `README.md`。

---

## 一、仓库结构

```
OpenRM/
├─ CMakeLists.txt          顶层: 版本来源、测试注册、三个 add_subdirectory
├─ cmake/OpenRMPlugin.cmake  openrm_add_plugin(): 目标创建 + 字体 + 签名 + 部署 + 许可
├─ plugins/
│  ├─ common/              三插件共用, 唯一真源
│  │  ├─ Theme.h           调色板与设计 token (ThemeHue/ThemeSatMax/ThemeMode, COL_*, HSBToIColor, MakeORMStyle...)
│  │  ├─ dsp/FastMath.h    快速 log2 / dB 换算 (UI 绘制与电平表使用)
│  │  └─ controls/         SectionTitleControl, ThemeCornerResizer
│  ├─ BandPass/            效果器: 多段带通 + 随机调制 + 预设槽
│  ├─ Analyzer/            分析器: 频谱 / 示波器 / 电平 / 响度
│  └─ Narrator/            乐器: 复古 TTS, MIDI 触发说话
├─ tests/                  离线 DSP 测试 (纯 C++17, 不依赖 iPlug2)
├─ tools/                  ROM / 词表 / 图标生成器
├─ scripts/                VST3 SDK 获取、安装、DMG 打包
├─ installer/              Windows 安装器 (版本由 CMake 注入)
├─ assets/                 字体源、图标 SVG、参考 PDF (ROM dump 不入库)
└─ third_party/iPlug2      git 子模块
```

每个插件的 `src/` 只放插件特有内容：`<Plugin>.h/.cpp`、`Params.h`、`Strings.h`、`Theme.h`（薄包装）、`dsp/`、`controls/`。跨插件共用的东西一律进 `plugins/common/`。

---

## 二、线程模型

音频线程 = 宿主回调线程（`ProcessBlock` / `ProcessMidiMsg`）；UI 线程 = 编辑器消息循环 + `OnIdle`。

| 方向 | 机制 | 位置 |
|---|---|---|
| 音频 → UI | `IPlugQueue<T>`（无锁单向队列） | `Narrator.h` 的 `mNoteQueue` / `mProgressQueue` / `mTimelineQueue` |
| UI → 音频 | `orm::ParamMailbox<T>`（无锁单生产/单消费） | `BandPassCore.h`；`BandPass.h` 的 `mParamMailbox` / `mRandomDeltaMailbox` |
| 音频 → UI | `std::atomic` 标量快照 | `Analyzer.h` 的电平 / 响度 / 频谱统计量 |
| UI ↔ UI / 交叉 | `std::atomic` 请求标志 + `OnIdle` 轮询 | Analyzer 的 `mLevelResetFlag`、`mLoudSetSR`；Narrator 的 `mPendingSelect` / `mSelectChanged` |

**约定**

- `ProcessBlock` / `ProcessMidiMsg` 内不做动态分配、不持锁、不做与帧数无关的重计算。
- MIDI 事件用定长数组接收：`Narrator.h` 的 `kMaxMidiPerBlock = 256` + `mMidiCount`，越界丢弃。不要改回 `std::vector`——音频线程扩容即分配。
- UI 到音频传参数优先用 mailbox，不要直接写共享容器。
- 音频线程需要"整块数据"时，走"快照发布 + 原子换指针"，不要让音频线程排队等锁。

**已知例外**：Narrator 的 TTS 合成仍在音频线程内（`EnsureRendered` / `RenderBankEntry`），且存在音频线程持锁与锁外改共享容器两处竞争。修复方向是"渲染工作线程 + 快照发布"，**不能**简单搬到 `OnIdle`（`OnIdle` 在 `#if IPLUG_EDITOR` 内，宿主中关闭界面时不执行）。

---

## 三、iPlug2 的 `IPLUG_DSP` / `IPLUG_EDITOR` 约定

这是本项目最容易踩的一处格式约定，三插件已统一：

- **虚函数覆盖必须无条件定义。** `OnIdle`、`OnParamChangeUI` 在头文件里是 `override` 声明，只要类被实例化，它们的定义就必须存在，否则链接期报"声明了但没定义"。因此这两个函数的**定义不能整体放进 `#if IPLUG_EDITOR`**；若函数体内用了 `GetUI()` 等编辑器专有接口，用**函数体内**的 `#if IPLUG_EDITOR` 包住那几行。
- **纯 UI 辅助函数**（`ApplyLanguage` / `ApplyTooltips` / `ApplyTheme` / `RefreshThemeColors` / `ToggleSettingsPanel`）在头文件与 `.cpp` 里**都**放在 `#if IPLUG_EDITOR` 内，声明与定义的保护范围必须一致。
- 快照与撤销/重做一组（`Snapshot` / `ApplySnapshot` / `Undo` / `Redo` / `MaybePushGestureUndo` 等）在头文件里是无条件声明的，因此定义也放在保护块外；其中会碰 UI 的部分同样用函数体内的 `#if IPLUG_EDITOR` 包住。

---

## 四、构建产物与部署

由 `cmake/OpenRMPlugin.cmake` 的 `openrm_add_plugin()` 统一负责：

- **macOS**：每个插件产出 `.app` / `.vst3` / `.component`；先拷字体与许可声明到 `Contents/Resources`，再 ad-hoc 签名，最后部署到 `~/Applications` 与 `~/Library/Audio/Plug-Ins/{VST3,Components}`。签名必须在拷贝之后。
- **Windows**：产出 `.app.exe` 与 `.vst3`；字体嵌入 dll。
- **许可声明**：仓库根的 `LICENSE`、`THIRD_PARTY_NOTICES.md`、`LICENSES/*.txt` 会拷进每个产物的 `Contents/Resources/Licenses/`。

iPlug2 自带的自动部署（`IPLUG_DEPLOY_PLUGINS`）在顶层被显式关闭，避免同一产物被复制两份、以及缺字体的副本先落盘。

---

## 五、版本号来源（单一来源）

```
plugins/BandPass/config.h   PLUG_VERSION_STR
        │
        ├─→ 根 CMakeLists.txt  project(OpenRM VERSION ...)
        │        └─→ configure_file(installer/ORM-version.iss.in)
        │                 └─→ build/installer/ORM-version.iss
        │                          └─→ installer/ORMBandPass.iss  (#include)
        └─→ 各插件 CMakeLists.txt  project(ORM<Name> VERSION ...)
```

- 修改版本只改 `config.h` 的 `PLUG_VERSION_STR`。
- `PLUG_VERSION_HEX` 必须与 `STR` 一致，格式 `0xVVVVRRMM`（V=version, R=revision, M=minor revision），例如 `0.5.1` → `0x00000501`。
- 编译 Windows 安装器前需先执行过一次 cmake 配置，否则 `installer/ORMBandPass.iss` 的 `#include` 找不到文件。

---

## 六、测试

三个离线测试都是纯 C++17、不依赖 iPlug2，可用 `ctest --test-dir build` 一次跑完：

| 测试 | 覆盖 |
|---|---|
| `dsp_test` | `BandPassCore`（带通：峰值位置、带宽、link、干湿、归一化、Q 稳定性、slope）+ `common/dsp/FastMath` 精度 |
| `loudness_test` | `LoudnessMeter`（ITU-R BS.1770：M/S/I/LRA/TruePeak） |
| `narrator_test` | 语音引擎（SAM / TMS5220 / TMS5110 / TSI S14001A / SP0256 / CTS256A / DECtalk） |

新增测试在根 `CMakeLists.txt` 里 `add_executable` + `add_test` 注册。

---

## 七、许可

分层：`plugins/common`、BandPass、Analyzer、Narrator 的 `src/dsp/` 以外部分、`tests/`（`narrator_test.cpp` 除外）、`tools/`、`scripts/`、`cmake/` 为 MIT；Narrator 语音引擎与 `tests/narrator_test.cpp` 为 GPL-3.0-or-later。第三方组件的归属与来源见 `THIRD_PARTY_NOTICES.md`。**Narrator 当前不可公开发布**（内含无许可证的 SAM 与专有的 DECtalk）。
