#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "Params.h"
#include "dsp/SpectrumSTFT.h"
#include "dsp/VQTAnalyzer.h"
#include "dsp/PAZAnalyzer.h"
#include "dsp/MultirateFFTAnalyzer.h"
#include "dsp/LevelMeter.h"
#if ORM_ENABLE_TEST_GEN
#include "dsp/TestSignalGenerator.h"
#endif
#include "Strings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace iplug;
using namespace igraphics;

namespace iplug {
namespace igraphics {
class IControl;
class IVButtonControl;
class SpectrumPad;
class ORMSlider;
class SettingsPanelControl;
class CpuMeterControl;
class FlatToggleControl;
class FlatCycleButton;
} // namespace igraphics
} // namespace iplug

class ORMAnalyzer final : public Plugin {
public:
  ORMAnalyzer(const InstanceInfo &info);

#if IPLUG_DSP
  void ProcessBlock(sample **inputs, sample **outputs, int nFrames) override;
  void OnReset() override;
  void OnParamChange(int paramIdx, EParamSource source, int sampleOffset) override;
  void OnParamChangeUI(int paramIdx, EParamSource source) override;
#endif
  void OnIdle() override;
  void OnUIOpen() override;
  void OnUIClose() override;
  void OnParentWindowResize(int width, int height) override;
  bool ConstrainEditorResize(int &w, int &h) const override;

private:
  std::atomic<bool> mUIOpen{false};

  SpectrumSTFT<3> mSpectrum;
  VQTAnalyzer<3> mVQT;
  PAZAnalyzer<3> mPAZ;
  MultirateFFTAnalyzer<3> mMRFFT;

  static constexpr int kMaxBlock = 16384;
  std::array<sample, kMaxBlock> mSpecInL{};
  std::array<sample, kMaxBlock> mSpecInR{};
  std::array<sample, kMaxBlock> mSpecInM{};

  // 时域峰值 (音频线程按 block 计算并做 attack/release 平滑, UI 线程 OnIdle 读取转发给 Gain 条)
  std::atomic<float> mPeakL{0.f};
  std::atomic<float> mPeakR{0.f};

  // 电平表输出快照 (音频线程写入, UI 线程 OnIdle 读取)
  LevelMeter mLevelMeter;
  std::atomic<float> mTrueL{0.f}, mTrueR{0.f};
  std::atomic<float> mRmsL{0.f}, mRmsR{0.f};
  std::atomic<float> mVuL{0.f}, mVuR{0.f};
  std::atomic<float> mHoldL{0.f}, mHoldR{0.f};
  std::atomic<int> mOverL{0}, mOverR{0};
  std::atomic<float> mHoldSec{2.f};
  std::atomic<bool> mLevelResetFlag{false};     // UI 线程置位, 音频线程下一 block 清除 hold/over
  std::atomic<bool> mLevelResetHoldFlag{false}; // UI 线程置位, 音频线程下一 block 清除峰值保持 (模式切换)
  std::atomic<double> mLevelSetSR{-1.0};        // UI 线程置位, 音频线程下一 block 执行 SetSampleRate+Reset (-1=无请求)

  // 频谱配置缓存（用于在 OnIdle 中防抖去重）
  double mSentSampleRate = 0.0;
  int mSentFFTSize = 0;
  double mSentRelease = -1.0;
  double mSentRange = -1.0;
  double mSentAttack = -1.0;
  double mSentLfRes = -1.0;
  double mSentSlope = -1e9; // 当前模式生效斜率 (dB/oct), 用于 OnIdle 增量去重
  int mSentChanMode = -1; // 存储三态值 (0=LR,1=PWR,2=SUM), 用于 OnIdle 增量去重

  SpectrumPad *mSpectrumPad = nullptr;
  FlatCycleButton *mResBtn = nullptr;      // STFT 分辨率循环按钮 (LOW/MID/HIGH)
  FlatCycleButton *mWindowBtn = nullptr;   // 窗函数循环按钮 (SHARP/CLEAN, STFT 与 VQT 各自独立档位, 按模式改绑参数)
  FlatCycleButton *mPazLfResBtn = nullptr; // PAZ 低频分辨率循环按钮 (40/20/10 Hz)
  FlatCycleButton *mPyramidBtn = nullptr;  // VQT 金字塔算法循环按钮 (LIN / MIN)
  FlatCycleButton *mRangeBtn = nullptr;    // 动态范围循环按钮 (刻度底部 80/100/120)
  FlatCycleButton *mSlopeBtn = nullptr;    // 频谱斜率循环按钮 (刻度底部左缘, 档值随引擎)
  ORMSlider *mAttackSlider = nullptr;
  ORMSlider *mReleaseSlider = nullptr;
  CpuMeterControl *mCpuMeter = nullptr;
  FlatCycleButton *mModeBtn = nullptr;
  FlatCycleButton *mChanModeBtn = nullptr;
  FlatCycleButton *mLevelModeBtn = nullptr;
  IVButtonControl *mLevelResetBtn = nullptr;
  FlatToggleControl *mLevelHoldBtn = nullptr;   // 峰值保持开关 (HOLD, 反色开关样式)
  FlatCycleButton *mLevelHoldTimeBtn = nullptr; // 峰值保持时长循环按钮 (0.5s / 2s / ∞)
  FlatToggleControl *mFreezeBtn = nullptr; // 冻结开关 (FREEZE, 反色开关样式, 同 HOLD)

