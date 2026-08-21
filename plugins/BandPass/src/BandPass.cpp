#include "BandPass.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"
#include "controls/FilterNodePad.h"

#include <cstring>
#include <cstdio>
#include <functional>
#include <chrono>

// ---------------------------------------------------------------------------
// GRM 深色扁平配色 (Valhalla 风格基础 + GRM 琥珀/冷灰)
// ---------------------------------------------------------------------------
static const IColor COL_BG       (255, 24, 24, 24);    // 面板
static const IColor COL_PANEL    (255, 40, 40, 40);    // 控件底面
static const IColor COL_PAD      (255, 30, 30, 30);    // 可视化窗口
static const IColor COL_ACCENT   (255, 224, 180, 92);  // 琥珀主色
static const IColor COL_ACCENT_HI(255, 244, 214, 140);
static const IColor COL_FRAME    (255, 62, 62, 62);
static const IColor COL_TEXT     (255, 205, 205, 205);
static const IColor COL_DIM      (255, 120, 120, 120);
static const IColor COL_TITLE    (255, 190, 150, 70);  // 区标题琥珀

static IVStyle MakeGRMStyle()
{
  IVColorSpec colors = { COL_PANEL, COL_ACCENT, COL_ACCENT_HI, COL_FRAME,
                         COL_ACCENT_HI, COL_BG, COL_ACCENT, COL_ACCENT, COL_ACCENT };
  const IText labelText(10, COL_TEXT, "Roboto-Regular", EAlign::Center, EVAlign::Bottom);
  const IText valueText(10, COL_ACCENT, "Roboto-Regular", EAlign::Center, EVAlign::Top);
  return IVStyle(true, true, colors, labelText, valueText,
                 true, true, false, false, 0.f, 1.f, 0.f, 1.f, 0.f);
}

static IVStyle MakeSectionTitleStyle()
{
  IVColorSpec colors = { COL_BG, COL_TITLE, COL_TITLE, COL_FRAME,
                         COL_TITLE, COL_BG, COL_TITLE, COL_TITLE, COL_TITLE };
  const IText labelText(11, COL_TITLE, "Roboto-Regular", EAlign::Near, EVAlign::Bottom);
  return IVStyle(false, false, colors, labelText, labelText, true, false, false, false,
                 0.f, 1.f, 0.f, 1.f, 0.f);
}

// ---------------------------------------------------------------------------
// 按钮专用深色配色: 深底 + 浅字, 避免 "浅底配浅字" 看不清
//   kFG(常态填充) 用深色面板色, kPR(按下) 用琥珀, kHL(hover) 轻微提亮
//   滑块/旋钮仍用 MakeGRMStyle (kFG=琥珀 作手柄/弧线), 与按钮区分
// ---------------------------------------------------------------------------
static const IColor COL_HOVER(255, 78, 78, 82);   // hover 轻微提亮

static IVStyle MakeButtonStyle()
{
  IVColorSpec colors = { COL_PANEL, COL_PANEL, COL_ACCENT, COL_FRAME,
                         COL_HOVER, COL_BG, COL_ACCENT, COL_ACCENT, COL_ACCENT };
  const IText labelText(11, COL_TEXT, "Roboto-Regular", EAlign::Center, EVAlign::Middle);
  const IText valueText(11, COL_TEXT, "Roboto-Regular", EAlign::Center, EVAlign::Middle);
  return IVStyle(true, true, colors, labelText, valueText, true, true, false, false,
                 0.f, 1.f, 0.f, 1.f, 0.f);
}

