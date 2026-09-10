# 新增一个插件

步骤按"能构建 → 能出界面 → 能发布"排列。可先用 `plugins/BandPass` 当模板整目录复制再改名。

---

## 1. `config.h`

从 `plugins/BandPass/config.h` 复制，逐项修改：

| 宏 | 说明 |
|---|---|
| `PLUG_NAME` / `PLUG_MFR` | 显示名与厂牌 |
| `PLUG_VERSION_HEX` / `PLUG_VERSION_STR` | 两者必须一致；HEX 格式 `0xVVVVRRMM`（`0.5.1` → `0x00000501`） |
| `PLUG_UNIQUE_ID` / `PLUG_MFR_ID` | 各 4 个字符，全系列不可重复 |
| `PLUG_CLASS_NAME` | C++ 类名 |
| `BUNDLE_NAME` / `SHARED_RESOURCES_SUBPATH` | 与 CMake 目标名一致 |
| `PLUG_CHANNEL_IO` / `PLUG_TYPE` / `PLUG_DOES_MIDI_IN` 等 | 通道与插件类型 |
| `PLUG_WIDTH` / `PLUG_HEIGHT` / `PLUG_FPS` | 画布尺寸与刷新率 |
| `AUV2_*` | AUv2 入口符号名，需唯一 |
| `MIXED_*_FN` | 打包字体文件名 |

`PLUG_VERSION_STR` 是**全项目版本号的唯一来源**，安装器版本由它派生（见 `docs/architecture.md` 第五节）。

---

## 2. `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.14)

# 版本单一来源: config.h (PLUG_VERSION_STR)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/config.h" _xx_version REGEX "PLUG_VERSION_STR \"[0-9.]+\"")
string(REGEX MATCH "\"([0-9.]+)\"" _xx_version_match "${_xx_version}")
project(ORMNew VERSION ${CMAKE_MATCH_1})

# iPlug2 初始化必须在目录作用域 (内含 enable_language(OBJC/OBJCXX))
if(NOT DEFINED IPLUG2_DIR)
  set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/iPlug2" CACHE PATH "iPlug2 root directory")
endif()
include(${IPLUG2_DIR}/iPlug2.cmake)
find_package(iPlug2 REQUIRED)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
include(OpenRMPlugin)

openrm_add_plugin(${PROJECT_NAME}
  SOURCES
    src/New.h
    src/New.cpp
    src/Params.h
    src/Strings.h
    src/Theme.h
    src/controls/...
  FORMATS APP AU VST3
  DEFINES SAMPLE_TYPE_FLOAT
  FONTS Mixed-Regular.ttf Mixed-SemiBold.ttf Mixed-Bold.ttf
)

# macOS 渲染节拍改由 CVDisplayLink (vsync) 驱动, 与现有三插件一致;
# 默认 60Hz NSTimer 与 CAMetalLayer 的 vsync 双钟漂移, 拖拽缩放时文字会发糊。
if(APPLE)
  foreach(_fmt_tgt ${PROJECT_NAME}-app ${PROJECT_NAME}-vst3 ${PROJECT_NAME}-au)
    if(TARGET ${_fmt_tgt})
      target_compile_definitions(${_fmt_tgt} PRIVATE IGRAPHICS_CVDISPLAYLINK)
    endif()
  endforeach()
endif()
```

**不要把 `include(${IPLUG2_DIR}/iPlug2.cmake)` 和 `find_package(iPlug2)` 挪进函数**：其内部调用 `enable_language(OBJC/OBJCXX)`，在函数作用域执行会导致生成阶段缺少编译规则。

`openrm_add_plugin()` 支持 `SOURCES` / `FORMATS` / `DEFINES` / `FONTS` / `FONT_DIR`，并自动负责字体拷贝、ad-hoc 签名、macOS 部署与许可声明分发。

---

## 3. 顶层注册

在根 `CMakeLists.txt` 的 `add_subdirectory` 列表中加一行：

```cmake
add_subdirectory(plugins/New)
```

若有离线测试，照现有三个的样子注册（纯 C++17、不依赖 iPlug2）：

```cmake
add_executable(new_test tests/new_test.cpp)
target_compile_features(new_test PRIVATE cxx_std_17)
add_test(NAME new_test COMMAND new_test)
```

---

## 4. 复用 `plugins/common/`

路径基准是 `plugins/<NAME>/src/`，所以共用的东西都是 `../../common/...`：

```cpp
#include "../../common/Theme.h"                  // 调色板与设计 token
#include "../../common/dsp/FastMath.h"           // 快速 log2 / dB
#include "../../common/controls/SectionTitleControl.h"
```

插件自己的 `src/Theme.h` 写成薄包装，只放插件专属项：

```cpp
#pragma once
#include "../../common/Theme.h"
#include "Strings.h" // 控件经本文件间接取用 Tr()/kTxt, 勿删

