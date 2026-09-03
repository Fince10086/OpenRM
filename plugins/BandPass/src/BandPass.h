#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "Params.h"
#include "dsp/BandPassCore.h"
#include "dsp/SpectrumSTFT.h"
#include "Strings.h"
#include "IGraphicsPopupMenu.h"

#include <array>
#include <atomic>
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
class IVSliderControl;
class FilterNodePad;
class BandRangeSlider;
class PresetSlotControl;
class ORMSlider;
class GainSlider;
class RandomColorPickerControl;
class SettingsPanelControl;
} // namespace igraphics
} // namespace iplug

class ORMBandPass final : public Plugin {
public:
  ORMBandPass(const InstanceInfo &info);

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
  orm::BandPassCore mCore;
  orm::ParamMailbox<orm::BandPassCore::Params> mParamMailbox;
  orm::ParamMailbox<orm::BandPassCore::RandomDeltas> mRandomDeltaMailbox;
  orm::BandPassCore::RandomDeltas mRandomDeltas;

  SpectrumSTFT<2> mSpectrumL;
  SpectrumSTFT<2> mSpectrumR;

  std::atomic<int> mObservedNOuts{2};
  bool mMonoDisplay = false;

  static constexpr int kMaxBlock = 16384;
  std::array<sample, kMaxBlock> mSpecInL{};
  std::array<sample, kMaxBlock> mSpecInR{};
  std::array<sample, kMaxBlock> mWetL{};
  std::array<sample, kMaxBlock> mWetR{};
  std::array<sample, kMaxBlock> mMonoIn{};

  double mSentSampleRate = 0.0;
  int mSentFFTSize = 0;

  FilterNodePad *mPadL = nullptr;
  FilterNodePad *mPadR = nullptr;
  BandRangeSlider *mBandL = nullptr;
  BandRangeSlider *mBandR = nullptr;
  ORMSlider *mMixSlider = nullptr;
  GainSlider *mGainSliderL = nullptr;
  GainSlider *mGainSliderR = nullptr;
  ORMSlider *mRandomRangeSlider[4] = {};
  ORMSlider *mRandomSpeedSlider[4] = {};
  RandomColorPickerControl *mRandomPicker = nullptr;
  int mRandomSelColor = 0;

  std::array<ParamSnapshot, kNumPresets> mPresets;
  std::array<int, kNumPresets> mSlotNumber;
  ParamSnapshot mDefaultSnapshot{};
  int mCurrentPreset = 0;
  double mFadePos = 0.0;
  IVSliderControl *mFadeSlider = nullptr;
  double mFadeTime = 0.25;
  bool mFading = false;
  bool mInFadeApply = false;
  double mFadeStartTime = 0.0;
  ParamSnapshot mFadeFrom{};
  ParamSnapshot mFadeTo{};
  PresetSlotControl *mSlotButtons[kNumPresets] = {};
  int mDragSourceSlot = -1;
  int mDragTargetSlot = -1;
  WDL_String mDialogFileName, mDialogPath;
  std::deque<ParamSnapshot> mUndoStack, mRedoStack;

  ParamSnapshot mStableSnapshot{};
  double mLastUIChangeTime = -1e9;
  bool mGesturePending = false;

  int mThemeMode = 0;
  SettingsPanelControl *mSettingsPanel = nullptr;
  std::vector<std::pair<int, std::function<void(const char *)>>> mTextBindings;
  std::vector<std::pair<IControl *, int>> mTooltipBindings;

  orm::BandPassCore::Params CollectParams() const;
  void PublishParamsToCore();
  void SendSpectrumConfig();

  void SetParamFromEditor(int idx, double value);
  void RefreshAfterEdit();
  void UpdatePads();

  void EditCorner(int kFreq, int kBw, int cornerId, double value);
  void EditBand(int kFreq, int kBw, double lowNorm, double highNorm);
  void ClampAndSet(int kFreq, int kBw, double centerHz, double bw);
  void SetSlopeFromMenu(int slopeParamIdx, int slopeDb);
  void TogglePass(int passIdx);

  ParamSnapshot Snapshot() const;
  void ApplySnapshot(const ParamSnapshot &s);
  void PushUndo();
  void PushUndoSnapshot(const ParamSnapshot &s);
  void MaybePushGestureUndo();
  void MarkStateStable();
  void Undo();
  void Redo();
  void SaveToSlot(int idx);
  void LoadSlot(int idx);
  void RestoreDefault(int idx);
  void SwapSlots(int posA, int posB);
  void RefreshSlotLabels();
  void SaveFile();
  void LoadFile();
  void WritePresetFileTo(const std::string &path, std::string &err);
  void ReadPresetFileFrom(const std::string &path, std::string &err);
  void OnDragBegin(int src);
  void OnDragMove(float x, float y);
  void OnDragDrop(int src, float x, float y);
  int HitTestSlot(float x, float y);
  void CopyLtoR();
  void CopyRtoL();
  void FlipLR();
  void ApplyLtoRParams();
  void ApplyRtoLParams();

  void MirrorLinkedParams(int paramIdx);

  ParamSnapshot InterpolatePresets(double pos);
  ParamSnapshot MixSnapshots(const ParamSnapshot &a, const ParamSnapshot &b, double t) const;
  void OnFadeDrag(double normalizedPos);
  void StartFade(const ParamSnapshot &to);

  void ToggleRandomMap(int enableParamIdx);
  void SetRandomMapColor(int colorParamIdx, int colorIdx);
  void SetRandomSelectedColor(int colorIdx);
  void UpdateRandomMaps();
  void RandomDisplayPush();
  void ApplyMonoDisplay(bool mono);
  void ApplyLanguage();
  void ApplyTooltips();
  void ApplyTheme();
  void RefreshThemeColors();
  void ToggleSettingsPanel();
  void SaveSettingsToDisk();
};
