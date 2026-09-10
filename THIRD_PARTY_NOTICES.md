# 第三方组件与许可声明

本文件汇总 OpenRM Tools 使用、移植、打包的第三方组件及其许可与来源。
本仓库自身代码的许可范围按目录划分，见 `LICENSE`；各许可证全文见 `LICENSES/`。

---

## 一、发布可行性

| 插件 | 可否公开发布 | 原因 |
|---|---|---|
| BandPass | 可以 | 仅依赖 MIT / Zlib 组件 |
| Analyzer | 可以 | 仅依赖 MIT / Zlib 组件 |
| Narrator | **不可（当前）** | 内含**无许可证**的 SAM 与**专有**的 DECtalk 代码，二者均未授予再分发权利 |

---

## 二、组件清单

| 组件 | 许可 | 用途 | 位置 |
|---|---|---|---|
| [iPlug2](https://github.com/iPlug2/iPlug2)（含 WDL） | Zlib | 插件框架、图形、VST3/AU/APP 封装 | 子模块 `third_party/iPlug2` |
| Steinberg VST 3 SDK（≥ 3.8） | MIT | VST3 接口与宿主支持 | `scripts/get_vst3_sdk.sh` 下载，钉 `v3.8.1_build_84` |
| [MAME](https://github.com/mamedev/mame) sound 设备实现 | BSD-3-Clause | TMS5110 / S14001A / SP0256 状态机与参数表 | `plugins/Narrator/src/dsp/{Tms5110Engine,TsiS14001Engine,Sp0256Engine}.*` |
| [Talkie](https://github.com/ArminJo/Talkie) | GPL | TMS5220 LPC 算法与量化表 | `plugins/Narrator/src/dsp/Tms5220Engine.*`、`dsp/tms/Vocab*.h` |
| [GmEsoft SP0256_CTS256A-AL2](https://github.com/GmEsoft/SP0256_CTS256A-AL2) | GPL-3.0-or-later | CTS256A 控制器、SP0256 / CTS256A 掩膜 ROM | `plugins/Narrator/src/dsp/{Cts256aEngine,Sp0256Engine}.*`、`dsp/cts256a/*`、`dsp/sp0256/*` |
| [nlohmann/json](https://github.com/nlohmann/json) 3.12.0 | MIT | 预设/设置文件的 JSON 读写 | 随 iPlug2，`Dependencies/Extras/nlohmann` |
| [s-macke/SAM](https://github.com/s-macke/SAM) | **无许可证** | SAM 语音合成内核 | `plugins/Narrator/src/dsp/sam/*` |
| [DECtalk 4.x](https://github.com/dectalk/DECtalkMini) | **专有** | DECtalk 语音合成内核 | `plugins/Narrator/src/dsp/dectalk/*` |
| [Outfit](https://github.com/Outfitio/Outfit-Fonts) | OFL-1.1 | 界面西文字体 | `assets/fonts/Outfit-*.ttf`，子集内嵌于 `plugins/*/resources/fonts/Mixed-*.ttf` |
| OPPO Sans 4.0 | 专有（免费使用条款） | 界面中文字体 | `assets/fonts/OPPOSans-Source.ttf`，子集内嵌于 `Mixed-*.ttf` |

---

## 三、逐项说明

### 1. iPlug2 / WDL — Zlib

`third_party/iPlug2` 是上游 [iPlug2](https://github.com/iPlug2/iPlug2) 的 fork
（`Fince10086/iPlug2` 的 `openrm` 分支，含本项目补丁），以 git 子模块引入。
iPlug2 及其内置的 WDL 采用 Zlib 许可（`third_party/iPlug2/LICENSE.txt`），
允许自由使用、修改、再分发，含商业用途，须保留版权与许可声明、不得谎称原创。
全文见 `LICENSES/Zlib.txt`。

### 2. Steinberg VST 3 SDK — MIT（≥ 3.8）

自 **VST 3.8** 起，VST 3 以 MIT 许可发布（此前为 GPLv3 / Steinberg 专有双许可）。
本项目的 VST3 目标按 MIT 处理，无需签署任何协议。首次构建时由
`scripts/get_vst3_sdk.sh` 下载，版本钉在 `v3.8.1_build_84`。

注意：**VST 2 源码不在该 MIT 许可范围内**，本项目不构建、不使用 VST2 目标。
"VST" 是 Steinberg Media Technologies GmbH 的商标；使用该名称/标识须遵守其商标规范，
本项目未使用其 Logo。

### 3. MAME — BSD-3-Clause

MAME 项目整体以 GPL-2.0+ 分发（因其并入了多种 GPL 兼容许可的代码），
但**超过 90% 的文件（含核心）标注 BSD-3-Clause**。本项目所移植的 sound 设备实现
（`src/devices/sound/tms5110.cpp`、`s14001a.cpp`、`sp0256.cpp`）在其文件头标注
**BSD-3-Clause**，故本项目按 BSD-3-Clause 处理其算法与参数表。

BSD-3-Clause 要求保留版权与免责声明、不得以 MAME 名义为衍生品背书。
全文见 `LICENSES/BSD-3-Clause.txt`。注意 MAME 是注册商标，本项目未使用该名称推广。

### 4. Talkie — GPL

`Tms5220Engine` 的 LPC-10 位流合成核与量化表取自 Talkie 库
（原作 Peter Knight / `going-digital/Talkie`；复刻版 Armin Joachimsmeyer /
`ArminJo/Talkie`，标注 GPL-3.0）。`dsp/tms/VocabTI99.h`、`VocabAcorn.h`、
`VocabClock.h` 为其中的词表数据。

GPL 具有传染性：凡包含该代码的整部作品须以 GPL 分发。Narrator 因此整体适用
GPL-3.0-or-later（见 `LICENSE` 第 2 节），源码已在仓库中提供，满足 GPL 的
对应源码义务。

### 5. GmEsoft SP0256_CTS256A-AL2 — GPL-3.0-or-later

`Cts256aEngine`（TMS7000 CPU + CTS256A 内存映射）、`Sp0256Roms.h`、
`Cts256aRoms.h` 移植/抽取自 GmEsoft 的逆向工程仓库，许认为 **GPL-3.0-or-later**。
与第 4 项同为 Narrator 采用 GPL 的原因。

### 6. nlohmann/json — MIT

随 iPlug2 以单头文件形式提供（3.12.0），用于预设与设置文件的 JSON 序列化。
MIT 许可，与本项目兼容，全文见 `LICENSES/MIT.txt`。

### 7. s-macke/SAM — ⚠️ 无许可证

`dsp/sam/` 是 [s-macke/SAM](https://github.com/s-macke/SAM) 对 SoftVoice SAM
（1979/1982）的逆向工程实现。**该仓库未附任何许可证** —— 无许可证即"保留一切
权利"，不构成任何再分发或使用授权。SAM 的算法与代码权利归属 SoftVoice, Inc.。

本仓库按项目所有者的决策收录该代码，**但这不改变其无权再分发的法律状态**。
在获得权利人授权或移除该代码之前，**Narrator 不得公开发布二进制或源码**。

### 8. DECtalk 4.x / DECtalkMini — ⚠️ 专有

`dsp/dectalk/` 移植自 [dectalk/DECtalkMini](https://github.com/dectalk/DECtalkMini)
（其代码与上游 `dectalk/dectalk` 基本一致）。源码自带版权头（DEC 1996/1997、
Fonix 2002/2003、Force Computers、Sensimetrics HLSyn 2.2 等）均为
"Restricted Rights / 仅凭书面授权使用"的专有措辞，**不是开源许可证**。

DECtalk 自 2015 年起经 DECtalk 邮件列表公开散布，事实处于 abandonware 状态，
**但公开可得不等于授权**。原始版权头已原样保留（见 `dsp/dectalk/README.md`）。
与第 7 项同为 Narrator 不可发布的原因。

### 9. 字体

- **Outfit** — `Copyright 2021 The Outfit Project Authors`，**OFL-1.1**。
  允许使用、修改、再分发（含嵌入与子集化），须随附 OFL 文本与版权声明。
  全文见 `LICENSES/OFL-1.1.txt`。
- **OPPO Sans 4.0** — `copyright © 2019-2024 by OPPO. All rights reserved.`，
  OPPO 官方条款：允许个人与企业免费使用（含商用），但明确禁止
  **"对字体进行改编或二次开发"**、售卖、提供其他下载渠道。

界面实际加载的 `plugins/*/resources/fonts/Mixed-*.ttf` 是 **Outfit 与 OPPO Sans
子集的合并产物**（BandPass 含 107 个、Narrator 含 240 个 CJK 码位）。对 OPPO Sans
而言，子集化可能落入其"改编/二次开发"禁令，需按第四节处理。

### 10. ROM 数据

- `dsp/s14001/S14001Roms.h`、`dsp/tms/VocabSspell.h` — 从 MAME romset
  （停产街机/玩具固件的掩膜 ROM）提取。
  **ROM 二进制内容的版权属原始厂商**（TI、TSI/SSi 等），MAME 并不主张其版权。
  本项目按"自行承担风险"收录。
- `dsp/sp0256/Sp0256Roms.h`、`dsp/cts256a/Cts256aRoms.h` — 从 GmEsoft 仓库抽取，
  适用 GPL-3.0-or-later（见第 5 项）。
- 各词表的生成脚本见 `tools/`。

---

## 四、待处理事项

| 优先级 | 事项 |
|---|---|
| 高 | **Narrator 发布前必须解决 SAM 与 DECtalk 的授权**：取得 SoftVoice / 权利人书面许可，或移除这两个引擎（保留 MAME/Talkie/GmEsoft 部分） |
| 高 | **中文字体的改编问题**：`Mixed-*.ttf` 对 OPPO Sans 做了子集化。选项：(a) 改用 OFL 中文字体（如思源黑体 / Noto Sans SC）；(b) 与 OPPO 确认子集化是否获准；(c) 改用未经修改的 OPPO Sans 整字重（体积显著增大） |
| 中 | **ROM 数据**：`S14001Roms.h` / `VocabSspell.h` 等掩膜 ROM 的原始厂商版权未获授权，公开发布存在风险 |
| 低 | 本文件随构建产物分发（见 CMake 许可拷贝逻辑），确保用户可获取完整声明 |
