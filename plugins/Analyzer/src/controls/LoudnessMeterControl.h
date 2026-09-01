#pragma once

// LoudnessMeterControl — 响度读数面板 (右栏, 无词标数字仪表面板)
//
// 位于右栏上方 (784, 58 → 940, 214), 纵向堆叠 (全部读数 + 刻度, 无说明小字):
//   [I 大读数 26px + LUFS 单位]
//   [目标差 Δ 16px 语义色]
//   [±10 LU 迷你刻度条: 轨道 + 中央 |Δ|≤1 绿区 + 2px 指针 + 刻度 −10/0/+10]
//    —— 条的中心就是目标, 与下方预设按钮 (−14/−16/−23 LUFS) 呼应
//   [LRA 16px]
//   [0..20 LU 迷你刻度条 + 刻度 0/10/20]
//   [M/S 实时值行 16px]
// 目标差着色与电平表安全色惯例一致 (固定色, 随饱和档位降饱和): |Δ| ≤ 1 LU 绿 /
// Δ > +1 LU 红 / Δ < -1 LU 黄。无效值 (I 未到首个 400ms 门限块 / LRA 未满 60s /
// 数值低于 -99) 显示 "—"。绿区/指针填充走 satScale 处理, 纯黑白主题下自动变灰阶。
//
// 数据由插件 OnIdle 每帧经 kMsgTagLoudnessData 下发 (LoudnessUiData, 见 UiUtils.h)。
// M/S/I 响度条仍在频谱面板右缘 (SpectrumPad::DrawLoudBars), True Peak 在图例行。

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
      mTarget = d.target;
      mIValid = d.iValid != 0;
      mLraValid = d.lraValid != 0;
      SetDirty(false);
    }
  }

  void Draw(IGraphics &g) override {
    const IRECT R = mRECT;
    const float xL = R.L;
    char buf[32];

    // ── I 大读数 + LUFS 单位 (26px 主读数, 单位 14px 同底对齐, legend 值+单位先例) ──
    const IText iText(26.f, mIValid ? COL_900() : COL_500(), kFontBold, EAlign::Near, EVAlign::Bottom);
    if (mIValid)
      std::snprintf(buf, sizeof(buf), "%.1f", mIntegrated);
    else
      std::snprintf(buf, sizeof(buf), "%s", "—");
    IRECT iR(xL, R.T + 2.f, R.R, R.T + 36.f);
    g.MeasureText(iText, buf, iR); // iR 缩为文字实际范围
    g.DrawText(iText, buf, iR);
    if (mIValid)
      g.DrawText(IText(14.f, COL_700(), kFontRegular, EAlign::Near, EVAlign::Bottom), "LUFS",
                 IRECT(iR.R + 6.f, R.T + 16.f, R.R, R.T + 36.f));

    // ── 目标差 Δ (16px 语义色; 目标即下条的中心) ──────────────────────────
    const bool dValid = mIValid && mTarget > -100.f;
    IColor dCol = COL_500();
    if (dValid) {
      const float delta = mIntegrated - mTarget;
      std::snprintf(buf, sizeof(buf), "%+.1f LU", delta);
      dCol = SemColor((std::fabs(delta) <= 1.0f) ? MeterGreen() : (delta > 0.f ? MeterRed() : MeterYellow()));
    } else
      std::snprintf(buf, sizeof(buf), "%s", "—");
    g.DrawText(IText(16.f, dCol, kFontSemiBold, EAlign::Near, EVAlign::Middle), buf,
               IRECT(xL, R.T + 40.f, R.R, R.T + 58.f));

    // ── ±10 LU 迷你刻度条 (轨道 + 中央 |Δ|≤1 绿区 + 2px 指针) ──────────────
    const float trL = xL + 16.f, trR = R.R - 16.f; // 让位两端 14px 刻度文字
    const IRECT dTrack(trL, R.T + 62.f, trR, R.T + 68.f);
    g.FillRect(COL_300(), dTrack);
    g.FillRect(SemColor(MeterGreen()),
               IRECT(0.5f * (trL + trR) - 0.1f * (trR - trL), dTrack.T,
                     0.5f * (trL + trR) + 0.1f * (trR - trL), dTrack.B)); // |Δ|≤1 绿区
    if (dValid) {
      const float mid = 0.5f * (trL + trR);
      const float vx = mid + std::clamp(mIntegrated - mTarget, -10.f, 10.f) / 10.f * (0.5f * (trR - trL));
      g.FillRect(dCol, IRECT(vx - 1.f, dTrack.T, vx + 1.f, dTrack.B));
    }
    const IText tickT(14.f, COL_700(), kFontRegular, EAlign::Near, EVAlign::Top);
    g.DrawText(tickT, "-10", IRECT(trL, R.T + 70.f, trL + 34.f, R.T + 86.f));
    g.DrawText(IText(14.f, COL_700(), kFontRegular, EAlign::Center, EVAlign::Top), "0",
               IRECT(0.5f * (trL + trR) - 10.f, R.T + 70.f, 0.5f * (trL + trR) + 10.f, R.T + 86.f));
    g.DrawText(IText(14.f, COL_700(), kFontRegular, EAlign::Far, EVAlign::Top), "+10",
               IRECT(trR - 34.f, R.T + 70.f, trR, R.T + 86.f));

    // ── LRA + 0..20 LU 迷你刻度条 ──────────────────────────────────────
    const IText lrText(16.f, mLraValid ? COL_900() : COL_500(), kFontSemiBold, EAlign::Near, EVAlign::Middle);
    if (mLraValid)
      std::snprintf(buf, sizeof(buf), "%.1f LU", mRange);
    else
      std::snprintf(buf, sizeof(buf), "%s", "—");
    g.DrawText(lrText, buf, IRECT(xL, R.T + 90.f, R.R, R.T + 108.f));
    const IRECT lTrack(trL, R.T + 112.f, trR, R.T + 118.f);
    g.FillRect(COL_300(), lTrack);
    if (mLraValid) {
      const float vx = trL + std::clamp(mRange, 0.f, 20.f) / 20.f * (trR - trL);
      g.FillRect(COL_900(), IRECT(vx - 1.f, lTrack.T, vx + 1.f, lTrack.B));
    }
    g.DrawText(tickT, "0", IRECT(trL, R.T + 120.f, trL + 20.f, R.T + 136.f));
    g.DrawText(IText(14.f, COL_700(), kFontRegular, EAlign::Center, EVAlign::Top), "10",
               IRECT(0.5f * (trL + trR) - 14.f, R.T + 120.f, 0.5f * (trL + trR) + 14.f, R.T + 136.f));
    g.DrawText(IText(14.f, COL_700(), kFontRegular, EAlign::Far, EVAlign::Top), "20",
               IRECT(trR - 20.f, R.T + 120.f, trR, R.T + 136.f));

    // ── M / S 实时值行 (16px, 无效 "—") ────────────────────────────────
    const IText msT(16.f, COL_900(), kFontSemiBold, EAlign::Near, EVAlign::Middle);
    const IText msD(16.f, COL_500(), kFontSemiBold, EAlign::Near, EVAlign::Middle);
    if (mMomentary > -99.f)
      std::snprintf(buf, sizeof(buf), "M %.1f", mMomentary);
    else
      std::snprintf(buf, sizeof(buf), "M —");
    g.DrawText(mMomentary > -99.f ? msT : msD, buf, IRECT(xL, R.T + 140.f, xL + 76.f, R.T + 158.f));
    if (mShortTerm > -99.f)
      std::snprintf(buf, sizeof(buf), "S %.1f", mShortTerm);
    else
      std::snprintf(buf, sizeof(buf), "S —");
    g.DrawText(mShortTerm > -99.f ? msT : msD, buf, IRECT(xL + 80.f, R.T + 140.f, R.R, R.T + 158.f));
  }

private:
  // 语义色随主题饱和档位降饱和 (黑白主题下变灰阶; 与声像仪表行同式)
  static float MeterSatScale() {
    const int sat = ThemeSatMax();
    return (sat <= 30) ? (float)sat / 30.f : (1.f + (float)(sat - 30) / 55.f);
  }
  static IColor SemColor(IColor c) {
    const float m = std::clamp(MeterSatScale(), 0.f, 1.f);
    const float lum = 0.299f * c.R + 0.587f * c.G + 0.114f * c.B;
    return IColor(c.A, (int)std::lround(c.R * m + lum * (1.f - m)),
                       (int)std::lround(c.G * m + lum * (1.f - m)),
                       (int)std::lround(c.B * m + lum * (1.f - m)));
  }

  float mMomentary = -120.f, mShortTerm = -120.f; // M/S (LUFS, 迷你条行读数)
  float mIntegrated = -120.f;
  float mRange = 0.f, mTarget = -14.f;
  bool mIValid = false, mLraValid = false;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
