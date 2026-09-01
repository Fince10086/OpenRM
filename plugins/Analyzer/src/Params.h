#pragma once

// 插件全局参数枚举与档位常量 (未发布, 无旧状态文件兼容约束, 按子系统分组排列)

#include <array>

enum EParams {
  kSpeed,         // 频谱响应速度预设档位 (0: MIN 最慢 .. 4: MAX 最快; 决定释放时间常数, LOG/LIN 各一套值)
  kReleaseMode,   // 释放回落模式 (0: 对数域单极点, 1: 匀速 dB 速率)
  kRange,         // 频谱显示动态范围下限 (dBFS)
  kRes,           // FFT 分辨率档位 (2048/4096/8192)
  kLfRes,         // PBT 低频分辨率档位 (低/中/高 = 40/20/10 Hz)
  kMode,          // 分析引擎模式 (0: STFT, 1: VQT, 2: PBT, 3: RTA)
  kChannelMode,   // 声道显示模式 (0: PWR, 1: LR, 2: SUM)
  kLevelMode,     // 电平表模式 (L/R 条: 0: dBTP, 1: dBFS+RMS; 独立 VU 表常驻)
  kLevelHold,     // 峰值保持时长档位 (0: 0.5s, 1: 2s, 2: ∞ 无限保持)
  kLevelHoldOn,   // 峰值保持开关 (频谱 hold 曲线与电平表 hold 亮线共用; 默认关闭, 用户状态持久化于全局设置文件)
  kFreeze,        // 冻结开关 (0: 实时, 1: FREEZE 定格; 冻结中切换引擎用新算法重算冻结音频)
  kSlopeFFT,      // 频谱斜率档位索引, FFT 引擎独立保存 (0/3/4.5 dB/oct)
  kSlopeVQT,      // 频谱斜率档位索引, VQT 引擎独立保存 (-3/0/1.5 dB/oct)
  kSlopePBT,      // 频谱斜率档位索引, PBT 引擎独立保存 (-3/0/1.5 dB/oct)
  kSlopeRTA,      // 频谱斜率档位索引, RTA 引擎独立保存 (-3/0/1.5 dB/oct)
  kRtaOctave,     // RTA 分数倍频程档位 (0: 1/6, 1: 1/12, 2: 1/24)
  kFFTWindow,     // STFT 窗函数档位 (0: Hann/锐利, 1: BH5 5阶Blackman-Harris/纯净)
  kWindowVQT,     // VQT 窗函数档位 (0: Hann, 1: BH5), 与 STFT 独立保存
  kVQTGamma,      // VQT 低频带宽下限 γ (Hz): bw = fc/q + γ (低/中/高 = 20/10/5, 越小越精细)
  kLoudPreset,    // 响度目标预设档位 (0: -9, 1: -14, 2: -23, 3: -24 LUFS)
  kLoudScale,     // 响度条刻度窗偏移档位 (顶 = 目标+偏移, 1/3 = 目标, 底 = 目标-2×偏移; 0: +9, 1: +18 LU)
  kScopeRange,    // 声像显示范围档位 (极坐标电平半径 dB 底限: 0: -60, 1: -80, 2: -100)
  kNumParams
};

// 窗函数档位: 0 = Hann (锐利), 1 = 5-term Blackman-Harris (纯净, -125 dB)
enum EFFTWindow { kFFTWindowHann = 0, kFFTWindowClean = 1, kNumFFTWindows = 2 };

// 频谱响应速度预设档位 (kSpeed): MIN 最慢 .. MAX 最快。每档的释放时间常数按释放
// 模式取两套值: LOG = 0.2~4s (Pro-Q 五档实测值); LIN = LOG ×2 (0.5~9.6s,
// 匀速档视觉恒速, 拉慢一档体感: 同档位下匀速回落比对数回落慢 ~2 倍, 对齐档位体感)。
// 上升时间常数全部档位固定 0.05s。
enum ESpeed { kSpeedMIN = 0, kSpeedSLOW, kSpeedMED, kSpeedFAST, kSpeedMAX, kNumSpeedOptions = 5 };
constexpr double kSpeedReleaseLog[] = {4.0, 2.0, 1.0, 0.5, 0.2};  // LOG 档释放时间 (s), 按档位索引
constexpr double kSpeedReleaseLin[] = {9.6, 4.8, 2.4, 1.2, 0.5}; // LIN 档释放时间 (s) = LOG ×2
constexpr double kSpeedAttackSec = 0.05;                          // 固定上升时间常数 (s)

