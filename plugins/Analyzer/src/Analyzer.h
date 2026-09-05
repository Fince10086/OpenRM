#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "Params.h"
#include "dsp/SpectrumSTFT.h"
#include "dsp/VQTAnalyzer.h"
#include "dsp/PBTAnalyzer.h"
#include "dsp/RTAAnalyzer.h"
#include "dsp/LevelMeter.h"
#include "dsp/LoudnessMeter.h"
#include "dsp/StereoScope.h"
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
class StereoFieldControl;
class ORMSlider;
class SettingsPanelControl;
class CpuMeterControl;
class FlatToggleControl;
class FlatCycleButton;
class FlatColorToggleControl;
class OscilloscopeControl;
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
  PBTAnalyzer<3> mPBT;
  RTAAnalyzer<3> mRTA;
  StereoScope<> mScope; // 音频线程只攒 hop 样本包，方位角/弹道计算在 UI 线程

  static constexpr int kMaxBlock = 16384;
  std::array<sample, kMaxBlock> mSpecInL{};
  std::array<sample, kMaxBlock> mSpecInR{};
  std::array<sample, kMaxBlock> mSpecInM{};

  // 时域峰值：音频线程计算，UI 线程读取
  std::atomic<float> mPeakL{0.f};
  std::atomic<float> mPeakR{0.f};

  // 电平表快照（音频线程写，UI 线程读）
  LevelMeter mLevelMeter;
  std::atomic<float> mTrueL{0.f}, mTrueR{0.f};
  std::atomic<float> mRmsL{0.f}, mRmsR{0.f};
  std::atomic<float> mVuL{0.f}, mVuR{0.f};
  std::atomic<float> mVuHoldL{0.f}, mVuHoldR{0.f};
  std::atomic<float> mHoldL{0.f}, mHoldR{0.f};
  std::atomic<float> mPersistL{0.f}, mPersistR{0.f};
  std::atomic<int> mOverL{0}, mOverR{0};
  std::atomic<float> mHoldSec{2.f};
  // 以下 flag 均为 UI 线程置位、音频线程下一 block 消费
  std::atomic<bool> mLevelResetFlag{false};
  std::atomic<bool> mLevelResetHoldFlag{false};
  std::atomic<bool> mLevelResetPersistFlag{false};
  std::atomic<bool> mLevelResetMeterHoldFlag{false};
  std::atomic<bool> mLevelResetOverFlag{false};
  std::atomic<double> mLevelSetSR{-1.0}; // 采样率变更请求，-1=无

  // 响度计快照（同电平表模式）
  LoudnessMeter mLoudness;
  std::atomic<float> mLoudM{0.f}, mLoudS{0.f}, mLoudI{0.f}, mLoudLra{0.f}, mLoudTp{0.f};
  std::atomic<float> mLoudLraMin{0.f}, mLoudLraMax{0.f}; // LRA 直方图 10%/95% 端点
  std::atomic<bool> mLoudIValid{false}, mLoudLraValid{false};
  std::atomic<bool> mLoudResetFlag{false};
  std::atomic<double> mLoudSetSR{-1.0};

  // OnIdle 频谱配置去重缓存
  double mSentSampleRate = 0.0;
  int mSentFFTSize = 0;
  double mSentRelease = -1.0;
  int mSentReleaseMode = -1;
  int mSentSpeed = -1;
  double mSentRange = -1.0;
  double mSentLfRes = -1.0;
  double mSentSlope = -1e9;
  int mSentChanMode = -1;

  SpectrumPad *mSpectrumPad = nullptr;
  StereoFieldControl *mScopeCtrl = nullptr;
  OscilloscopeControl *mOscilloscopeCtrl = nullptr;
  FlatCycleButton *mResBtn = nullptr;
  FlatCycleButton *mWindowBtn = nullptr;
  FlatCycleButton *mPbtLfResBtn = nullptr;
  FlatCycleButton *mGammaBtn = nullptr;
  FlatCycleButton *mRtaOctBtn = nullptr;
  FlatCycleButton *mRangeBtn = nullptr;
  FlatCycleButton *mSlopeBtn = nullptr;
  FlatCycleButton *mReleaseModeBtn = nullptr;
  FlatCycleButton *mSpeedBtn = nullptr;
  CpuMeterControl *mCpuMeter = nullptr;
  FlatCycleButton *mModeBtn = nullptr;
  FlatCycleButton *mChanModeBtn = nullptr;
  FlatCycleButton *mLevelModeBtn = nullptr;
  IVButtonControl *mLevelResetBtn = nullptr;
  FlatToggleControl *mLevelHoldBtn = nullptr;
  FlatCycleButton *mLevelHoldTimeBtn = nullptr;
  FlatToggleControl *mFreezeBtn = nullptr;
  FlatCycleButton *mScopeTrigBtn = nullptr;
  FlatColorToggleControl *mScopeBtnL = nullptr;
  FlatColorToggleControl *mScopeBtnR = nullptr;
  FlatColorToggleControl *mScopeBtnM = nullptr;
  FlatCycleButton *mScopeTimeBtn = nullptr;
  FlatCycleButton *mScopeZoomBtn = nullptr;
  void SyncScopeChanMask(); // L/R/M 开关状态 -> 示波器声道掩码

  int mSentMode = -1;
  int mSentWindowFFT = -1;
  int mSentWindowVQT = -1;
  int mSentRtaOct = -1;

  // Freeze：音频线程把最近输入滚环记录，冻结后停止写入；UI 线程用冻结缓冲在新配置下确定性回放（10秒缓冲）
  static constexpr int kFreezeRingLen = 1 << 19; // ≈10.92s @48k，覆盖最长释放弹道收敛与长时基示波
  std::array<std::array<float, kFreezeRingLen>, 3> mFreezeRing{}; // [0]=L [1]=R [2]=M
  std::atomic<int> mFreezeRingPos{0};
  std::array<std::atomic<int>, kNumModes> mEngineHopPhase{}; // 各引擎输入侧 hop 相位
  bool mFreezeOn = false;
  // 冻结中已重算的档位快照，-1=未同步
  int mFreezeRes = -1;
  int mFreezeWindowFFT = -1;
  int mFreezeWindowVQT = -1;
  int mFreezeLf = -1;
  int mFreezeRtaOct = -1;

  // 冻结回放分 tick 泵送，避免长 UI 卡顿
  static constexpr int kReplayFramesPerTick = 16;
  int mReplayMode = -1; // kMode*，-1=空闲
  int mReplayFrame = 0;
  int mReplayNFrames = 0;
  int mReplayLastStart = 0;
  void StartFreezeReplay();
  void PumpFreezeReplay();

  // 内置测试信号发生器（开发者工具，ORM_ENABLE_TEST_GEN）
  // UI 写、音频读，全部用原子标量
