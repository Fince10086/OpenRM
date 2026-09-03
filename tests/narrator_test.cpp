// narrator_test — ORM Narrator DSP 内核离线自测
// 纯 C++17, 不依赖 iPlug2。
// 覆盖: 5 大语音引擎 (SAM, DEC, SP, TMS, TSI) 及 VoiceRenderer 回放引擎。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../plugins/Narrator/src/dsp/SamEngine.h"
#include "../plugins/Narrator/src/dsp/DectalkEngine.h"
#include "../plugins/Narrator/src/dsp/Sp0256Engine.h"
#include "../plugins/Narrator/src/dsp/Tms5220Engine.h"
#include "../plugins/Narrator/src/dsp/Tms5110Engine.h"
#include "../plugins/Narrator/src/dsp/TsiS14001Engine.h"
#include "../plugins/Narrator/src/dsp/VoiceRenderer.h"

static int gChecks = 0;
static int gFails = 0;

static void check(bool cond, const char* name)
{
  gChecks++;
  if (!cond)
  {
    gFails++;
    printf("  FAIL: %s\n", name);
  }
}

static void expect_near(double a, double b, double tol, const char* name)
{
  gChecks++;
  if (std::abs(a - b) > tol)
  {
    gFails++;
    printf("  FAIL: %s (%f vs %f, tol %f)\n", name, a, b, tol);
  }
}

// ============================================================================
// Engine 0: SAM (Software Automatic Mouth, 1982)
// ============================================================================

static void TestSam()
{
  printf("[SAM]\n");
  orm::SamSettings settings; // 默认 pitch 64 / speed 72 / mouth 128 / throat 128

  // 1. 英文文本渲染 (自动经 Reciter 转换为音素并渲染)
  std::vector<float> out;
  check(orm::SamEngine::Render("HELLO WORLD", false, settings, out), "render HELLO WORLD");
  check(!out.empty(), "output non-empty");

  const double seconds = (double) out.size() / orm::SamEngine::kSampleRate;
  check(seconds > 0.3 && seconds < 3.0, "duration plausible");

  // 2. 峰值归一化 (0.85)
  float peak = 0.f;
  for (float s : out)
    peak = std::max(peak, std::abs(s));
  check(peak > 0.5f, "audible content");
  expect_near(peak, 0.85, 0.01, "peak normalized to 0.85");

  // 3. 确定性 (同参数两次渲染逐样本一致)
  std::vector<float> out2;
  orm::SamEngine::Render("HELLO WORLD", false, settings, out2);
  check(out.size() == out2.size(), "deterministic length");
  bool same = true;
  for (size_t i = 0; i < out.size() && same; i++)
    if (out[i] != out2[i])
      same = false;
  check(same, "deterministic samples");

  // 4. 语速参数影响: speed 越小语速越快, 渲染长度变短
  orm::SamSettings fast = settings;
  fast.speed = 30;
  std::vector<float> outFast;
  orm::SamEngine::Render("HELLO WORLD", false, fast, outFast);
  check(outFast.size() < out.size() / 2, "lower speed value renders faster/shorter");

  // 5. 音高参数影响: 输出波形改变
  orm::SamSettings low = settings;
  low.pitch = 20;
  std::vector<float> outLow;
  orm::SamEngine::Render("HELLO WORLD", false, low, outLow);
  bool differs = outLow.size() != out.size();
  for (size_t i = 0; !differs && i < out.size(); i++)
    if (std::abs(outLow[i] - out[i]) > 0.01f)
      differs = true;
  check(differs, "pitch changes samples");

  // 6. 音素直通模式
  std::vector<float> outPh;
  check(orm::SamEngine::Render("/HEH3LOW2", true, settings, outPh), "phonetic input succeeds");

  // 7. 异常输入防御
  check(!orm::SamEngine::Render("", false, settings, out), "empty text fails");
  std::vector<float> outSp;
  check(orm::SamEngine::Render("   ", false, settings, outSp) == false || outSp.empty(),
        "whitespace-only fails or silent");
}

// ============================================================================
// Engine 1: DECtalk (Dennis Klatt / Digital, 1984)
// ============================================================================

