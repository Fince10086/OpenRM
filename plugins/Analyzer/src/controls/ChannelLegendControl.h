#pragma once

// ChannelLegend — 频谱三通道色图例 (纯展示)
//
// 在频谱区域上方显示 L / R / M 三个色块, 颜色与 SpectrumPad 实际绘制完全一致
// (取色逻辑共用 Theme.h 的 GetChannelColors, 跟随当前主题)。无交互。

#include "IControls.h"
#include "../Theme.h"

#include <array>
#include <utility>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class ChannelLegendControl : public IControl {
public:
  explicit ChannelLegendControl(const IRECT &bounds) : IControl(bounds) {}

  void Draw(IGraphics &g) override {
    IColor cL, cR, cM;
    GetChannelColors(cL, cR, cM);

    constexpr float kSwatch = 16.f;
    constexpr float kStep = 48.f; // 每个条目 (色块+标签) 的横向步进
    const float y = mRECT.MH() - kSwatch * 0.5f;

    IText labelText(20, COL_700(), kFontSemiBold, EAlign::Near, EVAlign::Middle);
    const std::array<std::pair<IColor, const char *>, 3> entries = {
        std::make_pair(cL, "L"), std::make_pair(cR, "R"), std::make_pair(cM, "M")};

    float x = mRECT.L;
    for (const auto &entry : entries) {
      g.FillRect(entry.first, IRECT(x, y, x + kSwatch, y + kSwatch));
      g.DrawText(labelText, entry.second, IRECT(x + kSwatch + 4.f, mRECT.T, x + kStep, mRECT.B));
      x += kStep;
    }
  }
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
