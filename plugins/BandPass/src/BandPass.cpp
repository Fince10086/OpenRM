#include "BandPass.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"
#include "Theme.h"
#include "controls/FilterNodePad.h"
#include "controls/PresetSlotControl.h"
#include "PresetFileIO.h"

#include <cstring>
#include <cstdio>
#include <functional>
#include <chrono>
#include <cmath>
#include <algorithm>

static IVStyle MakeGRMStyle()
{
  IVColorSpec colors = { COL_BG, COL_BG, COL_DIM, COL_BLACK,
                         COL_HOVER, COL_TRACK, COL_BLACK, COL_BLACK, COL_BLACK };
  const IText labelText(10, COL_DIM, "Outfit", EAlign::Center, EVAlign::Bottom);
  const IText valueText(10, COL_BLACK, "Outfit-SemiBold", EAlign::Center, EVAlign::Top);
  return IVStyle(true, true, colors, labelText, valueText,
                 true, true, false, false, 0.2f, 1.5f, 0.f, 1.f, 0.f);
}

static IVStyle MakeButtonStyle()
{
  IVColorSpec colors = { COL_BG, COL_BG, COL_BLACK, COL_BLACK,
                         COL_HOVER, COL_BG, COL_BLACK, COL_BLACK, COL_BLACK };
  const IText labelText(11, COL_BLACK, "Outfit-SemiBold", EAlign::Center, EVAlign::Middle);
  const IText valueText(11, COL_BLACK, "Outfit-SemiBold", EAlign::Center, EVAlign::Middle);
  return IVStyle(true, true, colors, labelText, valueText, true, true, false, false,
                 0.2f, 2.f, 0.f, 1.f, 0.f);
}

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

class PresetMorphSlider : public IVSliderControl
{
public:
  PresetMorphSlider(const IRECT& bounds, IActionFunction aF, const IVStyle& style)
  : IVSliderControl(bounds, aF, "", style, false, EDirection::Horizontal)
  {
  }

  void DrawTrack(IGraphics& g, const IRECT& filledArea) override
  {
    IVSliderControl::DrawTrack(g, filledArea);

    const float x0 = mTrackBounds.L, w = mTrackBounds.W();
    for (int i = 0; i < kNumQuick; ++i)
    {
      const float x = x0 + w * i / (kNumQuick - 1.f);
      g.FillRect(COL_BLACK, IRECT(x - 1.f, mTrackBounds.T - 3.f, x + 1.f, mTrackBounds.T));
      g.FillRect(COL_BLACK, IRECT(x - 1.f, mTrackBounds.B, x + 1.f, mTrackBounds.B + 3.f));
    }
  }
};

class GRMSlider : public IVSliderControl
{
public:
  GRMSlider(const IRECT& bounds, int paramIdx, const char* label, const IVStyle& style,
            EDirection dir)
  : IVSliderControl(bounds, paramIdx, label, style, false, dir)
  , mHeaderLabel(label ? label : "")
  {

    mStyle.showLabel = false;
    mStyle.showValue = false;
  }

  void OnResize() override
  {

    if (mDirection == EDirection::Horizontal)
    {
      mWidgetBounds = mRECT.GetReducedFromTop(kHeaderH);
      mTrackBounds  = mWidgetBounds.GetPadded(-mHandleSize)
                                   .GetMidVPadded(mTrackSize);
    }
    else
    {
      mWidgetBounds = mRECT.GetReducedFromLeft(kHeaderW);
      mTrackBounds  = mWidgetBounds.GetPadded(-mHandleSize)
                                   .GetMidHPadded(mTrackSize);
    }
    SetTargetRECT(mRECT);
    mValueBounds = IRECT();
    SetDirty(false);
  }

  bool IsHit(float x, float y) const override
  {
    return mRECT.Contains(x, y);
  }

