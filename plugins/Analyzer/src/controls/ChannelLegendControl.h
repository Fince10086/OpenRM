#pragma once

// ChannelLegend — 电平表读数 (纯展示)
//
// 位于频谱区顶部右侧: 显示 True Peak 最高值 (响度链锁存, 原响度计横条读数移入此处;
// 原 L/R 两声道实时读数已移除, 由电平条本身承载)。> -1 dBTP 红字 (与过载链同阈值)。

#include "IControls.h"
#include "../Theme.h"

#include <cstdio>
#include <cstring>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class ChannelLegendControl : public IControl {
public:
  enum MsgTags { kMsgTagTpMax = 1 };

  explicit ChannelLegendControl(const IRECT &bounds) : IControl(bounds) {}

  void OnMsgFromDelegate(int msgTag, int dataSize, const void *pData) override {
    if (msgTag == kMsgTagTpMax && dataSize == (int)sizeof(float)) {
      mTpMax = *(const float *)pData;
      SetDirty(false);
    }
  }

  void Draw(IGraphics &g) override { DrawReadout(g); }

private:
  // 右侧读数: True Peak 锁存值, 右对齐到电平条上方, > -1 dBTP 红字
  void DrawReadout(IGraphics &g) {
    const float roR = mRECT.R;
    char buf[32];
    if (mTpMax > -99.f)
      std::snprintf(buf, sizeof(buf), "%.1f dBTP", mTpMax);
    else
      std::snprintf(buf, sizeof(buf), "%s", "—");
    const IColor col = (mTpMax > -1.0f) ? MeterRed() : COL_900();
    const IText readText(16.f, col, kFontSemiBold, EAlign::Far, EVAlign::Middle);
    g.DrawText(readText, buf, IRECT(roR - 140.f, mRECT.T, roR, mRECT.B));
  }

  float mTpMax = -120.f;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE