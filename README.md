# OpenRM Tools

Open Realtime Music Tools （开源实时音乐工具），启发自 GRM Tools，基于 iPlug2。

| 插件 | 类型 | 说明 |
|---|---|---|
| BandPass | 效果器 | 双通道带通/带阻，启自 GRM Tools BandPass |
| Analyzer | 分析器 | 频谱/响度/立体声场测量 |
| Narrator | 乐器 | 复古 TTS 语音合成器（MIDI 触发说话）。引擎：SAM 1979/82、TMS5220、TMS5110、TSI S14001A、SP0256-AL2、DECtalk 4.x |

## 构建

第三方依赖 iPlug2 以 git 子模块管理（指向本组织 fork `Fince10086/iPlug2` 的 `openrm` 分支，含本项目补丁）；其中 VST3 目标还需要 Steinberg VST3 SDK（上游不随 iPlug2 分发），首次构建请运行脚本下载。

```sh
git clone --recurse-submodules <本仓库地址> OpenRM
cd OpenRM
./scripts/get_vst3_sdk.sh     # 首次构建: 下载 VST3 SDK (~500MB, 已钉版本)
cmake -B build -S .
cmake --build build           # 产出 APP / AU / VST3 (build/out/)
```

iPlug2 补丁更新方式：在 `third_party/iPlug2` 提交后推送 fork 的 `openrm` 分支，主仓库再 `git add third_party/iPlug2` 更新子模块指针。

## 测试

```sh
ctest --test-dir build       # 或直接运行 build/dsp_test
```

## 格式

APP / AU / VST3。

## macOS 打开说明

macOS 15+ 首次打开 App 可能提示"无法验证开发者"（App 未签名，属正常提示，不是软件损坏）：

- **右键点 App → 打开**，在确认弹窗中点"打开"
- 或 **系统设置 → 隐私与安全性**，在"已阻止使用"列表中点**"仍要打开"**
- 仍无法打开时可清除下载标记后重试：
```sh
sudo xattr -r -d com.apple.quarantine /Applications/ORMBandPass.app
```
（或替换为 App 所在目录）

## 许可

本仓库采用分层许可，许可证全文见 `LICENSE` 与 `LICENSES/`。

## 参考与致谢

本项目构建过程中使用和借鉴了以下开源项目与文献：

### 第三方组件

| 项目 | 许可 | 用途 |
|---|---|---|
| [iPlug2](https://github.com/iPlug2/iPlug2)（含 WDL） | Zlib | 插件框架、图形、各格式封装 |
| Steinberg VST 3 SDK（≥ 3.8） | MIT | VST3 接口 |
| [MAME](https://github.com/mamedev/mame) sound 设备实现 | BSD-3-Clause | TMS5110 / S14001A / SP0256 状态机与参数表 |
| [Talkie](https://github.com/ArminJo/Talkie) | GPL | TMS5220 LPC 算法与量化表 |
| [GmEsoft SP0256_CTS256A-AL2](https://github.com/GmEsoft/SP0256_CTS256A-AL2) | GPL-3.0-or-later | CTS256A 控制器 |
| [nlohmann/json](https://github.com/nlohmann/json) 3.12.0 | MIT | 预设 / 设置文件读写 |
| [s-macke/SAM](https://github.com/s-macke/SAM) | 无许可证 | SAM 内核 |
| [DECtalkMini / DECtalk 4.x](https://github.com/dectalk/DECtalkMini) | 废弃 | DECtalk 4.x 内核 |
| [Outfit](https://github.com/Outfitio/Outfit-Fonts) | OFL-1.1 | 界面西文字体 |
| OPPO Sans 4.0 | 专有（免费使用） | 界面中文字体 |

### 算法参考

| 文献 | 借鉴内容 |
|---|---|
| Vadim Zavalishin, *The Art of VA Filter Design* (2012) | TPT（Topology-Preserving Transform） |
| Andy Cytomic, *The Audio EQ Cookbook* | SVF 推导 |

### 声明

本项目与Ina GRM组织无任何关系，也并未使用GRM Tools系列插件的任何代码。