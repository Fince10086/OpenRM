#pragma once

// 系列共享的 UI 工具函数

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

inline void DrawKnob(IGraphics &g, float cx, float cy) {
  g.FillCircle(COL_100(), cx, cy, HANDLE_R + HANDLE_RING);
  g.FillCircle(COL_900(), cx, cy, HANDLE_R);
}

inline void DrawGhostOverlay(IGraphics &g, const IRECT &r) {
  const IColor base = COL_100();
  g.FillRect(IColor(150, base.R, base.G, base.B), r);
}

constexpr int kRandomColorNameIds[kNumRandomColors] = {orm::kTxtRed, orm::kTxtYellow, orm::kTxtBlue, orm::kTxtGreen};

// onPick 按值捕获: 调用方多传临时 std::function, 按引用捕获会在菜单异步回调时悬空
inline void OpenColorPopup(IGraphics &g, IControl &host, IPopupMenu &menu, const IRECT &anchor, int selected,
                           std::function<void(int)> onPick) {
  menu.Clear();
  menu.SetFunction([onPick = std::move(onPick)](IPopupMenu *m) {
    const int idx = m ? m->GetChosenItemIdx() : -1;
    if (idx < 0 || idx >= kNumRandomColors)
      return;
    if (onPick)
      onPick(idx);
  });
  for (int c = 0; c < kNumRandomColors; ++c)
    menu.AddItem(orm::Tr(kRandomColorNameIds[c], orm::UILang()));
  menu.CheckItemAlone(std::clamp(selected, 0, kNumRandomColors - 1));
  g.CreatePopupMenu(host, menu, anchor, kNoValIdx);
}

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
