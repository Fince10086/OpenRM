#pragma once

#include "ICornerResizerControl.h"
#include "../Theme.h"

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class ThemeCornerResizer : public ICornerResizerControl {
public:
  ThemeCornerResizer(const IRECT &graphicsBounds) : ICornerResizerControl(graphicsBounds, 20.f) {}

  void Draw(IGraphics &g) override {
    const IColor col = mDragging ? COL_700() : GetMouseIsOver() ? COL_900() : COL_500();
    g.FillTriangle(col, mRECT.L, mRECT.B, mRECT.R, mRECT.T, mRECT.R, mRECT.B);
  }

  void OnMouseDown(float x, float y, const IMouseMod &mod) override {
    mDragging = true;
    ICornerResizerControl::OnMouseDown(x, y, mod);
  }

  void OnMouseUp(float x, float y, const IMouseMod &mod) override {
    mDragging = false;
    IControl::OnMouseUp(x, y, mod);
  }

private:
  bool mDragging = false;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
