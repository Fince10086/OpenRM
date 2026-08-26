#pragma once

// ChannelLegend — 频谱三通道色图例 + 电平表读数 (纯展示)
//
// 左侧: L / R / M 三个色块, 颜色与 SpectrumPad 实际绘制完全一致
// (取色逻辑共用 Theme.h 的 GetChannelColors, 跟随当前主题)。无交互。
// 右侧: 电平表 L/R 当前数值读数 (无单位, 模式由右栏按钮指示), 与图例同一水平位置。

#include "IControls.h"
#include "UiUtils.h"
#include "../Theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class ChannelLegendControl : public IControl {
public:
  enum MsgTags { kMsgTagLevelReadout = 1 };

  explicit ChannelLegendControl(const IRECT &bounds) : IControl(bounds) {}

  void OnMsgFromDelegate(int msgTag, int dataSize, const void *pData) override {
    if (msgTag == kMsgTagLevelReadout && dataSize == (int)sizeof(LevelMeterUiData)) {
      LevelMeterUiData d;
      std::memcpy(&d, pData, sizeof(d));
      mMeterMode = std::clamp(d.mode, 0, 2);
      if (mMeterMode == 0) {
        mValL = d.trueL;
        mValR = d.trueR;
      } else if (mMeterMode == 1) {
        mValL = d.peakL;
        mValR = d.peakR;
      } else {
        mValL = d.vuL;
        mValR = d.vuR;
      }
      SetDirty(false);
    }
  }

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

    DrawReadout(g);
  }

private:
  // 右侧读数: L/R 当前值, 与电平表竖条同一水平线, 右对齐
  void DrawReadout(IGraphics &g) {
    const float roR = mRECT.R;
    char bufL[16], bufR[16];
    if (mMeterMode == 2) {
      std::snprintf(bufL, sizeof(bufL), "%+d", (int)std::lround(mValL + 18.f));
      std::snprintf(bufR, sizeof(bufR), "%+d", (int)std::lround(mValR + 18.f));
    } else if (mMeterMode == 0) {
      std::snprintf(bufL, sizeof(bufL), "%.1f", mValL);
      std::snprintf(bufR, sizeof(bufR), "%.1f", mValR);
    } else {
      std::snprintf(bufL, sizeof(bufL), "%.1f", mValL);
      std::snprintf(bufR, sizeof(bufR), "%.1f", mValR);
    }
    const IText readText(20, COL_900(), kFontRegular, EAlign::Far, EVAlign::Middle);
    g.DrawText(readText, bufL, IRECT(roR - 124.f, mRECT.T, roR - 64.f, mRECT.B));
    g.DrawText(readText, bufR, IRECT(roR - 64.f, mRECT.T, roR, mRECT.B));
  }

  int mMeterMode = 0;
  float mValL = -120.f, mValR = -120.f;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
