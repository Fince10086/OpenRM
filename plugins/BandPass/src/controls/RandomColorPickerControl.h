#pragma once

#include "IControls.h"
#include "../Theme.h"
#include "UiUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <utility>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class RandomColorPickerControl : public IControl {
public:
  RandomColorPickerControl(const IRECT &bounds, std::function<void(int)> onPick)
      : IControl(bounds), mOnPick(std::move(onPick)) {}

  void SetColor(int idx) {
    mColorIdx = std::clamp(idx, 0, kNumRandomColors - 1);
    SetDirty(false);
  }

  void Draw(IGraphics &g) override { g.FillRect(RandomColor(mColorIdx), mRECT); }

  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
    if (!GetUI() || !mOnPick)
      return;
    OpenColorPopup(*GetUI(), *this, mMenu, mRECT, mColorIdx, [this](int idx) { mOnPick(idx); });
  }

private:
  std::function<void(int)> mOnPick;
  IPopupMenu mMenu;
  int mColorIdx = 0;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
