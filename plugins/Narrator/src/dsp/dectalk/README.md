# dectalk/ — DECtalk 4.x 合成引擎 (移植)

本目录为 DECtalk 4.x (DTC01 软件版, 1996 年 DEC/2003 年 Fonix 版本) 的完整移植,
取自 [dectalk/DECtalkMini](https://github.com/dectalk/DECtalkMini)
(其代码与上游 [dectalk/dectalk](https://github.com/dectalk/dectalk) 几乎一致)。

## 结构

- `src/` — 引擎源码 (文本前端 cm_/par_/ls_/lsa_*, 韵律 ph_*, 合成器 HLSyn
  vtm_/reson/hlframe 等, 输入 API `epsonapi.c`: TextToSpeech*)
- `include/` — 头文件与编译期数据表 (主词典 maindict.c、各语种 ROM、音色参数
  p_us_vdf_*.c 等; 其中 .c 文件被 src/ 反向 #include, 不单独编译)

## 构建范围: 87 个 src/*.c 仅编译 55 个

插件 CMakeLists 用显式白名单而非 GLOB, 排除了经实测不需要的 32 个文件
(文件保留在树中, 不参与构建):

- **19 个运行期零引用的死代码**: brent / crypt2 (FONIX 授权加密) / dbgwins
  (调试窗口) / decstd97 / dtmmio (Windows MMIO) / frame / hlframe / inithl /
  llinit / log10table / nasalf1x / phinit / playstub / reson / sample /
  sqrttable / voice / acxf1c / circuit。
  其中 `frame hlframe inithl llinit reson sample acxf1c circuit nasalf1x
  log10table sqrttable brent` 是老式 Frame 合成器及其数值表, 本平台的
  11025Hz 输出走 VTM (`vtm.c`/`vtmiont.c`), 不经过它。
- **13 个在当前宏下编译为空文件**: charset (NO_CHARSET) / loadable
  (NO_FILESYSTEM) / lsa_fr / lsa_gr / lsa_ir / lsa_it / lsa_ja / lsa_sl /
  lsa_sp (非英语 LTS, ENGLISH_US 下整文件排除) / maindict_be (大端词典孪生,
  小端时整文件 #ifdef 排除) / mmalloc / par_ambi / ph_syntx。

瘦身依据为实测: 排除前后用 `-dead_strip` 对象级对比, 全部 9 个音色 +
音素模式 + 语速 500 + 音高 300 的输出逐字节一致 (见 /tmp/dtstrip 实验)。
若某日需要 Frame 合成器或大端目标, 恢复相应文件即可。

## 构建配置 (与 DECtalkMini 的 CMake 一致)

以下宏只作用于本目录源文件 (见插件 CMakeLists):

```
_REENTRANT NOMME LTSSIM TTSSIM ANSI BLD_DECTALK_DLL ENGLISH ENGLISH_US
ACCESS32 TYPING_MODE ACNA DISABLE_AUDIO SINGLE_THREADED LIKE_43_OR_44
NO_FILESYSTEM NO_CHARSET
```

- `NO_FILESYSTEM`: 词典编译进二进制 (maindict.c), 不读外部 .dic 文件 —
  插件运行时不可访问文件系统, 必须开启。
- `NO_CHARSET` + `NO_FILESYSTEM`: 绕过 macOS 上 charset.c 的透传分支缺陷
  (该分支会导致 epsonapi 对输入指针执行 free())。
- 输出: 11025 Hz 单声道 16-bit, API 见 include/epsonapi.h
  (TextToSpeechInit/Start/Sync, WAVE_FORMAT_1M16)。

## 许可证

源码自带版权头: DEC 1996/1997、Fonix 2002/2003、Sensimetrics (HLSyn 2.2) 等,
均为 "Restricted Rights / 仅凭书面授权使用" 的专有措辞, **不是开源许可证**。
DECTalk 自 2015 年起经 DECtalk 邮件列表公开散布, 事实上处于 abandonware 状态;
本仓库按用户决策收录 (README 代码复用表有相应说明), 版权头原样保留。