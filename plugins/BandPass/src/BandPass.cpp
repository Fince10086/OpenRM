#include "BandPass.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"
#include "Theme.h"
#include "controls/FilterNodePad.h"
#include "controls/BandRangeSlider.h"
#include "controls/PresetSlotControl.h"
#include "PresetFileIO.h"

#include <cstring>
#include <cstdio>
#include <functional>
#include <chrono>
#include <cmath>
#include <algorithm>

static IVStyle MakeORMStyle()
{
  IVColorSpec colors = { COL_BG, COL_BG, COL_DIM, COL_BLACK,
                         COL_HOVER, COL_TRACK, COL_BLACK, COL_BLACK, COL_BLACK };
  const IText labelText(20, COL_DIM, "Outfit", EAlign::Center, EVAlign::Bottom);
  const IText valueText(20, COL_BLACK, "Outfit-SemiBold", EAlign::Center, EVAlign::Top);
  return IVStyle(true, true, colors, labelText, valueText,
                 true, true, false, false, 0.2f, 1.5f, 0.f, 1.f, 0.f);
}

static IVStyle MakeButtonStyle()
{
  IVColorSpec colors = { COL_BG, COL_BG, COL_BLACK, COL_BLACK,
                         COL_HOVER, COL_BG, COL_BLACK, COL_BLACK, COL_BLACK };
  const IText labelText(20, COL_BLACK, "Outfit-SemiBold", EAlign::Center, EVAlign::Middle);
  const IText valueText(20, COL_BLACK, "Outfit-SemiBold", EAlign::Center, EVAlign::Middle);
  return IVStyle(true, true, colors, labelText, valueText, true, true, false, false,
                 0.2f, 2.f, 0.f, 1.f, 0.f);
}

// Flat variants used inside merged button grids (see PresetGridFrame): no own
// border/background, just hover shade, inverted colors while active/pressed.
class FlatActionButton : public IVButtonControl
{
public:
  FlatActionButton(const IRECT& bounds, IActionFunction aF, const char* label,
                   const IVStyle& style)
  : IVButtonControl(bounds, aF, label, style) {}

  void Draw(IGraphics& g) override
  {
    const IRECT b = GetWidgetBounds();
    const bool pressed = GetValue() > 0.5;
    if (pressed || GetMouseIsOver())
      g.FillRect(pressed ? COL_BLACK : COL_HOVER, b);
    IText t = mStyle.valueText;
    t.mFGColor = pressed ? COLOR_WHITE : COL_BLACK;
    g.DrawText(t, mLabelStr.Get(), b);
  }
};

static IVButtonControl* MakeMomentary(const IRECT& r,
                                       std::function<void(IControl*)> fn,
                                       const char* label, const IVStyle& st)
{
  return new FlatActionButton(r, [fn](IControl* p) {
    fn(p);
    p->SetValue(0.0);
    p->SetDirty(false);
  }, label, st);
}

// Bandwidth is stored/displayed as a multiplier (1–31, exponential shape);
// the DSP core still works in octaves.
static double BwMultToOct(double m) { return 2. * std::log2(m); }

class PresetMorphSlider : public IVSliderControl
{
public:
  PresetMorphSlider(const IRECT& bounds, IActionFunction aF, const IVStyle& style)
  : IVSliderControl(bounds, aF, "", style, false, EDirection::Horizontal)
  {
  }

  void DrawTrack(IGraphics& g, const IRECT& filledArea) override
  {
    // Visible track extends one handle radius past each end (matches ORMSlider);
    // the min-side (left) extension is always black, the right one stays grey.
    const float cr = GetRoundedCornerRadius(mTrackBounds);
    const IRECT tb = mTrackBounds.GetHPadded(mHandleSize);
    g.FillRoundRect(COL_TRACK, tb, cr, &mBlend);
    const IRECT fill(tb.L, filledArea.T, std::max(filledArea.R, mTrackBounds.L), filledArea.B);
    g.FillRoundRect(COL_BLACK, fill, cr, &mBlend);

    const float x0 = mTrackBounds.L, w = mTrackBounds.W();
    for (int i = 0; i < kNumQuick; ++i)
    {
      const float x = x0 + w * i / (kNumQuick - 1.f);
      g.FillRect(COL_BLACK, IRECT(x - 1.f, mTrackBounds.T - 3.f, x + 1.f, mTrackBounds.T));
      g.FillRect(COL_BLACK, IRECT(x - 1.f, mTrackBounds.B, x + 1.f, mTrackBounds.B + 3.f));
    }
  }
};

// Shared outer frame for a block of preset slots: one rounded border around
// the whole grid, with plain separator lines between cells.
class PresetGridFrame : public IControl
{
public:
  PresetGridFrame(const IRECT& bounds, int cols, int rows, float cellW, float cellH)
  : IControl(bounds), mCols(cols), mRows(rows), mCellW(cellW), mCellH(cellH)
  {
    // Purely decorative overlay: let all clicks pass through to the slots.
    SetIgnoreMouse(true);
  }

  void Draw(IGraphics& g) override
  {
    // Match the corner radius of individual buttons:
    // IVStyle roundness (0.2) * cell height / 2.
    const float cr = 0.2f * (mCellH / 2.f);
    g.DrawRoundRect(COL_BLACK, mRECT, cr, nullptr, 2.f);
    for (int c = 1; c < mCols; ++c)
    {
      const float x = mRECT.L + c * mCellW;
      g.DrawLine(COL_BLACK, x, mRECT.T, x, mRECT.B, nullptr, 1.5f);
    }
    for (int r = 1; r < mRows; ++r)
    {
      const float y = mRECT.T + r * mCellH;
      g.DrawLine(COL_BLACK, mRECT.L, y, mRECT.R, y, nullptr, 1.5f);
    }
  }

private:
  int mCols, mRows;
  float mCellW, mCellH;
};

class ORMSlider : public IVSliderControl
{
public:
  ORMSlider(const IRECT& bounds, int paramIdx, const char* label, const IVStyle& style,
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
    // Extend the visible track by one handle radius on both ends, so its
    // edges line up with the outermost edge of the handle (the handle's
    // centre travel logic is untouched).
    const bool horiz = (mDirection == EDirection::Horizontal);
    const float cr = GetRoundedCornerRadius(mTrackBounds);
    const IRECT tb = horiz ? mTrackBounds.GetHPadded(mHandleSize)
                           : mTrackBounds.GetVPadded(mHandleSize);
    g.FillRoundRect(COL_TRACK, tb, cr, &mBlend);

    // The min-side extension is always part of the filled (black) region;
    // the max-side extension stays grey.
    const IRECT fill = horiz
      ? IRECT(tb.L, filledArea.T, std::max(filledArea.R, mTrackBounds.L), filledArea.B)
      : IRECT(filledArea.L, filledArea.T, filledArea.R, tb.B);
    g.FillRoundRect(COL_BLACK, fill, cr, &mBlend);
  }

  void DrawHandle(IGraphics& g, const IRECT& bounds) override
  {
    const float cx = bounds.MW(), cy = bounds.MH();
    const float r  = bounds.W() * 0.5f;
    g.FillCircle(COLOR_WHITE, cx, cy, r);
    g.DrawCircle(COL_BLACK, cx, cy, r - 0.75f, nullptr, 1.5f);
  }

protected:
  static constexpr float kHeaderH = 26.f;
  static constexpr float kHeaderW = 26.f;

  virtual IRECT ValueRect() const
  {
    if (mDirection == EDirection::Horizontal)
      return IRECT(mRECT.L, mRECT.T, mRECT.R, mRECT.T + kHeaderH);
    return IRECT(mRECT.L, mRECT.T, mRECT.L + kHeaderW + 4.f, mRECT.T + 44.f);
  }