  void Draw(IGraphics& g) override
  {
    IVSliderControl::Draw(g);
    DrawHeader(g, mDirection == EDirection::Vertical ? -90.f : 0.f);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {

    if (mod.L && !mod.R && !mod.A && ValueRect().Contains(x, y))
    {
      PromptUserInput(ValueRect());
      return;
    }
    IVSliderControl::OnMouseDown(x, y, mod);
  }

  void DrawTrack(IGraphics& g, const IRECT& filledArea) override
  {
    const float cr = GetRoundedCornerRadius(mTrackBounds);
    g.FillRoundRect(COL_TRACK, mTrackBounds, cr, &mBlend);
    if (filledArea.W() > 0.5f && filledArea.H() > 0.5f)
      g.FillRoundRect(COL_BLACK, filledArea, cr, &mBlend);
  }

  void DrawHandle(IGraphics& g, const IRECT& bounds) override
  {
    const float cx = bounds.MW(), cy = bounds.MH();
    const float r  = bounds.W() * 0.5f;
    g.FillCircle(COLOR_WHITE, cx, cy, r);
    g.DrawCircle(COL_BLACK, cx, cy, r - 0.75f, nullptr, 1.5f);
  }

private:
  static constexpr float kHeaderH = 14.f;
  static constexpr float kHeaderW = 14.f;

  IRECT ValueRect() const
  {
    if (mDirection == EDirection::Horizontal)
      return IRECT(mRECT.L, mRECT.T, mRECT.R, mRECT.T + kHeaderH);
    return IRECT(mRECT.L, mRECT.T, mRECT.L + kHeaderW + 4.f, mRECT.T + 28.f);
  }

  void DrawHeader(IGraphics& g, float rot)
  {
    WDL_String ds;
    if (GetParam()) GetParam()->GetDisplay(ds, false);

    if (rot == 0.f)
    {
      const IRECT hdr(mRECT.L, mRECT.T, mRECT.R, mRECT.T + kHeaderH);
      g.DrawText(IText(10, COL_BLACK, "Outfit-SemiBold", EAlign::Near, EVAlign::Middle),
                 mHeaderLabel.Get(), IRECT(hdr.L, hdr.T, hdr.MW(), hdr.B));
      g.DrawText(IText(10, COL_DIM, "Outfit", EAlign::Far, EVAlign::Middle),
                 ds.Get(), IRECT(hdr.MW(), hdr.T, hdr.R, hdr.B));
    }
    else
    {
      const IRECT hdr(mRECT.L, mRECT.T, mRECT.L + kHeaderW, mRECT.B);
      g.DrawText(IText(10, COL_BLACK, "Outfit-SemiBold", EAlign::Near, EVAlign::Bottom, rot),
                 mHeaderLabel.Get(), hdr);
      g.DrawText(IText(10, COL_DIM, "Outfit", EAlign::Far, EVAlign::Top, rot),
                 ds.Get(), hdr);
    }
  }

  WDL_String mHeaderLabel;
};

class InvertToggleControl : public IVToggleControl
{
public:
  InvertToggleControl(const IRECT& bounds, int paramIdx, const char* label,
                      const IVStyle& style, const char* offText, const char* onText)
  : IVToggleControl(bounds, paramIdx, label, style, offText, onText)
  {
    SetActionFunction(EmptyClickActionFunc);
  }

