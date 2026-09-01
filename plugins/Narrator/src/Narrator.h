#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "Params.h"
#include "Strings.h"
#include "IPlugQueue.h"
#include "dsp/SamEngine.h"
#include "dsp/VoiceRenderer.h"

#include <array>
#include <atomic>
#include <functional>
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
} // namespace igraphics
} // namespace iplug

// 复音声部数 (Phase 1: 单个短语的 4 个独立回放声部)
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
  bool SerializeState(IByteChunk &chunk) const override;
  int UnserializeState(const IByteChunk &chunk, int startPos) override;
#endif

  void OnIdle() override;
  bool ConstrainEditorResize(int &w, int &h) const override;

  // ---- 编辑器侧入口 (控件回调经 delegate 调用) ----
  void OnNoteOnFromUI(int note);   // 屏幕键盘/试听按钮触发
  void OnNoteOffFromUI(int note);
  void SetPhraseText(const std::string &text); // 文本框提交
  void SetPhoneticMode(bool phonetic);         // TEXT/PHONEMES 切换

private:
  // ---- 渲染与回放 ----
  struct Voice
  {
    int note = -1;
    orm::VoiceRenderer renderer;
  };
  std::array<Voice, kMaxVoices> mVoices;
  int mNextVoice = 0;

  // MIDI 事件队列 (ProcessMidiMsg 收集, ProcessBlock 按采样偏移处理)
  struct MidiEvent
  {
    int offset;
    bool isNoteOn;
    int note;
    int velocity;
  };
  std::vector<MidiEvent> mMidiQueue;
  std::vector<float> mPhraseBuffer; // 已渲染短语 (引擎原生采样率, 单声道)

  // 文本状态 (编辑器写 / 音频线程读, 互斥保护)
  mutable std::mutex mTextMutex;
  std::string mPhraseText = "HELLO WORLD.";
  bool mPhonetic = false;

  // 音频线程 -> UI 队列 (OnIdle 转发, 同 BandPass 的 ISender 惯例)
  struct NoteMsg { bool on; int note; };
  struct ProgressMsg { float progress; };
  struct TimelineMsg { std::array<float, kPhraseEnvPoints> env; };
  IPlugQueue<NoteMsg> mNoteQueue{64};
  IPlugQueue<ProgressMsg> mProgressQueue{32};
  IPlugQueue<TimelineMsg> mTimelineQueue{4};
  float mAudioProgress = 0.f; // 仅音频线程读写

  std::atomic<double> mPhraseDuration{0.0};
  std::atomic<bool> mRenderDirty{true};
  double mLastUIProgress = 0.0; // 仅主线程

  void EnsureRendered();          // 渲染脏标记时重渲染 (音频线程调用)
  void TriggerVoice(int note);
  void ReleaseVoice(int note);
  double PitchRatioForNote(int note) const;
  void RenderSegment(sample *out, int from, int to);
  void PushPhraseToUI();          // 渲染后向时间线控件推送包络
  void PushNoteStateToUI(bool on, int note);

  // ---- UI ----
  int mThemeMode = 0;
  SettingsPanelControl *mSettingsPanel = nullptr;
  PianoKeyboardControl *mKeyboard = nullptr;
  PhraseEditorControl *mPhraseEditor = nullptr;
  UtteranceTimelineControl *mTimeline = nullptr;
  ORMSlider *mParamSliders[9] = {};
  FlatSegmentControl *mModeSegment = nullptr;
  std::vector<std::pair<int, std::function<void(const char *)>>> mTextBindings;
  std::vector<std::pair<IControl *, int>> mTooltipBindings;

#if IPLUG_EDITOR
  void ApplyLanguage();
  void ApplyTooltips();
  void ApplyTheme();
  void RefreshThemeColors();
  void ToggleSettingsPanel();
#endif
  void SaveSettingsToDisk();
};
