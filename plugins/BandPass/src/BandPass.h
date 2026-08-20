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
  kGainL,
  // RIGHT 模块
  kFreqR,
  kBwR,
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
  class FilterNodePad;
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
  FilterNodePad*  mPadL = nullptr;
  FilterNodePad*  mPadR = nullptr;

  // 预设 / undo / redo
  std::array<ParamSnapshot, kNumPresets> mPresets;
  int mCurrentPreset = 0;
  std::deque<ParamSnapshot> mUndoStack, mRedoStack;

  void SyncParamsToCore();
  void UpdateParamDisplays();
  void UpdatePads();

  ParamSnapshot Snapshot() const;
  void ApplySnapshot(const ParamSnapshot& s);
  void PushUndo();
  void Undo();
  void Redo();
  void SaveToSlot(int idx);
  void LoadSlot(int idx);

  // 声像区: 数值拷贝/交换 (click 触发, 非开关)
  void CopyLtoR();   // 把当前 L 的数值发送给 R
  void CopyRtoL();   // 把当前 R 的数值发送给 L
  void FlipLR();     // 一次性互换 L / R

  static void FormatFreq(char* buf, int n, double hz);
};
