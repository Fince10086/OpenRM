#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "Params.h"
#include "dsp/SpectrumSTFT.h"
#include "dsp/CQTAnalyzer.h"
#include "Strings.h"

#include <algorithm>
#include <array>
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
class SpectrumPad;
class ORMSlider;
class SettingsPanelControl;
class CpuMeterControl;
class FlatToggleControl;
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
  void OnUIClose() override;
  void OnParentWindowResize(int width, int height) override;
  bool ConstrainEditorResize(int &w, int &h) const override;

private:
  SpectrumSTFT<2> mSpectrum;
  CQTAnalyzer<2> mCQT;

  static constexpr int kMaxBlock = 16384;
  std::array<sample, kMaxBlock> mSpecInL{};
  std::array<sample, kMaxBlock> mSpecInR{};

  // 频谱配置去重: 仅当采样率/FFT 尺寸/释放/下限/上升时间变化时才向 UI 控件重发 (OnIdle 节流)
  double mSentSampleRate = 0.0;
  int mSentFFTSize = 0;
  double mSentRelease = -1.0;
  double mSentRange = -1.0;
  double mSentAttack = -1.0;

  SpectrumPad *mSpectrumPad = nullptr;
  ORMSlider *mResSlider = nullptr;
  ORMSlider *mRangeSlider = nullptr;
  ORMSlider *mAttackSlider = nullptr;
  ORMSlider *mReleaseSlider = nullptr;
  ORMSlider *mMixSlider = nullptr;
  CpuMeterControl *mCpuMeter = nullptr;
  FlatToggleControl *mModeToggle = nullptr;

  // 分析模式去重: 模式变化时切换滑块参数 (RES<->LF RES) 并重发 pad 配置
  int mSentMode = -1;

  // CPU 占用率: 音频线程在 ProcessBlock 内测量 处理耗时/块时长, 一阶平滑后
  // 由 OnIdle 推送给 UI (0.0 ~ 1.0+, 显示为两位小数, 不带 %)。
  double mCpuPct = 0.0;

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

  // 分析档位 -> 实际值 (参数存档位索引)
  int CurrentFFTSize() const {
    const int idx = (int)std::clamp(GetParam(kRes)->Value(), 0.0, (double)kNumResOptions - 1);
    return kResOptions[idx];
  }
  int CurrentLfRes() const {
    const int idx = (int)std::clamp(GetParam(kLfRes)->Value(), 0.0, (double)kNumLfResOptions - 1);
    return kLfResOptions[idx];
  }
  void SendCQTBandFreqs();
  void UpdateResHeader();

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
