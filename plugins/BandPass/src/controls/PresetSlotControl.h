#pragma once
// ============================================================================
// PresetSlotControl.h — 预设槽按钮 (IVButtonControl 子类)
//
// 交互设计:
//   左键单击      → 加载该预设
//   左键拖拽      → 拖到另一个槽上松手 = 两个槽内容互换
//   ⌘+左键        → 把当前设置保存到该槽   (macOS: Cmd 键落在 mod.R, 但 L=true)
//   ⌥+左键        → 恢复该槽为出厂默认     (macOS: Opt/Alt 落在 mod.A)
//   右键          → 弹出菜单: Save Here / Restore Default
//   悬停          → 英文 tooltip (拖动替换提示, 不写"点击加载")
//
// ⚠ macOS 键位陷阱: 左键事件里 Cmd 键被映射到 mod.R 字段 (L=true, R=true),
//   而右键事件是 (L=false, R=true)。两者共用 R 字段, 必须先用 L 分流,
//   不能只判断 R。Windows 上无此问题 (Cmd 不存在), 但逻辑同样正确。
// ============================================================================
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
    std::function<void()> onLoad;             // 单击加载
    std::function<void()> onSaveHere;         // ⌘+点击 / 菜单 "Save Here"
    std::function<void()> onRestoreDefault;   // ⌥+点击 / 菜单 "Restore Default"
    std::function<void()> onDragBegin;        // 进入拖拽 (源槽高亮等)
    std::function<void(float, float)> onDragMove;  // 拖拽移动 (更新目标高亮)
    std::function<void(float, float)> onDragDrop;  // 松手 (插件层判定落点并交换)
    std::function<std::string()> getTooltipPrefix; // tooltip 首行 (如 "Preset 9")
  };

  PresetSlotControl(const IRECT& bounds, Hooks hooks, const char* label, const IVStyle& style)
    : IVButtonControl(bounds, nullptr, label, style)
    , mHooks(std::move(hooks))
  {
    SetTooltip(BuildTooltip().c_str());
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    // 用 L 分流 Cmd 与右键 (见文件头注释) —— 必须保留 L 判断
    if (mod.L)
    {
      if (mod.R)            // ⌘+左键: 立即保存, 不进入拖拽
      {
        if (mHooks.onSaveHere) mHooks.onSaveHere();
      }
      else if (mod.A)       // ⌥+左键: 立即恢复默认
      {
        if (mHooks.onRestoreDefault) mHooks.onRestoreDefault();
      }
      else
      {
        // 普通左键: 进入"潜在拖拽", 按下态保持到 OnMouseUp 决定 (单击 vs 拖拽)
        mPotentialDrag = true;
        mDragging = false;
        mDownX = x; mDownY = y;
        SetValue(1.0); SetDirty();
        return;
      }
      SetValue(0.0); SetDirty(false);   // ⌘/⌥: 立即复位按下态 (规避按钮停留在浅色态)
      return;
    }
    if (mod.R)
      ShowPopupMenu(x, y);
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override
  {
    if (!mPotentialDrag) return;

    // 超过阈值进入拖拽模式 (之后松手 = 交换, 不再触发加载)
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
      if (mHooks.onLoad) mHooks.onLoad();   // 未拖动 = 单击加载
    }
    SetValue(0.0);   // 复位按下态 (规避按钮停留在浅色态)
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

  // 外部设置"拖拽目标"高亮状态 (插件层在 OnDragMove 中更新)
  void SetDragTarget(bool on)
  {
    if (mDragTarget == on) return;
    mDragTarget = on;
    SetDirty();
  }

  // 外部刷新标签 (编号随拖拽交换变化)
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
      // 拖拽中: 源槽半透明黑底
      g.FillRect(IColor(70, 0, 0, 0), GetWidgetBounds());
    }
    if (mDragTarget)
    {
      // 目标槽: 3px 黑框高亮
      g.DrawRect(IColor(255, 0, 0, 0), GetWidgetBounds(), nullptr, 3.f);
    }
  }

private:
  static constexpr float kDragThreshold = 8.f;  // 触发拖拽的移动阈值 (UI 坐标)

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
  bool mPotentialDrag = false;  // 左键按下, 尚未决定单击/拖拽
  bool mDragging = false;       // 已进入拖拽模式
  bool mDragTarget = false;     // 被外部标为拖拽目标
  float mDownX = 0.f, mDownY = 0.f;
  IPopupMenu mMenu;  // 必须为成员变量 (iPlug2 框架要求, 临时构造会崩)
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