  void DrawValue(IGraphics& g, bool) override
  {
    const bool on = GetValue() > 0.5;
    IText t = mStyle.valueText;
    t.mFGColor = on ? COLOR_WHITE : COL_BLACK;
    g.DrawText(t, on ? mOnText.Get() : mOffText.Get(), mWidgetBounds, &mBlend);
  }
};

GRMBandPass::GRMBandPass(const InstanceInfo& info)
: Plugin(info, MakeConfig(kNumParams, 1))
{
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

  for (int i = 0; i < kNumPresets; ++i)
  {
    mPresets[i] = Snapshot();
    mSlotNumber[i] = i;
  }

  mDefaultSnapshot = Snapshot();
  mStableSnapshot  = Snapshot();

  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 500.;  s[kBwL] = 0.2;  s[kFreqR] = 500.; s[kBwR] = 0.2; s[kLink] = 1.;
    mPresets[1] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 2000.; s[kBwL] = 3.0;  s[kFreqR] = 2000.; s[kBwR] = 3.0; s[kLink] = 1.;
    mPresets[2] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 400.; s[kBwL] = 0.3; s[kFreqR] = 4000.; s[kBwR] = 1.5;
    mPresets[3] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 3000.; s[kBwL] = 0.5; s[kFreqR] = 3000.; s[kBwR] = 0.5; s[kLink] = 1.;
    s[kAgOn] = 1.; s[kAgAmount] = 0.3; s[kAgRate] = 4.;
    mPresets[4] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 150.; s[kBwL] = 2.5; s[kFreqR] = 150.; s[kBwR] = 2.5; s[kLink] = 1.;
    mPresets[5] = s;
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
    pGraphics->LoadFont("Outfit", OUTFIT_FN);
    pGraphics->LoadFont("Outfit-SemiBold", OUTFIT_SB_FN);
    pGraphics->LoadFont("Outfit-Bold", OUTFIT_BD_FN);

    const IVStyle style   = MakeGRMStyle();
    const IVStyle btnStyle= MakeButtonStyle();
    IVStyle toggleStyle = btnStyle;
    toggleStyle.showLabel = false;
    toggleStyle.showValue = false;

    constexpr float kCol1X     = 740.f;
    constexpr float kCol2X     = 804.f;
    constexpr float kBtnW      = 56.f;
    constexpr float kBtnH      = 22.f;
    constexpr float kSlotPitch = 24.f;
    constexpr float kSliderH   = 34.f;

    auto padHooks = [&](int kF, int kB) -> FilterNodePad::Hooks {
      return FilterNodePad::Hooks{
        [this] { MaybePushGestureUndo(); },
        [this, kF, kB](int id, double v) { EditCorner(kF, kB, id, v); },
        [this, kF, kB](double lN, double hN) { EditBand(kF, kB, lN, hN); },
      };
    };

    mPadL = new FilterNodePad(IRECT(20, 38, 668, 270), { kFreqL, kBwL }, "LEFT", style, padHooks(kFreqL, kBwL));
    pGraphics->AttachControl(mPadL);

    mPadR = new FilterNodePad(IRECT(20, 302, 668, 534), { kFreqR, kBwR }, "RIGHT", style, padHooks(kFreqR, kBwR));
    pGraphics->AttachControl(mPadR);

    pGraphics->AttachControl(new GRMSlider(IRECT(672, 38, 730, 270), kGainL, "GAIN L", style, EDirection::Vertical));
    pGraphics->AttachControl(new GRMSlider(IRECT(672, 302, 730, 534), kGainR, "GAIN R", style, EDirection::Vertical));

    pGraphics->AttachControl(new ITextControl(IRECT(kCol1X, 14, 900, 34), "PRESETS",
      IText(11, COL_BLACK, "Outfit-Bold", EAlign::Near, EVAlign::Middle)));

    auto makeSlotHooks = [this](int pos) -> PresetSlotControl::Hooks
    {
      return PresetSlotControl::Hooks{
        [this, pos]() { LoadSlot(mSlotNumber[pos]); },
        [this, pos]() { SaveToSlot(mSlotNumber[pos]); },
        [this, pos]() { RestoreDefault(mSlotNumber[pos]); },
        [this, pos]() { OnDragBegin(pos); },
        [this, pos](float x, float y) { OnDragMove(x, y); },
        [this, pos](float x, float y) { OnDragDrop(pos, x, y); },
        [this, pos]() -> std::string {
          char buf[24];
          snprintf(buf, sizeof(buf), "Preset %d", mSlotNumber[pos] + 1);
          return buf;
        },
      };
    };

    for (int r = 0; r < 8; ++r)
    {
      for (int c = 0; c < 2; ++c)
      {
        const int pos = kNumBottom + r * 2 + c;
        char label[8];
        snprintf(label, 8, "%d", mSlotNumber[pos] + 1);
        PresetSlotControl* btn = new PresetSlotControl(
          IRECT(kCol1X + c * 64, 40 + r * kSlotPitch,
                kCol1X + c * 64 + kBtnW, 40 + r * kSlotPitch + kBtnH),
          makeSlotHooks(pos), label, btnStyle);
        mSlotButtons[pos] = btn;
        pGraphics->AttachControl(btn);
      }
    }

    pGraphics->AttachControl(new ITextControl(IRECT(kCol1X, 242, 900, 262), "AGITATION",
      IText(11, COL_BLACK, "Outfit-Bold", EAlign::Near, EVAlign::Middle)));
    pGraphics->AttachControl(new InvertToggleControl(IRECT(kCol1X, 268, kCol1X + kBtnW, 290), kAgOn, " ", toggleStyle, "OFF", "ON"));
    pGraphics->AttachControl(new GRMSlider(IRECT(kCol1X, 296, 988, 330), kAgAmount, "INTENSITY", style, EDirection::Horizontal));
    pGraphics->AttachControl(new GRMSlider(IRECT(kCol1X, 336, 988, 370), kAgRate, "RATE", style, EDirection::Horizontal));

    pGraphics->AttachControl(MakeMomentary(IRECT(kCol1X, 376, kCol1X + kBtnW, 398), [this](IControl*) { CopyLtoR(); }, "L->R", btnStyle));
    pGraphics->AttachControl(MakeMomentary(IRECT(kCol2X, 376, kCol2X + kBtnW, 398), [this](IControl*) { CopyRtoL(); }, "R->L", btnStyle));
    pGraphics->AttachControl(new InvertToggleControl(IRECT(kCol1X, 404, kCol1X + kBtnW, 426), kLink, " ", toggleStyle, "LINK", "LINK"));
    pGraphics->AttachControl(MakeMomentary(IRECT(kCol2X, 404, kCol2X + kBtnW, 426), [this](IControl*) { FlipLR(); }, "FLIP", btnStyle));
    pGraphics->AttachControl(new GRMSlider(IRECT(kCol1X, 432, 988, 466), kMix, "MIX", style, EDirection::Horizontal));

    pGraphics->AttachControl(MakeMomentary(IRECT(kCol1X, 472, kCol1X + kBtnW, 494), [this](IControl*) { Undo(); }, "UNDO", btnStyle));
    pGraphics->AttachControl(MakeMomentary(IRECT(kCol2X, 472, kCol2X + kBtnW, 494), [this](IControl*) { Redo(); }, "REDO", btnStyle));

    for (int i = 0; i < kNumBottom; ++i)
    {
      char label[8];
      snprintf(label, 8, "%d", mSlotNumber[i] + 1);
      PresetSlotControl* btn = new PresetSlotControl(
        IRECT(20 + i * 82, 546, 94 + i * 82, 568),
        makeSlotHooks(i), label, btnStyle);
      mSlotButtons[i] = btn;
      pGraphics->AttachControl(btn);
    }

    mMorphSlider = new PresetMorphSlider(IRECT(49, 572, 639, 592),
      [this](IControl* pCtrl) {
        MaybePushGestureUndo();
        mMorphPos = pCtrl->GetValue(0) * (kNumQuick - 1.0);
        OnMorphDrag(pCtrl->GetValue(0));
      }, btnStyle);
    pGraphics->AttachControl(mMorphSlider);
    pGraphics->AttachControl(MakeMomentary(IRECT(740, 546, 796, 568), [this](IControl*) { SaveFile(); }, "SAVE", btnStyle));
    pGraphics->AttachControl(MakeMomentary(IRECT(804, 546, 860, 568), [this](IControl*) { LoadFile(); }, "LOAD", btnStyle));

    pGraphics->AttachControl(new ITextControl(IRECT(kCol1X, 500, 988, 524), "GRM BANDPASS",
      IText(16, COL_BLACK, "Outfit-Bold", EAlign::Far, EVAlign::Middle)));
    pGraphics->AttachControl(new ITextControl(IRECT(868, 528, 988, 544), "v" PLUG_VERSION_STR,
      IText(9, COL_FAINT, "Outfit", EAlign::Far, EVAlign::Middle)));

    pGraphics->EnableTooltips(true);
    UpdatePads();
  };
#endif
}