// 瞬时按钮: 点击执行动作后立即把值复位为 0, 规避 iPlug2
// IButtonControlBase 在缺少动画时 "按下后永久停留在浅色态" 的行为
static IVButtonControl* MakeMomentary(const IRECT& r,
                                       std::function<void(IControl*)> fn,
                                       const char* label, const IVStyle& st)
{
  return new IVButtonControl(r, [fn](IControl* p) {
    fn(p);
    p->SetValue(0.0);
    p->SetDirty(false);
  }, label, st);
}

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
GRMBandPass::GRMBandPass(const InstanceInfo& info)
: Plugin(info, MakeConfig(kNumParams, 1))
{
  // ---- 参数注册 (纯带通: 仅 freq / bw / gain) ----
  GetParam(kFreqL)->InitDouble("FreqL", 1000., 20., 20000., 0.01, "Hz", 0, "", IParam::ShapeExp());
  GetParam(kBwL)  ->InitDouble("BW L", 1., 0.05, 4., 0.01, "oct");
  GetParam(kGainL)->InitDouble("Gain L", 1., 0., 2., 0.01, "");
  GetParam(kFreqR)->InitDouble("FreqR", 1000., 20., 20000., 0.01, "Hz", 0, "", IParam::ShapeExp());
  GetParam(kBwR)  ->InitDouble("BW R", 1., 0.05, 4., 0.01, "oct");
  GetParam(kGainR)->InitDouble("Gain R", 1., 0., 2., 0.01, "");
  GetParam(kLink) ->InitBool("Link", false);
  GetParam(kMix)  ->InitDouble("Mix", 1., 0., 1., 0.01, "");
  GetParam(kAgOn) ->InitBool("Agitation", false);
  GetParam(kAgAmount)->InitDouble("Ag Amount", 0.1, 0., 1., 0.01, "");
  GetParam(kAgRate)->InitDouble("Ag Rate", 1., 0.05, 20., 0.01, "Hz");

  // ---- 16 预设槽位: 先填默认快照 ----
  for (auto& p : mPresets)
    p = Snapshot();

  mStableSnapshot = Snapshot();

  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 500.;  s[kBwL] = 0.2;  s[kFreqR] = 500.; s[kBwR] = 0.2; s[kLink] = 1.;
    mPresets[1] = s; // 窄带 500
  }
  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 2000.; s[kBwL] = 3.0;  s[kFreqR] = 2000.; s[kBwR] = 3.0; s[kLink] = 1.;
    mPresets[2] = s; // 宽带 2k
  }
  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 400.; s[kBwL] = 0.3; s[kFreqR] = 4000.; s[kBwR] = 1.5;
    mPresets[3] = s; // 分离 L/R
  }
  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 3000.; s[kBwL] = 0.5; s[kFreqR] = 3000.; s[kBwR] = 0.5; s[kLink] = 1.;
    s[kAgOn] = 1.; s[kAgAmount] = 0.3; s[kAgRate] = 4.;
    mPresets[4] = s; // 抖动
  }
  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 150.; s[kBwL] = 2.5; s[kFreqR] = 150.; s[kBwR] = 2.5; s[kLink] = 1.;
    mPresets[5] = s; // 低频
  }

