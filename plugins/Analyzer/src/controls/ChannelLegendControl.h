#pragma once

// ChannelLegend — 电平表读数 (纯展示)
//
// 位于频谱区顶部右侧: 显示电平表 L/R 当前数值读数 (无单位, 模式由右栏按钮指示)。
// 原 L/R/M 色块图例已由顶部声道按钮 (FlatCycleButton 通道分半样式) 取代。

#include "IControls.h"
#include "UiUtils.h"
#include "../Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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
      mMeterMode = std::clamp(d.mode, 0, 1);
      if (mMeterMode == 0) {
        mValL = d.trueL;
        mValR = d.trueR;
      } else {
        mValL = d.peakL;
        mValR = d.peakR;
      }
      SetDirty(false);
    }
  }

  void Draw(IGraphics &g) override { DrawReadout(g); }

private:
  // 右侧读数: L/R 当前值, 与电平表竖条同一水平线, 右对齐
  void DrawReadout(IGraphics &g) {
    const float roR = mRECT.R;
    char bufL[16], bufR[16];
    std::snprintf(bufL, sizeof(bufL), "%.1f", mValL);
    std::snprintf(bufR, sizeof(bufR), "%.1f", mValR);
    const IText readText(20, COL_900(), kFontRegular, EAlign::Far, EVAlign::Middle);
    g.DrawText(readText, bufL, IRECT(roR - 124.f, mRECT.T, roR - 64.f, mRECT.B));
    g.DrawText(readText, bufR, IRECT(roR - 64.f, mRECT.T, roR, mRECT.B));
  }

  int mMeterMode = 0;
  float mValL = -120.f, mValR = -120.f;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
