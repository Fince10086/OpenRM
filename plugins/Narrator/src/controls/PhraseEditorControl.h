#pragma once

#include "IControls.h"
#include "../Theme.h"
#include "../Strings.h"

#include <algorithm>
#include <cmath>
#include <functional>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

// 短语文本框: 显示当前语句文本, 点击唤起平台文本输入 (IGraphics::CreateTextEntry)。
// 合成文本限英文 (SAM Reciter 仅英语, 插件内明示); 界面文案仍双语。
class PhraseEditorControl : public IControl
{
public:
  struct Hooks
  {
    std::function<std::string()> getText;
    std::function<void(const std::string &)> onCommit;
  };

  PhraseEditorControl(const IRECT &bounds, Hooks hooks)
      : IControl(bounds), mHooks(std::move(hooks))
  {
  }

  void RefreshText()
  {
    SetDirty(false);
  }

  void OnMouseDown(float x, float y, const IMouseMod &mod) override
  {
    if (mod.R || !mHooks.getText)
      return;
    const std::string cur = mHooks.getText();
    if (GetUI())
      GetUI()->CreateTextEntry(*this,
                               IText(20, COL_900(), kFontRegular, EAlign::Near, EVAlign::Middle),
                               mRECT.GetPadded(-6.f), cur.c_str());
  }

  void OnTextEntryCompletion(const char *str, int) override
  {
    if (mHooks.onCommit && str)
      mHooks.onCommit(str);
  }

  void Draw(IGraphics &g) override
  {
    const IRECT b = mRECT;
    g.FillRect(COL_300(), b);
    std::string text = mHooks.getText ? mHooks.getText() : std::string();
    if (text.empty())
      text = orm::Tr(orm::kTxtEmptyPhrase, orm::UILang());
    // 保留换行显示为空格 (单行框)
    std::replace(text.begin(), text.end(), '\n', ' ');
    IText t(20, COL_900(), kFontRegular, EAlign::Near, EVAlign::Middle);
    g.DrawText(t, text.c_str(), b.GetPadded(-6.f));
  }

private:
  Hooks mHooks;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