static void TestDectalk()
{
  printf("[DEC]\n");
  orm::DectalkSettings settings; // 默认 Paul / rate 200 / pitch 0

  // 1. 文本渲染
  std::vector<float> out;
  check(orm::DectalkEngine::Render("hello world", false, settings, out), "render text succeeds");
  check(!out.empty(), "output non-empty");

  const double seconds = (double) out.size() / orm::DectalkEngine::kSampleRate;
  check(seconds > 0.2 && seconds < 8.0, "duration plausible");

  // 2. 峰值归一化
  float peak = 0.f;
  for (float s : out)
    peak = std::max(peak, std::abs(s));
  check(peak > 0.5f, "audible content");
  expect_near(peak, 0.85, 0.01, "peak normalized to 0.85");

  // 3. 确定性
  std::vector<float> out2;
  orm::DectalkEngine::Render("hello world", false, settings, out2);
  check(out.size() == out2.size(), "deterministic length");
  bool same = out.size() == out2.size();
  for (size_t i = 0; same && i < out.size(); i++)
    if (out[i] != out2[i])
      same = false;
  check(same, "deterministic samples");

  // 4. 语速参数: 词/分钟 (数值越大越快, 时长变短)
  orm::DectalkSettings slow = settings;
  slow.rate = 120;
  orm::DectalkSettings fast = settings;
  fast.rate = 400;
  std::vector<float> oSlow, oFast;
  orm::DectalkEngine::Render("hello world", false, slow, oSlow);
  orm::DectalkEngine::Render("hello world", false, fast, oFast);
  const double ratio = (double) oSlow.size() / (double) oFast.size();
  check(ratio > 2.0 && ratio < 5.0, "rate 400 vs 120 shortens ~3x");

  // 5. 平均音高 AP 参数: 改变输出样本, 保持时长
  orm::DectalkSettings pitchy = settings;
  pitchy.pitch = 200;
  std::vector<float> oPitch;
  orm::DectalkEngine::Render("hello world", false, pitchy, oPitch);
  check(oPitch.size() == out.size(), "pitch keeps length");
  bool differs = oPitch.size() != out.size();
  for (size_t i = 0; !differs && i < out.size(); i++)
    if (std::abs(oPitch[i] - out[i]) > 1e-3f)
      differs = true;
  check(differs, "pitch changes samples");

  // 6. 音色切换: Betty 与默认 Paul 输出样本不同
  orm::DectalkSettings betty = settings;
  betty.voice = orm::kDectalkBetty;
  std::vector<float> oBetty;
  orm::DectalkEngine::Render("hello world", false, betty, oBetty);
  differs = oBetty.size() != out.size();
  for (size_t i = 0; !differs && i < out.size(); i++)
    if (std::abs(oBetty[i] - out[i]) > 1e-3f)
      differs = true;
  check(differs, "voice changes samples");

  // 7. 音素直通模式
  std::vector<float> oPh;
  check(orm::DectalkEngine::Render("HX EH L OW", true, settings, oPh), "phonetic input succeeds");
  check(!oPh.empty(), "phonetic non-empty");

  // 8. 异常输入防御
  check(!orm::DectalkEngine::Render("", false, settings, out), "empty text fails");
  std::vector<float> oPunct;
  check(!orm::DectalkEngine::Render(",.", false, settings, oPunct), "punctuation-only silent");
}

// ============================================================================
// Engine 2: SP0256 (General Instrument Narrator, 1981)
// ============================================================================

static void TestSp0256()
{
  printf("[SP0256]\n");
  std::vector<float> out;
  const double rate = orm::Sp0256Engine::RateFor(1.0f);
  const int kText = orm::Sp0256Engine::kTextVariant;    // 文本模式 (经 CTS256A 转写)
  const int kPhon = orm::Sp0256Engine::kPhonemeVariant; // 音素模式 (AL2+012 合并)

  // 1. 文本模式 (内置 CTS256A 文本转写测试)
  check(orm::Sp0256Engine::Render("HELLO WORLD", kText, 1.0f, out), "text mode HELLO WORLD");
  check(!out.empty(), "text non-empty");
  const double sT = (double) out.size() / rate;
  check(sT > 0.05 && sT < 4.0, "text duration plausible");

  float peak = 0.f;
  for (float s : out)
    peak = std::max(peak, std::abs(s));
  expect_near(peak, 0.85, 0.01, "peak normalized to 0.85");

  // 句子与数字转写
  std::vector<float> oSent;
  check(orm::Sp0256Engine::Render("THE QUICK BROWN FOX. 123", kText, 1.0f, oSent), "text sentence with digits");

  // 2. 音素模式 (短语、Allophone 串联、012 单词)
  std::vector<float> oPh;
  check(orm::Sp0256Engine::Render("HELLO", kPhon, 1.0f, oPh), "phoneme phrase HELLO");
  std::vector<float> oExp;
  check(orm::Sp0256Engine::Render("HH1 EH L OW", kPhon, 1.0f, oExp), "render allophone string");
  check(oExp.size() == oPh.size(), "phrase equals allophone string");

  // 确定性
  std::vector<float> oPh2;
  orm::Sp0256Engine::Render("HELLO", kPhon, 1.0f, oPh2);
  check(oPh2.size() == oPh.size(), "deterministic length");

  // XTAL 时钟速率调节: 芯片执行周期数不变, 播放速率翻倍 => 渲染时长减半
  std::vector<float> oFast;
  check(orm::Sp0256Engine::Render("HELLO", kPhon, 2.0f, oFast), "rate 2.0 renders");
  check(oFast.size() == oPh.size(), "rate keeps sample count");
  const double fastDur = (double) oFast.size() / orm::Sp0256Engine::RateFor(2.0f);
  check(fastDur < sT * 0.7, "rate 2.0 halves duration");

  // 012 ROM 单词及 AL2+012 跨 ROM 混合
  std::vector<float> o012;
  check(orm::Sp0256Engine::Render("THREE", kPhon, 1.0f, o012), "012 ROM word THREE");
  std::vector<float> oMix;
  check(orm::Sp0256Engine::Render("HH1 ZERO", kPhon, 1.0f, oMix), "mixed AL2+012");
  check(oMix.size() > oPh.size() / 2, "mixed non-empty");

  // 多词与未知 token 跳过
  std::vector<float> oMulti, oSkip;
  check(orm::Sp0256Engine::Render("HH1 EH", kPhon, 1.0f, oMulti), "multi-token renders");
  check(orm::Sp0256Engine::Render("HH1 NOSUCH", kPhon, 1.0f, oSkip), "unknown token skipped");
  check(oSkip.size() < oMulti.size(), "skip adds no samples");

  // 3. 异常输入防御
  check(!orm::Sp0256Engine::Render("", kText, 1.0f, out), "empty text fails");
  check(!orm::Sp0256Engine::Render("   ", kText, 1.0f, out), "whitespace silent");
  check(!orm::Sp0256Engine::Render("NOSUCH", kPhon, 1.0f, out), "unknown allophone fails");
  check(!orm::Sp0256Engine::Render("PA1", kPhon, 1.0f, out), "pause-only silent");
}

// ============================================================================
// Engine 3: TMS (TMS5220 + TMS5110 多音色词库引擎, 1978/1980)
// ============================================================================

static void TestTms()
{
  printf("[TMS]\n");
  std::vector<float> out;

  // 1. Bank 0 (Military / 经典军用词库): 基础渲染与峰值归一
  check(orm::Tms5220Engine::Render("DANGER", 0, 1.0f, out), "bank 0 DANGER");
  check(!out.empty(), "DANGER non-empty");
  const double seconds = (double) out.size() / orm::Tms5220Engine::kSampleRate;
  check(seconds > 0.1 && seconds < 4.0, "DANGER duration plausible");

  float peak = 0.f;
  for (float s : out)
    peak = std::max(peak, std::abs(s));
  expect_near(peak, 0.85, 0.01, "peak normalized to 0.85");

  // 2. 确定性
  std::vector<float> out2;
  orm::Tms5220Engine::Render("DANGER", 0, 1.0f, out2);
  check(out.size() == out2.size(), "tms deterministic length");
  bool same = true;
  for (size_t i = 0; i < out.size() && same; i++)
    if (out[i] != out2[i])
      same = false;
  check(same, "tms deterministic samples");

  // 3. 语速缩放: 慢一倍时长翻倍左右
  std::vector<float> slow;
  orm::Tms5220Engine::Render("DANGER", 0, 2.0f, slow);
  check(slow.size() > out.size() * 1.8 && slow.size() < out.size() * 2.2, "speed scale 2.0 doubles length");

  // 4. 多词串联与未知词跳过
  std::vector<float> one, multi, skip;
  orm::Tms5220Engine::Render("ONE", 0, 1.0f, one);
  orm::Tms5220Engine::Render("ONE TWO THREE", 0, 1.0f, multi);
  check(!multi.empty(), "multi-word renders");
  expect_near((double) multi.size() / (double) one.size(), 3.0, 0.2, "multi-word length ~3x single");

  check(orm::Tms5220Engine::Render("ONE NOSUCHWORD TWO", 0, 1.0f, skip), "unknown words skipped");
  expect_near((double) skip.size() / (double) one.size(), 2.0, 0.2, "skipped word contributes no length");

  // 5. 跨 Bank 路由与音色词表覆盖
  // Bank 1 (TI-99/4A): ABOUT 存在, 但 DANGER 不在词表中
  check(orm::Tms5220Engine::Render("ABOUT", 1, 1.0f, out), "ti99 ABOUT");
  check(!orm::Tms5220Engine::Render("DANGER", 1, 1.0f, out), "DANGER not in TI-99");

  // Bank 2 (Acorn): COMPUTER 存在
  check(orm::Tms5220Engine::Render("COMPUTER", 2, 1.0f, out), "acorn COMPUTER");

  // Bank 3 (TMS5110 / Speak & Spell): ISLE 渲染成功且归一化
  check(orm::Tms5110Engine::Render("ISLE", 1.0f, out), "tms5110 ISLE");
  check(!out.empty(), "sspell non-empty");
  std::vector<float> sMulti;
  check(orm::Tms5110Engine::Render("COLOR ISLE", 1.0f, sMulti), "sspell multi-word");

  // Bank 4 (Clock / 女声): TIME 渲染成功
  check(orm::Tms5220Engine::Render("TIME", 4, 1.0f, out), "clock TIME");

  // 6. 异常输入防御
  check(!orm::Tms5220Engine::Render("", 0, 1.0f, out), "empty word fails");
  check(!orm::Tms5220Engine::Render("NOSUCHWORD", 0, 1.0f, out), "all-unknown fails");
  check(!orm::Tms5110Engine::Render("", 1.0f, out), "tms5110 empty fails");
}

// ============================================================================
// Engine 4: TSI S14001A (Telesensory Systems Inc, 1976)
// ============================================================================

static void TestS14001()
{
  printf("[TSI]\n");
  std::vector<float> out;

  // 1. 词索引点播 (Set 0 / Berzerk): "3" 与别名 "W03" 等价
  check(orm::TsiS14001Engine::Render("3", 0, 1.0f, out), "render berzerk w3");
  check(!out.empty(), "w3 non-empty");
  const double rate = orm::TsiS14001Engine::RateForSet(0, 1.0f);
  const double seconds = (double) out.size() / rate;
  check(seconds > 0.05 && seconds < 4.0, "w3 duration plausible");

  float peak = 0.f;
  for (float s : out)
    peak = std::max(peak, std::abs(s));
  expect_near(peak, 0.85, 0.01, "peak normalized to 0.85");

  std::vector<float> oW;
  check(orm::TsiS14001Engine::Render("W03", 0, 1.0f, oW), "render W03");
  check(oW.size() == out.size(), "W03 same as 3");

  // 2. 确定性
  std::vector<float> o2;
  orm::TsiS14001Engine::Render("3", 0, 1.0f, o2);
  check(o2.size() == out.size(), "deterministic samples");

  // 3. 时钟缩放: 采样数不变, 原生时钟翻倍 => 时长减半
  std::vector<float> fast;
  check(orm::TsiS14001Engine::Render("3", 0, 2.0f, fast), "rate 2.0 renders");
  const double fastDur = (double) fast.size() / orm::TsiS14001Engine::RateForSet(0, 2.0f);
  check(fastDur < seconds * 0.7, "rate 2.0 halves duration");

  // 4. 多词串联与未知词跳过
  std::vector<float> multi, skip;
  check(orm::TsiS14001Engine::Render("3 3", 0, 1.0f, multi), "multi-word renders");
  check(multi.size() > out.size(), "multi longer than single");

  check(orm::TsiS14001Engine::Render("3 NOSUCH", 0, 1.0f, skip), "unknown token skipped");
  check(skip.size() == out.size(), "skip adds nothing");

  // 5. 覆盖全部 9 个 Sound Set
  for (int s = 0; s < orm::s14001::kNumSets; ++s)
  {
    std::vector<float> o;
    const bool ok = orm::TsiS14001Engine::Render("0 1 2 3", s, 1.0f, o);
    check(ok && !o.empty(), "sound set plays");
  }

  // 6. 异常输入防御
  check(!orm::TsiS14001Engine::Render("", 0, 1.0f, out), "empty fails");
  check(!orm::TsiS14001Engine::Render("NOSUCH", 0, 1.0f, out), "unknown fails");
}