namespace iplug { namespace igraphics {
// 本插件专属的颜色 / 布局常量
} }
```

在 `plugins/common/` 下新增共用符号时，记得同步登记到 `cmake/OpenRMPlugin.cmake` 里 `openrm_add_plugin()` 的 `_p_SOURCES`（仅用于 IDE 分组，不影响编译）。

**不要**把插件专属内容放进 `common/`——`common/` 必须保持纯原创且许可为 MIT，Narrator 的 GPL 引擎与词表绝不沉入。

---

## 5. `resources/`

从现有插件复制并改名：

```
resources/
├─ New-AU-Info.plist  New-VST3-Info.plist  New-macOS-Info.plist
├─ New-macOS-MainMenu.xib
├─ New.icns  New.ico
├─ main.rc  main.rc_mac_dlg  main.rc_mac_menu
├─ resource.h
└─ fonts/          打包进产物的字体
```

图标可用 `scripts/make_icon_*.py` 从 `assets/icons/` 的 SVG 生成。

---

## 6. 字体（界面含中文时必做）

1. **中文只写在 `src/Strings.h` 里。** 不要在控件里硬编码中文——字体子集脚本只扫描 `Strings.h`，硬编码的字不会被收进子集，界面上会缺字。
2. 复制 `plugins/BandPass/scripts/subset_font.py`，按文件头注释的用法生成 `resources/fonts/Mixed-*.ttf`（需要 `python3` + `fontTools`）。
3. 字重集合由脚本的 `WEIGHTS` 定义（Regular 400 / SemiBold 600 / Bold 700），与 `config.h` 的 `MIXED_*_FN` 对应。
4. 字体链路目前是手动的：改完 `Strings.h` 要重跑一次脚本并提交新的 `Mixed-*.ttf`。

---

## 7. 参数与设置

- `src/Params.h`：`enum EParams` 从 0 起编号；`kNumParams` 是快照数组的尺寸。**枚举 ID 在插件之间不可复用**——`Tr()` 按 ID 查表，索引撞车不会报错，只会取到含义不同的字符串或数值。
- 撤销/重做：用 `ParamSnapshot`（`std::array<double, kNumParams>`）+ 现有的一组函数（`Snapshot` / `PushUndoSnapshot` / `MaybePushGestureUndo` / `MarkStateStable` / `Undo` / `Redo`），保护块的写法见 `docs/architecture.md` 第三节。
- 设置落盘：参考现有 `SettingsFileIO.h`（JSON，`nlohmann/json` 随 iPlug2 提供）。
- **控件不要反向依赖宿主参数枚举。** 数值格式化走 `SetValueFormatter(ds, p)` 回调，把格式逻辑留在插件侧；控件内部用 `switch (GetParamIdx())` 枚举本插件参数是已有的反面教材。

---

## 8. 交付前检查

- [ ] `cmake -B build -S . && cmake --build build`：退出码 0，无 error
- [ ] `ctest --test-dir build`：全部通过
- [ ] `config.h` 的 `PLUG_VERSION_HEX` 与 `PLUG_VERSION_STR` 一致
- [ ] 中文只出现在 `Strings.h`，字体已重新子集化并提交
- [ ] 新代码默认 MIT（见 `LICENSE`）；若引入第三方代码，在 `THIRD_PARTY_NOTICES.md` 补条目、在 `LICENSES/` 补全文
- [ ] `plugins/common/` 未被写入插件专属内容
