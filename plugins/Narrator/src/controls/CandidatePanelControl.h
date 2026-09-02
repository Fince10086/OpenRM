#pragma once

#include "IControls.h"
#include "../Theme.h"
#include "../Strings.h"
#include "../Params.h"
#include "../dsp/tms/TmsVocab.h"
#include "../dsp/tms/VocabTI99.h"
#include "../dsp/tms/VocabAcorn.h"
#include "../dsp/tms/VocabSspell.h"
#include "../dsp/tms/VocabClock.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class CandidatePanelControl : public IControl
{
public:
  CandidatePanelControl(const IRECT &bounds, std::function<void(const std::string &)> onWordSelected = nullptr)
      : IControl(bounds), mOnWordSelected(std::move(onWordSelected))
  {
    UpdateSections();
  }

  void SetWordSelectHandler(std::function<void(const std::string &)> handler)
  {
    mOnWordSelected = std::move(handler);
  }

  void SetEngine(int engine)
  {
    if (mEngine == engine)
      return;
    mEngine = engine;
    mScrollY = 0.f;
    mHoverIdx = -1;
    mPressedIdx = -1;
    UpdateSections();
    SetDirty(false);
  }

  void SetTmsBank(int bank)
  {
    if (mTmsBank == bank && mEngine == kEngineTMS)
      return;
    mTmsBank = bank;
    mScrollY = 0.f;
    mHoverIdx = -1;
    mPressedIdx = -1;
    UpdateSections();
    SetDirty(false);
  }

  void SetLanguage(int lang)
  {
    mLang = lang;
    UpdateSections();
    SetDirty(false);
  }

  void OnResize() override
  {
    RebuildLayout();
    SetDirty(false);
  }

  void Draw(IGraphics &g) override
  {
    const IRECT b = mRECT;

    // 绘制面板背景 (COL_100, 无任何内边框/描边)
    g.FillRect(COL_100(), b);

    // 非 TMS 模式: 显示说明书
    if (mEngine != kEngineTMS)
    {
      const IRECT infoRect = b.GetPadded(-8.f);
      IText infoHdr(13, COL_900(), kFontSemiBold, EAlign::Near, EVAlign::Top);
      IText infoBody(12, COL_700(), kFontRegular, EAlign::Near, EVAlign::Top);

      const IRECT hdrR(infoRect.L, infoRect.T, infoRect.R, infoRect.T + 20.f);
      const IRECT bodyR(infoRect.L, infoRect.T + 26.f, infoRect.R, infoRect.B);
      g.DrawText(infoHdr, mNonTmsTitle.c_str(), hdrR);
      g.DrawText(infoBody, mNonTmsBody.c_str(), bodyR);
      return;
    }

    // TMS 模式: 视口裁剪
    g.PathClipRegion(b);

    // 1. 绘制小标题 (无下划线, 纯文字排版)
    for (const auto &sec : mSectionLayouts)
    {
      const IRECT r = sec.titleRect.GetTranslated(0.f, b.T - mScrollY);
      if (r.B < b.T || r.T > b.B)
        continue;
      IText t(12, COL_700(), kFontSemiBold, EAlign::Near, EVAlign::Middle);
      g.DrawText(t, sec.title.c_str(), r);
    }

    // 2. 绘制候选词标签 (Flow Layout)
    for (size_t i = 0; i < mItems.size(); ++i)
    {
      const auto &item = mItems[i];
      const IRECT r = item.rect.GetTranslated(0.f, b.T - mScrollY);
      if (r.B < b.T || r.T > b.B)
        continue;

      const bool isPressed = ((int) i == mPressedIdx);
      const bool isHover = ((int) i == mHoverIdx);
      const IColor fill = isPressed ? COL_900() : (isHover ? COL_500() : COL_300());

      g.FillRect(fill, r);
      IText t(13, isPressed ? COL_100() : COL_900(), kFontSemiBold, EAlign::Center, EVAlign::Middle);
      g.DrawText(t, item.word.c_str(), r);
    }

    g.PathClipRegion();

    // 3. 绘制右侧拖拽条 (直角矩形, 无圆角, 无边框)
    const float vpH = b.H();
    if (mContentHeight > vpH)
    {
      const float trackL = b.R - 5.f;
      const float trackR = b.R;
      const float ratio = vpH / mContentHeight;
      const float thumbH = std::max(20.f, vpH * ratio);
      const float maxScroll = mContentHeight - vpH;
      const float scrollNorm = (maxScroll > 0.f) ? (mScrollY / maxScroll) : 0.f;
      const float thumbT = b.T + scrollNorm * (vpH - thumbH);
      const IRECT thumb(trackL, thumbT, trackR, thumbT + thumbH);
      const IColor thumbCol = mDraggingScrollbar ? COL_900() : COL_500();
      g.FillRect(thumbCol, thumb); // 直角, 无圆角
    }
  }

  void OnMouseDown(float x, float y, const IMouseMod &mod) override
  {
    if (mod.R)
      return;

    if (!mRECT.Contains(x, y))
      return;

    // 检测是否点击右侧直角滚动条轨道
    if (x >= mRECT.R - 8.f && mContentHeight > mRECT.H())
    {
      mDraggingScrollbar = true;
      mScrollbarDragStartY = y;
      mScrollbarDragStartScroll = mScrollY;

      const float thumbH = GetThumbHeight(mRECT.H());
      const float maxScroll = mContentHeight - mRECT.H();
      const float thumbT = mRECT.T + (maxScroll > 0.f ? (mScrollY / maxScroll) : 0.f) * (mRECT.H() - thumbH);
      if (y < thumbT || y > thumbT + thumbH)
      {
        const float targetNorm = std::clamp((y - mRECT.T - thumbH * 0.5f) / (mRECT.H() - thumbH), 0.f, 1.f);
        mScrollY = targetNorm * maxScroll;
        mScrollbarDragStartScroll = mScrollY;
      }
      SetDirty(false);
      return;
    }

    if (mEngine == kEngineTMS)
    {
      const int idx = HitTestItem(x, y);
      if (idx >= 0)
      {
        mPressedIdx = idx;
        SetDirty(false);
      }
    }
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod &mod) override
  {
    if (mDraggingScrollbar && mContentHeight > mRECT.H())
    {
      const float thumbH = GetThumbHeight(mRECT.H());
      const float maxScroll = mContentHeight - mRECT.H();
      const float trackSpace = mRECT.H() - thumbH;
      if (trackSpace > 0.f)
      {
        const float delta = (y - mScrollbarDragStartY) / trackSpace * maxScroll;
        mScrollY = std::clamp(mScrollbarDragStartScroll + delta, 0.f, maxScroll);
        SetDirty(false);
      }
      return;
    }

    if (mPressedIdx >= 0)
    {
      const int idx = HitTestItem(x, y);
      if (idx != mPressedIdx)
      {
        mPressedIdx = -1;
        SetDirty(false);
      }
    }
  }

  void OnMouseUp(float x, float y, const IMouseMod &mod) override
  {
    if (mDraggingScrollbar)
    {
      mDraggingScrollbar = false;
      SetDirty(false);
      return;
    }

    if (mPressedIdx >= 0)
    {
      const int idx = HitTestItem(x, y);
      if (idx == mPressedIdx && mPressedIdx < (int) mItems.size())
      {
        if (mOnWordSelected)
          mOnWordSelected(mItems[mPressedIdx].word);
      }
      mPressedIdx = -1;
      SetDirty(false);
    }
  }

  void OnMouseWheel(float x, float y, const IMouseMod &mod, float d) override
  {
    const float maxScroll = std::max(0.f, mContentHeight - mRECT.H());
    if (maxScroll <= 0.f)
      return;

    mScrollY = std::clamp(mScrollY - d * 40.f, 0.f, maxScroll);
    mHoverIdx = HitTestItem(x, y);
    SetDirty(false);
  }

  void OnMouseOver(float x, float y, const IMouseMod &mod) override
  {
    if (mEngine == kEngineTMS)
    {
      const int idx = HitTestItem(x, y);
      if (idx != mHoverIdx)
      {
        mHoverIdx = idx;
        SetDirty(false);
      }
    }
  }

  void OnMouseOut() override
  {
    mHoverIdx = -1;
    mPressedIdx = -1;
    mDraggingScrollbar = false;
    SetDirty(false);
  }

private:
  float GetThumbHeight(float vpH) const
  {
    const float ratio = vpH / mContentHeight;
    return std::max(20.f, vpH * ratio);
  }

  int HitTestItem(float x, float y) const
  {
    if (!mRECT.Contains(x, y) || x >= mRECT.R - 8.f)
      return -1;

    const float relY = y - mRECT.T + mScrollY;
    for (size_t i = 0; i < mItems.size(); ++i)
    {
      if (mItems[i].rect.Contains(x, relY))
        return (int) i;
    }
    return -1;
  }

  struct WordSection
  {
    std::string title;
    std::vector<std::string> words;
  };

  void UpdateSections()
  {
    mSections.clear();
    const bool zh = (mLang == orm::kLangZH);

    if (mEngine == kEngineTMS)
    {
      switch (mTmsBank)
      {
        case 0: // MILITARY
        {
          // 1. 数字与序数 (34)
          static const char *const kMilNum[] = {
              "ZERO", "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE", "TEN",
              "ELEVEN", "TWELVE", "THIR_", "FIF_", "_TEEN", "TWENTY", "HUNDRED", "THOUSAND", "THIRTEEN",
              "FOURTEEN", "FIFTEEN", "SIXTEEN", "SEVENTEEN", "EIGHTEEN", "NINETEEN", "THIRTY", "FOURTY",
              "FIFTY", "SIXTY", "SEVENTY", "EIGHTY", "NINETY", "MILLION"};

          // 2. 单字母 (26)
          static const char *const kMilLetters[] = {
              "A", "B", "C", "D", "E", "F", "G", "H", "I", "L", "J", "K", "M",
              "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};

          // 3. NATO 音标字母 (28)
          static const char *const kMilNato[] = {
              "ALPHA", "BRAVO", "CHARLIE", "DELTA", "ECHO", "FOXTROT", "GOLF", "HENRY", "INDIA",
              "JULIET", "KILO", "LIMA", "MIKE", "NOVEMBER", "OSCAR", "PAPA", "QUEBEC", "ROMEO",
              "SIERRA", "TANGO", "UNIFORM", "VICTOR", "WHISKY", "XRAY", "YANKEE", "ZULU", "HOTEL", "WHISKEY"};

          std::unordered_set<std::string> used;
          WordSection secNum{zh ? "数字与序数" : "NUMBERS & ORDINALS", {}};
          for (const char *s : kMilNum)
          {
            secNum.words.emplace_back(s);
            used.insert(s);
          }

          WordSection secLetters{zh ? "单字母" : "SINGLE LETTERS", {}};
          for (const char *s : kMilLetters)
          {
            secLetters.words.emplace_back(s);
            used.insert(s);
          }

          WordSection secNato{zh ? "NATO 音标字母" : "NATO PHONETICS", {}};
          for (const char *s : kMilNato)
          {
            secNato.words.emplace_back(s);
            used.insert(s);
          }

          WordSection secWords{zh ? "词条" : "VOCABULARY", {}};
          for (int i = 0; i < orm::tms::military::kNumWords; ++i)
          {
            const char *w = orm::tms::military::kWords[i].name;
            if (used.find(w) == used.end())
              secWords.words.emplace_back(w);
          }

          mSections.push_back(std::move(secNum));
          mSections.push_back(std::move(secLetters));
          mSections.push_back(std::move(secNato));
          mSections.push_back(std::move(secWords));
          break;
        }

        case 1: // TI-99/4A
        {
          WordSection secWords{zh ? "词条 (TI-99/4A)" : "VOCABULARY (TI-99/4A)", {}};
          for (int i = 0; i < orm::tms::ti99::kNumWords; ++i)
            secWords.words.emplace_back(orm::tms::ti99::kWords[i].name);
          mSections.push_back(std::move(secWords));
          break;
        }

        case 2: // ACORN BBC
        {
          static const char *const kAcornTones[] = {"PAUSE1", "PAUSE2", "TONE1", "TONE2"};
          static const char *const kAcornSuffix[] = {"_D", "_ED", "_ING", "_S", "_TEEN", "_TH", "_T", "_Z"};
          static const char *const kAcornNum[] = {
              "ZERO", "HUNDRED", "THOUSAND", "ONE", "TWO", "TWEN_", "THREE", "THIR_", "FOUR", "FOUR_",
              "FIVE", "FIF_", "SIX", "SIX_", "SEVEN", "SEVEN_", "EIGHT", "EIGH_", "NINE", "NINE_",
              "TEN", "ELEVEN", "TWELVE"};

          std::unordered_set<std::string> used;
          WordSection secTones{zh ? "停顿与音调" : "TONES & PAUSES", {}};
          for (const char *s : kAcornTones)
          {
            secTones.words.emplace_back(s);
            used.insert(s);
          }

          WordSection secSuffix{zh ? "音节后缀" : "SUFFIXES", {}};
          for (const char *s : kAcornSuffix)
          {
            secSuffix.words.emplace_back(s);
            used.insert(s);
          }

          WordSection secNum{zh ? "数字与计数" : "NUMBERS", {}};
          for (const char *s : kAcornNum)
          {
            secNum.words.emplace_back(s);
            used.insert(s);
          }

          WordSection secWords{zh ? "词条" : "VOCABULARY", {}};
          for (int i = 0; i < orm::tms::acorn::kNumWords; ++i)
          {
            const char *w = orm::tms::acorn::kWords[i].name;
            if (used.find(w) == used.end())
              secWords.words.emplace_back(w);
          }

          mSections.push_back(std::move(secTones));
          mSections.push_back(std::move(secSuffix));
          mSections.push_back(std::move(secNum));
          mSections.push_back(std::move(secWords));
          break;
        }

        case 3: // SPEAK & SPELL
        {
          static const char *const kSspellNum[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
          static const char *const kSspellPhrases[] = {
              "HERE_IS_YOUR_SCORE", "I_WIN", "NEXT_SPELL", "NOW_SPELL", "NOW_TRY", "PERFECT_SCORE",
              "SAY_IT", "THAT_IS_CORRECT", "THAT_IS_INCORRECT", "THAT_IS_RIGHT", "YOU_ARE_CORRECT",
              "YOU_ARE_RIGHT", "YOU_WIN"};

          std::unordered_set<std::string> used;
          WordSection secLetters{zh ? "单字母" : "LETTERS", {}};
          for (char c = 'A'; c <= 'Z'; ++c)
          {
            std::string s(1, c);
            secLetters.words.push_back(s);
            used.insert(s);
          }

          WordSection secNum{zh ? "数字" : "NUMBERS", {}};
          for (const char *s : kSspellNum)
          {
            secNum.words.emplace_back(s);
            used.insert(s);
          }

          WordSection secPhrases{zh ? "短语" : "PHRASES", {}};
          for (const char *s : kSspellPhrases)
          {
            secPhrases.words.emplace_back(s);
            used.insert(s);
          }

          WordSection secWords{zh ? "单词" : "WORDS", {}};
          for (int i = 0; i < orm::tms::sspell::kNumWords; ++i)
          {
            const char *w = orm::tms::sspell::kWords[i].name;
            if (used.find(w) == used.end())
              secWords.words.emplace_back(w);
          }

          mSections.push_back(std::move(secLetters));
          mSections.push_back(std::move(secNum));
          mSections.push_back(std::move(secPhrases));
          mSections.push_back(std::move(secWords));
          break;
        }

        case 4: // CLOCK
        {
          static const char *const kClockNum[] = {
              "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE", "TEN",
              "ELEVEN", "TWELVE", "THIRTEEN", "FOURTEEN", "FIFTEEN", "SIXTEEN", "SEVENTEEN",
              "EIGHTEEN", "NINETEEN", "TWENTY", "THIRTY", "FOURTY", "FIFTY"};

          std::unordered_set<std::string> used;
          WordSection secNum{zh ? "数字" : "NUMBERS", {}};
          for (const char *s : kClockNum)
          {
            secNum.words.emplace_back(s);
            used.insert(s);
          }

          WordSection secPhrases{zh ? "报时短语" : "PHRASES", {}};
          for (int i = 0; i < orm::tms::clock::kNumWords; ++i)
          {
            const char *w = orm::tms::clock::kWords[i].name;
            if (used.find(w) == used.end())
              secPhrases.words.emplace_back(w);
          }

          mSections.push_back(std::move(secPhrases));
          mSections.push_back(std::move(secNum));
          break;
        }

        default:
          break;
      }
    }
    else if (mEngine == kEngineSAM)
    {
      mNonTmsTitle = "SAM (SOFTWARE AUTOMATIC MOUTH)";
      mNonTmsBody = zh ? "• 文本模式: 输入英文单词或句子 (如 'HELLO WORLD')。\n"
                         "• 音素模式: 输入 SAM 专有音素码 (如 'HX EH L OW')。\n"
                         "• 控制参数: Pitch 音高, Speed 语速, Mouth 嘴形, Throat 喉形。\n"
                         "• 点击左侧 TEXT / PHONETICS 分段切换输入模式。"
                       : "• TEXT mode: English words/sentences (e.g. 'HELLO WORLD').\n"
                         "• PHONETICS mode: SAM phonemes (e.g. 'HX EH L OW').\n"
                         "• Controls: Pitch, Speed, Mouth, Throat.\n"
                         "• Click TEXT / PHONETICS on the left to toggle.";
    }
    else if (mEngine == kEngineTSI)
    {
      mNonTmsTitle = "TSI S14001A (ROM SPEECH CONTROLLER)";
      mNonTmsBody = zh ? "• 硬件波形点播芯片 (1975 TSI/SSi S14001A)。\n"
                         "• 文本框输入词索引号 (0..63, 空格分隔, 如 'W03 12 7')。\n"
                         "• 左侧选择不同 ROM 子集 (BZ: 街机, F2: 弹球, C0..C6: 国际象棋)。\n"
                         "• 语速与音高完全由芯片外部时钟 (Rate 滑块) 同步缩放。"
                       : "• ROM wave playback chip (1975 TSI/SSi S14001A).\n"
                         "• Type word indices (0..63, space-separated, e.g. 'W03 12 7').\n"
                         "• Select ROM subset (BZ, F2, C0..C6) on the left.\n"
                         "• Pitch and speed scaled via Rate slider.";
    }
    else if (mEngine == kEngineSP)
    {
      mNonTmsTitle = "SP0256 (GI / MICROCHIP NARRATOR)";
      mNonTmsBody = zh ? "• 12 阶格型滤波器 Allophone 合成器 (1981)。\n"
                         "• 文本模式: 经 CTS256A-AL2 控制器内置规则引擎自动转音素。\n"
                         "• 音素模式: 直接输入 AL2 标签 (如 PA1..PA5 停顿, 音素码) 与\n"
                         "  012 单词标签 (ZERO, ONE, TWO...)。"
                       : "• 12-pole lattice filter allophone synthesizer (1981).\n"
                         "• Text mode: Auto-translated via CTS256A-AL2 rule engine.\n"
                         "• Phonetic mode: Input AL2 phonemes (PA1..PA5, etc.)\n"
                         "  and 012 words (ZERO, ONE, TWO...).";
    }
    else if (mEngine == kEngineDEC)
    {
      mNonTmsTitle = "DECTALK (DEC TTS SYNTHESIZER)";
      mNonTmsBody = zh ? "• 完整经典 DECtalk 文本转语音引擎。\n"
                         "• 直接输入英文文本，支持内联语法指令 (如 '[:phoneme on]')。\n"
                         "• 左侧可选 9 款经典预设音色 (Perfect Paul, Betty, Harry...)。\n"
                         "• 支持多词、连读与自然语调起伏。"
                       : "• Classic DECtalk text-to-speech engine.\n"
                         "• Plain English text, supports inline commands (e.g. '[:phoneme on]').\n"
                         "• 9 classic voices available on the left.\n"
                         "• Rich intonation and natural speech flow.";
    }

    RebuildLayout();
  }

  void RebuildLayout()
  {
    mItems.clear();
    mSectionLayouts.clear();
    if (mSections.empty() || mEngine != kEngineTMS)
    {
      mContentHeight = 0.f;
      return;
    }

    const float contentL = mRECT.L;
    const float contentR = mRECT.R - 8.f; // 预留右侧直角滚动条
    const float maxChipW = contentR - contentL;
    const float chipH = 22.f;
    const float gapX = 4.f;
    const float gapY = 4.f;

    float curY = 0.f;

    for (size_t s = 0; s < mSections.size(); ++s)
    {
      const auto &sec = mSections[s];

      // 放置小标题 (无下划线)
      if (!sec.title.empty())
      {
        if (s > 0)
          curY += 14.f; // 栏间距
        mSectionLayouts.push_back({sec.title, IRECT(contentL, curY, contentR, curY + 18.f)});
        curY += 22.f; // 标题高度 18px + 4px 间隙
      }

      float curX = contentL;
      for (const auto &w : sec.words)
      {
        float wLen = 16.f;
        for (char c : w)
        {
          if (c == 'I' || c == '1' || c == ' ' || c == '.' || c == '\'')
            wLen += 4.5f;
          else if (c == 'M' || c == 'W')
            wLen += 11.f;
          else if (c == '_')
            wLen += 8.f;
          else
            wLen += 8.0f;
        }
        wLen = std::clamp(wLen, 30.f, maxChipW);

        if (curX + wLen > contentR && curX > contentL)
        {
          curX = contentL;
          curY += chipH + gapY;
        }

        mItems.push_back({w, IRECT(curX, curY, curX + wLen, curY + chipH)});
        curX += wLen + gapX;
      }
      curY += chipH;
    }

    mContentHeight = curY + 8.f;
  }

  struct SectionLayout
  {
    std::string title;
    IRECT titleRect;
  };

  struct Item
  {
    std::string word;
    IRECT rect;
  };

  std::function<void(const std::string &)> mOnWordSelected;
  int mEngine = kEngineTMS;
  int mTmsBank = 0;
  int mLang = orm::UILang();

  std::string mNonTmsTitle;
  std::string mNonTmsBody;

  std::vector<WordSection> mSections;
  std::vector<SectionLayout> mSectionLayouts;
  std::vector<Item> mItems;

  float mContentHeight = 0.f;
  float mScrollY = 0.f;
  int mHoverIdx = -1;
  int mPressedIdx = -1;

  bool mDraggingScrollbar = false;
  float mScrollbarDragStartY = 0.f;
  float mScrollbarDragStartScroll = 0.f;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
