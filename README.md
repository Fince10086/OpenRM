# GRMClone — GRM BandPass 克隆

GRM Tools BandPass 风格的立体声带通滤波器插件, 基于 iPlug2 (TPT SVF / Zavalishin)。

当前版本: **v0.0.3** (统一版本号, 定义于 `plugins/BandPass/config.h` 的 `PLUG_VERSION_STR`)

## 目录结构

- `plugins/BandPass/` — 插件源码 (`src/`), 资源 (`resources/`), 构建脚本
  - `src/dsp/BandPassCore.h` — 零依赖 DSP 核心 (纯头文件)
  - `src/controls/FilterNodePad.h` — XY 滤波节点可视化控件
- `tests/dsp_test.cpp` — DSP 核心离线自测 (不依赖 iPlug2)
- `third_party/iPlug2/` — iPlug2 子模块
- `build/` — CMake 构建目录

## 构建

```sh
cmake -B build -S .
cmake --build build          # 产出 APP / AU / VST3 (build/out/)
```

## 测试

```sh
ctest --test-dir build       # 或直接运行 build/dsp_test
```

## 格式

APP (独立运行, 便于调试) / AU / VST3。CLAP 待引入 CLAP SDK 后追加。