#if IPLUG_DSP
void GRMBandPass::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  grm::BandPassCore::Params p;
  if (mParamMailbox.consume(p))
    mCore.setParams(p);

  mCore.updateSmoothing(nFrames);

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
  mCore.setParams(CollectParams());
  mCore.prepare(GetSampleRate(), GetBlockSize());
}

void GRMBandPass::OnParamChange(int paramIdx, EParamSource source, int sampleOffset)
{
  if (source == EParamSource::kHost)
  {
    mCore.setParams(CollectParams());
  }
  else
  {
    PublishParamsToCore();
  }
}

void GRMBandPass::OnParamChangeUI(int paramIdx, EParamSource source)
{
  PublishParamsToCore();
  if (source == EParamSource::kUI)
  {
    MaybePushGestureUndo();
    MirrorLinkedParams(paramIdx);
  }
  UpdatePads();
}
#endif

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
    SendCurrentParamValuesFromDelegate();
    GetUI()->SetAllControlsDirty();
  }
#endif
  UpdatePads();
  MarkStateStable();
}

void GRMBandPass::EditCorner(int kFreq, int kBw, int cornerId, double value)
{
  PushUndo();
  const IParam* pf = GetParam(kFreq);
  const double center = pf->FromNormalized(GetParam(kFreq)->GetNormalized());
  const double bw = GetParam(kBw)->Value();
  double lowHz = center * std::pow(2., -bw / 2.);
  double highHz = center * std::pow(2., bw / 2.);
  double nc = center, nb = bw;
  switch (cornerId)
  {
    case kCornerCenter: nc = value;                                  nb = bw;    break;
    case kCornerBw:     nc = center;                                nb = value; break;
    case kCornerLow:    lowHz = value;  nc = std::sqrt(lowHz * highHz); nb = std::log2(highHz / lowHz); break;
    case kCornerHigh:   highHz = value; nc = std::sqrt(lowHz * highHz); nb = std::log2(highHz / lowHz); break;
  }
  ClampAndSet(kFreq, kBw, nc, nb);
}