  virtual void DrawHeader(IGraphics& g, float rot)
  {
    WDL_String ds;
    if (GetParam()) GetParam()->GetDisplay(ds, false);

    if (rot == 0.f)
    {
      const IRECT hdr(mRECT.L, mRECT.T, mRECT.R, mRECT.T + kHeaderH);
      g.DrawText(IText(20, COL_BLACK, "Outfit-SemiBold", EAlign::Near, EVAlign::Middle),
                 mHeaderLabel.Get(), IRECT(hdr.L, hdr.T, hdr.MW(), hdr.B));
      g.DrawText(IText(20, COL_DIM, "Outfit", EAlign::Far, EVAlign::Middle),
                 ds.Get(), IRECT(hdr.MW(), hdr.T, hdr.R, hdr.B));
    }
    else
    {
      const IRECT hdr(mRECT.L, mRECT.T, mRECT.L + kHeaderW, mRECT.B);
      g.DrawText(IText(20, COL_BLACK, "Outfit-SemiBold", EAlign::Near, EVAlign::Bottom, rot),
                 mHeaderLabel.Get(), hdr);
      g.DrawText(IText(20, COL_DIM, "Outfit", EAlign::Far, EVAlign::Top, rot),
                 ds.Get(), hdr);
    }
  }

  WDL_String mHeaderLabel;
};

// Vertical gain slider variant: track on the left, text column on the right —
// numeric value top-right, GAIN (L/R) label bottom-right. Both texts are
// rotated so their bottoms face left (they read top-to-bottom).
class GainSlider : public ORMSlider
{
public:
  GainSlider(const IRECT& bounds, int paramIdx, const char* label, const IVStyle& style)
  : ORMSlider(bounds, paramIdx, label, style, EDirection::Vertical) {}

protected:
  IRECT TextRect() const { return IRECT(mRECT.R - kHeaderW, mRECT.T, mRECT.R, mRECT.B); }

  void OnResize() override
  {
    mWidgetBounds = mRECT.GetReducedFromRight(kHeaderW);
    mTrackBounds  = mWidgetBounds.GetPadded(-mHandleSize)
                                 .GetMidHPadded(mTrackSize);
    SetTargetRECT(mRECT);
    mValueBounds = IRECT();
    SetDirty(false);
  }

  IRECT ValueRect() const override
  {
    const IRECT hdr = TextRect();
    return IRECT(hdr.L - 4.f, hdr.T, hdr.R, hdr.MH());
  }

  void DrawHeader(IGraphics& g, float) override
  {
    WDL_String ds;
    if (GetParam()) GetParam()->GetDisplay(ds, false);

    const IRECT hdr = TextRect();
    g.DrawText(IText(20, COL_DIM, "Outfit", EAlign::Center, EVAlign::Middle, 90.f),
               ds.Get(), IRECT(hdr.L, hdr.T, hdr.R, hdr.MH()));
    g.DrawText(IText(20, COL_BLACK, "Outfit-SemiBold", EAlign::Center, EVAlign::Middle, 90.f),
               mHeaderLabel.Get(), IRECT(hdr.L, hdr.MH(), hdr.R, hdr.B));
  }
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

// Flat variant of InvertToggleControl for merged button grids: black cell
// with white text while on, hover shade while off.
class FlatToggleControl : public InvertToggleControl
{
public:
  using InvertToggleControl::InvertToggleControl;

  void Draw(IGraphics& g) override
  {
    const IRECT b = GetWidgetBounds();
    const bool on = GetValue() > 0.5;
    if (on || GetMouseIsOver())
      g.FillRect(on ? COL_BLACK : COL_HOVER, b);
    DrawValue(g, false);
  }
};

ORMBandPass::ORMBandPass(const InstanceInfo& info)
: Plugin(info, MakeConfig(kNumParams, 1))
{
  GetParam(kFreqL)->InitDouble("FreqL", 1000., 20., 20000., 0.01, "Hz", 0, "", IParam::ShapeExp());
  GetParam(kBwL)  ->InitDouble("BW L", 1.41, 1., 31., 0.01, "x", 0, "", IParam::ShapeExp());
  GetParam(kGainL)->InitDouble("Gain L", 1., 0., 2., 0.01, "");
  GetParam(kFreqR)->InitDouble("FreqR", 1000., 20., 20000., 0.01, "Hz", 0, "", IParam::ShapeExp());
  GetParam(kBwR)  ->InitDouble("BW R", 1.41, 1., 31., 0.01, "x", 0, "", IParam::ShapeExp());
  GetParam(kGainR)->InitDouble("Gain R", 1., 0., 2., 0.01, "");
  GetParam(kLink) ->InitBool("Link", false);
  GetParam(kMix)  ->InitDouble("Mix", 1., 0., 1., 0.01, "");
  GetParam(kAgOn) ->InitBool("Agitation", false);
  GetParam(kAgAmount)->InitDouble("Ag Amount", 0.1, 0., 1., 0.01, "");
  GetParam(kAgRate)->InitDouble("Ag Speed", 1., 0.01, 60., 0.01, "s", 0, "", IParam::ShapeExp());

  for (int i = 0; i < kNumPresets; ++i)
  {
    mPresets[i] = Snapshot();
    mSlotNumber[i] = i;
  }

  mDefaultSnapshot = Snapshot();
  mStableSnapshot  = Snapshot();

  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 500.;  s[kBwL] = 1.07; s[kFreqR] = 500.; s[kBwR] = 1.07; s[kLink] = 1.;
    mPresets[1] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 2000.; s[kBwL] = 2.83; s[kFreqR] = 2000.; s[kBwR] = 2.83; s[kLink] = 1.;
    mPresets[2] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 400.; s[kBwL] = 1.11; s[kFreqR] = 4000.; s[kBwR] = 1.69;
    mPresets[3] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 3000.; s[kBwL] = 1.19; s[kFreqR] = 3000.; s[kBwR] = 1.19; s[kLink] = 1.;
    s[kAgOn] = 1.; s[kAgAmount] = 0.3; s[kAgRate] = 0.25;
    mPresets[4] = s;
  }
  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 150.; s[kBwL] = 2.38; s[kFreqR] = 150.; s[kBwR] = 2.38; s[kLink] = 1.;
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

    const IVStyle style   = MakeORMStyle();
    const IVStyle btnStyle= MakeButtonStyle();
    IVStyle toggleStyle = btnStyle;
    toggleStyle.showLabel = false;
    toggleStyle.showValue = false;

    constexpr float kCol1X     = 740.f;
    constexpr float kCol2X     = 824.f;
    constexpr float kBtnW      = 72.f;
    constexpr float kBtnH      = 30.f;
    constexpr float kSlotPitch = 34.f;
    constexpr float kSliderH   = 42.f;
    constexpr float kPanelR    = kCol2X + kBtnW;

    auto padHooks = [&](int kF, int kB) -> FilterNodePad::Hooks {
      return FilterNodePad::Hooks{
        [this, kF, kB](int id, double v) { EditCorner(kF, kB, id, v); },
      };
    };

    auto bandHooks = [&](int kF, int kB) -> BandRangeSlider::Hooks {
      return BandRangeSlider::Hooks{
        [this] { MaybePushGestureUndo(); },
        [this, kF, kB](int id, double v) { EditCorner(kF, kB, id, v); },
        [this, kF, kB](double lN, double hN) { EditBand(kF, kB, lN, hN); },
      };
    };

    mPadL = new FilterNodePad(IRECT(56, 38, 668, 218), { kFreqL, kBwL }, "LEFT", style, padHooks(kFreqL, kBwL));
    pGraphics->AttachControl(mPadL);
    mBandL = new BandRangeSlider(IRECT(56, 224, 668, 270), { kFreqL, kBwL }, bandHooks(kFreqL, kBwL));
    pGraphics->AttachControl(mBandL);

    mPadR = new FilterNodePad(IRECT(56, 296, 668, 476), { kFreqR, kBwR }, "RIGHT", style, padHooks(kFreqR, kBwR));
    pGraphics->AttachControl(mPadR);
    mBandR = new BandRangeSlider(IRECT(56, 482, 668, 528), { kFreqR, kBwR }, bandHooks(kFreqR, kBwR));
    pGraphics->AttachControl(mBandR);

    pGraphics->AttachControl(new GainSlider(IRECT(672, 38, 730, 218), kGainL, "GAIN L", style));
    pGraphics->AttachControl(new GainSlider(IRECT(672, 296, 730, 476), kGainR, "GAIN R", style));

    pGraphics->AttachControl(new ITextControl(IRECT(kCol1X, 38, 1050, 60), "PRESETS",
      IText(20, COL_BLACK, "Outfit-Bold", EAlign::Near, EVAlign::Middle)));

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

    for (int r = 0; r < 4; ++r)
    {
      for (int c = 0; c < 4; ++c)
      {
        const int pos = kNumBottom + r * 4 + c;
        char label[8];
        snprintf(label, 8, "%d", mSlotNumber[pos] + 1);
        PresetSlotControl* btn = new PresetSlotControl(
          IRECT(kCol1X + c * 39, 66 + r * 32,
                kCol1X + c * 39 + 39, 66 + r * 32 + 32),
          makeSlotHooks(pos), label, btnStyle);
        btn->SetFlatGrid(true);
        mSlotButtons[pos] = btn;
        pGraphics->AttachControl(btn);
      }
    }
    pGraphics->AttachControl(new PresetGridFrame(IRECT(kCol1X, 66, kPanelR, 66 + 128), 4, 4, 39.f, 32.f));

    pGraphics->AttachControl(new ITextControl(IRECT(kCol1X, 202, 1050, 230), "AGITATION",
      IText(20, COL_BLACK, "Outfit-Bold", EAlign::Near, EVAlign::Middle)));
    pGraphics->AttachControl(new FlatToggleControl(IRECT(kCol1X + 116, 203, kPanelR, 229), kAgOn, " ", toggleStyle, "OFF", "ON"));
    pGraphics->AttachControl(new ORMSlider(IRECT(kCol1X, 236, kPanelR, 278), kAgAmount, "AMP", style, EDirection::Horizontal));
    pGraphics->AttachControl(new ORMSlider(IRECT(kCol1X, 284, kPanelR, 326), kAgRate, "SPEED", style, EDirection::Horizontal));

    pGraphics->AttachControl(MakeMomentary(IRECT(kCol1X, 352, kCol1X + 78, 382), [this](IControl*) { CopyLtoR(); }, "L->R", btnStyle));
    pGraphics->AttachControl(MakeMomentary(IRECT(kCol1X + 78, 352, kPanelR, 382), [this](IControl*) { CopyRtoL(); }, "R->L", btnStyle));
    pGraphics->AttachControl(new FlatToggleControl(IRECT(kCol1X, 382, kCol1X + 78, 412), kLink, " ", toggleStyle, "LINK", "LINK"));
    pGraphics->AttachControl(MakeMomentary(IRECT(kCol1X + 78, 382, kPanelR, 412), [this](IControl*) { FlipLR(); }, "FLIP", btnStyle));
    pGraphics->AttachControl(new ORMSlider(IRECT(kCol1X, 424, kPanelR, 466), kMix, "MIX", style, EDirection::Horizontal));

    pGraphics->AttachControl(MakeMomentary(IRECT(kCol1X, 474, kCol1X + 78, 504), [this](IControl*) { Undo(); }, "UNDO", btnStyle));
    pGraphics->AttachControl(MakeMomentary(IRECT(kCol1X + 78, 474, kPanelR, 504), [this](IControl*) { Redo(); }, "REDO", btnStyle));
    pGraphics->AttachControl(MakeMomentary(IRECT(kCol1X, 504, kCol1X + 78, 534), [this](IControl*) { SaveFile(); }, "SAVE", btnStyle));
    pGraphics->AttachControl(MakeMomentary(IRECT(kCol1X + 78, 504, kPanelR, 534), [this](IControl*) { LoadFile(); }, "LOAD", btnStyle));
    pGraphics->AttachControl(new PresetGridFrame(IRECT(kCol1X, 352, kPanelR, 412), 2, 2, 78.f, 30.f));
    pGraphics->AttachControl(new PresetGridFrame(IRECT(kCol1X, 474, kPanelR, 534), 2, 2, 78.f, 30.f));

    for (int i = 0; i < kNumBottom; ++i)
    {
      char label[8];
      snprintf(label, 8, "%d", mSlotNumber[i] + 1);
      // Same cell width as the PRESETS grid. Align with the morph slider's
      // tick marks: its track is inset by the handle radius on both sides and
      // carries kNumQuick evenly spaced ticks (both ends included).
      constexpr float kHandleInset = 8.f;
      const float trackL = 56.f + kHandleInset;
      const float trackR = 668.f - kHandleInset;
      const float tick = trackL + (trackR - trackL) * i / (kNumQuick - 1.f);
      float l = tick - 19.5f;                       // centred on the tick
      if (i == 0) l = tick;                         // first: left-aligned
      else if (i == kNumBottom - 1) l = tick - 39.f; // last: right-aligned
      PresetSlotControl* btn = new PresetSlotControl(
        IRECT(l, 560, l + 39.f, 590),
        makeSlotHooks(i), label, btnStyle);
      mSlotButtons[i] = btn;
      pGraphics->AttachControl(btn);
    }

