#pragma once

#include "IControls.h"
#include "../Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <utility>
#include <cstring>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class SectionTitleControl : public ITextControl {
public:
  SectionTitleControl(const IRECT &bounds, const char *str, const IText &text, int colorStep = 0, int fontStep = 2)
      : ITextControl(bounds, str, text), mColorStep(colorStep), mFontStep(fontStep) {}

  void Draw(IGraphics &g) override {
    IText t = mText;
    t.mFGColor = mColorStep == 0 ? COL_900() : COL_500();
    strcpy(t.mFont, mFontStep == 0 ? kFontRegular : mFontStep == 1 ? kFontSemiBold : kFontBold);
    g.DrawText(t, mStr.Get(), mRECT, &mBlend);
  }

private:
  int mColorStep = 0;
  int mFontStep = 2;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