// ---- VoiceRenderer 回放 ----

static std::vector<float> MakeSine(int n, double freq, double rate)
{
  std::vector<float> v(n);
  for (int i = 0; i < n; i++)
    v[(size_t) i] = (float) std::sin(2.0 * M_PI * freq * i / rate);
  return v;
}

static void TestVoiceRenderer()
{
  printf("[VoiceRenderer]\n");
  const double srcRate = 22050.0, hostRate = 48000.0;
  const int srcN = 22050; // 1 秒正弦
  auto phrase = MakeSine(srcN, 220.0, srcRate);

  orm::VoiceRenderer vp;
  vp.SetEnvelope(5.0, 80.0);

  // 未触发时无输出
  {
    std::vector<float> buf(256, 0.f);
    vp.ProcessAdd(buf.data(), (int) buf.size());
    float peak = 0.f;
    for (float s : buf)
      peak = std::max(peak, std::abs(s));
    check(peak == 0.f, "silent before trigger");
  }

  // pitch 1.0: 时长约等于 (1s * srcRate/hostRate) 个宿主采样
  vp.SetPhrase(phrase.data(), (int) phrase.size(), srcRate, hostRate);
  vp.Trigger(1.0);
  double played = 0;
  std::vector<float> buf(480, 0.f);
  while (vp.IsPlaying() && played < srcRate * 3.0)
  {
    std::fill(buf.begin(), buf.end(), 0.f);
    vp.ProcessAdd(buf.data(), (int) buf.size());
    played += buf.size();
  }
  expect_near(played / hostRate, 1.0, 0.05, "1.0 ratio plays ~1s");

  // 变调: 2.0 ratio 播放时长减半
  vp.SetPhrase(phrase.data(), (int) phrase.size(), srcRate, hostRate);
  vp.Trigger(2.0);
  played = 0;
  while (vp.IsPlaying() && played < srcRate * 3.0)
  {
    std::fill(buf.begin(), buf.end(), 0.f);
    vp.ProcessAdd(buf.data(), (int) buf.size());
    played += buf.size();
  }
  expect_near(played / hostRate, 0.5, 0.03, "2.0 ratio plays ~0.5s");

  // 包络: 触发后第一个样本不应爆音 (从 0 开始)
  vp.SetPhrase(phrase.data(), (int) phrase.size(), srcRate, hostRate);
  vp.SetEnvelope(50.0, 50.0);
  vp.Trigger(1.0);
  std::vector<float> buf2(64, 0.f);
  vp.ProcessAdd(buf2.data(), (int) buf2.size());
  expect_near(std::abs(buf2[0]), 0.0, 0.02, "no click at attack");

  // release: note-off 后包络衰减到静音
  vp.Release();
  std::vector<float> buf3(256, 0.f);
  vp.ProcessAdd(buf3.data(), (int) buf3.size());
  expect_near(std::abs(buf3.back()), 0.0, 1e-4, "silence after release");

  // 进度报告
  check(vp.Progress() > 0.0, "progress reported");
}

int main()
{
  TestSam();
  TestDectalk();
  TestSp0256();
  TestTms();
  TestS14001();
  TestVoiceRenderer();
  printf("%d checks, %d failures\n", gChecks, gFails);
  return gFails == 0 ? 0 : 1;
}
