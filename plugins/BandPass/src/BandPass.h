#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "dsp/BandPassCore.h"
#include "Strings.h"
#include "IGraphicsPopupMenu.h"

#include <array>
#include <deque>
#include <functional>
#include <utility>
#include <vector>

enum EParams
{
  kFreqL = 0,
  kBwL,
  kGainL,
  kFreqR,
  kBwR,
  kGainR,
  kLink,
  kMix,
  kAgOn,
  kAgAmount,
  kAgRate,
  kNumParams
};

using ParamSnapshot = std::array<double, kNumParams>;

constexpr int kNumPresets = 24;
constexpr int kNumQuick   = 8;
constexpr int kNumBottom  = 8;

using namespace iplug;
using namespace igraphics;

namespace iplug { namespace igraphics {
  class IControl;
  class IVSliderControl;
  class FilterNodePad;
  class BandRangeSlider;
  class PresetSlotControl;
} }

class ORMBandPass final : public Plugin
{
public:
  ORMBandPass(const InstanceInfo& info);

#if IPLUG_DSP
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
  void OnReset() override;
  void OnParamChange(int paramIdx, EParamSource source, int sampleOffset) override;
  void OnParamChangeUI(int paramIdx, EParamSource source) override;
#endif
  void OnIdle() override;

private:
  orm::BandPassCore mCore;
  orm::ParamMailbox<orm::BandPassCore::Params> mParamMailbox;

  FilterNodePad*  mPadL = nullptr;
  FilterNodePad*  mPadR = nullptr;
  BandRangeSlider* mBandL = nullptr;
  BandRangeSlider* mBandR = nullptr;

  std::array<ParamSnapshot, kNumPresets> mPresets;
  std::array<int, kNumPresets> mSlotNumber;
  ParamSnapshot mDefaultSnapshot {};
  int mCurrentPreset = 0;
  double mFadePos = 0.0;
  IVSliderControl* mFadeSlider = nullptr;
  double mFadeTime = 0.0;
  bool mFading = false;
  bool mInFadeApply = false;
  double mFadeStartTime = 0.0;
  ParamSnapshot mFadeFrom {};
  ParamSnapshot mFadeTo {};
  PresetSlotControl* mSlotButtons[kNumPresets] = {};
  int mDragSourceSlot = -1;
  int mDragTargetSlot = -1;
  WDL_String mDialogFileName, mDialogPath;
  std::deque<ParamSnapshot> mUndoStack, mRedoStack;

  ParamSnapshot mStableSnapshot {};
  double mLastUIChangeTime = -1e9;
  bool mGesturePending = false;

  int mThemeMode = 0;
  int mThemeColorIdx = 0;
  std::vector<std::pair<int, std::function<void(const char*)>>> mTextBindings;
  std::vector<std::pair<IControl*, int>> mTooltipBindings;
  IPopupMenu mSettingsMenu;
  IPopupMenu* mLangMenuPtr = nullptr;
  IPopupMenu* mColorMenuPtr = nullptr;

  orm::BandPassCore::Params CollectParams() const;
  void PublishParamsToCore();

  void SetParamFromEditor(int idx, double value);
  void RefreshAfterEdit();
  void UpdatePads();

  void EditCorner(int kFreq, int kBw, int cornerId, double value);
  void EditBand(int kFreq, int kBw, double lowNorm, double highNorm);
  void ClampAndSet(int kFreq, int kBw, double centerHz, double bw);

  ParamSnapshot Snapshot() const;
  void ApplySnapshot(const ParamSnapshot& s);
  void PushUndo();
  void PushUndoSnapshot(const ParamSnapshot& s);
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
  void WritePresetFileTo(const std::string& path, std::string& err);
  void ReadPresetFileFrom(const std::string& path, std::string& err);
  void OnDragBegin(int src);
  void OnDragMove(float x, float y);
  void OnDragDrop(int src, float x, float y);
  int HitTestSlot(float x, float y);
  void CopyLtoR();
  void CopyRtoL();
  void FlipLR();

  void MirrorLinkedParams(int paramIdx);

  ParamSnapshot InterpolatePresets(double pos);
  ParamSnapshot MixSnapshots(const ParamSnapshot& a, const ParamSnapshot& b, double t) const;
  void OnFadeDrag(double normalizedPos);
  void StartFade(const ParamSnapshot& to);
  void ApplyLanguage();
  void ApplyTooltips();
  void OpenSettingsMenu(IControl* caller, float x, float y);
  void OnSettingsMenuChoice(IPopupMenu* menu);
};