void GRMBandPass::EditBand(int kFreq, int kBw, double lowNorm, double highNorm)
{
  const IParam* pf = GetParam(kFreq);
  const double lowHz = pf->FromNormalized(lowNorm);
  const double highHz = pf->FromNormalized(highNorm);
  const double center = std::sqrt(lowHz * highHz);
  const double bw = std::log2(highHz / lowHz);
  ClampAndSet(kFreq, kBw, center, bw);
}

void GRMBandPass::ClampAndSet(int kFreq, int kBw, double centerHz, double bwOct)
{
  centerHz = std::clamp(centerHz, 20., 20000.);
  bwOct    = std::clamp(bwOct, 0.05, 4.);
  double lowHz = centerHz * std::pow(2., -bwOct / 2.);
  double highHz = centerHz * std::pow(2., bwOct / 2.);
  if (lowHz < 20.)     { lowHz = 20.;   centerHz = highHz * std::pow(2., -bwOct / 2.); }
  if (highHz > 20000.) { highHz = 20000.; centerHz = lowHz * std::pow(2., bwOct / 2.); }
  SetParamFromEditor(kFreq, centerHz);
  SetParamFromEditor(kBw, bwOct);
  RefreshAfterEdit();
}

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
  if (!mUndoStack.empty() && mUndoStack.back() == s) return;
  mUndoStack.push_back(s);
  if (mUndoStack.size() > 100) mUndoStack.pop_front();
  mRedoStack.clear();
}

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