#if ORM_ENABLE_TEST_GEN
  orm::TestSignalGenerator mTestGen;
  std::atomic<int> mGenType{orm::kGenOff};
  std::atomic<float> mGenFreq{1000.f};
  std::atomic<float> mGenLevel{-12.f};
  std::atomic<bool> mGenHold{false};
  std::atomic<bool> mGenToOutput{false};
  std::atomic<bool> mGenRestartReq{false};
  std::atomic<int> mGenSeedReq{0};
  std::atomic<bool> mGenSavePending{false};
  std::chrono::steady_clock::time_point mGenSaveTp{};
#endif

  // CPU 占用率（音频线程 + UI 分析，归一化为单核百分比）
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
  void SendResetToPad(); // 引擎配置重建后通知 pad 清空平滑缓冲

  // 分析档位 -> 实际值
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
  int CurrentPbtLfRes() const {
    const int idx = (int)std::clamp(GetParam(kLfRes)->Value(), 0.0, (double)kNumPbtLfResOptions - 1);
    return kPbtLfResOptions[idx];
  }
    int CurrentVQTGamma() const {
      const int idx = (int)std::clamp(GetParam(kVQTGamma)->Value(), 0.0, (double)kNumVQTGammaOptions - 1);
      return kVQTGammaOptions[idx];
    }
  int CurrentRtaOctave() const {
    const int idx = (int)std::clamp(GetParam(kRtaOctave)->Value(), 0.0, (double)kNumRtaOctaveOptions - 1);
    return idx;
  }
  // 用 lround 取档，避免宿主旧状态恢复出档位间值时截断到低档
  float CurrentRangeDb() const {
    const int idx = (int)std::clamp(std::lround(GetParam(kRange)->Value()), 0L, 2L);
    static constexpr float kRangeDb[3] = {80.f, 100.f, 120.f};
    return kRangeDb[idx];
  }
  // 开关关闭 -> 0（不保持）；开启 -> 档位值（∞ 档为 1e9）
  double CurrentHoldSec() const {
    if (GetParam(kLevelHoldOn)->Value() < 0.5)
      return 0.0;
    const int idx = (int)std::clamp(std::lround(GetParam(kLevelHold)->Value()), 0L, (long)kNumHoldTimeOptions - 1);
    return kHoldTimeSecs[idx];
  }
  static int SlopeParamForMode(int mode) {
    switch (mode) {
      case kModeFFT: return kSlopeFFT;
      case kModeVQT: return kSlopeVQT;
      case kModePBT: return kSlopePBT;
      case kModeRTA: return kSlopeRTA;
      default: return kSlopeFFT;
    }
  }
  // 各引擎斜率档独立保存；FFT 与逐 band 引擎相差 -3 dB/oct，
  // 使同一信号（如粉噪）跨显示模式视觉斜率一致
  double EffectiveSlopeDb() const {
    const int mode = (int)std::clamp(GetParam(kMode)->Value(), 0.0, (double)kNumModes - 1);
    const int paramIdx = SlopeParamForMode(mode);
    const int idx = (int)std::clamp(GetParam(paramIdx)->Value(), 0.0, (double)kNumSlopeOptions - 1);
    return (mode == kModeFFT) ? kSlopeDbFFT[idx] : kSlopeDbLog[idx];
  }
  void SendVQTBandFreqs();
  void SendPBTBandFreqs();
  void SendRTABandFreqs();

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
