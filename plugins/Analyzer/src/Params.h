#pragma once

// 插件全局参数枚举与档位常量

#include <array>

enum EParams {
  kRelease,       // 频谱回落释放时间 (s)
  kRange,         // 频谱显示动态范围下限 (dBFS)
  kAttack,        // 频谱上升响应时间 (s)
  kRes,           // FFT 分辨率档位 (2048/4096/8192)
  kLfRes,         // 低频分辨率档位 (PAZ: 40/20/10 Hz; VQT 不再使用)
  kBpo,           // 已废弃: MR-FFT 每八度频带数, 固定 24 不再暴露 UI (下标保留保旧状态文件兼容, 勿复用)
  kMode,          // 分析引擎模式 (0: STFT, 1: VQT, 2: PAZ, 3: MR-FFT)
  kChannelMode,   // 声道显示模式 (0: LR, 1: PWR, 2: SUM)
  kLevelMode,     // 电平表模式 (0: dBTP, 1: dBFS+RMS, 2: VU)
  kLevelHold,     // 峰值保持时长档位 (0: 0.5s, 1: 2s, 2: KEEP 持久)
  kPazAlgo,       // 已废弃: PAZ 固定滤波器组算法, 切换按钮已移除 (下标保留保旧状态文件兼容, 勿复用)
  kFreeze,        // 冻结开关 (0: 实时, 1: FREEZE 定格; 冻结中切换引擎用新算法重算冻结音频)
  kPyramidDecim,  // VQT 金字塔降采样档位 (0: LIN 线性相位, 1: MIN 最小相位)
  kLevelHoldOn,   // 峰值保持开关 (频谱 hold 曲线与电平表 hold 亮线共用; 默认关闭, 用户状态持久化于全局设置文件; 追加末尾, 保旧状态文件下标兼容)
  kSlopeFFT,      // 频谱斜率档位索引, FFT 引擎独立保存 (0/3/4.5 dB/oct)
  kSlopeVQT,      // 频谱斜率档位索引, VQT 引擎独立保存 (-3/0/1.5 dB/oct)
  kSlopePAZ,      // 频谱斜率档位索引, PAZ 引擎独立保存 (-3/0/1.5 dB/oct)
  kSlopeMRFFT,    // 频谱斜率档位索引, MR-FFT 引擎独立保存 (-3/0/1.5 dB/oct)
  kFFTWindow,     // STFT 窗函数档位 (0: Hann, 1: BH4 4阶Blackman-Harris)
  kWindowVQT,     // VQT 窗函数档位 (0: Hann, 1: BH4), 与 STFT 独立保存 (追加末尾, 保旧状态文件下标兼容)
  kPazKernel,     // PAZ 解调核长系数档位索引 (T = k/bw, 档值表 kPazKernelLenOptions; 追加末尾保兼容)
  kNumParams
};

enum EFFTWindow { kFFTWindowHann = 0, kFFTWindowBH4 = 1, kNumFFTWindows = 2 };

using ParamSnapshot = std::array<double, kNumParams>;

// 算法档位可选值列表（参数实际存储对应的索引）
constexpr int kResOptions[] = {2048, 4096, 8192};
constexpr int kNumResOptions = 3;
constexpr int kLfResOptions[] = {20, 10, 5};
constexpr int kNumLfResOptions = 3;
constexpr int kPazLfResOptions[] = {40, 20, 10};
constexpr int kNumPazLfResOptions = 3;

// PAZ 解调核长系数档值 (kPazKernel 存索引)。T = k/bw: k 越小延迟越低、带间读数越平,
// 但 k ≈ 4.6 起最坏邻带滑入主瓣开始泄漏 (详见 PAZAnalyzer::SetKernelLen 注释)。
constexpr double kPazKernelLenOptions[] = {3.0, 3.5, 4.0, 4.5, 5.0, 5.5, 6.0, 7.0, 8.0};
constexpr int kNumPazKernelLenOptions = 9;

// 峰值保持时长档位 (kLevelHold 存索引)。持久档用 1e9s 表示: LevelMeter 的超时逻辑
// (holdT >= holdSec 才回落) 永不触发, 即无限保持, 且 holdSec > 0 的"画 hold 线"判定照常成立。
constexpr double kHoldTimeSecs[] = {0.5, 2.0, 1e9};
constexpr int kNumHoldTimeOptions = 3;

// 频谱斜率档位 (kSlopeFFT/kSlopeVQT/kSlopePAZ/kSlopeMRFFT 存索引, 各引擎独立):
// 显示域每 band 施加 S·log2(f/f_pivot) dB 的倾斜。FFT 按 bin 显示白噪天生平直,
// 逐 band 能量积分显示 (VQT/PAZ/MR-FFT) 白噪天生 +3 dB/oct, 故两组档值相差 -3,
// 使同一信号的视觉斜率在两种显示下一致 (如粉噪在 FFT|3 与 VQT|0 下都平直)。
constexpr double kSlopeDbFFT[] = {0.0, 3.0, 4.5};
constexpr double kSlopeDbLog[] = {-3.0, 0.0, 1.5};
constexpr int kNumSlopeOptions = 3;

// VQT / MR-FFT 固定档位 (不再暴露 UI): γ 取原 HIGH 档, BPO 均固定 24
constexpr int kVQTGammaHz = 5;
constexpr int kVQTBpo = 24;
constexpr int kMRFFTBpo = 24;

// 分析引擎模式 (四态: STFT, VQT, PAZ, MR-FFT)
enum EAnalyzerMode { kModeFFT = 0, kModeVQT = 1, kModePAZ = 2, kModeMRFFT = 3, kNumModes = 4 };

// 声道显示模式 (三态: LR / PWR(Merge) / SUM(Merge))
enum EChannelMode { kChanModeLR = 0, kChanModePWR = 1, kChanModeSUM = 2, kNumChanModes = 3 };

// 电平表模式
enum ELevelMode { kLevelModeDBTP = 0, kLevelModeDBFS = 1, kLevelModeVU = 2, kNumLevelModes = 3 };

// UI 控件消息标签
enum EControlTags { kCtrlTagPad = 100, kCtrlTagCpu = 101, kCtrlTagLegend = 102 };