    mMorphSlider = new PresetMorphSlider(IRECT(56, 600, 668, 624),
      [this](IControl* pCtrl) {
        MaybePushGestureUndo();
        mMorphPos = pCtrl->GetValue(0) * (kNumQuick - 1.0);
        OnMorphDrag(pCtrl->GetValue(0));
      }, btnStyle);
    pGraphics->AttachControl(mMorphSlider);

    pGraphics->AttachControl(new ITextControl(IRECT(kCol1X, 552, kCol1X + 120, 586), "ORM",
      IText(32, COL_BLACK, "Outfit-Bold", EAlign::Near, EVAlign::Bottom)));
    pGraphics->AttachControl(new ITextControl(IRECT(kCol1X, 584, 1060, 618), "BandPass",
      IText(32, COL_BLACK, "Outfit-Bold", EAlign::Near, EVAlign::Middle)));
    pGraphics->AttachControl(new ITextControl(IRECT(kCol1X + 92, 552, 1060, 586), "v" PLUG_VERSION_STR,
      IText(20, COL_FAINT, "Outfit", EAlign::Near, EVAlign::Bottom)));

    pGraphics->EnableTooltips(true);
    UpdatePads();
  };
#endif
}

#if IPLUG_DSP
void ORMBandPass::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  orm::BandPassCore::Params p;
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

void ORMBandPass::OnReset()
{
  mCore.setParams(CollectParams());
  mCore.prepare(GetSampleRate(), GetBlockSize());
}

void ORMBandPass::OnParamChange(int paramIdx, EParamSource source, int sampleOffset)
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

void ORMBandPass::OnParamChangeUI(int paramIdx, EParamSource source)
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

orm::BandPassCore::Params ORMBandPass::CollectParams() const
{
  orm::BandPassCore::Params p;
  p.freqL  = GetParam(kFreqL)->Value();
  p.bwL    = BwMultToOct(GetParam(kBwL)->Value());
  p.gainL  = static_cast<float>(GetParam(kGainL)->Value());
  p.freqR  = GetParam(kFreqR)->Value();
  p.bwR    = BwMultToOct(GetParam(kBwR)->Value());
  p.gainR  = static_cast<float>(GetParam(kGainR)->Value());
  p.linked = GetParam(kLink)->Value() > 0.5;
  p.mix    = static_cast<float>(GetParam(kMix)->Value());
  p.agOn   = GetParam(kAgOn)->Value() > 0.5;
  p.agAmount = static_cast<float>(GetParam(kAgAmount)->Value());
  p.agRate = 1.0 / GetParam(kAgRate)->Value(); // UI is seconds (period), core expects Hz
  return p;
}

void ORMBandPass::PublishParamsToCore()
{
  mParamMailbox.publish(CollectParams());
}

void ORMBandPass::SetParamFromEditor(int idx, double value)
{
  GetParam(idx)->Set(value);
  InformHostOfParamChange(idx, GetParam(idx)->GetNormalized());
  PublishParamsToCore();
}

void ORMBandPass::RefreshAfterEdit()
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

void ORMBandPass::EditCorner(int kFreq, int kBw, int cornerId, double value)
{
  PushUndo();
  const IParam* pf = GetParam(kFreq);
  const double center = pf->FromNormalized(GetParam(kFreq)->GetNormalized());
  const double bw = GetParam(kBw)->Value();
  double lowHz = center / bw;
  double highHz = center * bw;
  double nc = center, nb = bw;
  switch (cornerId)
  {
    case kCornerCenter: nc = value;     break;
    case kCornerBw:     nb = value;     break;
    case kCornerLow:    lowHz = value;  nc = std::sqrt(lowHz * highHz); nb = std::sqrt(highHz / lowHz); break;
    case kCornerHigh:   highHz = value; nc = std::sqrt(lowHz * highHz); nb = std::sqrt(highHz / lowHz); break;
  }
  ClampAndSet(kFreq, kBw, nc, nb);
}

