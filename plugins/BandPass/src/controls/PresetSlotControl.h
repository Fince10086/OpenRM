#pragma once
#include "IControls.h"

#include <cmath>
#include <functional>
#include <string>
#include <utility>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class PresetSlotControl : public IVButtonControl
{
public:
  struct Hooks
  {
    std::function<void()> onLoad;
    std::function<void()> onSaveHere;
    std::function<void()> onRestoreDefault;
    std::function<void()> onDragBegin;
    std::function<void(float, float)> onDragMove;
    std::function<void(float, float)> onDragDrop;
    std::function<std::string()> getTooltipPrefix;
  };

  PresetSlotControl(const IRECT& bounds, Hooks hooks, const char* label, const IVStyle& style)
    : IVButtonControl(bounds, nullptr, label, style)
    , mHooks(std::move(hooks))
  {
    SetTooltip(BuildTooltip().c_str());
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    if (mod.L)
    {
      if (mod.R)
      {
        if (mHooks.onSaveHere) mHooks.onSaveHere();
      }
      else if (mod.A)
      {
        if (mHooks.onRestoreDefault) mHooks.onRestoreDefault();
      }
      else
      {
        mPotentialDrag = true;
        mDragging = false;
        mDownX = x; mDownY = y;
        SetValue(1.0); SetDirty();
        return;
      }
      SetValue(0.0); SetDirty(false);
      return;
    }
    if (mod.R)
      ShowPopupMenu(x, y);
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override
  {
    if (!mPotentialDrag) return;

    if (!mDragging &&
        (std::fabs(x - mDownX) + std::fabs(y - mDownY) > kDragThreshold))
    {
      mDragging = true;
      if (mHooks.onDragBegin) mHooks.onDragBegin();
    }
    if (mDragging && mHooks.onDragMove)
      mHooks.onDragMove(x, y);
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    if (!mPotentialDrag) return;
    mPotentialDrag = false;

    if (mDragging)
    {
      mDragging = false;
      if (mHooks.onDragDrop) mHooks.onDragDrop(x, y);
    }
    else
    {
      if (mHooks.onLoad) mHooks.onLoad();
    }
    SetValue(0.0);
    SetDirty(false);
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override
  {
    const std::string t = BuildTooltip();
    if (t != GetTooltip())
    {
      SetTooltip(t.c_str());
      if (GetUI()) GetUI()->UpdateTooltips();
    }
    IControl::OnMouseOver(x, y, mod);
  }

  void SetDragTarget(bool on)
  {
    if (mDragTarget == on) return;
    mDragTarget = on;
    SetDirty();
  }

  void SetSlotLabel(const char* s)
  {
    SetLabelStr(s);
    SetDirty(false);
  }

  void Draw(IGraphics& g) override
  {
    IVButtonControl::Draw(g);
    if (mDragging)
    {
      g.FillRect(IColor(70, 0, 0, 0), GetWidgetBounds());
    }
    if (mDragTarget)
    {
      g.DrawRect(IColor(255, 0, 0, 0), GetWidgetBounds(), nullptr, 3.f);
    }
  }

private:
  static constexpr float kDragThreshold = 8.f;

  std::string BuildTooltip() const
  {
    std::string s = mHooks.getTooltipPrefix ? mHooks.getTooltipPrefix() : std::string();
    if (!s.empty()) s += "\n";
    s += "Drag onto another slot to swap · ⌘Click: save here · ⌥Click: restore default";
    return s;
  }

  void ShowPopupMenu(float x, float y)
  {
    mMenu.Clear();
    mMenu.AddItem("Save Here");
    mMenu.AddItem("Restore Default");
    mMenu.SetFunction([this](IPopupMenu* p) {
      switch (p->GetChosenItemIdx())
      {
        case 0:  if (mHooks.onSaveHere)      mHooks.onSaveHere();      break;
        case 1:  if (mHooks.onRestoreDefault) mHooks.onRestoreDefault(); break;
        default: break;
      }
    });
    GetUI()->CreatePopupMenu(*this, mMenu, x, y, kNoValIdx);
  }

  Hooks mHooks;
  bool mPotentialDrag = false;
  bool mDragging = false;
  bool mDragTarget = false;
  float mDownX = 0.f, mDownY = 0.f;
  IPopupMenu mMenu;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
