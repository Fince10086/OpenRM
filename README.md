# OpenRM Tools BandPass

Open Realtime Music Tools BandPass （开源实时音乐工具 带通效果器）, 启发自GRM Tools BandPass，基于 iPlug2。

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

APP / AU / VST3。

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