void ORMBandPass::EditBand(int kFreq, int kBw, double lowNorm, double highNorm)
{
  const IParam* pf = GetParam(kFreq);
  const double lowHz = pf->FromNormalized(lowNorm);
  const double highHz = pf->FromNormalized(highNorm);
  const double center = std::sqrt(lowHz * highHz);
  // Edges are center/bw and center*bw, so high/low == bw^2.
  const double bw = std::sqrt(highHz / lowHz);
  ClampAndSet(kFreq, kBw, center, bw);
}

void ORMBandPass::ClampAndSet(int kFreq, int kBw, double centerHz, double bw)
{
  centerHz = std::clamp(centerHz, 20., 20000.);
  bw       = std::clamp(bw, 1., 31.);
  double lowHz = centerHz / bw;
  double highHz = centerHz * bw;
  if (lowHz < 20.)     { lowHz = 20.;   centerHz = highHz / bw; }
  if (highHz > 20000.) { highHz = 20000.; centerHz = lowHz * bw; }
  SetParamFromEditor(kFreq, centerHz);
  SetParamFromEditor(kBw, bw);
  RefreshAfterEdit();
}

void ORMBandPass::UpdatePads()
{
  if (mPadL)
  {
    mPadL->SetValueFromDelegate(GetParam(kFreqL)->GetNormalized(), 0);
    mPadL->SetValueFromDelegate(GetParam(kBwL)->GetNormalized(), 1);
    mPadL->SetDirty(false);
  }
  if (mBandL)
  {
    mBandL->SetValueFromDelegate(GetParam(kFreqL)->GetNormalized(), 0);
    mBandL->SetValueFromDelegate(GetParam(kBwL)->GetNormalized(), 1);
    mBandL->SetDirty(false);
  }
  if (mPadR)
  {
    mPadR->SetValueFromDelegate(GetParam(kFreqR)->GetNormalized(), 0);
    mPadR->SetValueFromDelegate(GetParam(kBwR)->GetNormalized(), 1);
    mPadR->SetDirty(false);
  }
  if (mBandR)
  {
    mBandR->SetValueFromDelegate(GetParam(kFreqR)->GetNormalized(), 0);
    mBandR->SetValueFromDelegate(GetParam(kBwR)->GetNormalized(), 1);
    mBandR->SetDirty(false);
  }
}

ParamSnapshot ORMBandPass::Snapshot() const
{
  ParamSnapshot s;
  for (int i = 0; i < kNumParams; ++i)
    s[i] = GetParam(i)->Value();
  return s;
}

void ORMBandPass::ApplySnapshot(const ParamSnapshot& s)
{
  for (int i = 0; i < kNumParams; ++i)
    SetParamFromEditor(i, s[i]);
  RefreshAfterEdit();
}

void ORMBandPass::PushUndo()
{
  PushUndoSnapshot(Snapshot());
}

void ORMBandPass::PushUndoSnapshot(const ParamSnapshot& s)
{
  if (!mUndoStack.empty() && mUndoStack.back() == s) return;
  mUndoStack.push_back(s);
  if (mUndoStack.size() > 100) mUndoStack.pop_front();
  mRedoStack.clear();
}

static constexpr double kGestureGapSec = 0.4;

void ORMBandPass::MaybePushGestureUndo()
{
  using namespace std::chrono;
  const double now = duration<double>(steady_clock::now().time_since_epoch()).count();
  if (now - mLastUIChangeTime > kGestureGapSec)
    PushUndoSnapshot(mStableSnapshot);
  mLastUIChangeTime = now;
  mGesturePending = true;
}

void ORMBandPass::OnIdle()
{
  using namespace std::chrono;
  const double now = duration<double>(steady_clock::now().time_since_epoch()).count();
  if (mGesturePending && now - mLastUIChangeTime > kGestureGapSec)
  {
    mStableSnapshot = Snapshot();
    mGesturePending = false;
  }
}

void ORMBandPass::MarkStateStable()
{
  mStableSnapshot = Snapshot();
  mGesturePending = false;
}

void ORMBandPass::Undo()
{
  if (mUndoStack.empty()) return;
  mRedoStack.push_back(Snapshot());
  const ParamSnapshot s = mUndoStack.back();
  mUndoStack.pop_back();
  ApplySnapshot(s);
}

void ORMBandPass::Redo()
{
  if (mRedoStack.empty()) return;
  mUndoStack.push_back(Snapshot());
  const ParamSnapshot s = mRedoStack.back();
  mRedoStack.pop_back();
  ApplySnapshot(s);
}

void ORMBandPass::SaveToSlot(int idx)
{
  if (idx < 0 || idx >= kNumPresets) return;
  mPresets[idx] = Snapshot();
}

void ORMBandPass::LoadSlot(int idx)
{
  if (idx < 0 || idx >= kNumPresets) return;
  PushUndo();
  mCurrentPreset = idx;
  ApplySnapshot(mPresets[idx]);
}

void ORMBandPass::RestoreDefault(int idx)
{
  if (idx < 0 || idx >= kNumPresets) return;
  mPresets[idx] = mDefaultSnapshot;
  if (idx == mCurrentPreset)
  {
    PushUndo();
    ApplySnapshot(mDefaultSnapshot);
  }
}

void ORMBandPass::SwapSlots(int posA, int posB)
{
  if (posA == posB) return;
  if (posA < 0 || posA >= kNumPresets || posB < 0 || posB >= kNumPresets) return;
  std::swap(mSlotNumber[posA], mSlotNumber[posB]);
  RefreshSlotLabels();
}

void ORMBandPass::RefreshSlotLabels()
{
  for (int i = 0; i < kNumPresets; ++i)
  {
    if (!mSlotButtons[i]) continue;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", mSlotNumber[i] + 1);
    mSlotButtons[i]->SetSlotLabel(buf);
  }
}

void ORMBandPass::SaveFile()
{
  if (!GetUI()) return;
  mDialogFileName.Set("ORMBandPass Presets");
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

void ORMBandPass::LoadFile()
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

void ORMBandPass::WritePresetFileTo(const std::string& path, std::string& err)
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

void ORMBandPass::ReadPresetFileFrom(const std::string& path, std::string& err)
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

void ORMBandPass::OnDragBegin(int src)
{
  mDragSourceSlot = src;
  mDragTargetSlot = -1;
}

int ORMBandPass::HitTestSlot(float x, float y)
{
  for (int i = 0; i < kNumPresets; ++i)
    if (mSlotButtons[i] && mSlotButtons[i]->GetWidgetBounds().Contains(x, y))
      return i;
  return -1;
}

void ORMBandPass::OnDragMove(float x, float y)
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

void ORMBandPass::OnDragDrop(int src, float x, float y)
{
  if (mDragTargetSlot >= 0 && mSlotButtons[mDragTargetSlot])
    mSlotButtons[mDragTargetSlot]->SetDragTarget(false);
  mDragTargetSlot = -1;

  const int target = HitTestSlot(x, y);
  mDragSourceSlot = -1;
  if (target >= 0 && target != src)
    SwapSlots(src, target);
}

void ORMBandPass::CopyLtoR()
{
  PushUndo();
  SetParamFromEditor(kFreqR, GetParam(kFreqL)->Value());
  SetParamFromEditor(kBwR,   GetParam(kBwL)->Value());
  SetParamFromEditor(kGainR, GetParam(kGainL)->Value());
  RefreshAfterEdit();
}

void ORMBandPass::CopyRtoL()
{
  PushUndo();
  SetParamFromEditor(kFreqL, GetParam(kFreqR)->Value());
  SetParamFromEditor(kBwL,   GetParam(kBwR)->Value());
  SetParamFromEditor(kGainL, GetParam(kGainR)->Value());
  RefreshAfterEdit();
}

void ORMBandPass::FlipLR()
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

void ORMBandPass::MirrorLinkedParams(int paramIdx)
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

ParamSnapshot ORMBandPass::InterpolatePresets(double pos)
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

void ORMBandPass::OnMorphDrag(double normalizedPos)
{
  ApplySnapshot(InterpolatePresets(normalizedPos * (kNumQuick - 1)));
}