#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]()
  {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS,
                        GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
  };

  mLayoutFunc = [&](IGraphics* pGraphics)
  {
    pGraphics->AttachCornerResizer(EUIResizerMode::Scale, false);
    pGraphics->AttachPanelBackground(COL_BG);
    pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);

    const IVStyle style   = MakeGRMStyle();
    const IVStyle sec     = MakeSectionTitleStyle();
    const IVStyle btnStyle= MakeButtonStyle();

    // ================= 主控区: LEFT / RIGHT 双通道 =================
    // LEFT 模块
    mFreqLText = new ITextControl(IRECT(24, 12, 200, 34),
      "CENTER 1.00k", IText(12, COL_ACCENT, "Roboto-Regular", EAlign::Far, EVAlign::Middle));
    pGraphics->AttachControl(mFreqLText);
    mBwLText = new ITextControl(IRECT(204, 12, 344, 34),
      "BW 1.00", IText(12, COL_ACCENT, "Roboto-Regular", EAlign::Far, EVAlign::Middle));
    pGraphics->AttachControl(mBwLText);

    // LEFT 滤波节点板 (纯带通: X=中心频率, Y=带宽). 移除原 pass 按钮与 HP/LP 滑块, 直接放大填充足区域
    mPadL = new FilterNodePad(IRECT(24, 52, 324, 416), { kFreqL, kBwL }, "LEFT", style);
    pGraphics->AttachControl(mPadL);

    // RIGHT 模块 (x 偏移 +348)
    mFreqRText = new ITextControl(IRECT(372, 12, 548, 34),
      "CENTER 1.00k", IText(12, COL_ACCENT, "Roboto-Regular", EAlign::Far, EVAlign::Middle));
    pGraphics->AttachControl(mFreqRText);
    mBwRText = new ITextControl(IRECT(552, 12, 692, 34),
      "BW 1.00", IText(12, COL_ACCENT, "Roboto-Regular", EAlign::Far, EVAlign::Middle));
    pGraphics->AttachControl(mBwRText);

    // RIGHT 滤波节点板 (同 LEFT)
    mPadR = new FilterNodePad(IRECT(372, 52, 672, 416), { kFreqR, kBwR }, "RIGHT", style);
    pGraphics->AttachControl(mPadR);

    // gain 纵向列 (最右侧)
    pGraphics->AttachControl(new IVSliderControl(IRECT(700, 76, 724, 222), kGainL, "GAIN L", style, false, EDirection::Vertical));
    pGraphics->AttachControl(new IVSliderControl(IRECT(700, 230, 724, 374), kGainR, "GAIN R", style, false, EDirection::Vertical));

    // ================= 右侧控制面板 =================
    pGraphics->AttachControl(new ITextControl(IRECT(748, 10, 908, 30), "PRESETS",
      IText(12, COL_TITLE, "Roboto-Regular", EAlign::Near, EVAlign::Middle)));

    for (int r = 0; r < 8; ++r)
    {
      for (int c = 0; c < 2; ++c)
      {
        const int idx = r * 2 + c;
        char label[8];
        snprintf(label, 8, "%d", idx + 1);
        pGraphics->AttachControl(MakeMomentary(
          IRECT(748 + c * 64, 36 + r * 26, 806 + c * 64, 56 + r * 26),
          [this, idx](IControl*) { LoadSlot(idx); }, label, btnStyle));
      }
    }

    // Agitation 区 (TIME 区已移除: kTime1/kTime2 为无 DSP 行为的死参数)
    pGraphics->AttachControl(new ITextControl(IRECT(748, 250, 908, 268), "AGITATION",
      IText(11, COL_TITLE, "Roboto-Regular", EAlign::Near, EVAlign::Middle)));
    pGraphics->AttachControl(new IVSwitchControl(IRECT(748, 272, 804, 294), kAgOn, "ON", btnStyle));
    pGraphics->AttachControl(new IVKnobControl(IRECT(812, 268, 890, 326), kAgAmount, "INTENSITY", style));
    pGraphics->AttachControl(new IVKnobControl(IRECT(898, 268, 976, 326), kAgRate, "RATE", style));

    // 声像区
    pGraphics->AttachControl(new ITextControl(IRECT(748, 334, 908, 352), "PAN",
      IText(11, COL_TITLE, "Roboto-Regular", EAlign::Near, EVAlign::Middle)));
    // L->R / R->L / FLIP 现在是 click 触发: 拷贝/交换 L,R 数值 (非开关)
    pGraphics->AttachControl(MakeMomentary(IRECT(748, 356, 804, 378), [this](IControl*) { CopyLtoR(); }, "L->R", btnStyle));
    pGraphics->AttachControl(MakeMomentary(IRECT(812, 356, 868, 378), [this](IControl*) { CopyRtoL(); }, "R->L", btnStyle));
    pGraphics->AttachControl(new IVSwitchControl(IRECT(876, 356, 932, 378), kLink, "LINK", btnStyle)); // 仍是开关
    pGraphics->AttachControl(MakeMomentary(IRECT(940, 356, 996, 378), [this](IControl*) { FlipLR(); }, "FLIP", btnStyle));
    pGraphics->AttachControl(new IVSliderControl(IRECT(748, 390, 996, 416), kMix, "MIX", style, false, EDirection::Horizontal));

    // undo / redo
    pGraphics->AttachControl(MakeMomentary(IRECT(748, 428, 822, 456), [this](IControl*) { Undo(); }, "UNDO", btnStyle));
    pGraphics->AttachControl(MakeMomentary(IRECT(830, 428, 904, 456), [this](IControl*) { Redo(); }, "REDO", btnStyle));

    // ================= 底部条 =================
    for (int i = 0; i < kNumQuick; ++i)
    {
      char label[8];
      snprintf(label, 8, "Q%d", i + 1);
      pGraphics->AttachControl(MakeMomentary(
        IRECT(24 + i * 78, 602, 90 + i * 78, 630),
        [this, i](IControl*) { LoadSlot(i); }, label, btnStyle));
    }
    pGraphics->AttachControl(MakeMomentary(IRECT(748, 602, 822, 630), [this](IControl*) { SaveToSlot(mCurrentPreset); }, "SAVE", btnStyle));
    pGraphics->AttachControl(MakeMomentary(IRECT(830, 602, 904, 630), [this](IControl*) { LoadSlot(mCurrentPreset); }, "LOAD", btnStyle));

    // 品牌标识 + 版本号 (标题下方)
    pGraphics->AttachControl(new ITextControl(IRECT(960, 596, 1152, 618), "GRM BANDPASS",
      IText(15, COL_ACCENT, "Roboto-Regular", EAlign::Far, EVAlign::Middle)));
    pGraphics->AttachControl(new ITextControl(IRECT(960, 618, 1152, 640), "v" PLUG_VERSION_STR,
      IText(9, COL_DIM, "Roboto-Regular", EAlign::Far, EVAlign::Middle)));

    // 初始状态
    UpdateParamDisplays();
  };
