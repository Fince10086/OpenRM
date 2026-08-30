#pragma once

// LoudnessMeterControl — 响度计数字读数控件 (右栏版)
//
// 位于右栏上方 (784, 58 → 940, 170), 纵向堆叠:
//   [INTEGRATED 标签 + 大字号] [目标差 + TARGET] [LRA]
// 底部横条已移除 (频谱下方留空, 无分隔线); M/S/I 响度条位于频谱面板右缘
// (SpectrumPad::DrawLoudBars), True Peak 最高值在图例行 (ChannelLegendControl)。
//
// 数据由插件 OnIdle 每帧经 kMsgTagLoudnessData 下发 (LoudnessUiData, 见 UiUtils.h)。
// 目标差着色与电平表安全色惯例一致 (固定色): |Δ| ≤ 1 LU 绿 / Δ > +1 LU 红 / Δ < -1 LU 黄。
// 无效值 (I 未到首个 400ms 门限块 / LRA 未满 60s / 数值低于 -99) 显示 "—"。

#include "IControls.h"
#include "UiUtils.h"
#include "../Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class LoudnessMeterControl : public IControl {
public:
  enum MsgTags { kMsgTagLoudnessData = 1 };

  explicit LoudnessMeterControl(const IRECT &bounds) : IControl(bounds) {}

  void OnMsgFromDelegate(int msgTag, int dataSize, const void *pData) override {
    if (msgTag == kMsgTagLoudnessData && dataSize == (int)sizeof(LoudnessUiData)) {
      LoudnessUiData d;
      std::memcpy(&d, pData, sizeof(d));
      mIntegrated = d.integrated;
      mRange = d.range;
      mTarget = d.target;
      mIValid = d.iValid != 0;
      mLraValid = d.lraValid != 0;
      SetDirty(false);
    }
  }

  // 翻译文字 (ApplyLanguage 绑定; Tr() 返回静态表指针, 可直接暂存)
  void SetIntText(const char *s) { mIntText = s; SetDirty(false); }
  void SetLraText(const char *s) { mLraText = s; SetDirty(false); }
  void SetTargetText(const char *s) { mTargetText = s; SetDirty(false); }

  void Draw(IGraphics &g) override {
    const IRECT R = mRECT;
    const float xL = R.L;
    char buf[48];

    // ── INTEGRATED: 标签 + 大字号 ──────────────────────────────────────
    DrawLabel(g, mIntText, IRECT(xL, R.T, R.R, R.T + 16.f));
    if (mIValid)
      std::snprintf(buf, sizeof(buf), "%.1f", mIntegrated);
    else
      std::snprintf(buf, sizeof(buf), "%s", "—");
    const IText iText(26.f, COL_900(), kFontBold, EAlign::Near, EVAlign::Middle);
    g.DrawText(iText, buf, IRECT(xL, R.T + 14.f, R.R, R.T + 46.f));

    // ── 目标差 + TARGET ────────────────────────────────────────────────
    if (mIValid) {
      const float delta = mIntegrated - mTarget;
      std::snprintf(buf, sizeof(buf), "%+.1f LU", delta);
      const IColor dCol = (std::fabs(delta) <= 1.0f) ? MeterGreen() : (delta > 0.f ? MeterRed() : MeterYellow());
      const IText dText(13.f, dCol, kFontSemiBold, EAlign::Near, EVAlign::Middle);
      g.DrawText(dText, buf, IRECT(xL, R.T + 46.f, R.R, R.T + 68.f));
    }
    if (mTargetText && mTarget > -100.f) {
      char tbuf[32];
      std::snprintf(tbuf, sizeof(tbuf), "%s %.0f LUFS", mTargetText, mTarget);
      const IText tText(10.f, COL_500(), kFontRegular, EAlign::Near, EVAlign::Top);
      g.DrawText(tText, tbuf, IRECT(xL, R.T + 64.f, R.R, R.T + 82.f));
    }

    // ── LRA ────────────────────────────────────────────────────────────
    DrawLabel(g, mLraText, IRECT(xL, R.T + 86.f, R.R, R.T + 102.f));
    if (mLraValid)
      std::snprintf(buf, sizeof(buf), "%.1f LU", mRange);
    else
      std::snprintf(buf, sizeof(buf), "%s", "—");
    const IText lrText(15.f, mLraValid ? COL_900() : COL_500(), kFontSemiBold, EAlign::Near, EVAlign::Middle);
    g.DrawText(lrText, buf, IRECT(xL, R.T + 100.f, R.R, R.T + 124.f));
  }

private:
  // 区块小标签 (顶部 10.5px)
  void DrawLabel(IGraphics &g, const char *text, const IRECT &rect) {
    if (!text)
      return;
    const IText labelText(10.5f, COL_500(), kFontRegular, EAlign::Near, EVAlign::Middle);
    g.DrawText(labelText, text, rect);
  }

  const char *mIntText = nullptr;
  const char *mLraText = nullptr;
  const char *mTargetText = nullptr;
  float mIntegrated = -120.f;
  float mRange = 0.f, mTarget = -14.f;
  bool mIValid = false, mLraValid = false;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE