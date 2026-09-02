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

// 屏幕钢琴键盘: 鼠标点击触发 + 宿主 MIDI 回显
class PianoKeyboardControl : public IControl
{
public:
  static constexpr int kLowNoteDefault = 48;  // C3
  static constexpr int kHighNoteDefault = 84; // C6
  static constexpr int kMsgTagNoteOn = 1;
  static constexpr int kMsgTagNoteOff = 2;

  struct Hooks
  {
    std::function<void(int note)> onNoteOn;
    std::function<void(int note)> onNoteOff;
    std::function<int()> baseKey;
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
      const float x0 = b.L + wIdx * whiteW;
      const float x1 = (wIdx + 1 == nWhite) ? b.R : (b.L + (wIdx + 1) * whiteW);
      const IRECT key(x0, b.T, x1, b.B);
      const IColor keyCol = WarmGray(OctaveBandV(note));
      const IColor fill = mActive[note] ? COL_500() : keyCol;
      g.FillRect(fill, key);
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
      const float cx = KeyCenterX(note, whiteW, b.L);
      const IRECT key(cx - blackW * 0.5f, b.T, cx + blackW * 0.5f, b.T + blackH);
      const IColor keyCol = mActive[note] ? COL_500() : COL_900();
      g.FillRect(keyCol, key);
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

    // 选中键高亮描边
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

  void SetSelectedNote(int note)
  {
    if (mSelectedNote == note)
      return;
    mSelectedNote = note;
    SetDirty(false);
  }

private:
  int OctaveBandV(int note) const
  {
    static const int kWhiteInOct[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
    const int wInOct = kWhiteInOct[note % 12];
    if (wInOct < 0)
      return 210;

    const int relOct = std::max(0, (note - mLowNote) / 12);
    struct OctBand { float v0, v1; };
    static const OctBand kBands[] = {
      {194.f, 218.f},
      {205.f, 229.f},
      {216.f, 240.f},
      {227.f, 239.f},
    };
    const int bandIdx = std::clamp(relOct, 0, (int)(sizeof(kBands) / sizeof(kBands[0])) - 1);
    const OctBand &b = kBands[bandIdx];

    if (note == mHighNote && wInOct == 0)
      return (int)std::lround(b.v0);

    const float t = (float)wInOct / 6.f;
    return (int)std::lround(b.v0 + (b.v1 - b.v0) * t);
  }

  int WhiteIndex(int note) const
  {
    int idx = 0;
    for (int n = mLowNote; n < note; ++n)
      if (!IsBlack(n))
        ++idx;
    return idx;
  }

  // 黑键中心 = 其左侧白键的右边缘
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

    // 优先命中黑键
    if (inBlackZone)
    {
      for (int candidate = mLowNote; candidate <= mHighNote; ++candidate)
      {
        if (!IsBlack(candidate))
          continue;
        const float cx = KeyCenterX(candidate, whiteW, mRECT.L);
        if (std::fabs(x - cx) < whiteW * 0.31f && y - mRECT.T < blackH)
          return candidate;
      }
    }
    // 白键
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
