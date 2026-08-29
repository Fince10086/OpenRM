#pragma once

// 插件全局参数枚举与档位常量

#include <array>

enum EParams {
  kRelease,       // 频谱回落释放时间 (s)
  kRange,         // 频谱显示动态范围下限 (dBFS)
  kAttack,        // 频谱上升响应时间 (s)
  kRes,           // FFT 分辨率档位 (2048/4096/8192)
  kLfRes,         // 低频分辨率档位 (40/20/10 Hz; VQT 不再使用)
  kMode,          // 分析引擎模式 (0: STFT, 1: VQT, 2: PBT, 3: MR-FFT)
  kChannelMode,   // 声道显示模式 (0: PWR, 1: LR, 2: SUM)
  kLevelMode,     // 电平表模式 (0: dBTP, 1: dBFS+RMS, 2: VU)
  kLevelHold,     // 峰值保持时长档位 (0: 0.5s, 1: 2s, 2: ∞ 无限保持)
  kFreeze,        // 冻结开关 (0: 实时, 1: FREEZE 定格; 冻结中切换引擎用新算法重算冻结音频)
  kLevelHoldOn,   // 峰值保持开关 (频谱 hold 曲线与电平表 hold 亮线共用; 默认关闭, 用户状态持久化于全局设置文件; 追加末尾, 保旧状态文件下标兼容)
  kSlopeFFT,      // 频谱斜率档位索引, FFT 引擎独立保存 (0/3/4.5 dB/oct)
  kSlopeVQT,      // 频谱斜率档位索引, VQT 引擎独立保存 (-3/0/1.5 dB/oct)
  kSlopePBT,      // 频谱斜率档位索引, PBT 引擎独立保存 (-3/0/1.5 dB/oct)
  kSlopeMRFFT,    // 频谱斜率档位索引, MR-FFT 引擎独立保存 (-3/0/1.5 dB/oct)
  kFFTWindow,     // STFT 窗函数档位 (0: Hann, 1: BH4 4阶Blackman-Harris)
  kWindowVQT,     // VQT 窗函数档位 (0: Hann, 1: BH4), 与 STFT 独立保存 (追加末尾, 保旧状态文件下标兼容)
  kSlopeRTA,      // 频谱斜率档位索引, RTA 引擎独立保存 (-3/0/1.5 dB/oct; 追加末尾, 保旧状态文件下标兼容)
  kRtaOctave,     // RTA 分数倍频程档位 (0: 1/3 Oct, 1: 1/4 Oct, 2: 1/6 Oct; 追加末尾, 保旧状态文件下标兼容)
  kVQTGamma,      // VQT 低频带宽下限 γ (Hz): bw = fc/q + γ (5/10/15/20; 追加末尾)
  kNumParams
};

enum EFFTWindow { kFFTWindowHann = 0, kFFTWindowBH4 = 1, kNumFFTWindows = 2 };

using ParamSnapshot = std::array<double, kNumParams>;

// 算法档位可选值列表（参数实际存储对应的索引）
constexpr int kResOptions[] = {2048, 4096, 8192};
constexpr int kNumResOptions = 3;
constexpr int kLfResOptions[] = {20, 10, 5};
constexpr int kNumLfResOptions = 3;
constexpr int kPbtLfResOptions[] = {40, 20, 10};
constexpr int kNumPbtLfResOptions = 3;
constexpr int kRtaOctaveOptions[] = {3, 4, 6};
constexpr int kNumRtaOctaveOptions = 3;
constexpr int kVQTGammaOptions[] = {5, 10, 15, 20};
constexpr int kNumVQTGammaOptions = 4;

// 峰值保持时长档位 (kLevelHold 存索引)。持久档用 1e9s 表示: LevelMeter 的超时逻辑
// (holdT >= holdSec 才回落) 永不触发, 即无限保持, 且 holdSec > 0 的"画 hold 线"判定照常成立。
constexpr double kHoldTimeSecs[] = {0.5, 2.0, 1e9};
constexpr int kNumHoldTimeOptions = 3;

// 频谱斜率档位 (kSlopeFFT/kSlopeVQT/kSlopePBT/kSlopeMRFFT/kSlopeRTA 存索引, 各引擎独立):
// 显示域每 band 施加 S·log2(f/f_pivot) dB 的倾斜。FFT 按 bin 显示白噪天生平直,
// 逐 band 能量积分显示 (VQT/PBT/MR-FFT/RTA) 白噪天生 +3 dB/oct, 故两组档值相差 -3,
// 使同一信号的视觉斜率在两种显示下一致 (如粉噪在 FFT|3 与 VQT|0 下都平直)。
constexpr double kSlopeDbFFT[] = {0.0, 3.0, 4.5};
constexpr double kSlopeDbLog[] = {-3.0, 0.0, 1.5};
constexpr int kNumSlopeOptions = 3;

// MR-FFT / VQT 固定档位: BPO 均固定 24 (VQT γ 由 GAMMA 按钮调节)
constexpr int kVQTBpo = 24;
constexpr int kMRFFTBpo = 24;

// 分析引擎模式 (五态: STFT, VQT, PBT, MR-FFT, RTA)
enum EAnalyzerMode {
  kModeFFT = 0,
  kModeVQT = 1,
  kModePBT = 2,
  kModeMRFFT = 3,
  kModeRTA = 4,
  kNumModes = 5
};

// 声道显示模式 (三态: PWR(Merge) / LR / SUM(Merge))
enum EChannelMode { kChanModePWR = 0, kChanModeLR = 1, kChanModeSUM = 2, kNumChanModes = 3 };

// 电平表模式
enum ELevelMode { kLevelModeDBTP = 0, kLevelModeDBFS = 1, kLevelModeVU = 2, kNumLevelModes = 3 };

// UI 控件消息标签
enum EControlTags { kCtrlTagPad = 100, kCtrlTagCpu = 101, kCtrlTagLegend = 102 };
