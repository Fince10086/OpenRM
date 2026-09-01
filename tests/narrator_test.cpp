// narrator_test — ORM Narrator DSP 内核离线自测
// 纯 C++17, 不依赖 iPlug2。覆盖: SAM 引擎渲染、Reciter 音素转换、VoiceRenderer 回放。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../plugins/Narrator/src/dsp/SamEngine.h"
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

// ---- SAM 引擎 ----

static void TestSamRender()
{
  printf("[SamRender]\n");
  orm::SamSettings settings; // 默认 pitch 64 / speed 72 / mouth 128 / throat 128

  std::vector<float> out;
  check(orm::SamEngine::Render("HELLO WORLD", false, settings, out), "render hello world succeeds");
  check(!out.empty(), "output non-empty");

  const double seconds = (double) out.size() / orm::SamEngine::kSampleRate;
  // 默认参数下 hello world 约 0.8~1.0 s
  check(seconds > 0.3 && seconds < 3.0, "duration plausible");

  // 非静音且峰值归一到 0.85
  float peak = 0.f;
  for (float s : out)
    peak = std::max(peak, std::abs(s));
  check(peak > 0.5f, "audible content");
  expect_near(peak, 0.85, 0.01, "peak normalized to 0.85");

  // 确定性: 同参数两次渲染比特一致
  std::vector<float> out2;
  orm::SamEngine::Render("HELLO WORLD", false, settings, out2);
  check(out.size() == out2.size(), "deterministic length");
  bool same = true;
  for (size_t i = 0; i < out.size() && same; i++)
    if (out[i] != out2[i])
      same = false;
  check(same, "deterministic samples");

  // 参数影响输出: SAM 的 speed 值越大语速越慢 (内核里 speedcounter = speed)
  orm::SamSettings fast = settings;
  fast.speed = 30;
  std::vector<float> outFast;
  orm::SamEngine::Render("HELLO WORLD", false, fast, outFast);
  check(outFast.size() < out.size() / 2, "lower speed value renders shorter");

  // 音高影响输出 (不同内容)
  orm::SamSettings low = settings;
  low.pitch = 20;
  std::vector<float> outLow;
  orm::SamEngine::Render("HELLO WORLD", false, low, outLow);
  bool differs = outLow.size() != out.size();
  if (!differs)
    for (size_t i = 0; i < out.size(); i++)
      if (std::abs(outLow[i] - out[i]) > 0.01f)
      {
        differs = true;
        break;
      }
  check(differs, "different pitch differs");

  // 音素直通模式
  std::vector<float> outPh;
  check(orm::SamEngine::Render("/HEH3LOW2", true, settings, outPh), "phonetic input succeeds");

  // 空文本/静音文本失败
  check(!orm::SamEngine::Render("", false, settings, out), "empty text fails");
  std::vector<float> outSp;
  check(orm::SamEngine::Render("   ", false, settings, outSp) == false || outSp.empty(),
        "whitespace-only fails or silent");
}

// ---- Reciter (经引擎间接验证, 保证文本前端可用) ----

static void TestReciter()
{
  printf("[Reciter]\n");
  orm::SamSettings settings;
  // 常见单词应能合成出可听内容
  for (const char* word : {"THE", "COMPUTER", "DANGER", "ONE", "TWO"})
  {
    std::vector<float> out;
    check(orm::SamEngine::Render(word, false, settings, out), word);
  }
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
  TestSamRender();
  TestReciter();
  TestVoiceRenderer();
  printf("%d checks, %d failures\n", gChecks, gFails);
  return gFails == 0 ? 0 : 1;
}
