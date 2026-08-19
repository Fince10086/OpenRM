#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "dsp/BandPassCore.h"

#include <array>
#include <deque>

// ---------------- 参数枚举 ----------------
enum EParams
{
  // LEFT 模块
  kFreqL = 0,
  kBwL,      // 带宽 octave
  kHpL,      // 高通截止
  kLpL,      // 低通截止
  kPassL,    // 0=BP 1=HP 2=LP
  kGainL,
  // RIGHT 模块
  kFreqR,
  kBwR,
  kHpR,
  kLpR,
  kPassR,
  kGainR,
  // 全局
  kLink,
  kMix,
  // Agitation
  kAgOn,
  kAgAmount,
  kAgRate,
  // 时间区 (预留)
  kTime1,
  kTime2,
  // 声像
  kPanLR,
  kPanRL,
  kPanFlip,
  kNumParams
};

using ParamSnapshot = std::array<double, kNumParams>;

constexpr int kNumPresets  = 16;  // 预设槽位
constexpr int kNumQuick    = 8;   // 快速预设按钮

using namespace iplug;
using namespace igraphics;

namespace iplug { namespace igraphics {
  class IVXYPadControl;
  class ITextControl;
  class IVButtonControl;
} }

class GRMBandPass final : public Plugin
{
public:
  GRMBandPass(const InstanceInfo& info);

#if IPLUG_DSP
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
  void OnReset() override;
  void OnParamChange(int paramIdx) override;
  void OnParamChangeUI(int paramIdx, EParamSource source) override;
#endif

private:
  grm::BandPassCore mCore;

  // UI 控件指针 (供回调更新)
  ITextControl*   mFreqLText = nullptr;
  ITextControl*   mBwLText   = nullptr;
  ITextControl*   mFreqRText = nullptr;
  ITextControl*   mBwRText   = nullptr;
  IVButtonControl* mPassLBtn = nullptr;
  IVButtonControl* mPassRBtn = nullptr;

  // 预设 / undo / redo
  std::array<ParamSnapshot, kNumPresets> mPresets;
  int mCurrentPreset = 0;
  std::deque<ParamSnapshot> mUndoStack, mRedoStack;

  void SyncParamsToCore();
  void UpdateParamDisplays();

  ParamSnapshot Snapshot() const;
  void ApplySnapshot(const ParamSnapshot& s);
  void PushUndo();
  void Undo();
  void Redo();
  void SaveToSlot(int idx);
  void LoadSlot(int idx);
  void CyclePass(IVButtonControl* btn, int paramIdx);
  static const char* PassName(int mode);
  static void FormatFreq(char* buf, int n, double hz);
};