#endif
}

#if IPLUG_DSP
void GRMBandPass::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  // 音频线程: 取走编辑器线程发布的最新参数 (写入中则沿用上一块)
  grm::BandPassCore::Params p;
  if (mParamMailbox.consume(p))
    mCore.setParams(p);

  mCore.updateSmoothing(nFrames);

  // 防护: 只有在输入输出都连了至少 2 声道时才走立体声路径, 否则按单声道处理并复制
  const int nOuts = NOutChansConnected();
  const int nIns = NInChansConnected();

  if (nOuts >= 2 && nIns >= 2)
  {
    mCore.process(inputs[0], inputs[1], outputs[0], outputs[1], nFrames);
    for (int c = 2; c < nOuts; ++c)
      std::memcpy(outputs[c], inputs[c], nFrames * sizeof(sample));
  }
  else
  {
    mCore.process(inputs[0], outputs[0], nFrames);
    for (int c = 1; c < nOuts; ++c)
      std::memcpy(outputs[c], outputs[0], nFrames * sizeof(sample));
  }
}

void GRMBandPass::OnReset()
{
  // 先同步参数再 prepare: 让平滑状态快照取到真实参数, 避免载入后从默认值滑音
  mCore.setParams(CollectParams());
  mCore.prepare(GetSampleRate(), GetBlockSize());
}

void GRMBandPass::OnParamChange(int paramIdx, EParamSource source, int sampleOffset)
{
  if (source == EParamSource::kHost)
  {
    // 宿主自动化在音频线程回调: 直接写核心, 与 ProcessBlock 天然串行, 不经信箱
    mCore.setParams(CollectParams());
  }
  else
  {
    PublishParamsToCore();
  }
}

void GRMBandPass::OnParamChangeUI(int paramIdx, EParamSource source)
{
  // 兜底发布: 部分格式 (如 APP) 的 UI 拖动不经宿主回合到 OnParamChange
  PublishParamsToCore();
  if (source == EParamSource::kUI)
    MaybePushGestureUndo();
  UpdateParamDisplays();
}
#endif