using ParamSnapshot = std::array<double, kNumParams>;

// 算法档位可选值列表（参数实际存储对应的索引; 档序均为低→中→高, Hz 越小精度越高）
constexpr int kResOptions[] = {2048, 4096, 8192};
constexpr int kNumResOptions = 3;
constexpr int kPbtLfResOptions[] = {40, 20, 10};
constexpr int kNumPbtLfResOptions = 3;
constexpr int kRtaOctaveOptions[] = {6, 12, 24};
constexpr int kNumRtaOctaveOptions = 3;
constexpr int kVQTGammaOptions[] = {20, 10, 5};
constexpr int kNumVQTGammaOptions = 3;

// 峰值保持时长档位 (kLevelHold 存索引)。持久档用 1e9s 表示: LevelMeter 的超时逻辑
// (holdT >= holdSec 才回落) 永不触发, 即无限保持, 且 holdSec > 0 的"画 hold 线"判定照常成立。
constexpr double kHoldTimeSecs[] = {0.5, 2.0, 1e9};
constexpr int kNumHoldTimeOptions = 3;

// 响度目标预设 (LUFS): -9 / -14 流媒体 / -23 EBU R128 广播 / -24
constexpr double kLoudTargets[] = {-9.0, -14.0, -23.0, -24.0};
constexpr int kNumLoudPresets = 4;

// 响度条刻度窗偏移 (kLoudScale 存索引, LU): M/S/I 条竖向刻度以目标为锚 ——
// 1/3 高度处 = 目标值, 顶部 = 目标+偏移, 底部 = 目标-2×偏移 (窗高 = 3×偏移)。
// 例: 目标 -14、偏移 +18 → 顶 +4 / 1/3 -14 / 底 -50。
constexpr double kLoudScaleOffsets[] = {9.0, 18.0};
constexpr int kNumLoudScaleOptions = 2;

// 声像显示范围档位 (kScopeRange 存索引): 极坐标电平扇形的半径 dB 底限
constexpr double kScopeRangeDb[] = {-60.0, -80.0, -100.0};
constexpr int kNumScopeRangeOptions = 3;

// 频谱斜率档位 (kSlopeFFT/kSlopeVQT/kSlopePBT/kSlopeRTA 存索引, 各引擎独立):
// 显示域每 band 施加 S·log2(f/f_pivot) dB 的倾斜。FFT 按 bin 显示白噪天生平直,
// 逐 band 能量积分显示 (VQT/PBT/RTA) 白噪天生 +3 dB/oct, 故两组档值相差 -3,
// 使同一信号的视觉斜率在两种显示下一致 (如粉噪在 FFT|3 与 VQT|0 下都平直)。
constexpr double kSlopeDbFFT[] = {0.0, 3.0, 4.5};
constexpr double kSlopeDbLog[] = {-3.0, 0.0, 1.5};
constexpr int kNumSlopeOptions = 3;

// VQT 固定档位: BPO 固定 24 (VQT γ 由 GAMMA 按钮调节)
constexpr int kVQTBpo = 24;

// 分析引擎模式 (四态: STFT, VQT, PBT, RTA)
enum EAnalyzerMode {
  kModeFFT = 0,
  kModeVQT = 1,
  kModePBT = 2,
  kModeRTA = 3,
  kNumModes = 4
};

// 声道显示模式 (三态: PWR(Merge) / LR / SUM(Merge))
enum EChannelMode { kChanModePWR = 0, kChanModeLR = 1, kChanModeSUM = 2, kNumChanModes = 3 };

// 电平表模式 (L/R 条): dBTP / dBFS+RMS; 独立 VU 表常驻显示, 不参与模式切换
enum ELevelMode { kLevelModeDBTP = 0, kLevelModeDBFS = 1, kNumLevelModes = 2 };

// UI 控件消息标签
enum EControlTags { kCtrlTagPad = 100, kCtrlTagCpu = 101, kCtrlTagLegend = 102, kCtrlTagLoudness = 103, kCtrlTagScope = 104 };