  int mSentMode = -1;
  int mSentWindowFFT = -1; // STFT 窗函数档位 (kFFTWindow), OnIdle 增量去重
  int mSentWindowVQT = -1; // VQT 窗函数档位 (kWindowVQT), OnIdle 增量去重

  // ── Freeze (冻结/保持), 确定性回放方案 ─────────────────────────────
  // 音频线程把最近输入滚环记录进 mFreezeRing (freeze 后停止写入, 即冻结时刻快照),
  // 并逐块发布活跃引擎的输入侧 hop 相位 mEngineHopPhase (非活跃引擎保持停用前值)。
  // UI 线程在冻结中跳过引擎消费 (画面定格); 切换引擎/PAZ 算法/同一算法内档位时
  // StartFreezeReplay: 复位引擎运行态 → pad 平滑缓冲清零 → 把冻结环按实时帧格
  // (hop 相位对齐, 最新回放帧 = 冻结瞬间实时显示的最后一帧) 逐帧回放, 每帧经
  // kUpdateMessage 走 pad 的攻击/释放平滑 (与实时同一弹道学)。
  // display(cfg) = replay(ring, cfg) 为纯函数: 冻结中切走再切回, 画面逐字节一致。
  static constexpr int kFreezeRingLen = 1 << 18;   // 262144 样本 ≈5.46s @48k:
                                                   // 覆盖 PAZ-IIR 最低频带 5τ (10Hz 档 τ≈0.8s)
                                                   // 与最长释放 (1s) 弹道的收敛 (96k 下约 2.7s, 深带欠收敛)
  std::array<std::array<float, kFreezeRingLen>, 3> mFreezeRing{}; // [0]=L [1]=R [2]=M
  std::atomic<int> mFreezeRingPos{0};              // 下一个写入位置 (= 最旧样本)
  std::array<std::atomic<int>, kNumModes> mEngineHopPhase{}; // 各引擎输入侧 hop 相位 (音频线程发布)
  bool mFreezeOn = false;                          // 冻结激活边沿/状态 (UI 线程)
  // 冻结中已重算的档位快照 (参数档位索引, -1 = 未同步); 变化时用冻结缓冲重算直显
  int mFreezeRes = -1;
  int mFreezeWindowFFT = -1; // STFT 窗函数档位快照 (冻结中重算去重)
  int mFreezeWindowVQT = -1; // VQT 窗函数档位快照 (冻结中重算去重)
  int mFreezeLf = -1;
  int mFreezePyramid = -1;

  // 冻结回放 (UI 线程, 定义见 Analyzer.cpp)。回放分 tick 泵送避免长 UI 卡顿;
  // 回放期间再次切换配置 → StartFreezeReplay 重启 (复位后重放, 确定性不变);
  // 解冻时中止, 引擎带部分预热历史续接实时 (仅分析侧, 无声学影响)。
  static constexpr int kReplayFramesPerTick = 16;  // 每 OnIdle 泵送帧数 (16 帧 ≈0.34s 音频)
  int mReplayMode = -1;                            // 回放中的引擎模式 (kMode*), -1 = 空闲
  int mReplayFrame = 0;                            // 下一待回放帧 (0 = 最旧)
  int mReplayNFrames = 0;                          // 总帧数 (整圈 255/256 帧)
  int mReplayLastStart = 0;                        // 最新帧 (= 实时定格帧) 的环内样本起点
  void StartFreezeReplay();                        // 置位回放: 复位引擎 + 清 pad + 计算帧格
  void PumpFreezeReplay();                         // OnIdle 每 tick 回放一批帧 (kUpdateMessage)

  // ── 内置测试信号发生器 (开发者工具, ORM_ENABLE_TEST_GEN) ────────────
  // UI 线程写、音频线程读: 全部用原子标量, 不做跨线程共享对象。
  // 发生器状态只能在音频线程推进 (Fill), 因此样本索引与 Freeze 环形缓冲天然同步。
#if ORM_ENABLE_TEST_GEN
  orm::TestSignalGenerator mTestGen;
  std::atomic<int> mGenType{orm::kGenOff};
  std::atomic<float> mGenFreq{1000.f};
  std::atomic<float> mGenLevel{-12.f};
  std::atomic<bool> mGenHold{false};     // 冻结时锁相位 (不推进样本索引)
  std::atomic<bool> mGenToOutput{false}; // 路由生成信号到输出 (默认关: 白噪/脉冲直送监听很危险)
  std::atomic<bool> mGenRestartReq{false};
  std::atomic<int> mGenSeedReq{0}; // 非 0 = 请求换种子
  // 频率/电平拖动的磁盘写入防抖 (仅 UI 线程访问 mGenSaveTp)
  std::atomic<bool> mGenSavePending{false};
  std::chrono::steady_clock::time_point mGenSaveTp{};
#endif