// ---------------------------------------------------------------------------
// 参数同步与显示
// ---------------------------------------------------------------------------
grm::BandPassCore::Params GRMBandPass::CollectParams() const
{
  grm::BandPassCore::Params p;
  p.freqL  = GetParam(kFreqL)->Value();
  p.bwL    = GetParam(kBwL)->Value();
  p.gainL  = static_cast<float>(GetParam(kGainL)->Value());
  p.freqR  = GetParam(kFreqR)->Value();
  p.bwR    = GetParam(kBwR)->Value();
  p.gainR  = static_cast<float>(GetParam(kGainR)->Value());
  p.linked = GetParam(kLink)->Value() > 0.5;
  p.mix    = static_cast<float>(GetParam(kMix)->Value());
  p.agOn   = GetParam(kAgOn)->Value() > 0.5;
  p.agAmount = static_cast<float>(GetParam(kAgAmount)->Value());
  p.agRate = GetParam(kAgRate)->Value();
  return p;
}

void GRMBandPass::PublishParamsToCore()
{
  mParamMailbox.publish(CollectParams());
}

void GRMBandPass::SetParamFromEditor(int idx, double value)
{
  GetParam(idx)->Set(value);
  InformHostOfParamChange(idx, GetParam(idx)->GetNormalized());
  PublishParamsToCore();
}

void GRMBandPass::RefreshAfterEdit()
{
#if IPLUG_EDITOR
  if (GetUI())
  {
    // 让所有绑定参数的控件 (滑条/旋钮/开关/XY pad) 从参数回读最新值
    SendCurrentParamValuesFromDelegate();
    GetUI()->SetAllControlsDirty();
  }
#endif
  UpdateParamDisplays();
  MarkStateStable();
}

void GRMBandPass::UpdateParamDisplays()
{
  char buf[32];
  if (mFreqLText)
  {
    FormatFreq(buf, 32, GetParam(kFreqL)->Value());
    mFreqLText->SetStrFmt(32, "CENTER %s", buf);
  }
  if (mBwLText)
  {
    snprintf(buf, 32, "%.2f", GetParam(kBwL)->Value());
    mBwLText->SetStrFmt(32, "BW %s", buf);
  }
  if (mFreqRText)
  {
    FormatFreq(buf, 32, GetParam(kFreqR)->Value());
    mFreqRText->SetStrFmt(32, "CENTER %s", buf);
  }
  if (mBwRText)
  {
    snprintf(buf, 32, "%.2f", GetParam(kBwR)->Value());
    mBwRText->SetStrFmt(32, "BW %s", buf);
  }
  UpdatePads();
}

// 把 freq/bw 参数同步到 XY 手柄位置: 通过 SetValueFromDelegate (只写控件内部值, 不回写参数)
// 解决 "预设/同步/翻转等改值时 XY 轴手柄与显示不跟随" 的问题
void GRMBandPass::UpdatePads()
{
  if (mPadL)
  {
    mPadL->SetValueFromDelegate(GetParam(kFreqL)->GetNormalized(), 0);
    mPadL->SetValueFromDelegate(GetParam(kBwL)->GetNormalized(), 1);
    mPadL->SetDirty(false);
  }
  if (mPadR)
  {
    mPadR->SetValueFromDelegate(GetParam(kFreqR)->GetNormalized(), 0);
    mPadR->SetValueFromDelegate(GetParam(kBwR)->GetNormalized(), 1);
    mPadR->SetDirty(false);
  }
}

// ---------------------------------------------------------------------------
// 预设 / undo / redo
// ---------------------------------------------------------------------------
ParamSnapshot GRMBandPass::Snapshot() const
{
  ParamSnapshot s;
  for (int i = 0; i < kNumParams; ++i)
    s[i] = GetParam(i)->Value();
  return s;
}

void GRMBandPass::ApplySnapshot(const ParamSnapshot& s)
{
  for (int i = 0; i < kNumParams; ++i)
    SetParamFromEditor(i, s[i]);
  RefreshAfterEdit();
}

void GRMBandPass::PushUndo()
{
  PushUndoSnapshot(Snapshot());
}

void GRMBandPass::PushUndoSnapshot(const ParamSnapshot& s)
{
  if (!mUndoStack.empty() && mUndoStack.back() == s) return;  // 与栈顶相同则不入栈
  mUndoStack.push_back(s);
  if (mUndoStack.size() > 100) mUndoStack.pop_front();
  mRedoStack.clear();
}