void GRMBandPass::RestoreDefault(int idx)
{
  if (idx < 0 || idx >= kNumPresets) return;
  mPresets[idx] = mDefaultSnapshot;
  if (idx == mCurrentPreset)
  {
    PushUndo();
    ApplySnapshot(mDefaultSnapshot);
  }
}

void GRMBandPass::SwapSlots(int posA, int posB)
{
  if (posA == posB) return;
  if (posA < 0 || posA >= kNumPresets || posB < 0 || posB >= kNumPresets) return;
  std::swap(mSlotNumber[posA], mSlotNumber[posB]);
  RefreshSlotLabels();
}

void GRMBandPass::RefreshSlotLabels()
{
  for (int i = 0; i < kNumPresets; ++i)
  {
    if (!mSlotButtons[i]) continue;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", mSlotNumber[i] + 1);
    mSlotButtons[i]->SetSlotLabel(buf);
  }
}

void GRMBandPass::SaveFile()
{
  if (!GetUI()) return;
  mDialogFileName.Set("GRMBandPass Presets");
  mDialogPath.Set("");
  GetUI()->PromptForFile(mDialogFileName, mDialogPath, EFileAction::Save, "json",
    [this](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength() == 0) return;
      std::string full = fileName.Get();
      if (full.size() < 5 || full.compare(full.size() - 5, 5, ".json") != 0)
        full += ".json";
      std::string err;
      WritePresetFileTo(full, err);
      if (!err.empty() && GetUI())
        GetUI()->ShowMessageBox(err.c_str(), "Save Failed", kMB_OK);
    });
}

void GRMBandPass::LoadFile()
{
  if (!GetUI()) return;
  mDialogFileName.Set("");
  GetUI()->PromptForFile(mDialogFileName, mDialogPath, EFileAction::Open, "json",
    [this](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength() == 0) return;
      std::string err;
      ReadPresetFileFrom(fileName.Get(), err);
      if (!err.empty() && GetUI())
        GetUI()->ShowMessageBox(err.c_str(), "Load Failed", kMB_OK);
    });
}

void GRMBandPass::WritePresetFileTo(const std::string& path, std::string& err)
{
  PresetFileData data;
  for (const auto& p : mPresets)
  {
    std::vector<double> vals(p.begin(), p.end());
    data.presets.push_back(std::move(vals));
  }
  const ParamSnapshot cur = Snapshot();
  data.currentValues.assign(cur.begin(), cur.end());
  data.currentPreset = mCurrentPreset;
  data.morphPos = mMorphPos;

  if (WritePresetFile(path, data, err))
    err.clear();
}

void GRMBandPass::ReadPresetFileFrom(const std::string& path, std::string& err)
{
  PresetFileData data;
  if (!ReadPresetFile(path, data, err)) return;

  if ((int) data.presets.size() != kNumPresets) { err = "Preset count mismatch (expected 24)"; return; }
  for (const auto& e : data.presets)
    if ((int) e.size() != kNumParams)           { err = "Preset parameter count mismatch (expected 11)"; return; }
  if ((int) data.currentValues.size() != kNumParams) { err = "Current values count mismatch (expected 11)"; return; }

  PushUndo();

  for (int i = 0; i < kNumPresets; ++i)
    std::copy(data.presets[i].begin(), data.presets[i].end(), mPresets[i].begin());
  mCurrentPreset = std::clamp(data.currentPreset, 0, kNumPresets - 1);

  ParamSnapshot cur {};
  std::copy(data.currentValues.begin(), data.currentValues.end(), cur.begin());
  ApplySnapshot(cur);

  mMorphPos = std::clamp(data.morphPos, 0.0, (double) (kNumQuick - 1));
  if (mMorphSlider)
  {
    mMorphSlider->SetValue((float) (mMorphPos / (kNumQuick - 1.0)));
    mMorphSlider->SetDirty(true);
  }

  for (int i = 0; i < kNumPresets; ++i)
    mSlotNumber[i] = i;
  RefreshSlotLabels();

  err.clear();
}

