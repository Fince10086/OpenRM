#pragma once

// CpuMeterControl — 右上角 CPU 占用率只读显示框。
// 样式与 FlatActionButton 完全一致 (COL_300 色块 + BLOCK_GAP 内缩 + SemiBold 深色文字,
// 占满右列整行宽 156x30), 但不可交互; 显示音频线程处理耗时/块时长的一阶平滑值,
// 精度 0.01% (如 0.35%), 由插件 OnIdle 经 kMsgTagCpu 消息实时推送。

#include "IControls.h"
#include "../Theme.h"

#include <cstdio>
#include <cstring>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class CpuMeterControl : public IControl {
public:
  enum MsgTags { kMsgTagCpu = 1 };

  explicit CpuMeterControl(const IRECT &bounds) : IControl(bounds) {}

  void OnMsgFromDelegate(int msgTag, int nDataSize, const void *pData) override {
    if (msgTag == kMsgTagCpu && nDataSize == (int)sizeof(double)) {
      double v;
      std::memcpy(&v, pData, sizeof(double));
      if (v != mCpu) {
        mCpu = v;
        SetDirty();
      }
    }
  }

  void Draw(IGraphics &g) override {
    const IRECT b = mRECT.GetPadded(-BLOCK_GAP);
    g.FillRect(COL_300(), b);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f%%", mCpu * 100.0);
    IText t(20, COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
    g.DrawText(t, buf, b);
  }

private:
  double mCpu = 0.0;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
