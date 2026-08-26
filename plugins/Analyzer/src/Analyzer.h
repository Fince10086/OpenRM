#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "Params.h"
#include "dsp/SpectrumSTFT.h"
#include "dsp/VQTAnalyzer.h"
#include "dsp/PAZAnalyzer.h"
#include "dsp/LevelMeter.h"
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
  double mSentBpo = -1.0;
  int mSentChanMode = -1; // 存储三态值 (0=LR,1=PWR,2=SUM), 用于 OnIdle 增量去重

  SpectrumPad *mSpectrumPad = nullptr;
  ORMSlider *mBpoSlider = nullptr;
  FlatCycleButton *mResBtn = nullptr;      // FFT 分辨率循环按钮 (LOW/MID/HIGH)
  FlatCycleButton *mLfResBtn = nullptr;    // VQT 低频分辨率循环按钮 (LOW/MID/HIGH: 20/10/5 Hz)
  FlatCycleButton *mPazLfResBtn = nullptr; // PAZ 低频分辨率循环按钮 (40/20/10 Hz)
  FlatCycleButton *mRangeBtn = nullptr;    // 动态范围循环按钮 (刻度底部 80/100/120)
  ORMSlider *mAttackSlider = nullptr;
  ORMSlider *mReleaseSlider = nullptr;
  CpuMeterControl *mCpuMeter = nullptr;
  FlatCycleButton *mModeBtn = nullptr;
  FlatCycleButton *mChanModeBtn = nullptr;
  FlatCycleButton *mLevelModeBtn = nullptr;
  IVButtonControl *mLevelResetBtn = nullptr;
  ORMSlider *mLevelHoldSlider = nullptr;

  int mSentMode = -1;

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
  int CurrentLfRes() const {
    const int idx = (int)std::clamp(GetParam(kLfRes)->Value(), 0.0, (double)kNumLfResOptions - 1);
    return kLfResOptions[idx];
  }
  int CurrentPazLfRes() const {
    const int idx = (int)std::clamp(GetParam(kLfRes)->Value(), 0.0, (double)kNumPazLfResOptions - 1);
    return kPazLfResOptions[idx];
  }
  int CurrentBpo() const {
    const int idx = (int)std::clamp(GetParam(kBpo)->Value(), 0.0, (double)kNumBpoOptions - 1);
    return kBpoOptions[idx];
  }
  // 频谱显示范围 (刻度底部 dB): 离散三档 80/100/120, 由 Range 循环按钮切换。
  // 用 lround 取档位 (与按钮显示取整一致), 避免宿主旧状态恢复出档位间值 (如 0.5) 时
  // 截断到低档导致按钮与频谱不同步。
  float CurrentRangeDb() const {
    const int idx = (int)std::clamp(std::lround(GetParam(kRange)->Value()), 0L, 2L);
    static constexpr float kRangeDb[3] = {80.f, 100.f, 120.f};
    return kRangeDb[idx];
  }
  void SendVQTBandFreqs();
  void SendPAZBandFreqs();

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
