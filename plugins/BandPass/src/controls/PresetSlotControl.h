#pragma once
// ============================================================================
// PresetSlotControl.h — 预设槽按钮 (IVButtonControl 子类)
//
// 交互设计 (面向可发现性):
//   左键单击   → 加载该预设
//   ⌘+左键     → 把当前设置保存到该槽        (macOS: Cmd 键落在 mod.R, 但 L=true)
//   ⌥+左键     → 恢复该槽为出厂默认          (macOS: Opt/Alt 落在 mod.A)
//   右键       → 弹出菜单: 加载 / 保存到此 / 恢复默认 / 重命名
//   悬停       → 系统 tooltip 提示全部操作
//
// ⚠ macOS 键位陷阱: 左键事件里 Cmd 键被映射到 mod.R 字段 (L=true, R=true),
//   而右键事件是 (L=false, R=true)。两者共用 R 字段, 必须先用 L 分流,
//   不能只判断 R。Windows 上无此问题 (Cmd 不存在), 但逻辑同样正确。
// ============================================================================
#include "IControls.h"

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
    std::function<void()> onSaveHere;         // ⌘+点击 / 菜单"保存到此"
    std::function<void()> onRestoreDefault;   // ⌥+点击 / 菜单"恢复默认"
    std::function<void(const char*)> onRenameCommit;  // 行内重命名提交 (空串 = 取消)
    std::function<std::string()> getTooltipPrefix;   // tooltip 首行 (预设名等)
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
      if (mod.R)            { if (mHooks.onSaveHere)       mHooks.onSaveHere(); }
      else if (mod.A)       { if (mHooks.onRestoreDefault) mHooks.onRestoreDefault(); }
      else                  { if (mHooks.onLoad)           mHooks.onLoad(); }
      SetValue(0.0);   // 复用 MakeMomentary 的"按下后复位"逻辑
      SetDirty(false);
      return;
    }
    if (mod.R)
      ShowPopupMenu(x, y);
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override
  {
    // 预设名/槽位变化时刷新系统 tooltip
    const std::string t = BuildTooltip();
    if (t != GetTooltip())
    {
      SetTooltip(t.c_str());
      if (GetUI()) GetUI()->UpdateTooltips();
    }
    IControl::OnMouseOver(x, y, mod);
  }

  // 行内重命名: 复用 CreateTextEntry + kNoValIdx (FilterNodePad 同款做法)
  void OnTextEntryCompletion(const char* str, int valIdx) override
  {
    if (!mRenaming) return;
    mRenaming = false;
    if (mHooks.onRenameCommit) mHooks.onRenameCommit(str);
  }

  // 外部刷新标签 (底部"最近使用"行随 MRU 变化)
  void SetSlotLabel(const char* s)
  {
    SetLabelStr(s);
    SetDirty(false);
  }

private:
  std::string BuildTooltip() const
  {
    std::string s = mHooks.getTooltipPrefix ? mHooks.getTooltipPrefix() : std::string();
    if (!s.empty()) s += "\n";
    s += "单击加载 · ⌘点击保存 · ⌥点击恢复默认 · 右击更多";
    return s;
  }

  void ShowPopupMenu(float x, float y)
  {
    mMenu.Clear();
    mMenu.AddItem("加载");
    mMenu.AddItem("保存到此");
    mMenu.AddItem("恢复默认");
    mMenu.AddItem("重命名");
    mMenu.SetFunction([this](IPopupMenu* p) {
      switch (p->GetChosenItemIdx())
      {
        case 0:  if (mHooks.onLoad)          mHooks.onLoad();          break;
        case 1:  if (mHooks.onSaveHere)      mHooks.onSaveHere();      break;
        case 2:  if (mHooks.onRestoreDefault) mHooks.onRestoreDefault(); break;
        case 3:  BeginRename();              break;
        default: break;
      }
    });
    GetUI()->CreatePopupMenu(*this, mMenu, x, y, kNoValIdx);
  }

  void BeginRename()
  {
    mRenaming = true;
    // 复用按钮样式字体/颜色, 在按钮矩形上弹出行内输入框
    GetUI()->CreateTextEntry(*this, mStyle.labelText, GetWidgetBounds(),
                             GetLabelStr(), kNoValIdx);
  }

  Hooks mHooks;
  bool mRenaming = false;      // 行内重命名进行中 (OnTextEntryCompletion 用)
  IPopupMenu mMenu;  // 必须为成员变量 (iPlug2 框架要求, 临时构造会崩)
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