void GRMBandPass::OnDragBegin(int src)
{
  mDragSourceSlot = src;
  mDragTargetSlot = -1;
}

int GRMBandPass::HitTestSlot(float x, float y)
{
  for (int i = 0; i < kNumPresets; ++i)
    if (mSlotButtons[i] && mSlotButtons[i]->GetWidgetBounds().Contains(x, y))
      return i;
  return -1;
}

void GRMBandPass::OnDragMove(float x, float y)
{
  if (mDragSourceSlot < 0) return;
  int target = HitTestSlot(x, y);
  if (target == mDragSourceSlot) target = -1;
  if (target == mDragTargetSlot) return;

  if (mDragTargetSlot >= 0 && mSlotButtons[mDragTargetSlot])
    mSlotButtons[mDragTargetSlot]->SetDragTarget(false);
  mDragTargetSlot = target;
  if (mDragTargetSlot >= 0 && mSlotButtons[mDragTargetSlot])
    mSlotButtons[mDragTargetSlot]->SetDragTarget(true);
}

void GRMBandPass::OnDragDrop(int src, float x, float y)
{
  if (mDragTargetSlot >= 0 && mSlotButtons[mDragTargetSlot])
    mSlotButtons[mDragTargetSlot]->SetDragTarget(false);
  mDragTargetSlot = -1;

  const int target = HitTestSlot(x, y);
  mDragSourceSlot = -1;
  if (target >= 0 && target != src)
    SwapSlots(src, target);
}

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

void GRMBandPass::MirrorLinkedParams(int paramIdx)
{
  if (GetParam(kLink)->Value() < 0.5) return;

  int mirror;
  switch (paramIdx)
  {
    case kFreqL: mirror = kFreqR; break;
    case kBwL:   mirror = kBwR;   break;
    case kGainL: mirror = kGainR; break;
    case kFreqR: mirror = kFreqL; break;
    case kBwR:   mirror = kBwL;   break;
    case kGainR: mirror = kGainL; break;
    default: return;
  }

  if (std::fabs(GetParam(mirror)->Value() - GetParam(paramIdx)->Value()) < 1e-9) return;

  SetParamFromEditor(mirror, GetParam(paramIdx)->Value());
#if IPLUG_EDITOR
  if (GetUI())
    SendParameterValueFromDelegate(mirror, GetParam(mirror)->GetNormalized(), true);
#endif
}

ParamSnapshot GRMBandPass::InterpolatePresets(double pos)
{
  const int i0 = std::clamp(static_cast<int>(std::floor(pos)), 0, kNumQuick - 1);
  const int i1 = std::min(i0 + 1, kNumQuick - 1);
  const double t = std::clamp(pos - i0, 0.0, 1.0);

  ParamSnapshot out;
  for (int i = 0; i < kNumParams; ++i)
  {
    const IParam* p = GetParam(i);
    const int n0 = mSlotNumber[i0], n1 = mSlotNumber[i1];
    const double a = p->FromNormalized(p->ToNormalized(mPresets[n0][i]));
    const double b = p->FromNormalized(p->ToNormalized(mPresets[n1][i]));
    out[i] = a + (b - a) * t;
  }
  return out;
}

void GRMBandPass::OnMorphDrag(double normalizedPos)
{
  ApplySnapshot(InterpolatePresets(normalizedPos * (kNumQuick - 1)));
}
