#pragma once

// 插件全局参数枚举与档位常量

#include <array>

enum EParams {
  kSpeed,         // 响应速度预设（0:MIN..4:MAX，决定释放时间常数）
  kReleaseMode,   // 释放回落模式（0:对数域单极点，1:匀速 dB/s）
  kRange,         // 频谱显示动态范围下限（dBFS）
  kRes,           // FFT 分辨率（2048/4096/8192）
  kLfRes,         // PBT 低频分辨率（40/20/10 Hz）
  kMode,          // 分析引擎（0:STFT, 1:VQT, 2:PBT, 3:RTA）
  kChannelMode,   // 声道显示（0:PWR, 1:LR, 2:SUM）
  kLevelMode,     // 电平表模式（0:dBTP, 1:dBFS+RMS）
  kLevelHold,     // 峰值保持时长（0:0.5s, 1:2s, 2:∞）
  kLevelHoldOn,   // 峰值保持开关（频谱 hold 曲线与电平表 hold 亮线共用）
  kFreeze,        // 冻结开关（0:实时, 1:定格）
  kSlopeFFT,      // FFT 斜率档（0/3/4.5 dB/oct）
  kSlopeVQT,      // VQT 斜率档（-3/0/1.5 dB/oct）
  kSlopePBT,      // PBT 斜率档（-3/0/1.5 dB/oct）
  kSlopeRTA,      // RTA 斜率档（-3/0/1.5 dB/oct）
  kRtaOctave,     // RTA 分数倍频程（0:1/6, 1:1/12, 2:1/24）
  kFFTWindow,     // STFT 窗函数（0:Hann/锐利, 1:BH5/纯净）
  kWindowVQT,     // VQT 窗函数（与 STFT 独立保存）
  kVQTGamma,      // VQT 低频带宽下限 γ（20/10/5 Hz，越小越精细）
  kLoudPreset,    // 响度目标预设（-9/-14/-23/-24 LUFS）
  kLoudScale,     // 响度条刻度窗偏移（+9/+18 LU）
  kNumParams
};

// 窗函数：0=Hann（锐利），1=5-term Blackman-Harris（纯净，-125 dB）
enum EFFTWindow { kFFTWindowHann = 0, kFFTWindowClean = 1, kNumFFTWindows = 2 };

// 速度档释放时间：LOG=0.2~4s（Pro-Q 实测），LIN=LOG×2（匀速回落体感对齐）
enum ESpeed { kSpeedMIN = 0, kSpeedSLOW, kSpeedMED, kSpeedFAST, kSpeedMAX, kNumSpeedOptions = 5 };
constexpr double kSpeedReleaseLog[] = {4.0, 2.0, 1.0, 0.5, 0.2};
constexpr double kSpeedReleaseLin[] = {9.6, 4.8, 2.4, 1.2, 0.5};

using ParamSnapshot = std::array<double, kNumParams>;

// 算法档位可选值（档序低→中→高，Hz 越小精度越高）
constexpr int kResOptions[] = {2048, 4096, 8192};
constexpr int kNumResOptions = 3;
constexpr int kPbtLfResOptions[] = {40, 20, 10};
constexpr int kNumPbtLfResOptions = 3;
constexpr int kRtaOctaveOptions[] = {6, 12, 24};
constexpr int kNumRtaOctaveOptions = 3;
constexpr int kVQTGammaOptions[] = {20, 10, 5};
constexpr int kNumVQTGammaOptions = 3;

constexpr double kHoldTimeSecs[] = {0.5, 2.0, 1e9};
constexpr int kNumHoldTimeOptions = 3;

constexpr double kLoudTargets[] = {-9.0, -14.0, -23.0, -24.0};
constexpr int kNumLoudPresets = 4;

// 响度条刻度窗偏移：1/3 高度=目标值，顶=目标+偏移，底=目标-2×偏移
constexpr double kLoudScaleOffsets[] = {9.0, 18.0};
constexpr int kNumLoudScaleOptions = 2;

// 斜率档：FFT 按 bin 显示白噪天生平直，逐 band 引擎（VQT/PBT/RTA）白噪天生 +3 dB/oct，
// 两组相差 -3 使同一信号视觉斜率跨显示一致
constexpr double kSlopeDbFFT[] = {0.0, 3.0, 4.5};
constexpr double kSlopeDbLog[] = {-3.0, 0.0, 1.5};
constexpr int kNumSlopeOptions = 3;

constexpr int kVQTBpo = 24;

enum EAnalyzerMode {
  kModeFFT = 0,
  kModeVQT = 1,
  kModePBT = 2,
  kModeRTA = 3,
  kNumModes = 4
};

enum EChannelMode { kChanModePWR = 0, kChanModeLR = 1, kChanModeSUM = 2, kNumChanModes = 3 };

enum ELevelMode { kLevelModeDBTP = 0, kLevelModeDBFS = 1, kNumLevelModes = 2 };

// UI 控件消息标签
enum EControlTags { kCtrlTagPad = 100, kCtrlTagCpu = 101, kCtrlTagScope = 104, kCtrlTagOscilloscope = 105 };
