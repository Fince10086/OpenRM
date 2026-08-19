#include "BandPass.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"
#include "controls/FilterNodePad.h"

#include <cstring>
#include <cstdio>

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
// 构造
// ---------------------------------------------------------------------------
GRMBandPass::GRMBandPass(const InstanceInfo& info)
: Plugin(info, MakeConfig(kNumParams, 1))
{
  // ---- 参数注册 ----
  GetParam(kFreqL)->InitDouble("FreqL", 1000., 20., 20000., 0.01, "Hz", 0, "", IParam::ShapeExp());
  GetParam(kBwL)  ->InitDouble("BW L", 1., 0.05, 4., 0.01, "oct");
  GetParam(kHpL)  ->InitDouble("HP L", 20., 20., 20000., 1., "Hz", 0, "", IParam::ShapeExp());
  GetParam(kLpL)  ->InitDouble("LP L", 20000., 20., 20000., 1., "Hz", 0, "", IParam::ShapeExp());
  GetParam(kPassL)->InitInt("Pass L", 0, 0, 2);
  GetParam(kGainL)->InitDouble("Gain L", 1., 0., 2., 0.01, "");
  GetParam(kFreqR)->InitDouble("FreqR", 1000., 20., 20000., 0.01, "Hz", 0, "", IParam::ShapeExp());
  GetParam(kBwR)  ->InitDouble("BW R", 1., 0.05, 4., 0.01, "oct");
  GetParam(kHpR)  ->InitDouble("HP R", 20., 20., 20000., 1., "Hz", 0, "", IParam::ShapeExp());
  GetParam(kLpR)  ->InitDouble("LP R", 20000., 20., 20000., 1., "Hz", 0, "", IParam::ShapeExp());
  GetParam(kPassR)->InitInt("Pass R", 0, 0, 2);
  GetParam(kGainR)->InitDouble("Gain R", 1., 0., 2., 0.01, "");
  GetParam(kLink) ->InitBool("Link", false);
  GetParam(kMix)  ->InitDouble("Mix", 1., 0., 1., 0.01, "");
  GetParam(kAgOn) ->InitBool("Agitation", false);
  GetParam(kAgAmount)->InitDouble("Ag Amount", 0.1, 0., 1., 0.01, "");
  GetParam(kAgRate)->InitDouble("Ag Rate", 1., 0.05, 20., 0.01, "Hz");
  GetParam(kTime1)->InitDouble("Time A", 0.5, 0., 5., 0.01, "s");
  GetParam(kTime2)->InitDouble("Time B", 1., 0., 5., 0.01, "s");
  GetParam(kPanLR)->InitBool("L->R", false);
  GetParam(kPanRL)->InitBool("R->L", false);
  GetParam(kPanFlip)->InitBool("Flip", false);

  // ---- 16 预设槽位: 先填默认快照 ----
  for (auto& p : mPresets)
    p = Snapshot();

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
    s[kHpL] = 60.; s[kHpR] = 60.; s[kLpL] = 400.; s[kLpR] = 400.;
    mPresets[5] = s; // 低频啜
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

    const IVStyle style = MakeGRMStyle();
    const IVStyle sec   = MakeSectionTitleStyle();

    // ================= 主控区: LEFT / RIGHT 双通道 =================
    // LEFT 模块
    pGraphics->AttachControl(new ITextControl(IRECT(24, 12, 200, 34),
      "", IText(11, COL_DIM, "Roboto-Regular", EAlign::Far, EVAlign::Middle)));
    mFreqLText = new ITextControl(IRECT(24, 12, 200, 34),
      "CENTER 1.00k", IText(12, COL_ACCENT, "Roboto-Regular", EAlign::Far, EVAlign::Middle));
    pGraphics->AttachControl(mFreqLText);
    mBwLText = new ITextControl(IRECT(204, 12, 344, 34),
      "BW 1.00", IText(12, COL_ACCENT, "Roboto-Regular", EAlign::Far, EVAlign::Middle));
    pGraphics->AttachControl(mBwLText);

    mPassLBtn = new IVButtonControl(IRECT(24, 44, 112, 68), [&](IControl* pCaller) {
      PushUndo();
      CyclePass(static_cast<IVButtonControl*>(pCaller), kPassL);
    }, PassName(0), style);
    pGraphics->AttachControl(mPassLBtn);

    pGraphics->AttachControl(new FilterNodePad(IRECT(24, 76, 324, 374), { kFreqL, kBwL }, "LEFT", style));
    pGraphics->AttachControl(new IVSliderControl(IRECT(24, 388, 166, 416), kHpL, "HP", style, false, EDirection::Horizontal));
    pGraphics->AttachControl(new IVSliderControl(IRECT(182, 388, 324, 416), kLpL, "LP", style, false, EDirection::Horizontal));

    // RIGHT 模块 (x 偏移 +348)
    mFreqRText = new ITextControl(IRECT(372, 12, 548, 34),
      "CENTER 1.00k", IText(12, COL_ACCENT, "Roboto-Regular", EAlign::Far, EVAlign::Middle));
    pGraphics->AttachControl(mFreqRText);
    mBwRText = new ITextControl(IRECT(552, 12, 692, 34),
      "BW 1.00", IText(12, COL_ACCENT, "Roboto-Regular", EAlign::Far, EVAlign::Middle));
    pGraphics->AttachControl(mBwRText);

    mPassRBtn = new IVButtonControl(IRECT(588, 44, 676, 68), [&](IControl* pCaller) {
      PushUndo();
      CyclePass(static_cast<IVButtonControl*>(pCaller), kPassR);
    }, PassName(0), style);
    pGraphics->AttachControl(mPassRBtn);

    pGraphics->AttachControl(new FilterNodePad(IRECT(372, 76, 672, 374), { kFreqR, kBwR }, "RIGHT", style));
    pGraphics->AttachControl(new IVSliderControl(IRECT(372, 388, 514, 416), kHpR, "HP", style, false, EDirection::Horizontal));
    pGraphics->AttachControl(new IVSliderControl(IRECT(530, 388, 672, 416), kLpR, "LP", style, false, EDirection::Horizontal));

    // gain 纵向列 (最右侧)
    pGraphics->AttachControl(new IVSliderControl(IRECT(700, 76, 724, 222), kGainL, "GAIN L", style, false, EDirection::Vertical));
    pGraphics->AttachControl(new IVSliderControl(IRECT(700, 230, 724, 374), kGainR, "GAIN R", style, false, EDirection::Vertical));

    // ================= 右侧控制面板 =================
    pGraphics->AttachControl(new ITextControl(IRECT(748, 10, 908, 30), "PRESETS",
      IText(12, COL_TITLE, "Roboto-Regular", EAlign::Near, EVAlign::Middle)));

    IVButtonControl* slotBtns[kNumPresets] = {};
    for (int r = 0; r < 8; ++r)
    {
      for (int c = 0; c < 2; ++c)
      {
        const int idx = r * 2 + c;
        char label[8];
        snprintf(label, 8, "%d", idx + 1);
        auto* btn = new IVButtonControl(
          IRECT(748 + c * 64, 36 + r * 26, 806 + c * 64, 56 + r * 26),
          [this, idx](IControl*) { LoadSlot(idx); }, label, style);
        slotBtns[idx] = btn;
        pGraphics->AttachControl(btn);
      }
    }

    // 时间参数区
    pGraphics->AttachControl(new ITextControl(IRECT(748, 250, 908, 268), "TIME",
      IText(11, COL_TITLE, "Roboto-Regular", EAlign::Near, EVAlign::Middle)));
    pGraphics->AttachControl(new IVKnobControl(IRECT(748, 270, 826, 330), kTime1, "TIME A", style));
    pGraphics->AttachControl(new IVKnobControl(IRECT(834, 270, 912, 330), kTime2, "TIME B", style));

    // Agitation 区
    pGraphics->AttachControl(new ITextControl(IRECT(748, 338, 908, 356), "AGITATION",
      IText(11, COL_TITLE, "Roboto-Regular", EAlign::Near, EVAlign::Middle)));
    pGraphics->AttachControl(new IVSwitchControl(IRECT(748, 358, 804, 380), kAgOn, "ON", style));
    pGraphics->AttachControl(new IVKnobControl(IRECT(812, 356, 890, 414), kAgAmount, "INTENSITY", style));
    pGraphics->AttachControl(new IVKnobControl(IRECT(898, 356, 976, 414), kAgRate, "DURATION", style));

    // 声像区
    pGraphics->AttachControl(new ITextControl(IRECT(748, 422, 908, 440), "PAN",
      IText(11, COL_TITLE, "Roboto-Regular", EAlign::Near, EVAlign::Middle)));
    pGraphics->AttachControl(new IVSwitchControl(IRECT(748, 442, 804, 464), kPanLR, "L->R", style));
    pGraphics->AttachControl(new IVSwitchControl(IRECT(812, 442, 868, 464), kPanRL, "R->L", style));
    pGraphics->AttachControl(new IVSwitchControl(IRECT(876, 442, 932, 464), kLink, "LINK", style));
    pGraphics->AttachControl(new IVSwitchControl(IRECT(940, 442, 996, 464), kPanFlip, "FLIP", style));
    pGraphics->AttachControl(new IVSliderControl(IRECT(748, 474, 996, 500), kMix, "MIX", style, false, EDirection::Horizontal));

    // undo / redo
    pGraphics->AttachControl(new IVButtonControl(IRECT(748, 512, 822, 540), [this](IControl*) { Undo(); }, "UNDO", style));
    pGraphics->AttachControl(new IVButtonControl(IRECT(830, 512, 904, 540), [this](IControl*) { Redo(); }, "REDO", style));

    // ================= 底部条 =================
    for (int i = 0; i < kNumQuick; ++i)
    {
      char label[8];
      snprintf(label, 8, "Q%d", i + 1);
      pGraphics->AttachControl(new IVButtonControl(
        IRECT(24 + i * 78, 602, 90 + i * 78, 630),
        [this, i](IControl*) { LoadSlot(i); }, label, style));
    }
    pGraphics->AttachControl(new IVButtonControl(IRECT(748, 602, 822, 630), [this](IControl*) { SaveToSlot(mCurrentPreset); }, "SAVE", style));
    pGraphics->AttachControl(new IVButtonControl(IRECT(830, 602, 904, 630), [this](IControl*) { LoadSlot(mCurrentPreset); }, "LOAD", style));
    pGraphics->AttachControl(new IVButtonControl(IRECT(912, 602, 986, 630), [](IControl*) {}, "MIDI", style));

    // 品牌标识
    pGraphics->AttachControl(new ITextControl(IRECT(960, 604, 1152, 640), "GRM BANDPASS",
      IText(15, COL_ACCENT, "Roboto-Regular", EAlign::Far, EVAlign::Middle)));

    // 初始状态
    UpdateParamDisplays();
  };
