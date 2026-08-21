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
  // 按来源分流: kHost (音频线程) 直写核心; 其余来源 (编辑器线程) 走信箱
  void OnParamChange(int paramIdx, EParamSource source, int sampleOffset) override;
  void OnParamChangeUI(int paramIdx, EParamSource source) override;
#endif
  void OnIdle() override;

private:
  grm::BandPassCore mCore;
  // 参数信箱: 编辑器线程 publish -> 音频线程 consume, 消除跨线程写 mCore 的竞争
  grm::ParamMailbox<grm::BandPassCore::Params> mParamMailbox;

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
  // 拖动手势检测: iPlug2 无手势回调, 以 kUI 参数事件间隔是否超过阈值判断新手势,
  // 新手势开始时推入"手势前"的稳定快照
  ParamSnapshot mStableSnapshot {};
  double mLastUIChangeTime = -1e9;   // steady_clock 秒
  bool mGesturePending = false;      // 有拖动事件待固化为稳定快照

  grm::BandPassCore::Params CollectParams() const;  // 从参数对象打包 DSP 参数
  void PublishParamsToCore();                       // 打包并发布到信箱 (仅编辑器线程调用)

  // 编辑器侧改单个参数: 写值 + 通知宿主 + 发布到 DSP (替代裸 GetParam()->Set)
  void SetParamFromEditor(int idx, double value);
  // 批量改参后刷新 UI: 所有绑定控件从参数回读最新值 (滑条/旋钮/开关/pad)
  void RefreshAfterEdit();
  void UpdateParamDisplays();
  void UpdatePads();

  ParamSnapshot Snapshot() const;
  void ApplySnapshot(const ParamSnapshot& s);
  void PushUndo();
  void PushUndoSnapshot(const ParamSnapshot& s);  // 带去重的入栈
  void MaybePushGestureUndo();                    // kUI 拖动事件: 判断是否新手势并入栈
  void MarkStateStable();                         // 批量操作后刷新"稳定快照"
  void Undo();
  void Redo();
  void SaveToSlot(int idx);
  void LoadSlot(int idx);

  // 声像区: 数值拷贝/交换 (click 触发, 非开关)
  void CopyLtoR();   // 把当前 L 的数值发送给 R
  void CopyRtoL();   // 把当前 R 的数值发送给 L
  void FlipLR();     // 一次性互换 L / R

  // LINK 开启时, 拖动一个通道的 freq/bw/gain, 另一通道参数实时跟随
  void MirrorLinkedParams(int paramIdx);

  // 预设 morph 条: 在 Q1..Q8 槽位之间对全部参数线性插值
  ParamSnapshot InterpolatePresets(double pos);  // pos: 0..kNumQuick-1
  void OnMorphDrag(double normalizedPos);

  static void FormatFreq(char* buf, int n, double hz);
};
