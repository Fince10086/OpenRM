#pragma once

// 插件全局参数枚举与档位常量

#include <array>

enum EParams {
  kRelease,       // 频谱回落释放时间 (s)
  kRange,         // 频谱显示动态范围下限 (dBFS)
  kAttack,        // 频谱上升响应时间 (s)
  kRes,           // FFT 分辨率档位 (2048/4096/8192)
  kLfRes,         // VQT 低频带宽保底 γ 档位 (低/中/高: 20/10/5 Hz)
  kBpo,           // VQT 每八度频带数 (12/24)
  kMode,          // 分析引擎模式 (0: FFT, 1: VQT)
  kChannelMode,   // 声道显示模式 (0: LR, 1: PWR(Merge), 2: SUM(Merge))
  kLevelMode,     // 电平表模式 (0: dBTP 真峰值, 1: dBFS+RMS, 2: VU)
  kLevelHold,     // 峰值保持时长 (s, 0=关)
  kNumParams
};

using ParamSnapshot = std::array<double, kNumParams>;

// 算法档位可选值列表（参数实际存储对应的索引）
constexpr int kResOptions[] = {2048, 4096, 8192};
constexpr int kNumResOptions = 3;
constexpr int kLfResOptions[] = {20, 10, 5};
constexpr int kNumLfResOptions = 3;
constexpr int kBpoOptions[] = {12, 24};
constexpr int kNumBpoOptions = 2;

// 分析引擎模式
enum EAnalyzerMode { kModeFFT = 0, kModeVQT = 1 };

// 声道显示模式 (三态: LR / PWR(Merge) / SUM(Merge))
enum EChannelMode { kChanModeLR = 0, kChanModePWR = 1, kChanModeSUM = 2, kNumChanModes = 3 };

// 电平表模式
enum ELevelMode { kLevelModeDBTP = 0, kLevelModeDBFS = 1, kLevelModeVU = 2, kNumLevelModes = 3 };

// UI 控件消息标签
enum EControlTags { kCtrlTagPad = 100, kCtrlTagCpu = 101, kCtrlTagLegend = 102 };
