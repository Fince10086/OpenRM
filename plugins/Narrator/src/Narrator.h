#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "Params.h"
#include "Strings.h"
#include "IPlugQueue.h"
#include "dsp/SamEngine.h"
#include "dsp/Tms5220Engine.h"
#include "dsp/Tms5110Engine.h"
#include "dsp/TsiS14001Engine.h"
#include "dsp/Sp0256Engine.h"
#include "dsp/DectalkEngine.h"
#include "dsp/VoiceRenderer.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace iplug;
using namespace igraphics;

namespace iplug {
namespace igraphics {
class IControl;
class SettingsPanelControl;
class PianoKeyboardControl;
class PhraseEditorControl;
class UtteranceTimelineControl;
class ORMSlider;
class FlatSegmentControl;
class CandidatePanelControl;
} // namespace igraphics
} // namespace iplug

constexpr int kMaxVoices = 4;

class ORMNarrator final : public Plugin
{
public:
  ORMNarrator(const InstanceInfo &info);

#if IPLUG_DSP
  void ProcessBlock(sample **inputs, sample **outputs, int nFrames) override;
  void ProcessMidiMsg(const IMidiMsg &msg) override;
  void OnReset() override;
  void OnParamChange(int paramIdx, EParamSource source, int sampleOffset) override;
  void OnParamChangeUI(int paramIdx, EParamSource source) override;
  bool SerializeState(IByteChunk &chunk) const override;
  int UnserializeState(const IByteChunk &chunk, int startPos) override;
#endif

  void OnIdle() override;
  void OnUIOpen() override;
  void OnParentWindowResize(int width, int height) override;
  bool ConstrainEditorResize(int &w, int &h) const override;

  // UI 侧入口
  void OnNoteOnFromUI(int note, bool held = true);
  void OnNoteOffFromUI(int note);
  void SetPhraseText(const std::string &text);
  void SetPhoneticMode(bool phonetic);
  void SetEngineFromUI(int idx);
  void SetVoiceFromUI(int idx);
  void SetTsiVoiceFromUI(int idx);
  void SetSp0256VoiceFromUI(int idx);
  void SetDectalkVoiceFromUI(int idx);
  void SetMapModeFromUI(int idx);

private:
  // BANK 模式: 每个琴键绑定独立的文本与参数
  struct BankEntry
  {
    std::string text;
    bool phonetic = false;
    int engine = 0; // 0=SAM 1=TMS5220 2=TSI S14001A 3=SP0256 4=DECTALK
    orm::SamSettings sam;
    orm::TmsSettings tms;
    orm::TsiSettings tsi;
    orm::Sp0256Settings sp0256;
    orm::DectalkSettings dectalk;
    std::vector<float> rendered;
    bool dirty = true;
  };
  std::map<int, BankEntry> mBank;
  std::atomic<int> mPendingSelect{-1};
  std::atomic<bool> mSelectChanged{false};
  int mUISelected = -1;
  FlatSegmentControl *mMapSegment = nullptr;
  FlatSegmentControl *mEngineSegment = nullptr;
  FlatSegmentControl *mVoiceSegment = nullptr;
  FlatSegmentControl *mTsiVoiceSegment = nullptr;
  FlatSegmentControl *mSp0256VoiceSegment = nullptr;
  FlatSegmentControl *mDectalkVoiceSegment = nullptr;

  struct Voice
  {
    int note = -1;
    orm::VoiceRenderer renderer;
    double ratio = 1.0;
    bool held = false;
  };
  std::array<Voice, kMaxVoices> mVoices;
  int mNextVoice = 0;

  struct MidiEvent
  {
    int offset;
    bool isNoteOn;
    int note;
    int velocity;
  };
  std::vector<MidiEvent> mMidiQueue;
  std::vector<float> mPhraseBuffer;

  // UI 侧每音保持标记: 屏幕键盘写 1, 试听按钮写 0 (预览不参与 Loop)
  std::array<std::atomic<uint8_t>, 128> mUIHoldState{};

  mutable std::mutex mTextMutex;
  std::string mPhraseText = "HELLO WORLD.";
  bool mPhonetic = false;

  // 音频线程 -> UI 队列
  struct NoteMsg { bool on; int note; };
  struct ProgressMsg { float progress; };
  struct TimelineMsg { std::array<float, kPhraseEnvPoints> env; };
  IPlugQueue<NoteMsg> mNoteQueue{64};
  IPlugQueue<ProgressMsg> mProgressQueue{32};
  IPlugQueue<TimelineMsg> mTimelineQueue{4};
  float mAudioProgress = 0.f;

  std::atomic<double> mPhraseDuration{0.0};
  std::atomic<bool> mRenderDirty{true};
  double mLastUIProgress = 0.0;

  void EnsureRendered();
  void TriggerVoice(int note);
  void ReleaseVoice(int note);
  double PitchRatioForNote(int note) const;
  void RenderSegment(sample *out, int from, int to);
  void PushPhraseToUI();
  void PushNoteStateToUI(bool on, int note);
  void RenderBankEntry(BankEntry &e);
  void PushBankPhraseToUI(const BankEntry &e);
  void ApplySelectionFromIdle();
  void RebindVoiceSliders(int engine);

  // 参数快照撤销/重做
  ParamSnapshot Snapshot() const;
  void SetParamFromEditor(int idx, double value);
  void ApplySnapshot(const ParamSnapshot &s);
  void RefreshAfterEdit();
  void PushUndoSnapshot(const ParamSnapshot &s);
  void MaybePushGestureUndo();
  void MarkStateStable();
  void Undo();
  void Redo();

  int mThemeMode = 0;
  SettingsPanelControl *mSettingsPanel = nullptr;
  PianoKeyboardControl *mKeyboard = nullptr;
  PhraseEditorControl *mPhraseEditor = nullptr;
  UtteranceTimelineControl *mTimeline = nullptr;
  CandidatePanelControl *mCandidatePanel = nullptr;
  ORMSlider *mParamSliders[9] = {};
  FlatSegmentControl *mPhoneticSegment = nullptr;
  std::vector<std::pair<int, std::function<void(const char *)>>> mTextBindings;
  std::vector<std::pair<IControl *, int>> mTooltipBindings;

  std::deque<ParamSnapshot> mUndoStack, mRedoStack;
  ParamSnapshot mStableSnapshot{};
  double mLastUIChangeTime = -1e9;
  bool mGesturePending = false;

#if IPLUG_EDITOR
  void ApplyLanguage();
  void ApplyTooltips();
  void ApplyTheme();
  void RefreshThemeColors();
  void ToggleSettingsPanel();
#endif
  void SaveSettingsToDisk();
};