  // CPU 占用率统计（音频线程处理耗时 + UI 分析耗时，归一化为单核百分比）
  double mCpuAudio = 0.0;
  double mCpuUi = 0.0;
  double mCpuPct = 0.0;
  double mUiWorkMs = 0.0;
  double mUiWinMs = 0.0;
  std::chrono::steady_clock::time_point mLastIdleTp = std::chrono::steady_clock::now();

  ParamSnapshot mDefaultSnapshot{};
  std::deque<ParamSnapshot> mUndoStack, mRedoStack;
  WDL_String mDialogFileName, mDialogPath;

  ParamSnapshot mStableSnapshot{};
  double mLastUIChangeTime = -1e9;
  bool mGesturePending = false;

  int mThemeMode = 0;
  SettingsPanelControl *mSettingsPanel = nullptr;
  std::vector<std::pair<int, std::function<void(const char *)>>> mTextBindings;
  std::vector<std::pair<IControl *, int>> mTooltipBindings;

  void SendSpectrumConfig();
  void SendResetToPad(); // 引擎配置重建 (γ/BPO/模式) 后通知 pad 清空平滑缓冲, 显示重新加载

  // 分析档位 -> 实际值 (参数存档位索引)
  int CurrentFFTSize() const {
    const int idx = (int)std::clamp(GetParam(kRes)->Value(), 0.0, (double)kNumResOptions - 1);
    return kResOptions[idx];
  }
  int CurrentFFTWindow() const {
    const int idx = (int)std::clamp(std::lround(GetParam(kFFTWindow)->Value()), 0L, (long)kNumFFTWindows - 1);
    return idx;
  }
  int CurrentVQTWindow() const {
    const int idx = (int)std::clamp(std::lround(GetParam(kWindowVQT)->Value()), 0L, (long)kNumFFTWindows - 1);
    return idx;
  }
  int CurrentPazLfRes() const {
    const int idx = (int)std::clamp(GetParam(kLfRes)->Value(), 0.0, (double)kNumPazLfResOptions - 1);
    return kPazLfResOptions[idx];
  }
  // 频谱显示范围 (刻度底部 dB): 离散三档 80/100/120, 由 Range 循环按钮切换。
  // 用 lround 取档位 (与按钮显示取整一致), 避免宿主旧状态恢复出档位间值 (如 0.5) 时
  // 截断到低档导致按钮与频谱不同步。
  float CurrentRangeDb() const {
    const int idx = (int)std::clamp(std::lround(GetParam(kRange)->Value()), 0L, 2L);
    static constexpr float kRangeDb[3] = {80.f, 100.f, 120.f};
    return kRangeDb[idx];
  }
  // 峰值保持有效时长 (s): 开关关闭 -> 0 (LevelMeter 不保持, UI 不画 hold 线/曲线);
  // 开启 -> kHoldTimeSecs 档位值 (∞ 档为 1e9, 超时永不触发 = 无限保持)。
  double CurrentHoldSec() const {
    if (GetParam(kLevelHoldOn)->Value() < 0.5)
      return 0.0;
    const int idx = (int)std::clamp(std::lround(GetParam(kLevelHold)->Value()), 0L, (long)kNumHoldTimeOptions - 1);
    return kHoldTimeSecs[idx];
  }
  // 当前模式生效的斜率值 (dB/oct): 各引擎档位独立保存 (kSlopeFFT + 模式连续排列);
  // FFT 用 kSlopeDbFFT 档值, 逐 band 引擎 (VQT/PAZ/MR-FFT) 用 kSlopeDbLog 档值。
  // 两组相差 -3 dB/oct: 逐 band 能量积分显示白噪天生 +3 dB/oct (FFT 按 bin 显示天生平直),
  // 使同一信号的视觉斜率跨显示一致 (如粉噪在 FFT|3 与 VQT|0 下都平直)。
  double EffectiveSlopeDb() const {
    const int mode = (int)std::clamp(GetParam(kMode)->Value(), 0.0, (double)kNumModes - 1);
    const int idx = (int)std::clamp(GetParam(kSlopeFFT + mode)->Value(), 0.0, (double)kNumSlopeOptions - 1);
    return (mode == kModeFFT) ? kSlopeDbFFT[idx] : kSlopeDbLog[idx];
  }
  void SendVQTBandFreqs();
  void SendPAZBandFreqs();
  void SendMRFFTBandFreqs();

  void SetParamFromEditor(int idx, double value);
  void RefreshAfterEdit();

  ParamSnapshot Snapshot() const;
  void ApplySnapshot(const ParamSnapshot &s);
  void PushUndo();
  void PushUndoSnapshot(const ParamSnapshot &s);
  void MaybePushGestureUndo();
  void MarkStateStable();
  void Undo();
  void Redo();
  void SaveFile();
  void LoadFile();
  void WriteStateFileTo(const std::string &path, std::string &err);
  void ReadStateFileFrom(const std::string &path, std::string &err);

  void ApplyLanguage();
  void ApplyTooltips();
  void ApplyTheme();
  void RefreshThemeColors();
  void ToggleSettingsPanel();
  void SaveSettingsToDisk();
};
