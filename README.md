# OpenRM Tools BandPass

Open Realtime Music Tools BandPass （开源实时音乐工具 带通效果器）, 启发自GRM Tools BandPass，基于 iPlug2。

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

## macOS 打开说明（网络下载的构建产物）

从 GitHub Actions / 网盘下载的 app 会带 macOS 的 quarantine 标记。当前产物为链接器自动签名（未做 Developer ID 公证），macOS 15+ 首次打开可能提示"无法验证开发者"或"已损坏"，任选一种方式解除：

```sh
# 方式一: 右键点击 app -> 打开 (仅需一次)
# 方式二: 终端去除 quarantine 标记 (推荐, 一劳永逸)
sudo xattr -cr /Applications/ORMBandPass.app
```

> 原因：macOS 15 (Sequoia) 起 Gatekeeper 对"ad-hoc 签名 + 网络下载"的 app 一律拦截；正式签名 + Apple 公证可彻底消除该提示（需 Apple Developer 账号）。AU/VST3 插件已做 ad-hoc 签名，装入 `~/Library/Audio/Plug-Ins/` 后可在 Logic 等宿主中加载。

## 参考与致谢

本项目构建过程中使用和借鉴了以下开源项目与文献：

### 代码复用

| 项目 | 许可 |
|---|---|---|
| [iPlug2](https://github.com/iPlug2/iPlug2) | MIT |

### 算法参考

| 文献 | 借鉴内容 |
|---|---|
| Vadim Zavalishin, *The Art of VA Filter Design* (2012) | TPT（Topology-Preserving Transform） |
| Andy Cytomic, *The Audio EQ Cookbook* | SVF 推导 |

### 声明

本项目与Ina GRM组织无任何关系，也并未使用GRM Tools系列插件的任何代码。