#pragma once

// 系列共享的 UI 工具函数: 频率格式化/解析、旋钮绘制、Ghost 覆盖、随机颜色菜单。
// 目的: 消除 FilterNodePad / BandRangeSlider / ORMSlider / SettingsPanel 等控件间的逐字重复。

#include "IControls.h"
#include "../Theme.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

inline void FormatFreq(char *b, int n, double hz, bool withUnit) {
  const char *u = withUnit ? "Hz" : "";
  std::snprintf(b, n, "%.0f%s", hz, u);
}

inline bool ParseFreq(const char *s, double &hz) {
  char *end = nullptr;
  const double v = std::strtod(s, &end);
  if (end == s)
    return false;
  while (*end && std::isspace((unsigned char)*end))
    ++end;
  if (*end == 'k' || *end == 'K')
    hz = v * 1000.;
  else
    hz = v;
  return true;
}

// 标准旋钮手柄: 白环 + 深色核心
inline void DrawKnob(IGraphics &g, float cx, float cy) {
  g.FillCircle(COL_100(), cx, cy, HANDLE_R + HANDLE_RING);
  g.FillCircle(COL_900(), cx, cy, HANDLE_R);
}

// Mono/禁用态的半透明覆盖层
inline void DrawGhostOverlay(IGraphics &g, const IRECT &r) {
  const IColor base = COL_100();
  g.FillRect(IColor(150, base.R, base.G, base.B), r);
}

constexpr int kRandomColorNameIds[kNumRandomColors] = {orm::kTxtRed, orm::kTxtYellow, orm::kTxtBlue, orm::kTxtGreen};

// 统一的随机颜色弹窗: 填充 4 色名称, 勾选当前项, 回调选中索引
inline void OpenColorPopup(IGraphics &g, IControl &host, IPopupMenu &menu, const IRECT &anchor, int selected,
                           const std::function<void(int)> &onPick) {
  menu.Clear();
  menu.SetFunction([&onPick](IPopupMenu *m) {
    const int idx = m ? m->GetChosenItemIdx() : -1;
    if (idx < 0 || idx >= kNumRandomColors)
      return;
    onPick(idx);
  });
  for (int c = 0; c < kNumRandomColors; ++c)
    menu.AddItem(orm::Tr(kRandomColorNameIds[c], orm::UILang()));
  menu.CheckItemAlone(std::clamp(selected, 0, kNumRandomColors - 1));
  g.CreatePopupMenu(host, menu, anchor, kNoValIdx);
}

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