#endif
}

#if IPLUG_DSP
void GRMBandPass::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  mCore.updateSmoothing(nFrames);
  const int nChans = NOutChansConnected();

  if (nChans <= 1)
    mCore.process(inputs[0], outputs[0], nFrames);
  else
  {
    mCore.process(inputs[0], inputs[1], outputs[0], outputs[1], nFrames);
    for (int c = 2; c < nChans; ++c)
      std::memcpy(outputs[c], inputs[c], nFrames * sizeof(sample));
  }
}

void GRMBandPass::OnReset()
{
  mCore.prepare(GetSampleRate(), GetBlockSize());
  SyncParamsToCore();
}

void GRMBandPass::OnParamChange(int paramIdx)
{
  SyncParamsToCore();
}

void GRMBandPass::OnParamChangeUI(int paramIdx, EParamSource source)
{
  UpdateParamDisplays();
}
#endif

// ---------------------------------------------------------------------------
// 参数同步与显示
// ---------------------------------------------------------------------------
void GRMBandPass::SyncParamsToCore()
{
  grm::BandPassCore::Params p;
  p.freqL  = GetParam(kFreqL)->Value();
  p.bwL    = GetParam(kBwL)->Value();
  p.hpL    = GetParam(kHpL)->Value();
  p.lpL    = GetParam(kLpL)->Value();
  p.passL  = GetParam(kPassL)->Int();
  p.gainL  = static_cast<float>(GetParam(kGainL)->Value());
  p.freqR  = GetParam(kFreqR)->Value();
  p.bwR    = GetParam(kBwR)->Value();
  p.hpR    = GetParam(kHpR)->Value();
  p.lpR    = GetParam(kLpR)->Value();
  p.passR  = GetParam(kPassR)->Int();
  p.gainR  = static_cast<float>(GetParam(kGainR)->Value());
  p.linked = GetParam(kLink)->Value() > 0.5;
  p.mix    = static_cast<float>(GetParam(kMix)->Value());
  p.agOn   = GetParam(kAgOn)->Value() > 0.5;
  p.agAmount = static_cast<float>(GetParam(kAgAmount)->Value());
  p.agRate = GetParam(kAgRate)->Value();
  p.panLR  = GetParam(kPanLR)->Value() > 0.5;
  p.panRL  = GetParam(kPanRL)->Value() > 0.5;
  p.panFlip = GetParam(kPanFlip)->Value() > 0.5;
  mCore.setParams(p);
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
  if (mPassLBtn) mPassLBtn->SetLabelStr(PassName(GetParam(kPassL)->Int()));
  if (mPassRBtn) mPassRBtn->SetLabelStr(PassName(GetParam(kPassR)->Int()));
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
    GetParam(i)->Set(s[i]);
  SyncParamsToCore();
  UpdateParamDisplays();
#if IPLUG_EDITOR
  if (GetUI()) GetUI()->SetAllControlsDirty();
#endif
}

void GRMBandPass::PushUndo()
{
  mUndoStack.push_back(Snapshot());
  if (mUndoStack.size() > 100) mUndoStack.pop_front();
  mRedoStack.clear();
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

void GRMBandPass::CyclePass(IVButtonControl* btn, int paramIdx)
{
  int v = GetParam(paramIdx)->Int();
  v = (v + 1) % 3;
  GetParam(paramIdx)->Set(v);
  if (btn) btn->SetLabelStr(PassName(v));
}

const char* GRMBandPass::PassName(int mode)
{
  switch (mode)
  {
    case 1: return "HP";
    case 2: return "LP";
    default: return "BP";
  }
}

void GRMBandPass::FormatFreq(char* buf, int n, double hz)
{
  if (hz >= 10000.) snprintf(buf, n, "%.1fk", hz / 1000.);
  else if (hz >= 1000.) snprintf(buf, n, "%.2fk", hz / 1000.);
  else snprintf(buf, n, "%.0f", hz);
}
