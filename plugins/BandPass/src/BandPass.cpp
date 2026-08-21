#include "BandPass.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"
#include "controls/FilterNodePad.h"
#include "controls/PresetSlotControl.h"
#include "PresetFileIO.h"

#include <cstring>
#include <cstdio>
#include <functional>
#include <chrono>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// 黑白极简配色 (模仿 nono.feizao.org / nonocross):
//   纯白底 + 纯黑 2px 边框, #ccc 细网格线, hover #f0f0f0,
//   激活态黑白反转, 4px 圆角, Outfit 字体 (400/600/700)
// ---------------------------------------------------------------------------
static const IColor COL_BG     (255, 255, 255, 255);  // 面板纯白
static const IColor COL_PANEL  (255, 255, 255, 255);  // 控件底面白
static const IColor COL_BLACK  (255,   0,   0,   0);  // 主黑: 边框/文字/手柄
static const IColor COL_TEXT   (255,   0,   0,   0);
static const IColor COL_DIM    (255, 102, 102, 102);  // #666 次要文字
static const IColor COL_FAINT  (255, 153, 153, 153);  // #999 弱化文字/刻度
static const IColor COL_LINE   (255, 204, 204, 204);  // #ccc 细网格线
static const IColor COL_TRACK  (255, 236, 236, 236);  // 滑轨底 #ececec
static const IColor COL_FILL   (255,   0,   0,   0);  // 滑块已填充电平: 纯黑
static const IColor COL_HOVER  (255, 240, 240, 240);  // #f0f0f0 hover

// 旋钮/滑条/XY pad 主样式: kFG=白(手柄, 黑框描边), kX1=黑(滑轨/弧线填充), kSH=浅灰轨底
// 注意: iPlug2 的 roundness 是比例 (乘以短边一半), 不是像素值
static IVStyle MakeGRMStyle()
{
  IVColorSpec colors = { COL_PANEL, COL_PANEL, COL_DIM, COL_BLACK,
                         COL_HOVER, COL_TRACK, COL_BLACK, COL_BLACK, COL_BLACK };
  const IText labelText(10, COL_DIM, "Outfit", EAlign::Center, EVAlign::Bottom);
  const IText valueText(10, COL_TEXT, "Outfit-SemiBold", EAlign::Center, EVAlign::Top);
  return IVStyle(true, true, colors, labelText, valueText,
                 true, true, false, false, 0.2f, 1.5f, 0.f, 1.f, 0.f);
}

// 按钮: 常态白底黑字黑框, hover #f0f0f0, 按下黑白反转 (黑底)
static IVStyle MakeButtonStyle()
{
  IVColorSpec colors = { COL_PANEL, COL_PANEL, COL_BLACK, COL_BLACK,
                         COL_HOVER, COL_PANEL, COL_BLACK, COL_BLACK, COL_BLACK };
  const IText labelText(11, COL_TEXT, "Outfit-SemiBold", EAlign::Center, EVAlign::Middle);
  const IText valueText(11, COL_TEXT, "Outfit-SemiBold", EAlign::Center, EVAlign::Middle);
  return IVStyle(true, true, colors, labelText, valueText, true, true, false, false,
                 0.2f, 2.f, 0.f, 1.f, 0.f);
}

// 瞬时按钮: 点击执行动作后立即把值复位为 0, 规避 iPlug2
// IButtonControlBase 在缺少动画时 "按下后永久停留在浅色态" 的行为
static IVButtonControl* MakeMomentary(const IRECT& r,
                                       std::function<void(IControl*)> fn,
                                       const char* label, const IVStyle& st)
{
  return new IVButtonControl(r, [fn](IControl* p) {
    fn(p);
    p->SetValue(0.0);
    p->SetDirty(false);
  }, label, st);
}

// ---------------------------------------------------------------------------
// 预设 morph 条: 贯穿 Q1..Q8 的水平拖动条, 位置对应 8 个槽位刻度,
// 在相邻槽位参数之间连续插值 (无参绑定, 纯 UI 驱动)
// ---------------------------------------------------------------------------
class PresetMorphSlider : public IVSliderControl
{
public:
  PresetMorphSlider(const IRECT& bounds, IActionFunction aF, const IVStyle& style)
  : IVSliderControl(bounds, aF, "", style, false, EDirection::Horizontal)
  {
  }

  void DrawTrack(IGraphics& g, const IRECT& filledArea) override
  {
    IVSliderControl::DrawTrack(g, filledArea);

    // 8 个槽位刻度 (与下方 Q1..Q8 按钮对齐)
    const float x0 = mTrackBounds.L, w = mTrackBounds.W();
    for (int i = 0; i < kNumQuick; ++i)
    {
      const float x = x0 + w * i / (kNumQuick - 1.f);
      g.FillRect(IColor(255, 0, 0, 0), IRECT(x - 1.f, mTrackBounds.T - 3.f, x + 1.f, mTrackBounds.T));
      g.FillRect(IColor(255, 0, 0, 0), IRECT(x - 1.f, mTrackBounds.B, x + 1.f, mTrackBounds.B + 3.f));
    }
  }
};

// ---------------------------------------------------------------------------
// 极简滑块: 轨道浅灰、无黑描边; 已填充电平用柔和深灰 (非纯黑);
// 圆形手柄白底黑描边 (与 pad 节点一致, 保留描边).
//   - 横条 (无 sideText): 标题在控件顶部左侧左对齐, 数值在顶部右侧右对齐,
//     点击数值区域弹行内编辑 (iPlug2 自动回写参数并通知宿主)
//   - 竖条 (GAIN, 传 sideText): 轨道靠左, 右侧竖排字样 + 数值, 同样支持点击编辑
// ⚠ 注意: iPlug2 绘制控件时会裁剪到控件矩形 (IGraphics::DrawControl),
//   因此标题/数值必须画在控件矩形内部 —— 轨道经 OnResize 平移留出空间
// ---------------------------------------------------------------------------
class GRMSlider : public IVSliderControl
{
public:
  GRMSlider(const IRECT& bounds, int paramIdx, const char* label, const IVStyle& style,
            EDirection dir, const char* sideText = nullptr)
  : IVSliderControl(bounds, paramIdx, label, style, false, dir)
  , mSideText(sideText ? sideText : "")
  , mHeaderLabel(label ? label : "")
  {
    // 标题/数值全部自绘 (关闭内置 label/value 显示)
    mStyle.showLabel = false;
    mStyle.showValue = false;
  }

  void OnResize() override
  {
    IVSliderControl::OnResize();
    // 轨道平移到控件矩形内部, 给顶部 header / 右侧侧标签留出空间
    if (mSideText.GetLength() > 0)
    {
      // 竖条: 轨道靠左 kTrackW 宽, 右侧留出竖排字样 + 数值
      const float dx = mWidgetBounds.W() - kTrackW;
      if (dx > 0.f)
      {
        mWidgetBounds = mWidgetBounds.GetTranslated(-dx, 0.f);
        mTrackBounds  = mTrackBounds.GetTranslated(-dx, 0.f);
      }
    }
    else
    {
      // 横条: 轨道下移 kHeaderH, 上方留出标题/数值
      mWidgetBounds = mWidgetBounds.GetTranslated(0.f, kHeaderH);
      mTrackBounds  = mTrackBounds.GetTranslated(0.f, kHeaderH);
    }
  }

  void Draw(IGraphics& g) override
  {
    IVSliderControl::Draw(g);
    if (mSideText.GetLength() > 0)
      DrawSideLabel(g);
    else
      DrawHeader(g);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    // 点击数值区域 → 行内编辑 (iPlug2 自动 StringToValue 回写 + 通知宿主)
    if (mod.L && !mod.R && !mod.A && ValueRect().Contains(x, y))
    {
      PromptUserInput(ValueRect());
      return;
    }
    IVSliderControl::OnMouseDown(x, y, mod);   // 其余区域正常拖动
  }

  void DrawTrack(IGraphics& g, const IRECT& filledArea) override
  {
    const float cr = GetRoundedCornerRadius(mTrackBounds);
    // 轨道底: 浅灰, 无黑描边
    g.FillRoundRect(COL_TRACK, mTrackBounds, cr, &mBlend);
    // 已填充 (电平) 部分: 柔和深灰, 非纯黑
    if (filledArea.W() > 0.5f && filledArea.H() > 0.5f)
      g.FillRoundRect(COL_FILL, filledArea, cr, &mBlend);
  }

  void DrawHandle(IGraphics& g, const IRECT& bounds) override
  {
    const float cx = bounds.MW(), cy = bounds.MH();
    const float r  = bounds.W() * 0.5f;
    // 圆形手柄: 白底 + 黑色描边 (保留描边, 与 pad 节点一致)
    g.FillCircle(COLOR_WHITE, cx, cy, r);
    g.DrawCircle(COL_BLACK, cx, cy, r - 0.75f, nullptr, 1.5f);
  }

private:
  static constexpr float kHeaderH = 7.f;   // 顶部标题/数值行高 (横条)
  static constexpr float kTrackW  = 24.f;  // 轨道宽度 (竖条, 与 GAIN 原宽一致)

  // 数值可点击区域 (与 DrawHeader/DrawSideLabel 的数值绘制位置一致)
  IRECT ValueRect() const
  {
    const IRECT r = mRECT;
    if (mSideText.GetLength() > 0)
    {
      const float labX = r.L + kTrackW + 12.f;      // 竖排字样中心
      const float numCx = labX + 6.f + 13.f;        // 数值中心
      return IRECT(numCx - 13.f, r.MH() - 7.f, numCx + 13.f, r.MH() + 7.f);
    }
    return IRECT(r.L, r.T, r.R, r.T + kHeaderH);    // 顶部整行 (右侧数值)
  }

  void DrawHeader(IGraphics& g)
  {
    const IRECT r = mRECT;
    const IRECT headerRect(r.L, r.T, r.R, r.T + kHeaderH);
    // 标题: 顶部左侧 左对齐
    IText lt(10, COL_BLACK, "Outfit-SemiBold", EAlign::Near, EVAlign::Middle);
    g.DrawText(lt, mHeaderLabel.Get(), IRECT(headerRect.L, headerRect.T, headerRect.MW(), headerRect.B));
    // 数值: 顶部右侧 右对齐 (用 IParam 显示格式, 如 "0.85" / "4.00 Hz")
    WDL_String ds;
    if (GetParam()) GetParam()->GetDisplay(ds, false);
    IText vt(10, COL_DIM, "Outfit", EAlign::Far, EVAlign::Middle);
    g.DrawText(vt, ds.Get(), IRECT(headerRect.MW(), headerRect.T, headerRect.R, headerRect.B));
  }

  void DrawSideLabel(IGraphics& g)
  {
    if (mSideText.GetLength() == 0) return;
    const IRECT r = mRECT;
    // 竖排字样: 轨道右侧 (控件矩形内)
    const float labX = r.L + kTrackW + 12.f;
    IText gt(11, COL_BLACK, "Outfit-SemiBold", EAlign::Center, EVAlign::Middle, -90.f);
    g.DrawText(gt, mSideText.Get(), IRECT(labX - 6.f, r.T, labX + 6.f, r.B));
    // 数值: 竖排字样右侧
    const float numCx = labX + 6.f + 13.f;
    WDL_String ds;
    if (GetParam()) GetParam()->GetDisplay(ds, false);
    IText nt(9, COL_DIM, "Outfit", EAlign::Center, EVAlign::Middle);
    g.DrawText(nt, ds.Get(), IRECT(numCx - 13.f, r.MH() - 7.f, numCx + 13.f, r.MH() + 7.f));
  }

  WDL_String mSideText;
  WDL_String mHeaderLabel;
};

// ---------------------------------------------------------------------------
// 锁定开关: ON 时黑白反转 (黑底白字), 对应 nono 的 .btn.active 语言;
// IVToggleControl 只反转填充不反转文字, ON 时会黑字配黑底, 故重写 DrawValue
// ---------------------------------------------------------------------------
class InvertToggleControl : public IVToggleControl
{
public:
  InvertToggleControl(const IRECT& bounds, int paramIdx, const char* label,
                      const IVStyle& style, const char* offText, const char* onText)
  : IVToggleControl(bounds, paramIdx, label, style, offText, onText)
  {
  }

  void DrawValue(IGraphics& g, bool mouseOver) override
  {
    if (mouseOver)
      g.FillRect(GetColor(kHL), mWidgetBounds, &mBlend);

    // 文本画在按钮本体中央 (showLabel/showValue 关闭后 widget 即整个矩形)
    const bool on = GetValue() > 0.5;
    IText t = mStyle.valueText;
    t.mFGColor = on ? COLOR_WHITE : COL_BLACK;
    g.DrawText(t, on ? mOnText.Get() : mOffText.Get(), mWidgetBounds, &mBlend);
  }
};

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
GRMBandPass::GRMBandPass(const InstanceInfo& info)
: Plugin(info, MakeConfig(kNumParams, 1))
{
  // ---- 参数注册 (纯带通: 仅 freq / bw / gain) ----
  GetParam(kFreqL)->InitDouble("FreqL", 1000., 20., 20000., 0.01, "Hz", 0, "", IParam::ShapeExp());
  GetParam(kBwL)  ->InitDouble("BW L", 1., 0.05, 4., 0.01, "oct");
  GetParam(kGainL)->InitDouble("Gain L", 1., 0., 2., 0.01, "");
  GetParam(kFreqR)->InitDouble("FreqR", 1000., 20., 20000., 0.01, "Hz", 0, "", IParam::ShapeExp());
  GetParam(kBwR)  ->InitDouble("BW R", 1., 0.05, 4., 0.01, "oct");
  GetParam(kGainR)->InitDouble("Gain R", 1., 0., 2., 0.01, "");
  GetParam(kLink) ->InitBool("Link", false);
  GetParam(kMix)  ->InitDouble("Mix", 1., 0., 1., 0.01, "");
  GetParam(kAgOn) ->InitBool("Agitation", false);
  GetParam(kAgAmount)->InitDouble("Ag Amount", 0.1, 0., 1., 0.01, "");
  GetParam(kAgRate)->InitDouble("Ag Rate", 1., 0.05, 20., 0.01, "Hz");

  // ---- 24 预设槽位: 先填默认快照 (纯数字, 无名字) ----
  for (int i = 0; i < kNumPresets; ++i)
  {
    mPresets[i] = Snapshot();
    mSlotNumber[i] = i;   // 初始: 位置 i 显示编号 i+1
  }

  mDefaultSnapshot = Snapshot();   // 出厂默认 (右键"恢复默认"用)
  mStableSnapshot  = Snapshot();

  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 500.;  s[kBwL] = 0.2;  s[kFreqR] = 500.; s[kBwR] = 0.2; s[kLink] = 1.;
    mPresets[1] = s; // 窄带 500
  }
  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 2000.; s[kBwL] = 3.0;  s[kFreqR] = 2000.; s[kBwR] = 3.0; s[kLink] = 1.;
    mPresets[2] = s; // 宽带 2k
  }
  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 400.; s[kBwL] = 0.3; s[kFreqR] = 4000.; s[kBwR] = 1.5;
    mPresets[3] = s; // 分离 L/R
  }
  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 3000.; s[kBwL] = 0.5; s[kFreqR] = 3000.; s[kBwR] = 0.5; s[kLink] = 1.;
    s[kAgOn] = 1.; s[kAgAmount] = 0.3; s[kAgRate] = 4.;
    mPresets[4] = s; // 抖动
  }
  {
    ParamSnapshot s = Snapshot();
    s[kFreqL] = 150.; s[kBwL] = 2.5; s[kFreqR] = 150.; s[kBwR] = 2.5; s[kLink] = 1.;
    mPresets[5] = s; // 低频
  }

#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]()
  {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS,
                        GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
  };

  mLayoutFunc = [&](IGraphics* pGraphics)
  {
    pGraphics->AttachCornerResizer(EUIResizerMode::Scale, false);
    pGraphics->AttachPanelBackground(COL_BG);
    pGraphics->LoadFont("Outfit", OUTFIT_FN);
    pGraphics->LoadFont("Outfit-SemiBold", OUTFIT_SB_FN);
    pGraphics->LoadFont("Outfit-Bold", OUTFIT_BD_FN);

    const IVStyle style   = MakeGRMStyle();
    const IVStyle btnStyle= MakeButtonStyle();
    // 开关用: 关闭标签条预留, 让可点区域占满整个按钮矩形
    IVStyle toggleStyle = btnStyle;
    toggleStyle.showLabel = false;
    toggleStyle.showValue = false;

    // ================= 主控区: LEFT / RIGHT 双通道 (上下堆叠) =================
    // 栅格: 页边距 20, 列间距 24; 左列 pad x20..668, gain 列 x692..716, 右面板 x740..988
    // pad 内四角可编辑 (CENTER/BANDWIDTH/LOWCUT/HIGHCUT) + 网格下方双点范围滑块;
    // 交互换算经 hooks 交回插件层, 底层仍由 kFreq + kBw 两个自由度驱动 (low/high 为派生视图)
    auto padHooks = [&](int kF, int kB) -> FilterNodePad::Hooks {
      return FilterNodePad::Hooks{
        [this] { MaybePushGestureUndo(); },
        [this, kF, kB](int id, double v) { EditCorner(kF, kB, id, v); },
        [this, kF, kB](double lN, double hN) { EditBand(kF, kB, lN, hN); },
      };
    };

    // LEFT 滤波节点板 (纯带通: X=中心频率, Y=带宽), 横长纵短
    mPadL = new FilterNodePad(IRECT(20, 38, 668, 270), { kFreqL, kBwL }, "LEFT", style, padHooks(kFreqL, kBwL));
    pGraphics->AttachControl(mPadL);

    // RIGHT 模块
    mPadR = new FilterNodePad(IRECT(20, 302, 668, 534), { kFreqR, kBwR }, "RIGHT", style, padHooks(kFreqR, kBwR));
    pGraphics->AttachControl(mPadR);

    // gain 纵向列 (紧贴 pad 右侧, 留一点间隙; 上下边与 pad 对齐; 竖排 GAIN 字样 + 数值在右侧,
    // 矩形右扩容纳标签区, 轨道经 OnResize 靠左)
    pGraphics->AttachControl(new GRMSlider(IRECT(672, 38, 730, 270), kGainL, "GAIN L", style, EDirection::Vertical, "GAIN"));
    pGraphics->AttachControl(new GRMSlider(IRECT(672, 302, 730, 534), kGainR, "GAIN R", style, EDirection::Vertical, "GAIN"));

    // ================= 右侧控制面板 =================
    pGraphics->AttachControl(new ITextControl(IRECT(740, 14, 900, 32), "PRESETS",
      IText(12, COL_TEXT, "Outfit-Bold", EAlign::Near, EVAlign::Middle)));

    // 预设槽交互工厂 (底部 1..8 固定 + 右侧 9..24 共用):
    // 单击加载 · 拖拽交换(编号+内容整体对调) · ⌘+点击保存 · ⌥+点击恢复默认 · 右键菜单 · 悬停提示
    // 闭包捕获"按钮位置", 编号经 mSlotNumber 查表 (交换后按钮仍绑定原位置)
    auto makeSlotHooks = [this](int pos) -> PresetSlotControl::Hooks
    {
      return PresetSlotControl::Hooks{
        [this, pos]() { LoadSlot(mSlotNumber[pos]); },
        [this, pos]() { SaveToSlot(mSlotNumber[pos]); },
        [this, pos]() { RestoreDefault(mSlotNumber[pos]); },
        [this, pos]() { OnDragBegin(pos); },
        [this, pos](float x, float y) { OnDragMove(x, y); },
        [this, pos](float x, float y) { OnDragDrop(pos, x, y); },
        [this, pos]() -> std::string {
          char buf[24];
          snprintf(buf, sizeof(buf), "Preset %d", mSlotNumber[pos] + 1);
          return buf;
        },
      };
    };

    // 右侧预设库: 槽 9..24 (2 列 x 8 行)
    for (int r = 0; r < 8; ++r)
    {
      for (int c = 0; c < 2; ++c)
      {
        const int pos = kNumBottom + r * 2 + c;   // 按钮位置 8..23 (初始编号 9..24)
        char label[8];
        snprintf(label, 8, "%d", mSlotNumber[pos] + 1);
        PresetSlotControl* btn = new PresetSlotControl(
          IRECT(740 + c * 64, 38 + r * 26, 796 + c * 64, 60 + r * 26),
          makeSlotHooks(pos), label, btnStyle);
        mSlotButtons[pos] = btn;
        pGraphics->AttachControl(btn);
      }
    }

    // Agitation 区 (旋钮已改滑块: 标题/数值在控件顶部, 轨道经 OnResize 下移)
    pGraphics->AttachControl(new ITextControl(IRECT(740, 260, 900, 278), "AGITATION",
      IText(11, COL_TEXT, "Outfit-Bold", EAlign::Near, EVAlign::Middle)));
    pGraphics->AttachControl(new InvertToggleControl(IRECT(740, 284, 796, 306), kAgOn, " ", toggleStyle, "OFF", "ON"));
    pGraphics->AttachControl(new GRMSlider(IRECT(740, 308, 988, 340), kAgAmount, "INTENSITY", style, EDirection::Horizontal));
    pGraphics->AttachControl(new GRMSlider(IRECT(740, 342, 988, 374), kAgRate, "RATE", style, EDirection::Horizontal));

    // 声像区 (无标题): L->R / R->L / LINK / FLIP 2x2 排列 (click 触发, 非开关)
    pGraphics->AttachControl(MakeMomentary(IRECT(740, 382, 796, 404), [this](IControl*) { CopyLtoR(); }, "L->R", btnStyle));
    pGraphics->AttachControl(MakeMomentary(IRECT(804, 382, 860, 404), [this](IControl*) { CopyRtoL(); }, "R->L", btnStyle));
    pGraphics->AttachControl(new InvertToggleControl(IRECT(740, 406, 796, 428), kLink, " ", toggleStyle, "LINK", "LINK")); // 黑白反转表示状态
    pGraphics->AttachControl(MakeMomentary(IRECT(804, 406, 860, 428), [this](IControl*) { FlipLR(); }, "FLIP", btnStyle));
    pGraphics->AttachControl(new GRMSlider(IRECT(740, 436, 988, 470), kMix, "MIX", style, EDirection::Horizontal));

    // undo / redo (统一按钮尺寸 56x22)
    pGraphics->AttachControl(MakeMomentary(IRECT(740, 480, 796, 502), [this](IControl*) { Undo(); }, "UNDO", btnStyle));
    pGraphics->AttachControl(MakeMomentary(IRECT(804, 480, 860, 502), [this](IControl*) { Redo(); }, "REDO", btnStyle));

    // ================= 底部条 (固定槽 1..8 + morph 条) =================
    // 底部固定显示 8 个按钮, 初始编号 1..8; 拖拽交换后按钮位置不变,
    // 编号与内容整体互换 (mSlotNumber 映射)
    for (int i = 0; i < kNumBottom; ++i)
    {
      char label[8];
      snprintf(label, 8, "%d", mSlotNumber[i] + 1);
      PresetSlotControl* btn = new PresetSlotControl(
        IRECT(20 + i * 82, 546, 94 + i * 82, 568),
        makeSlotHooks(i), label, btnStyle);
      mSlotButtons[i] = btn;
      pGraphics->AttachControl(btn);
    }
    // 预设 morph 条: 在槽 1..8 相邻参数间平滑插值; 控件矩形两端内缩 handleSize(8px),
    // 使轨道/刻度/手柄正好落在底部按钮中心线上 (57 + i*82)
    mMorphSlider = new PresetMorphSlider(IRECT(49, 572, 639, 592),
      [this](IControl* pCtrl) {
        MaybePushGestureUndo();          // 新手势起点记录 morph 前状态
        mMorphPos = pCtrl->GetValue(0) * (kNumQuick - 1.0);   // 记录位置供 JSON 保存
        OnMorphDrag(pCtrl->GetValue(0));
      }, btnStyle);
    pGraphics->AttachControl(mMorphSlider);
    pGraphics->AttachControl(MakeMomentary(IRECT(740, 546, 796, 568), [this](IControl*) { SaveFile(); }, "SAVE", btnStyle));
    pGraphics->AttachControl(MakeMomentary(IRECT(804, 546, 860, 568), [this](IControl*) { LoadFile(); }, "LOAD", btnStyle));

    // 品牌标识 + 版本号 (右下角, 矩形避开 SAVE/LOAD 的命中区域)
    pGraphics->AttachControl(new ITextControl(IRECT(740, 508, 988, 532), "GRM BANDPASS",
      IText(16, COL_TEXT, "Outfit-Bold", EAlign::Far, EVAlign::Middle)));
    pGraphics->AttachControl(new ITextControl(IRECT(868, 532, 988, 550), "v" PLUG_VERSION_STR,
      IText(9, COL_FAINT, "Outfit", EAlign::Far, EVAlign::Middle)));

    // 初始状态
    pGraphics->EnableTooltips(true);   // 预设槽悬停提示 (系统 tooltip)
    UpdatePads();
  };
#endif
}

#if IPLUG_DSP
void GRMBandPass::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  // 音频线程: 取走编辑器线程发布的最新参数 (写入中则沿用上一块)
  grm::BandPassCore::Params p;
  if (mParamMailbox.consume(p))
    mCore.setParams(p);

  mCore.updateSmoothing(nFrames);

  // 防护: 只有在输入输出都连了至少 2 声道时才走立体声路径, 否则按单声道处理并复制
  const int nOuts = NOutChansConnected();
  const int nIns = NInChansConnected();

  if (nOuts >= 2 && nIns >= 2)
  {
    mCore.process(inputs[0], inputs[1], outputs[0], outputs[1], nFrames);
    for (int c = 2; c < nOuts; ++c)
      std::memcpy(outputs[c], inputs[c], nFrames * sizeof(sample));
  }
  else
  {
    mCore.process(inputs[0], outputs[0], nFrames);
    for (int c = 1; c < nOuts; ++c)
      std::memcpy(outputs[c], outputs[0], nFrames * sizeof(sample));
  }
}

void GRMBandPass::OnReset()
{
  // 先同步参数再 prepare: 让平滑状态快照取到真实参数, 避免载入后从默认值滑音
  mCore.setParams(CollectParams());
  mCore.prepare(GetSampleRate(), GetBlockSize());
}

void GRMBandPass::OnParamChange(int paramIdx, EParamSource source, int sampleOffset)
{
  if (source == EParamSource::kHost)
  {
    // 宿主自动化在音频线程回调: 直接写核心, 与 ProcessBlock 天然串行, 不经信箱
    mCore.setParams(CollectParams());
  }
  else
  {
    PublishParamsToCore();
  }
}

void GRMBandPass::OnParamChangeUI(int paramIdx, EParamSource source)
{
  // 兜底发布: 部分格式 (如 APP) 的 UI 拖动不经宿主回合到 OnParamChange
  PublishParamsToCore();
  if (source == EParamSource::kUI)
  {
    MaybePushGestureUndo();
    MirrorLinkedParams(paramIdx);  // LINK 跟随仅响应真实 UI 手势
  }
  UpdatePads();
}
#endif

// ---------------------------------------------------------------------------
// 参数同步与显示
// ---------------------------------------------------------------------------
grm::BandPassCore::Params GRMBandPass::CollectParams() const
{
  grm::BandPassCore::Params p;
  p.freqL  = GetParam(kFreqL)->Value();
  p.bwL    = GetParam(kBwL)->Value();
  p.gainL  = static_cast<float>(GetParam(kGainL)->Value());
  p.freqR  = GetParam(kFreqR)->Value();
  p.bwR    = GetParam(kBwR)->Value();
  p.gainR  = static_cast<float>(GetParam(kGainR)->Value());
  p.linked = GetParam(kLink)->Value() > 0.5;
  p.mix    = static_cast<float>(GetParam(kMix)->Value());
  p.agOn   = GetParam(kAgOn)->Value() > 0.5;
  p.agAmount = static_cast<float>(GetParam(kAgAmount)->Value());
  p.agRate = GetParam(kAgRate)->Value();
  return p;
}

void GRMBandPass::PublishParamsToCore()
{
  mParamMailbox.publish(CollectParams());
}

void GRMBandPass::SetParamFromEditor(int idx, double value)
{
  GetParam(idx)->Set(value);
  InformHostOfParamChange(idx, GetParam(idx)->GetNormalized());
  PublishParamsToCore();
}

void GRMBandPass::RefreshAfterEdit()
{
#if IPLUG_EDITOR
  if (GetUI())
  {
    // 让所有绑定参数的控件 (滑条/旋钮/开关/XY pad) 从参数回读最新值
    SendCurrentParamValuesFromDelegate();
    GetUI()->SetAllControlsDirty();
  }
#endif
  UpdatePads();
  MarkStateStable();
}

// ---------------------------------------------------------------------------
// pad 四角 / 范围滑块换算: 保持底层 kFreq + kBw 两个自由度, low/high 为派生视图
//   low  = center * 2^(-bw/2),  high = center * 2^(+bw/2)
//   center = sqrt(low*high),    bw = log2(high/low)
// ---------------------------------------------------------------------------
void GRMBandPass::EditCorner(int kFreq, int kBw, int cornerId, double value)
{
  PushUndo();
  const IParam* pf = GetParam(kFreq);
  const double center = pf->FromNormalized(GetParam(kFreq)->GetNormalized());
  const double bw = GetParam(kBw)->Value();
  double lowHz = center * std::pow(2., -bw / 2.);
  double highHz = center * std::pow(2., bw / 2.);
  double nc = center, nb = bw;
  switch (cornerId)
  {
    case kCornerCenter: nc = value;                                  nb = bw;    break;
    case kCornerBw:     nc = center;                                nb = value; break;
    case kCornerLow:    lowHz = value;  nc = std::sqrt(lowHz * highHz); nb = std::log2(highHz / lowHz); break;
    case kCornerHigh:   highHz = value; nc = std::sqrt(lowHz * highHz); nb = std::log2(highHz / lowHz); break;
  }
  ClampAndSet(kFreq, kBw, nc, nb);
}

void GRMBandPass::EditBand(int kFreq, int kBw, double lowNorm, double highNorm)
{
  const IParam* pf = GetParam(kFreq);
  const double lowHz = pf->FromNormalized(lowNorm);
  const double highHz = pf->FromNormalized(highNorm);
  const double center = std::sqrt(lowHz * highHz);
  const double bw = std::log2(highHz / lowHz);
  ClampAndSet(kFreq, kBw, center, bw);
}

void GRMBandPass::ClampAndSet(int kFreq, int kBw, double centerHz, double bwOct)
{
  centerHz = std::clamp(centerHz, 20., 20000.);
  bwOct    = std::clamp(bwOct, 0.05, 4.);
  double lowHz = centerHz * std::pow(2., -bwOct / 2.);
  double highHz = centerHz * std::pow(2., bwOct / 2.);
  if (lowHz < 20.)     { lowHz = 20.;   centerHz = highHz * std::pow(2., -bwOct / 2.); }
  if (highHz > 20000.) { highHz = 20000.; centerHz = lowHz * std::pow(2., bwOct / 2.); }
  SetParamFromEditor(kFreq, centerHz);
  SetParamFromEditor(kBw, bwOct);
  RefreshAfterEdit();
}

// 把 freq/bw 参数同步到 XY 手柄位置: 通过 SetValueFromDelegate (只写控件内部值, 不回写参数)
// 解决 "预设/同步/翻转等改值时 XY 轴手柄与显示不跟随" 的问题
void GRMBandPass::UpdatePads()
{
  if (mPadL)
  {
    mPadL->SetValueFromDelegate(GetParam(kFreqL)->GetNormalized(), 0);
    mPadL->SetValueFromDelegate(GetParam(kBwL)->GetNormalized(), 1);
    mPadL->SetDirty(false);
  }
  if (mPadR)
  {
    mPadR->SetValueFromDelegate(GetParam(kFreqR)->GetNormalized(), 0);
    mPadR->SetValueFromDelegate(GetParam(kBwR)->GetNormalized(), 1);
    mPadR->SetDirty(false);
  }
}

// ---------------------------------------------------------------------------
// 预设 / undo / redo
// ---------------------------------------------------------------------------
ParamSnapshot GRMBandPass::Snapshot() const
{
  ParamSnapshot s;
  for (int i = 0; i < kNumParams; ++i)
    s[i] = GetParam(i)->Value();
  return s;
}

void GRMBandPass::ApplySnapshot(const ParamSnapshot& s)
{
  for (int i = 0; i < kNumParams; ++i)
    SetParamFromEditor(i, s[i]);
  RefreshAfterEdit();
}

void GRMBandPass::PushUndo()
{
  PushUndoSnapshot(Snapshot());
}

void GRMBandPass::PushUndoSnapshot(const ParamSnapshot& s)
{
  if (!mUndoStack.empty() && mUndoStack.back() == s) return;  // 与栈顶相同则不入栈
  mUndoStack.push_back(s);
  if (mUndoStack.size() > 100) mUndoStack.pop_front();
  mRedoStack.clear();
}

// iPlug2 无手势开始/结束回调: 以 kUI 参数事件的到达间隔判断,
// 超过 kGestureGapSec 视为一次新手势, 入栈"手势前"的稳定快照;
// 手势结束后由 OnIdle 把最终状态固化为新的稳定快照
static constexpr double kGestureGapSec = 0.4;

void GRMBandPass::MaybePushGestureUndo()
{
  using namespace std::chrono;
  const double now = duration<double>(steady_clock::now().time_since_epoch()).count();
  if (now - mLastUIChangeTime > kGestureGapSec)
    PushUndoSnapshot(mStableSnapshot);
  mLastUIChangeTime = now;
  mGesturePending = true;
}

void GRMBandPass::OnIdle()
{
  using namespace std::chrono;
  const double now = duration<double>(steady_clock::now().time_since_epoch()).count();
  if (mGesturePending && now - mLastUIChangeTime > kGestureGapSec)
  {
    mStableSnapshot = Snapshot();
    mGesturePending = false;
  }
}

void GRMBandPass::MarkStateStable()
{
  mStableSnapshot = Snapshot();
  mGesturePending = false;
}

void GRMBandPass::Undo()
{
  if (mUndoStack.empty()) return;
  mRedoStack.push_back(Snapshot());
  const ParamSnapshot s = mUndoStack.back();
  mUndoStack.pop_back();
  ApplySnapshot(s);
}

void GRMBandPass::Redo()
{
  if (mRedoStack.empty()) return;
  mUndoStack.push_back(Snapshot());
  const ParamSnapshot s = mRedoStack.back();
  mRedoStack.pop_back();
  ApplySnapshot(s);
}

void GRMBandPass::SaveToSlot(int idx)
{
  if (idx < 0 || idx >= kNumPresets) return;
  mPresets[idx] = Snapshot();
}

void GRMBandPass::LoadSlot(int idx)
{
  if (idx < 0 || idx >= kNumPresets) return;
  PushUndo();
  mCurrentPreset = idx;
  ApplySnapshot(mPresets[idx]);
}

void GRMBandPass::RestoreDefault(int idx)
{
  if (idx < 0 || idx >= kNumPresets) return;
  mPresets[idx] = mDefaultSnapshot;   // 参数恢复出厂
  if (idx == mCurrentPreset)
  {
    // 恢复的是当前选中槽: 立即应用便于试听 (可撤销)
    PushUndo();
    ApplySnapshot(mDefaultSnapshot);
  }
}

// 拖拽交换: 两个按钮位置的编号绑定互换 = 编号与内容整体对调
// (按钮位置不变, 但显示的编号和加载的内容都换成对方的)
void GRMBandPass::SwapSlots(int posA, int posB)
{
  if (posA == posB) return;
  if (posA < 0 || posA >= kNumPresets || posB < 0 || posB >= kNumPresets) return;
  std::swap(mSlotNumber[posA], mSlotNumber[posB]);
  RefreshSlotLabels();
}

// 刷新全部按钮的编号标签 (编号随交换/LOAD 变化)
void GRMBandPass::RefreshSlotLabels()
{
  for (int i = 0; i < kNumPresets; ++i)
  {
    if (!mSlotButtons[i]) continue;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", mSlotNumber[i] + 1);
    mSlotButtons[i]->SetSlotLabel(buf);
  }
}

// ---------------------------------------------------------------------------
// 预设文件 (JSON) / 拖拽
// ---------------------------------------------------------------------------

void GRMBandPass::SaveFile()
{
  if (!GetUI()) return;
  mDialogFileName.Set("GRMBandPass Presets");
  mDialogPath.Set("");   // 默认目录: 最近一次使用的目录
  GetUI()->PromptForFile(mDialogFileName, mDialogPath, EFileAction::Save, "json",
    [this](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength() == 0) return;   // 用户取消
      // 注意: macOS 回调里 fileName 是完整路径, path 只是目录
      std::string full = fileName.Get();
      if (full.size() < 5 || full.compare(full.size() - 5, 5, ".json") != 0)
        full += ".json";                       // NSSavePanel 不自动补扩展名
      std::string err;
      WritePresetFileTo(full, err);
      if (!err.empty() && GetUI())
        GetUI()->ShowMessageBox(err.c_str(), "Save Failed", kMB_OK);
    });
}

void GRMBandPass::LoadFile()
{
  if (!GetUI()) return;
  mDialogFileName.Set("");
  GetUI()->PromptForFile(mDialogFileName, mDialogPath, EFileAction::Open, "json",
    [this](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength() == 0) return;   // 用户取消
      std::string err;
      ReadPresetFileFrom(fileName.Get(), err);
      if (!err.empty() && GetUI())
        GetUI()->ShowMessageBox(err.c_str(), "Load Failed", kMB_OK);
    });
}

void GRMBandPass::WritePresetFileTo(const std::string& path, std::string& err)
{
  PresetFileData data;
  for (const auto& p : mPresets)
  {
    std::vector<double> vals(p.begin(), p.end());
    data.presets.push_back(std::move(vals));
  }
  const ParamSnapshot cur = Snapshot();
  data.currentValues.assign(cur.begin(), cur.end());
  data.currentPreset = mCurrentPreset;
  data.morphPos = mMorphPos;

  if (WritePresetFile(path, data, err))
    err.clear();
}

void GRMBandPass::ReadPresetFileFrom(const std::string& path, std::string& err)
{
  PresetFileData data;
  if (!ReadPresetFile(path, data, err)) return;

  // 字段校验
  if ((int) data.presets.size() != kNumPresets) { err = "Preset count mismatch (expected 24)"; return; }
  for (const auto& e : data.presets)
    if ((int) e.size() != kNumParams)           { err = "Preset parameter count mismatch (expected 11)"; return; }
  if ((int) data.currentValues.size() != kNumParams) { err = "Current values count mismatch (expected 11)"; return; }

  PushUndo();   // LOAD 对当前参数的改变可撤销 (库数据变化不撤销)

  for (int i = 0; i < kNumPresets; ++i)
    std::copy(data.presets[i].begin(), data.presets[i].end(), mPresets[i].begin());
  mCurrentPreset = std::clamp(data.currentPreset, 0, kNumPresets - 1);

  ParamSnapshot cur {};
  std::copy(data.currentValues.begin(), data.currentValues.end(), cur.begin());
  ApplySnapshot(cur);   // 恢复当前参数 (不被 morph 位置覆盖)

  // morph 条位置只恢复 UI 显示, 不重新触发插值 (参数以 currentValues 为准)
  mMorphPos = std::clamp(data.morphPos, 0.0, (double) (kNumQuick - 1));
  if (mMorphSlider)
  {
    mMorphSlider->SetValue((float) (mMorphPos / (kNumQuick - 1.0)));
    mMorphSlider->SetDirty(true);
  }

  // 按钮排列不随文件保存: LOAD 后重置为默认 (底部 1..8, 右侧 9..24)
  for (int i = 0; i < kNumPresets; ++i)
    mSlotNumber[i] = i;
  RefreshSlotLabels();

  err.clear();
}

// 拖拽: 源槽开始拖拽
void GRMBandPass::OnDragBegin(int src)
{
  mDragSourceSlot = src;
  mDragTargetSlot = -1;
}

// 拖拽移动: 实时更新目标槽高亮
void GRMBandPass::OnDragMove(float x, float y)
{
  if (mDragSourceSlot < 0) return;
  int target = -1;
  for (int i = 0; i < kNumPresets; ++i)
  {
    if (!mSlotButtons[i]) continue;
    if (mSlotButtons[i]->GetWidgetBounds().Contains(x, y)) { target = i; break; }
  }
  if (target == mDragSourceSlot) target = -1;   // 源槽本身不算目标
  if (target == mDragTargetSlot) return;

  if (mDragTargetSlot >= 0 && mSlotButtons[mDragTargetSlot])
    mSlotButtons[mDragTargetSlot]->SetDragTarget(false);
  mDragTargetSlot = target;
  if (mDragTargetSlot >= 0 && mSlotButtons[mDragTargetSlot])
    mSlotButtons[mDragTargetSlot]->SetDragTarget(true);
}

// 松手: 判定落点并交换
void GRMBandPass::OnDragDrop(int src, float x, float y)
{
  if (mDragTargetSlot >= 0 && mSlotButtons[mDragTargetSlot])
    mSlotButtons[mDragTargetSlot]->SetDragTarget(false);
  mDragTargetSlot = -1;

  int target = -1;
  for (int i = 0; i < kNumPresets; ++i)
  {
    if (!mSlotButtons[i]) continue;
    if (mSlotButtons[i]->GetWidgetBounds().Contains(x, y)) { target = i; break; }
  }
  mDragSourceSlot = -1;
  if (target >= 0 && target != src)
    SwapSlots(src, target);
}

// 声像区: 数值拷贝 / 交换 (click 触发)
// 经 SetParamFromEditor 写值: 同步 DSP 信箱 + 通知宿主; 不在 DSP 里做音频路由
void GRMBandPass::CopyLtoR()
{
  PushUndo();
  SetParamFromEditor(kFreqR, GetParam(kFreqL)->Value());
  SetParamFromEditor(kBwR,   GetParam(kBwL)->Value());
  SetParamFromEditor(kGainR, GetParam(kGainL)->Value());
  RefreshAfterEdit();
}

void GRMBandPass::CopyRtoL()
{
  PushUndo();
  SetParamFromEditor(kFreqL, GetParam(kFreqR)->Value());
  SetParamFromEditor(kBwL,   GetParam(kBwR)->Value());
  SetParamFromEditor(kGainL, GetParam(kGainR)->Value());
  RefreshAfterEdit();
}

void GRMBandPass::FlipLR()
{
  PushUndo();
  const double fL = GetParam(kFreqL)->Value(), bL = GetParam(kBwL)->Value(), gL = GetParam(kGainL)->Value();
  SetParamFromEditor(kFreqL, GetParam(kFreqR)->Value());
  SetParamFromEditor(kBwL,   GetParam(kBwR)->Value());
  SetParamFromEditor(kGainL, GetParam(kGainR)->Value());
  SetParamFromEditor(kFreqR, fL);
  SetParamFromEditor(kBwR,   bL);
  SetParamFromEditor(kGainR, gL);
  RefreshAfterEdit();
}

// ---------------------------------------------------------------------------
// LINK 跟随: 开启 LINK 后拖动一个通道的 freq/bw/gain, 另一通道参数实时同步,
// 使对侧 pad/滑条显示与 DSP 一致地跟随移动
// ---------------------------------------------------------------------------
void GRMBandPass::MirrorLinkedParams(int paramIdx)
{
  if (GetParam(kLink)->Value() < 0.5) return;

  int mirror;
  switch (paramIdx)
  {
    case kFreqL: mirror = kFreqR; break;
    case kBwL:   mirror = kBwR;   break;
    case kGainL: mirror = kGainR; break;
    case kFreqR: mirror = kFreqL; break;
    case kBwR:   mirror = kBwL;   break;
    case kGainR: mirror = kGainL; break;
    default: return;
  }

  // 值已一致则不写, 防止 L<->R 往返
  if (std::fabs(GetParam(mirror)->Value() - GetParam(paramIdx)->Value()) < 1e-9) return;

  SetParamFromEditor(mirror, GetParam(paramIdx)->Value());
#if IPLUG_EDITOR
  // 把镜像值推给绑定该参数的控件 (对侧 pad / GAIN 滑条), 让显示立即跟随
  if (GetUI())
    SendParameterValueFromDelegate(mirror, GetParam(mirror)->GetNormalized(), true);
#endif
}

// ---------------------------------------------------------------------------
// 预设 morph 条: 在 Q1..Q8 相邻槽位之间对全部参数做线性插值
// (freq 等指数形参数在归一化域插值, 保证听感对数平滑)
// ---------------------------------------------------------------------------
ParamSnapshot GRMBandPass::InterpolatePresets(double pos)
{
  const int i0 = std::clamp(static_cast<int>(std::floor(pos)), 0, kNumQuick - 1);
  const int i1 = std::min(i0 + 1, kNumQuick - 1);
  const double t = std::clamp(pos - i0, 0.0, 1.0);

  ParamSnapshot out;
  for (int i = 0; i < kNumParams; ++i)
  {
    const IParam* p = GetParam(i);
    // morph 条对齐底部按钮: 插值"底部位置当前显示的编号"对应的内容 (跟随交换)
    const int n0 = mSlotNumber[i0], n1 = mSlotNumber[i1];
    const double a = p->FromNormalized(p->ToNormalized(mPresets[n0][i]));
    const double b = p->FromNormalized(p->ToNormalized(mPresets[n1][i]));
    out[i] = a + (b - a) * t;
  }
  return out;
}

void GRMBandPass::OnMorphDrag(double normalizedPos)
{
  ApplySnapshot(InterpolatePresets(normalizedPos * (kNumQuick - 1)));
}
