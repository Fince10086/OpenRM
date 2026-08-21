#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "dsp/BandPassCore.h"

#include <array>
#include <deque>
#include <string>
#include <vector>

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

constexpr int kNumPresets = 16;  // 预设槽位
constexpr int kNumQuick   = 8;   // morph 条端点槽位 (预设 1..8)
constexpr int kNumRecent  = 8;   // 底部"最近使用"槽位数

// 单个预设: 名称 + 11 个参数
struct Preset
{
  std::string name;      // 预设名 (默认 "Preset N")
  ParamSnapshot values;  // 参数快照
};

using namespace iplug;
using namespace igraphics;

namespace iplug { namespace igraphics {
  class IVXYPadControl;
  class ITextControl;
  class IVButtonControl;
  class IVSliderControl;
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
  FilterNodePad*  mPadL = nullptr;
  FilterNodePad*  mPadR = nullptr;

  // 预设 / undo / redo
  std::array<Preset, kNumPresets> mPresets;   // 16 槽: 名称 + 参数
  ParamSnapshot mDefaultSnapshot {};          // 出厂默认值 (右键"恢复默认"用)
  int mCurrentPreset = 0;
  std::vector<int> mRecentSlots;              // 最近使用槽索引 (新→旧), 上限 kNumRecent
  double mMorphPos = 0.0;                     // morph 条位置 (0..kNumQuick-1, JSON 恢复用)
  IVButtonControl* mRecentButtons[kNumRecent] = {};  // 底部最近使用按钮指针 (刷新标签用)
  IVSliderControl* mMorphSlider = nullptr;    // morph 条指针 (LOAD 后恢复位置)
  WDL_String mDialogFileName, mDialogPath;    // 文件对话框的 fileName/path (成员避免悬垂引用)
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
  void UpdatePads();

  // pad 四角 / 范围滑块换算: 保持底层 kFreq + kBw 两个自由度, low/high 为派生视图
  void EditCorner(int kFreq, int kBw, int cornerId, double value);
  void EditBand(int kFreq, int kBw, double lowNorm, double highNorm);
  void ClampAndSet(int kFreq, int kBw, double centerHz, double bwOct);

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
  void RestoreDefault(int idx);                       // 恢复槽为出厂默认

  // 预设文件 (JSON) / 最近使用 / 重命名
  void SaveFile();                                    // SAVE 按钮: 弹保存对话框
  void LoadFile();                                    // LOAD 按钮: 弹打开对话框
  void WritePresetFileTo(const std::string& path, std::string& err);
  void ReadPresetFileFrom(const std::string& path, std::string& err);
  void AddToRecent(int idx);
  void UpdateRecentRow();                             // 刷新底部最近使用按钮标签
  void CommitRename(int idx, const char* name);       // 行内重命名提交

  // 声像区: 数值拷贝/交换 (click 触发, 非开关)
  void CopyLtoR();   // 把当前 L 的数值发送给 R
  void CopyRtoL();   // 把当前 R 的数值发送给 L
  void FlipLR();     // 一次性互换 L / R

  // LINK 开启时, 拖动一个通道的 freq/bw/gain, 另一通道参数实时跟随
  void MirrorLinkedParams(int paramIdx);

  // 预设 morph 条: 在 Q1..Q8 槽位之间对全部参数线性插值
  ParamSnapshot InterpolatePresets(double pos);  // pos: 0..kNumQuick-1
  void OnMorphDrag(double normalizedPos);
};