// iPlug2 无手势开始/结束回调: 以 kUI 参数事件的到达间隔判断,
// 超过 kGestureGapSec 视为一次新手势, 入栈"手势前"的稳定快照;
// 手势结束后由 OnIdle 把最终状态固化为新的稳定快照
static constexpr double kGestureGapSec = 0.4;

void GRMBandPass::MaybePushGestureUndo()
{
  using namespace std::chrono;
  const double now = duration<double>(steady_clock::now().time_since_epoch()).count();
  if (now - mLastUIChangeTime > kGestureGapSec)
    PushUndoSnapshot(mStableSnapshot);
  mLastUIChangeTime = now;
  mGesturePending = true;
}

void GRMBandPass::OnIdle()
{
  using namespace std::chrono;
  const double now = duration<double>(steady_clock::now().time_since_epoch()).count();
  if (mGesturePending && now - mLastUIChangeTime > kGestureGapSec)
  {
    mStableSnapshot = Snapshot();
    mGesturePending = false;
  }
}

void GRMBandPass::MarkStateStable()
{
  mStableSnapshot = Snapshot();
  mGesturePending = false;
}

void GRMBandPass::Undo()
{
  if (mUndoStack.empty()) return;
  mRedoStack.push_back(Snapshot());
  const ParamSnapshot s = mUndoStack.back();
  mUndoStack.pop_back();
  ApplySnapshot(s);
}

void GRMBandPass::Redo()
{
  if (mRedoStack.empty()) return;
  mUndoStack.push_back(Snapshot());
  const ParamSnapshot s = mRedoStack.back();
  mRedoStack.pop_back();
  ApplySnapshot(s);
}

void GRMBandPass::SaveToSlot(int idx)
{
  if (idx < 0 || idx >= kNumPresets) return;
  mPresets[idx] = Snapshot();
}

void GRMBandPass::LoadSlot(int idx)
{
  if (idx < 0 || idx >= kNumPresets) return;
  PushUndo();
  mCurrentPreset = idx;
  ApplySnapshot(mPresets[idx]);
}

// 声像区: 数值拷贝 / 交换 (click 触发)
// 经 SetParamFromEditor 写值: 同步 DSP 信箱 + 通知宿主; 不在 DSP 里做音频路由
void GRMBandPass::CopyLtoR()
{
  PushUndo();
  SetParamFromEditor(kFreqR, GetParam(kFreqL)->Value());
  SetParamFromEditor(kBwR,   GetParam(kBwL)->Value());
  SetParamFromEditor(kGainR, GetParam(kGainL)->Value());
  RefreshAfterEdit();
}

void GRMBandPass::CopyRtoL()
{
  PushUndo();
  SetParamFromEditor(kFreqL, GetParam(kFreqR)->Value());
  SetParamFromEditor(kBwL,   GetParam(kBwR)->Value());
  SetParamFromEditor(kGainL, GetParam(kGainR)->Value());
  RefreshAfterEdit();
}

void GRMBandPass::FlipLR()
{
  PushUndo();
  const double fL = GetParam(kFreqL)->Value(), bL = GetParam(kBwL)->Value(), gL = GetParam(kGainL)->Value();
  SetParamFromEditor(kFreqL, GetParam(kFreqR)->Value());
  SetParamFromEditor(kBwL,   GetParam(kBwR)->Value());
  SetParamFromEditor(kGainL, GetParam(kGainR)->Value());
  SetParamFromEditor(kFreqR, fL);
  SetParamFromEditor(kBwR,   bL);
  SetParamFromEditor(kGainR, gL);
  RefreshAfterEdit();
}

void GRMBandPass::FormatFreq(char* buf, int n, double hz)
{
  if (hz >= 10000.) snprintf(buf, n, "%.1fk", hz / 1000.);
  else if (hz >= 1000.) snprintf(buf, n, "%.2fk", hz / 1000.);
  else snprintf(buf, n, "%.0f", hz);
}
