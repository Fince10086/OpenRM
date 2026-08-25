#pragma once

// 插件全局参数枚举与档位常量

#include <array>

enum EParams {
  kMix = 0,       // 干湿比 / 状态快照占位
  kRelease,       // 频谱回落释放时间 (s)
  kRange,         // 频谱显示动态范围下限 (dBFS)
  kAttack,        // 频谱上升响应时间 (s)
  kRes,           // FFT 分辨率档位 (1024/2048/4096)
  kLfRes,         // VQT 低频带宽保底 γ 档位 (40/20/10 Hz)
  kBpo,           // VQT 每八度频带数 (12/24)
  kMode,          // 分析引擎模式 (0: FFT, 1: VQT)
  kChannelMode,   // 声道显示模式 (0: L/R, 1: ALL, 2: MERGE)
  kMergeAlgo,     // 合并算法 (0: PWR 功率和, 1: SUM 时域单声道和)
  kNumParams
};

using ParamSnapshot = std::array<double, kNumParams>;

// 算法档位可选值列表（参数实际存储对应的索引）
constexpr int kResOptions[] = {1024, 2048, 4096};
constexpr int kNumResOptions = 3;
constexpr int kLfResOptions[] = {40, 20, 10};
constexpr int kNumLfResOptions = 3;
constexpr int kBpoOptions[] = {12, 24};
constexpr int kNumBpoOptions = 2;

// 分析引擎模式
enum EAnalyzerMode { kModeFFT = 0, kModeVQT = 1 };

// 声道显示模式
enum EChannelMode { kChanModeLR = 0, kChanModeAll = 1, kChanModeMerge = 2, kNumChanModes = 3 };

// 合并算法
enum EMergeAlgo { kMergeAlgoPWR = 0, kMergeAlgoSUM = 1, kNumMergeAlgos = 2 };

// UI 控件消息标签
enum EControlTags { kCtrlTagPad = 100, kCtrlTagCpu = 101 };
