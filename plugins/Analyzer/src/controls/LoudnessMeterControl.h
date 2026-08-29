#pragma once

// LoudnessMeterControl — 响度计数字读数显示控件 (方案 A: 核心读数版)
//
// 位于频谱区下方底部横条 (20, 336 → 668, 424):
//   [INTEGRATED 大字号 + 目标差 + TARGET] [M 迷你竖条] [S 迷你竖条] [LRA] [TRUE PEAK]
//   [预设循环按钮] [RESET] 为独立控件 (Analyzer.cpp 创建, 置于横条右缘)。
//
// 数据由插件 OnIdle 每帧经 kMsgTagLoudnessData 下发 (LoudnessUiData, 见 UiUtils.h)。
// 目标差着色与现有电平表安全色惯例一致 (固定色, 不随主题色相旋转):
//   |Δ| ≤ 1 LU 绿 (MeterGreen) / Δ > +1 LU 红 (MeterRed) / Δ < -1 LU 黄 (MeterYellow)
// TP > -1 dBTP 红字 (过载链与电平表 dBTP 模式同阈值)。
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
      mMomentary = d.momentary;
      mShortTerm = d.shortTerm;
      mIntegrated = d.integrated;
      mRange = d.range;
      mTpMax = d.tpMax;
      mTarget = d.target;
      mIValid = d.iValid != 0;
      mLraValid = d.lraValid != 0;
      SetDirty(false);
    }
  }

  // 翻译文字 (ApplyLanguage 绑定; Tr() 返回静态表指针, 可直接暂存)
  void SetMomentaryText(const char *s) { mMomentaryText = s; SetDirty(false); }
  void SetShortText(const char *s) { mShortText = s; SetDirty(false); }
  void SetIntText(const char *s) { mIntText = s; SetDirty(false); }
  void SetLraText(const char *s) { mLraText = s; SetDirty(false); }
  void SetTpText(const char *s) { mTpText = s; SetDirty(false); }
  void SetTargetText(const char *s) { mTargetText = s; SetDirty(false); }

  void Draw(IGraphics &g) override {
    const IRECT R = mRECT;
    const float bodyT = R.T + 22.f;      // 数值行顶
    const float bodyB = R.B - 2.f;       // 数值行底
    const float barBottom = bodyB - 18.f; // 迷你竖条底 (下方留数值行)
    const float labelTop = R.T + 2.f;
    char buf[48];

    // 顶部分隔线 (与频谱区之间)
    g.FillRect(COL_300(), IRECT(R.L, R.T, R.R, R.T + 1.f));

    // ── INTEGRATED: 大字号读数 + 目标差 + TARGET ─────────────────────────
    const IRECT iRect(R.L, bodyT, R.L + 118.f, bodyB);
    DrawLabel(g, mIntText, IRECT(R.L, labelTop, R.L + 120.f, labelTop + 18.f));
    if (mIValid)
      std::snprintf(buf, sizeof(buf), "%.1f", mIntegrated);
    else
      std::snprintf(buf, sizeof(buf), "%s", "—");
    const IText iText(30.f, COL_900(), kFontBold, EAlign::Near, EVAlign::Middle);
    g.DrawText(iText, buf, iRect);

    const IRECT dRect(R.L + 122.f, bodyT, R.L + 196.f, bodyB);
    if (mIValid) {
      const float delta = mIntegrated - mTarget;
      std::snprintf(buf, sizeof(buf), "%+.1f LU", delta);
      const IColor dCol = (std::fabs(delta) <= 1.0f) ? MeterGreen() : (delta > 0.f ? MeterRed() : MeterYellow());
      const IText dText(15.f, dCol, kFontSemiBold, EAlign::Near, EVAlign::Top);
      g.DrawText(dText, buf, dRect);
    }
    if (mTargetText && mTarget > -100.f) {
      char tbuf[32];
      std::snprintf(tbuf, sizeof(tbuf), "%s %.0f LUFS", mTargetText, mTarget);
      const IText tText(10.f, COL_500(), kFontRegular, EAlign::Near, EVAlign::Bottom);
      g.DrawText(tText, tbuf, dRect);
    }

    // ── M / S 迷你竖条 (LUFS -60..0 标尺, 顶部目标刻度线) ─────────────────
    const float barTop = bodyT + 6.f;
    const float bar1L = R.L + 194.f;
    DrawMiniBar(g, bar1L, barTop, barBottom, mMomentary, mMomentaryText, labelTop);
    const float bar2L = R.L + 274.f;
    DrawMiniBar(g, bar2L, barTop, barBottom, mShortTerm, mShortText, labelTop);
    if (mTarget > -100.f) {
      const float yT = BarY(barTop, barBottom, mTarget);
      if (yT > barTop && yT < barBottom)
        g.FillRect(COL_900(), IRECT(bar1L, yT - 1.f, bar2L + 14.f, yT + 1.f));
    }

    // ── LRA ─────────────────────────────────────────────────────────────
    const IRECT lr(R.L + 356.f, bodyT, R.L + 416.f, bodyB);
    DrawLabel(g, mLraText, IRECT(lr.L, labelTop, lr.R, labelTop + 18.f));
    if (mLraValid)
      std::snprintf(buf, sizeof(buf), "%.1f LU", mRange);
    else
      std::snprintf(buf, sizeof(buf), "%s", "—");
    const IText lrText(15.f, mLraValid ? COL_900() : COL_500(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
    g.DrawText(lrText, buf, lr);

    // ── TRUE PEAK ───────────────────────────────────────────────────────
    const IRECT tp(R.L + 422.f, bodyT, R.L + 488.f, bodyB);
    DrawLabel(g, mTpText, IRECT(tp.L, labelTop, tp.R, labelTop + 18.f));
    if (mTpMax > -99.f)
      std::snprintf(buf, sizeof(buf), "%.1f dBTP", mTpMax);
    else
      std::snprintf(buf, sizeof(buf), "%s", "—");
    const IColor tpCol = (mTpMax > -1.0f) ? MeterRed() : COL_900();
    const IText tpText(15.f, tpCol, kFontSemiBold, EAlign::Center, EVAlign::Middle);
    g.DrawText(tpText, buf, tp);
  }

private:
  static float BarY(float barTop, float barBottom, float lufs) {
    return barBottom - std::clamp((lufs + 60.f) / 60.f, 0.f, 1.f) * (barBottom - barTop);
  }

  // 区块小标签 (顶部 10.5px, 居中于区块)
  void DrawLabel(IGraphics &g, const char *text, const IRECT &rect) {
    if (!text)
      return;
    const IText labelText(10.5f, COL_500(), kFontRegular, EAlign::Center, EVAlign::Middle);
    g.DrawText(labelText, text, rect);
  }

  // 迷你竖条: 轨道 + 渐变激活段 + 下方数值 (无效/过低时 "—")
  void DrawMiniBar(IGraphics &g, float barL, float barTop, float barBottom, float lufs,
                   const char *label, float labelTop) {
    constexpr float kBarW = 14.f;
    const IRECT bar(barL, barTop, barL + kBarW, barBottom);
    DrawLabel(g, label, IRECT(barL - 44.f, labelTop, barL + 44.f, labelTop + 18.f));
    g.FillRect(COL_300(), bar);

    if (lufs > -99.f) {
      const float y = BarY(barTop, barBottom, lufs);
      const IRECT active(bar.L, y, bar.R, bar.B);
      if (active.B - active.T > 0.5f) {
        IPattern grad = IPattern::CreateLinearGradient(bar.L, barTop, bar.L, barBottom);
        auto tOf = [&](float db) { return std::clamp((db + 60.f) / 60.f, 0.f, 1.f); };
        grad.AddStop(IColor(255, 96, 186, 96), 0.f);         // -60 绿
        grad.AddStop(IColor(232, 173, 40, 255), tOf(-18.f)); // -18 黄
        grad.AddStop(IColor(226, 60, 52, 255), tOf(-6.f));   // -6 红
        grad.AddStop(IColor(226, 60, 52, 255), 1.f);         // 0 红
        g.PathClear();
        g.PathRect(active);
        g.PathFill(grad);
      }
    }

    char vbuf[16];
    if (lufs > -99.f)
      std::snprintf(vbuf, sizeof(vbuf), "%.1f", lufs);
    else
      std::snprintf(vbuf, sizeof(vbuf), "%s", "—");
    const IText vText(12.5f, COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Top);
    g.DrawText(vText, vbuf, IRECT(barL - 44.f, barBottom, barL + 44.f, barBottom + 18.f));
  }

  const char *mMomentaryText = nullptr;
  const char *mShortText = nullptr;
  const char *mIntText = nullptr;
  const char *mLraText = nullptr;
  const char *mTpText = nullptr;
  const char *mTargetText = nullptr;
  float mMomentary = -120.f, mShortTerm = -120.f, mIntegrated = -120.f;
  float mRange = 0.f, mTpMax = -120.f, mTarget = -14.f;
  bool mIValid = false, mLraValid = false;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE