#pragma once

#include "IControls.h"
#include "../Theme.h"
#include "../Params.h"
#include "UiUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

// 屏幕钢琴键盘: 宿主 MIDI 回显 + 鼠标点击触发 (经 SendMidiMsgFromUI)。
// 琴键范围固定 C2(36)..C6(84), 与 960 宽窗口匹配。
class PianoKeyboardControl : public IControl
{
public:
  static constexpr int kLowNoteDefault = 36;  // C2
  static constexpr int kHighNoteDefault = 84; // C6
  static constexpr int kMsgTagNoteOn = 1;
  static constexpr int kMsgTagNoteOff = 2;

  struct Hooks
  {
    std::function<void(int note)> onNoteOn;
    std::function<void(int note)> onNoteOff;
    std::function<int()> baseKey; // 返回当前基准键 (画基准标记)
  };

  PianoKeyboardControl(const IRECT &bounds, Hooks hooks, int lowNote = kLowNoteDefault,
                       int highNote = kHighNoteDefault)
      : IControl(bounds), mHooks(std::move(hooks)), mLowNote(lowNote), mHighNote(highNote)
  {
    mIgnoreMouse = false;
  }

  void SetBaseKey(int note)
  {
    if (mBaseKey == note)
      return;
    mBaseKey = note;
    SetDirty(false);
  }

  // 宿主/delegate 回显
  // 约定: SendControlMsgFromDelegate(kCtrlTagKeyboard, msgTag, sizeof(int), &note)
  void OnMsgFromDelegate(int msgTag, int dataSize, const void *pData) override
  {
    if (!pData || dataSize < (int) sizeof(int))
      return;
    const int note = *static_cast<const int *>(pData);
    if (msgTag == kMsgTagNoteOn)
      SetNoteActive(note, true);
    else if (msgTag == kMsgTagNoteOff)
      SetNoteActive(note, false);
  }

  // ---- 几何 ----
  // 白键宽度自适应; 黑键按半音定位
  int NumWhiteKeys() const
  {
    int n = 0;
    for (int note = mLowNote; note <= mHighNote; ++note)
      if (!IsBlack(note))
        ++n;
    return n;
  }

  static bool IsBlack(int note)
  {
    static const bool kBlack[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
    return kBlack[note % 12];
  }

  void OnMouseDown(float x, float y, const IMouseMod &mod) override
  {
    if (mHooks.onNoteOn && !mod.R)
    {
      const int note = NoteAt(x, y);
      if (note >= 0)
      {
        mPressedNote = note;
        if (mHooks.onNoteOn)
          mHooks.onNoteOn(note);
        SetDirty(false);
      }
    }
  }

  void OnMouseUp(float x, float y, const IMouseMod &mod) override
  {
    if (mPressedNote >= 0 && mHooks.onNoteOff)
    {
      mHooks.onNoteOff(mPressedNote);
      mPressedNote = -1;
      SetDirty(false);
    }
  }

  void OnMouseOut() override
  {
    if (mPressedNote >= 0 && mHooks.onNoteOff)
    {
      mHooks.onNoteOff(mPressedNote);
      mPressedNote = -1;
      SetDirty(false);
    }
  }

  void Draw(IGraphics &g) override
  {
    const IRECT b = mRECT;
    const int nWhite = NumWhiteKeys();
    const float whiteW = b.W() / nWhite;

    // 白键
    for (int note = mLowNote; note <= mHighNote; ++note)
    {
      if (IsBlack(note))
        continue;
      const int wIdx = WhiteIndex(note);
      const IRECT key(b.L + wIdx * whiteW, b.T, b.L + (wIdx + 1) * whiteW, b.B);
      const IColor fill = mActive[note] ? COL_500() : COL_100();
      g.FillRect(fill, key);
      g.DrawRect(COL_500(), key);
      if (mPressedNote == note)
        g.FillRect(IColor(70, COL_900().R, COL_900().G, COL_900().B), key);
    }
    // 黑键
    const float blackW = whiteW * 0.62f;
    const float blackH = b.H() * 0.62f;
    for (int note = mLowNote; note <= mHighNote; ++note)
    {
      if (!IsBlack(note))
        continue;
      // 黑键中心落在该半音与左侧白键的交界
      const float cx = KeyCenterX(note, whiteW, b.L);
      const IRECT key(cx - blackW * 0.5f, b.T, cx + blackW * 0.5f, b.T + blackH);
      g.FillRect(mActive[note] ? COL_500() : COL_900(), key);
      if (mPressedNote == note)
        g.FillRect(IColor(70, COL_100().R, COL_100().G, COL_100().B), key);
    }

    // 基准键标记 (底部小三角)
    if (mBaseKey >= mLowNote && mBaseKey <= mHighNote && !IsBlack(mBaseKey))
    {
      const int wIdx = WhiteIndex(mBaseKey);
      const float cx = b.L + (wIdx + 0.5f) * whiteW;
      g.FillTriangle(COL_900(), cx - 5.f, b.B, cx + 5.f, b.B, cx, b.B - 9.f);
    }
    else if (mBaseKey >= mLowNote && mBaseKey <= mHighNote && IsBlack(mBaseKey))
    {
      const float cx = KeyCenterX(mBaseKey, whiteW, b.L);
      g.FillTriangle(COL_900(), cx - 5.f, b.B, cx + 5.f, b.B, cx, b.B - 9.f);
    }

    // 选中键标记 (高亮描边)
    if (mSelectedNote >= mLowNote && mSelectedNote <= mHighNote)
    {
      if (IsBlack(mSelectedNote))
      {
        const float cx = KeyCenterX(mSelectedNote, whiteW, b.L);
        const float blackW = whiteW * 0.62f;
        const float blackH = b.H() * 0.62f;
        g.DrawRect(IColor(255, 80, 190, 96),
                   IRECT(cx - blackW * 0.5f, b.T, cx + blackW * 0.5f, b.T + blackH), &mBlend, 2.f);
      }
      else
      {
        const int wIdx = WhiteIndex(mSelectedNote);
        g.DrawRect(IColor(255, 80, 190, 96),
                   IRECT(b.L + wIdx * whiteW, b.T, b.L + (wIdx + 1) * whiteW, b.B), &mBlend, 2.f);
      }
    }
  }

  void SetNoteActive(int note, bool on)
  {
    if (note < mLowNote || note > mHighNote)
      return;
    if (mActive[(size_t)note] == on)
      return;
    mActive[(size_t)note] = on;
    SetDirty(false);
  }

  // BANK 模式: 当前选中 (正在编辑) 的键, 画高亮框
  void SetSelectedNote(int note)
  {
    if (mSelectedNote == note)
      return;
    mSelectedNote = note;
    SetDirty(false);
  }

private:
  int WhiteIndex(int note) const
  {
    int idx = 0;
    for (int n = mLowNote; n < note; ++n)
      if (!IsBlack(n))
        ++idx;
    return idx;
  }

  // 黑键中心 = 其左侧白键与右侧白键的交界, 即第 WhiteIndex(note) 个白键的右边缘。
  // 之前多加 1 导致 C# 画在 D 右边、D# 画在 E 右边。
  float KeyCenterX(int note, float whiteW, float left) const
  {
    return left + WhiteIndex(note) * whiteW;
  }

  int NoteAt(float x, float y) const
  {
    if (!mRECT.Contains(x, y))
      return -1;
    const float whiteW = mRECT.W() / NumWhiteKeys();
    const int wIdx = std::min((int) std::floor((x - mRECT.L) / whiteW), NumWhiteKeys() - 1);
    const float blackH = mRECT.H() * 0.62f;
    const bool inBlackZone = y < mRECT.T + blackH;

    // 优先命中黑键: 检查左右两白键交界附近的黑键
    if (inBlackZone)
    {
      // 该白键 idx 左右边界可能各有黑键; 黑键中心在边界处
      for (int candidate = mLowNote; candidate <= mHighNote; ++candidate)
      {
        if (!IsBlack(candidate))
          continue;
        const float cx = KeyCenterX(candidate, whiteW, mRECT.L);
        if (std::fabs(x - cx) < whiteW * 0.31f && y - mRECT.T < blackH)
          return candidate;
      }
    }
    // 白键: 第 wIdx 个白键
    int count = -1;
    for (int n = mLowNote; n <= mHighNote; ++n)
    {
      if (!IsBlack(n))
        ++count;
      if (count == wIdx)
        return n;
    }
    return mLowNote;
  }

  Hooks mHooks;
  int mLowNote;
  int mHighNote;
  int mBaseKey = 48;
  int mPressedNote = -1;
  int mSelectedNote = -1;
  std::array<bool, 128> mActive{};
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
