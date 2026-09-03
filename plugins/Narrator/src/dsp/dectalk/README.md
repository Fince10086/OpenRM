# dectalk/ — DECtalk 4.x 合成引擎 (移植)

本目录为 DECtalk 4.x (DTC01 软件版, 1996 年 DEC/2003 年 Fonix 版本) 的完整移植,
取自 [dectalk/DECtalkMini](https://github.com/dectalk/DECtalkMini)
(其代码与上游 [dectalk/dectalk](https://github.com/dectalk/dectalk) 几乎一致)。

## 结构

- `src/` — 引擎源码 (文本前端 cm_/par_/ls_/lsa_*, 韵律 ph_*, 合成器 HLSyn
  vtm_/reson/hlframe 等, 输入 API `epsonapi.c`: TextToSpeech*)
- `include/` — 头文件与编译期数据表 (主词典 maindict.c、各语种 ROM、音色参数
  p_us_vdf_*.c 等; 其中 .c 文件被 src/ 反向 #include, 不单独编译)

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