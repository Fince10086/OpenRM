# ORM Narrator — TMS 内置词表

> 当前 5 个音色 (UI 音色选择段, kTmsBank):
> 0 **MIL** (官方/军事) / 1 **TI-99** / 2 **ACORN** / 3 **S&S** (TMS5110 Speak & Spell) / 4 **CLOCK** (女声)。
> 用法: TMS 引擎下在文本框输入词名 (大写), 如 `DANGER`、`ONE`、`ABOUT`、`ISLE`; **支持多词, 空格分隔**
> (如 `ROGER MAYDAY OVER`)。串接语义与 Talkie 词 FIFO 一致: 停止帧后接下一个词, 格型
> 滤波器状态跨词持续, 词间自然衰减衔接; 当前音色词库未收录的词自动跳过。
> 带下划线的条目 (`THIR_`、`_TEEN`、`A_M_` 等) 是 ROM 内的音节/子短语, 供拼接, 也可单独触发玩效果。
> 词表来源: 0/4 出自 Talkie (GPL, TI ROM 提取); 1/2 出自 Talkie Vocab_US_TI99/Acorn (GPL);
> 3 出自 TMS5110 (TMC0281) 完整 ROM 词表 (MAME snspell romset 提取, MAME tms5110r.hxx 参数)。

## 数字与序数 (34)

ZERO ONE TWO THREE FOUR FIVE SIX SEVEN EIGHT NINE TEN ELEVEN TWELVE THIR_ FIF_ _TEEN TWENTY HUNDRED THOUSAND THIRTEEN FOURTEEN FIFTEEN SIXTEEN SEVENTEEN EIGHTEEN NINETEEN THIRTY FOURTY FIFTY SIXTY SEVENTY EIGHTY NINETY MILLION

## 单字母 (26)

A B C D E F G H I L J K M N O P Q R S T U V W X Y Z

## NATO 音标字母 (28)

ALPHA BRAVO CHARLIE DELTA ECHO FOXTROT GOLF HENRY INDIA JULIET KILO LIMA MIKE NOVEMBER OSCAR PAPA QUEBEC ROMEO SIERRA TANGO UNIFORM VICTOR WHISKY XRAY YANKEE ZULU HOTEL WHISKEY

## 其余词条 (410, 按字母序)

ABEAN ABORT ABOUT ABOVE ACCELERATED ACKNOWLEDGE ACTION ADIS ADJUST ADVISE AERIAL AFFIRMATIVE AIRCRAFT AIRPORT AIRSPEED AIR AIR_BRAKES ALERT ALL ALOFT ALTERNATE ALTIMETER ALTITUDE AMPS AND ANSWER APPROACHES APPROACH AREA ARRIVAL AS ATU AT AUTOMATIC AUTOPILOT A_M BANK BARKER BASE BELOW BETWEEN BLOWING BOOST BRAKE BREAKING BREAK BROKEN BUTTON BY CABIN CALIBRATE CALL CALM CANCEL CAUTION CEILING CELCIUS CELSIUS CENTRE CHANGE CHECK CHECK_1 CIRCUIT CLEARANCE CLEARANCE_DELIVERY CLEAR CLIMB CLOCK CLOSE COMPLETE CONNECT CONTACT CONTROL CONVERGING COURSE COWL CRANE CROCS CROSSWIND CRYSTALS CURRENT CYCLE CYLINDER DANGER DECREASE DECREASING DEGREES DEGREE DEPARTURE DEPARTURE_1 DEVICE DIRECTION DISPLAY DIVIDED DOORS DOWNWIND DOWN DRIZZLE EAST ELECTRICIAN ELEVATION EMERGENCY ENGINE ENTER EQUALS EQUAL ERROR2 ERROR ESTIMATED ETA EVACUATE EVACUATION EXIT EXPECT FAILURE FAIL FARAD FARENHEIT FAHRENHEIT FAST FEET FILED FINAL FIRE FLAK_LOAD FLAME_OUT FLAPS FLIGHTWATCH FLIGHT FLIGHT_1 FLOW FOG FOR FREEDOM FREEZING FREQUENCY FROM FRONT FSS FUEL FULL GALLEY GALLONS GAP GAS GATE GAUGE GEAR GLIDE GO GREAT2 GREAT GREENWICH GREEN GROUND GUNDISH GUSTING_TO GUST HAIL HAVE HAZE HEADING HEAT HEAVY HERTZ HIGH HOLD HOURS HOUR ICE ICING IDEMTIFY IDLE IFR IGNITE IGNITION ILS IMMEDIATELY INBOUND INCH INCREASE INCREASING INCREASING_TO INDICATED INFLIGHT INFORMATION INNER INSPECTOR INSTRUMENTS INTRUDER IN IS KEY KNOTS LANDING LANDING_GEAR LAND LAUNCH LEAN LEFT LEG LESS_THAN LEVEL LEVEL_1 LEVEL_OFF LIGHTS LIGHT LINE LIST LOCALIZER LONG LONG_1 LOW2 LOWER LOW MACHINE MAGNETOS MAINTAIN MANUAL MAYDAY MEAN MEASURED MEASURE MEGA METER MICRO MIDDLE MIDPOINT MIG MILES MILLI MILL MINUS MINUTES MIST MIXTURE MODERATE MORE_THAN MOTOR MOVE MOVING MUCH NEAR NEGATIVE NEW NINER NORTHEAST NORTHWEST NORTH NOR NOTEM NOT NO NO_TURN NUMBER OBSCURED OCLOCK OFF OF OHMS OIL ON OPEN OPEN_1 OPERATOR OTHER OUTER OUT OVERCAST OVERSPEED OVER PAKM PARTIALLY PASS PAST PATH PELLETS PERCENT PER PHASE PICO PLAN PLEASE PLUS POINT POSITION POWER PRESSURE PRESS PROBE PULL PUMPS PUSH P_M RADAR RADIAL RADIOS RAIN RAISE RANGE READY REAR RED REFUELLING RELEASE REMARK REPAIR REPEAT RICH RIGHT ROGER ROLL_OUT RVRS R_NAV SAFE SAND SCATTERED SECONDS SECURITY SELCAL SELECT SEQUENCE SERVICE SET SEVERE SHORT SHOWERS SHUT SIDE SIGNET SLEET SLOKE SLOW SLOW_1 SMOKE SNOW SOUTHEAST SOUTHWEST SOUTH SPEED SPOILERS SQUALKING SQUALK STABILISER STALL START STOP STORM STRAY SWITCH TARGET TARGET_1 TAXI TELEPHONE TEMPERATURE TERMINAL TEST THEE THE THINLY THIN THUNDERSTORM TIMER TIMES TIME TOOL TOO_LOW TORNADO TOUCHDOWN TOWER TO TRAFFIC TRIM TRUE TURBULANCE TURN UH UNDERCARRIAGE UNDER UNICOM UNIT UNLIMITED UP USE2 USE VACUUM VALVE VAL VECTORS VERIFY VFR VHE VISIBILITY VOLTS VORTAC VOR WAKE WARNING WATCH WATTS WAY WEATHER WEIGHT WEST WHITE WINDOWS WIND YELLOW YOU ZONE

---

## 音色 1 — TI-99 (360 词, Talkie Vocab_US_TI99, TI-99/4A 语音模块 1979, 低沉美式南方男声)

A A1 ABOUT AFTER AGAIN ALL AM AN AND ANSWER ANY ARE AS ASSUME AT B BACK BASE BE BETWEEN BLACK BLUE BOTH BOTTOM BUT BUY BY BYE C CAN CASSETTE CENTER CHECK CHOICE CLEAR COLOR COME COMES COMMA COMMAND COMPLETE COMPLETED COMPUTER CONNECTED CONSOLE CORRECT COURSE CYAN D DATA DECIDE DEVICE DID DIFFERENT DISKETTE DO DOES DOING DONE DOUBLE DOWN DRAW DRAWING E EACH EIGHT EIGHTY ELEVEN ELSE END ENDS ENTER ERROR EXACTLY EYE F FIFTEEN FIFTY FIGURE FIND FINE FINISH FINISHED FIRST FIT FIVE FOR FORTY FOUR FOURTEEN FOURTH FROM FRONT G GAMES GET GETTING GIVE GIVES GO GOES GOING GOOD GOOD_WORK GOODBYE GOT GRAY GREEN GUESS H HAD HAND HANDHELD_UNIT HAS HAVE HEAD HEAR HELLO HELP HERE HIGHER HIT HOME HOW HUNDRED HURRY I I_WIN IF IN INCH INCHES INSTRUCTION INSTRUCTIONS IS IT J JOYSTICK JUST K KEY KEYBOARD KNOW L LARGE LARGER LARGEST LAST LEARN LEFT LESS LET LIKE LIKES LINE LOAD LONG LOOK LOOKS LOWER M MADE MAGENTA MAKE ME MEAN MEMORY MESSAGE MESSAGES MIDDLE MIGHT MODULE MORE MOST MOVE MUST N NAME NEAR NEED NEGATIVE NEXT NICE_TRY NINE NINETY NO NOT NOW NUMBER O OF OFF OH ON ONE ONLY OR ORDER OTHER OUT OVER P PART PEN PENCIL PERHAPS PICTURE PLAY PLAYING PLEASE POINT POSITIVE PRACTICE PRESS PROBLEM PROGRAM PUT R RADIO READ READING READY RECORD RED REMEMBER REMOVE RETURN RIGHT ROUND S SAME SCREEN SEE SET SHAPE SHOW SIDE SIGN SMALL SOME SORT SOUND SPELL SPELLING START STOP STORE SUBTRACT SUCCESS SWITCH T TAKE TAPE TEACH TERRIFIC THAN THAT THE THEN THERE THEY THINK THIS THOSE THREE TIME TINY TO TODAY TOO TOP TOUCH TRY TURN TWO U UNKNOWN UP USE V VERY W WANT WAS WATCH WAY WELL WHAT WHEN WHERE WHICH WHITE WHO WHY WILL WIN WINDOW WITH WORD WORDS WORK WRONG Y YES YOU YOUR

## 音色 2 — Acorn (165 词, Talkie Vocab_US_Acorn, Acorn BBC 语音系统 1983, RP 男声)

PAUSE1 PAUSE2 TONE1 TONE2 _D _ED _ING _S _TEEN _TH _T _Z ZERO HUNDRED THOUSAND ONE TWO TWEN_ THREE THIR_ FOUR FOUR_ FIVE FIF_ SIX SIX_ SEVEN SEVEN_ EIGHT EIGH_ NINE NINE_ A ACORN AFTER AGAIN AMOUNT AN AND ANOTHER ANSWER ANY AVAILABLE B BAD BETWEEN BOTH BUTTON C CASSETTE CHARACTER COMPLETE COMPUTER CORRECT D DATA DATE DO DOLLAR DONT DOWN E EACH ELEVEN ENGAGED ENTER ERROR ESCAPE F FEW FILE FIRST FOUND FROM G GOOD H HAVE I ILLEGAL IN_ INPUT IS J K KEY L LARGE LAST LINE M MANY MINUS MORE MUST N NAME NEGATIVE NEW NO NOT NOW NUMBER O OCLOCK OF OFF OLD ON ONLY OR P PARAMETER PENCE PLEASE PLUS POINT POSITIVE POUN_ PRESS PROGRAMME Q R RED RESET RETURN RUN RUNNING S SAME SCORE SECOND SMALL START STOP SWITCH T TEN THANK THAT THE THEN THIRD THIS TIME TRY TWELVE TYPE U UH UP V VERY W WANT WAS WERE WHAT WHICH X Y YEAR YES YOUR Z

## 音色 3 — S&S (TMS5110 / TMC0281, 1978 Speak & Spell, 完整 ROM 词表 **283 词**)

字母 A–Z: A B C D E F G H I J K L M N O P Q R S T U V W X Y Z
数字 (荧光屏显示名): 0 1 2 3 4 5 6 7 8 9 10
短语 (下划线 = 空格): HERE_IS_YOUR_SCORE I_WIN NEXT_SPELL NOW_SPELL NOW_TRY PERFECT_SCORE SAY_IT THAT_IS_CORRECT THAT_IS_INCORRECT THAT_IS_RIGHT YOU_ARE_CORRECT YOU_ARE_RIGHT YOU_WIN
拼写单词 (233): ABOVE ABSCESS ACHIEVE AGAINST ALMOST ALREADY ANCIENT ANGEL ANOTHER ANSWER ANXIOUS ANYTHING APPROVE BEAUTY BEIGE BELIEVE BLOOD BOULDER BROTHER BUILT BULLET BULLETIN BUREAU BUSHEL BUSINESS BUTCHER CALF CARAVAN CARRY CHALK CHILD CIRCUIT CLEANSER COLOR COMFORT COMING CONQUER CORSAGE COULDN'T COUNTRY COUPLE COURAGE COUSIN DANGER DISCOVER DOES DOZEN DREAD DUNGEON EARLY EARNEST EARTH ECHO ENOUGH ERROR EVERY EVERYONE EXTRA EYEBROW FEATHER FIELD FINGER FIRED FLOOD FLOOR FREIGHT FRONT GARAGE GASOLINE GLACIER GLOVE GREATER GUARD GUESS GUIDE HALF HASTE HEALTH HEALTHY HEAVEN HEAVY HEROES HONEY HONOR HOSTESS HYGIENE IMPROVE INSTEAD IRON ISLE JEALOUS JOURNEY KEY LANGUAGE LAUGH LAUGHTER LEARN LEATHER LEISURE LETTUCE LIBRARY LICORICE LINGER LOSE MACHINE MANGER MARRY MEADOW MEASURE MECHANIC MILD MINUTE MIRROR MONEY MOST MOTHER MOVIE MUSTACHE NARROW NEIGHBOR NIECE NUISANCE OCEAN ONCE ONION OTHER OUTDOOR OVEN PERIOD PIANOS PIERCE PINT PLAGUE PLEASANT PLEASURE PLUNGER PLURAL POLICE POSTAGE POULTRY PRETTY PRIEST PROMISE PULL PUSH QUESTION QUIET QUOTIENT RANGE RANGER READY REINDEER RELIEF RELIEVE REMOVE RHYTHM RURAL SARDINE SAYS SCHEDULE SCHOOL SCISSORS SEARCH SERIOUS SHIELD SHOULD SHOULDER SHOVEL SIGN SKI SMOTHER SOLDIER SOMEONE SOMETIME SOURCE SPELL SPONGE SPREAD SQUAD SQUASH SQUAT STATUE STOMACH STRANGER SUGAR SURE SURGEON SWAMP SWAN SWAP SWEAT SWEATER TALK TERROR TODAY TOMORROW TON TONGUE TOUCH TOUGH TOWARD TREASURE TROUBLE TRY UNCOVER UNION USUAL VIEW WALK WARM WAS WASH WATCH WATER WEALTH WEIRD WELCOME WILD WOLVES WOMAN WONDER WORD WORKMAN WORLD WORTH WRONG YACHT YIELD YOLK YOUNG YOURSELF YOUTH ZEROS

> 全部词条由 `tools/gen_sspell_vocab.py` 从 MAME `snspell` romset (tmc0351n2l +
> tmc0352n2l) 自动提取: ROM 内置词表索引 + 荧光屏文本标注 → 按 TMS5110 位序扫描到
> 停止帧。词条命名 = 荧光屏显示文本 (字母用单字母, 数字用 0-9, 短语空格转下划线,
> 撇号保留如 COULDN'T)。


## 音色 4 — Clock (34 词, Talkie Vocab_US_Clock, VM61002 衍生女声)

THE TIME IS A_M_ P_M_ OH OCLOCK ONE TWO THREE FOUR FIVE SIX SEVEN EIGHT NINE TEN ELEVEN TWELVE THIRTEEN FOURTEEN FIFTEEN SIXTEEN SEVENTEEN EIGHTEEN NINETEEN TWENTY THIRTY FOURTY FIFTY GOOD MORNING AFTERNOON EVENING

---

# TSI 引擎 — S14001A (1975 "Custom ROM Controller") 词表

> **这不是语音合成, 而是 ROM 增量波形点播芯片** (TSI/SSi S14001A): 每套语音 ROM 只有
> 64 个词位 (6-bit 词选择总线), 音高与语速都由外部时钟决定 (基频 = clock/128)。
> 用法: 引擎段选 **TSI**, 文本框输入**词索引** (大写/小写均可, `W03`/`3`/`w12`),
> 空格分隔多个 (如 `W03 12 7`); 0..63, 越界/跑飞/静音位自动跳过。
> 引擎状态机移植自 MAME `s14001a.cpp` (BSD-3-Clause); ROM 数据从 MAME romsets 提取
> (用户授权, 见 `tools/gen_s14001_vocab.py`)。

## 子集 (TSI 音色段标签 → 来源)

| 标签 | 来源 | ROM | 芯片时钟 |
|---|---|---|---|
| **BZ** | Stern Berzerk 街机 (VSU-1000 语音板) | 4KB | 20kHz |
| **F2** | Stern Flight 2000 弹球 (MP-200) | 2KB | 20kHz |
| **C0** | Fidelity 语音国际象棋 CSC (101-32107) | 4KB | 25kHz |
| **C1** | Fidelity CSC 101-64101 低半区 | 4KB | 25kHz |
| **C2** | Fidelity CSC 101-64101 高半区 | 4KB | 25kHz |
| **C3** | Fidelity CSC 101-64105 低半区 | 4KB | 25kHz |
| **C4** | Fidelity CSC 101-64105 高半区 | 4KB | 25kHz |
| **C5** | Fidelity CSC 101-64106 低半区 | 4KB | 25kHz |
| **C6** | Fidelity CSC 101-64106 高半区 | 4KB | 25kHz |

> 词名按索引 W00..W63 暴露 (芯片 ROM 内没有文本标注, 需试听后才能起名)。
> 下面的可用位 = 该子集里实际能出声的索引 (探测自插件同源引擎, 1.0 时钟);
> 时长 = 自然时长范围 (Rate=1.0)。BZ/F2 多为整句短语/音效, C0-C6 为国际象棋语句短词。

## BZ — Stern Berzerk (55/64 可用, 90–987ms)

`0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 33 34 35 36 37 39 40 41 42 43 44 45 46 47 48 49 52 53 54 55 59 60 61 62`

## F2 — Stern Flight 2000 (51/64 可用, 77–987ms)

`0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 24 25 26 28 29 31 32 33 34 35 36 37 38 39 40 42 43 44 45 48 49 50 53 54 56 60 61 63`

## C0 — Fidelity BIOS0 (55/64 可用, 82–3027ms)

`0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 56 58 62 63`

## C1 — BIOS1 低半区 (53/64 可用, 31–2175ms)

`0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 25 27 29 30 31 32 33 34 36 37 38 40 41 42 45 46 47 48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63`

## C2 — BIOS1 高半区 (55/64 可用, 31–1355ms)

`1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 37 38 39 41 43 44 45 47 48 49 50 52 53 54 55 56 57 59 60 61 62 63`

## C3 — BIOS2 低半区 (47/64 可用, 31–1838ms)

`0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 25 27 30 31 32 34 35 36 37 38 40 41 42 45 47 48 49 51 52 53 55 56 57 58 59 60 62`

## C4 — BIOS2 高半区 (48/64 可用, 31–3561ms)

`0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 32 33 35 36 39 40 42 43 44 48 52 55 56 57 60 62 63`

## C5 — BIOS3 低半区 (48/64 可用, 31–2237ms)

`0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 25 26 27 28 30 31 32 33 35 38 40 42 45 47 48 49 50 51 52 54 55 56 58 59 60 61 62 63`

## C6 — BIOS3 高半区 (43/64 可用, 31–3642ms)

`0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 31 33 34 35 40 42 43 47 51 56 57 61 62`

> 全部子集可用位清单由插件同源引擎 (`TsiS14001Engine`) 探测生成; 新增子集只需把
> 对应 ROM 放进 `assets/s14001rom/`、在 `tools/gen_s14001_vocab.py` 里登记并重跑
> (重新生成 ROM 头文件), 再以该引擎跑一遍探测即可更新本节数据。

---

## SP0256 引擎 — GI/Microchip "Narrator" (1981)

> 与前几颗芯片都不同, SP0256 不是整词播放器, 而是 **allophone (音素) 合成器**:
> 芯片内置微序控制器从 ROM 位流解码 13 种参数指令, 驱动 12 阶格型滤波器。
> 采样率 = XTAL/312, 默认 3.12MHz = **10 kHz**; "Rate" 滑杆缩放 XTAL
> (值/72 = 倍数, 1.0 原速; 越大时钟越快 => 语速更快、音高更高, 采样数不变)。
> 算法移植自 MAME `sp0256.cpp` (BSD-3-Clause); ROM 数据源自 GmEsoft 逆向
> (GPL-3.0-or-later), 两份 ROM 均为 2KB mask + 4KB 页零填充。
>
> 用法 (SP0256 引擎下文本框中输入, 大小写不敏感, 空格分隔):
> * **音素档** (与 SAM 控件一致的选择段): 标签输入 — 合并 **AL2** allophone 标签
>   (`PA1`..`PA5` 停顿 + 59 音素, 见下表) 与 **012** Intellivoice 单词标签
>   (`ZERO` `ONE` .. `AND`, 见下表), 另有少量预设英语短语 `HELLO` `THANK` `YOU`
>   `GOOD` `BYE` `WORLD` `SPEECH` `TEST` (自动展开为音素串)。**不接受数字码**
>   (纯数字 token 视为未收录跳过); AL2 音素与 012 单词可混排 (跨 ROM 衔接)。
> * **文本档**: 任意英文文本 (字母/数字/标点), 经 CTS256A-AL2 控制器 (TMS7000
>   单片机 + 4KB 规则引擎) 转成 allophone 码后渲染 — 与真实硬件 "CTS256A ->
>   SP0256" 的 TTS 链路一致。数字自动转词 (`123` -> "one two three")。
> * 未收录 token 自动跳过; 全部无效静音则不出声。多词连续播放, 词间无停顿。
>
> 文本档的转换器 (Cts256aEngine) 逐字节复刻 GmEsoft 仿真: 4KB 掩膜 ROM 内嵌
> (GPL-3.0-or-later), TMS7000 指令集/中断/内存映射与参考实现一致, 开机
> "O.K." 静默抑制。示例转写: `HELLO` -> HH1 EH LL OW、`ONE TWO THREE` ->
> WW AX NN1 / TT2 UW2 / TH RR2 IY、`SPEECH` -> SS SS PP IY CH。

### 音素档 — AL2 Narrator 全音素表 (59/64 可用, 117–375ms)

> 码 0–4 (PA1..PA5) 为停顿, 单独触发静音 (与 TSI 静音词同语义, 用于词间/句间
> 停顿拼写, 如 `HH1 EH L OW PA2 W ER1 L D`)。下表为码 5..63 的可用音素
> (标签输入; 旧版数字码已停用):

| 码 | 音素 | 码 | 音素 | 码 | 音素 | 码 | 音素 |
|---|---|---|---|---|---|---|---|
| 5 | OY | 20 | EY | 35 | VV | 50 | CH |
| 6 | AY | 21 | DD1 | 36 | GG1 | 51 | ER1 |
| 7 | EH | 22 | UW1 | 37 | SH | 52 | ER2 |
| 8 | KK3 | 23 | AO | 38 | ZH | 53 | OW |
| 9 | PP | 24 | AA | 39 | RR2 | 54 | DH2 |
| 10 | JH | 25 | YY2 | 40 | FF | 55 | SS |
| 11 | NN1 | 26 | AE | 41 | KK2 | 56 | NN2 |
| 12 | IH | 27 | HH1 | 42 | KK1 | 57 | HH2 |
| 13 | TT2 | 28 | BB1 | 43 | ZZ | 58 | OR |
| 14 | RR1 | 29 | TH | 44 | NG | 59 | AR |
| 15 | AX | 30 | UH | 45 | LL | 60 | YR |
| 16 | MM | 31 | UW2 | 46 | WW | 61 | GG2 |
| 17 | TT1 | 32 | AW | 47 | XR | 62 | EL |
| 18 | DH1 | 33 | DD2 | 48 | WH | 63 | BB2 |
| 19 | IY | 34 | GG3 | 49 | YY1 | — | — |

### 音素档 — 012 Intellivoice 单词表 (码 6..42, 372–1969ms)

> 码 0 为 SPB640 FIFO 模式, 1–5 为停顿 (静音); 6..42 为 ROM 单词。
> 音素档以单词标签输入 (大小写不敏感), 如 `THREE`、`ZERO`; 旧版数字码
> (如 `10` ≡ `THREE`) 已停用。

```
 6 Mattel Electronics Presents (1970ms)   25 Eighteen (770ms)   34 Ninety  (794ms)
 7 Zero             (534ms)   26 Nineteen  (930ms)   35 Hundred (537ms)
 8 One              (455ms)   27 Twenty    (608ms)   36 Thousand (740ms)
 9 Two              (486ms)   28 Thirty    (625ms)   37 -teen   (509ms)
10 Three            (603ms)   29 Fourty*   (753ms)   38 -ty     (319ms)
11 Four             (624ms)   30 Fifty     (716ms)   39 Press   (434ms)
12 Five             (713ms)   31 Sixty     (825ms)   40 Enter   (453ms)
13 Six              (496ms)   32 Seventy   (790ms)   41 Or      (470ms)
14 Seven            (552ms)   33 Eighty    (534ms)   42 And     (395ms)
15 Eight            (372ms)
16 Nine             (507ms)   17 Ten    (487ms)  18 Eleven (615ms)
19 Twelve           (596ms)   20 Thirteen (906ms) 21 Fourteen (943ms)
22 Fifteen          (938ms)   23 Sixteen (1015ms) 24 Seventeen (980ms)
```
> `*` 原 ROM 标签即写 "Fourty" (应是 Forty), 保留以保持码映射一致。
> 码 43..63 超出标签表 (ROM 内还有 14 个未标注条目, 93–3600ms, 如 57 号 3600ms),
> 数字码可玩但无名称; 码 43–47, 52, 61 静音。

> 时长与码表由插件同源引擎 (`Sp0256Engine`) 探测生成; 新增语音版本只需把
> 对应 2KB mask ROM 交给 `tools/gen_sp0256_rom.py` 重跑并登记进 `kVariants`。

---

# SAM 引擎 — Software Automatic Mouth (1982)

> SAM 是纯软件**规则合成器** (算法音素合成, 不是词表芯片): 文本模式由内置
> RECITER 程序 (约 450 条上下文相关字母→音素规则) 把英文拼写转成音素再合成;
> 音素模式下输入原样透传。内核移植自 discordier/sam (sam.c/render.c/reciter.c),
> 原始用户手册: <https://github.com/discordier/sam/blob/master/docs/manual.md>。
> 本节输出均经插件同源 reciter 探针实测。

## 文本模式 vs 音素模式

- **文本模式 (默认)**: 输入英文文本, RECITER 自动转音素, 拼写准确率约 90%,
  大小写不敏感。手册建议只用于"用户无法控制文本"的场景 (文件/外部输入);
  输入上限 256 字符 (插件侧截断 250)。
- **音素模式**: 输入即音素串, 不做转换, 发音与语调完全受控 (手册推荐用于
  最高质量发音)。
- **没有句内切换命令**: 原软件中两模式是独立程序、不同入口调用。插件文本模式
  靠行尾的 `[` 作内部终止标记 — 若在文本中间输入 `[`, 其后的内容会被丢弃,
  不能用来在句子中间嵌入音素。

## 文本模式 "指令" — 特殊字符 (RECITER rules2 规则表)

> 文本模式没有命令语法 (没有 `[rate=..]` 之类的控制符), 语速/音高由界面滑杆
> 控制; 以下为特殊字符行为。

### 数字 — 逐位朗读

- `0`..`9` 逐位念 (`123` → `WAH4N TUW4 THRIY4` = "one two three", 不是整读)
- 数字中的 `.` 念 point (`3.14` → "three point one four")
- 序数后缀特例: `1ST`→first、`2ND`→second、`3RD`→third、`5TH`→fifth、
  `8TH`→eighth、`10TH`→tenth; 整数两位组如 `64` → "sixty four"

### 标点 — 停顿与语调 (手册: S.A.M. 常规标点 + `!` `;` `:` 视为句号)

| 输入 | 效果 | 实测 |
|---|---|---|
| `.` | 长停顿 + 句终降调 | `HELLO.` → `/HEHLOW.` |
| `!` `;` `:` | 同句号 | `A: B; C.` → `AH. BIY4. SIY4.` |
| `,` | 短停顿 | `HELLO, WORLD` → `/HEHLOW, WERLD` |
| `?` | 句尾升调 | `HELLO?` → `/HEHLOW?` |
| `-` | 仅"两侧都是非字母 (空格两侧)"有短停顿; 紧贴字母不发声 | `RAT-FINK` 无停顿; `JIM - THIS` 有停顿 |
| `'` | 不发声 (仅帮助音节判断) | `APOSTROPHE'S` → `AEPOWSTROW5FEHS` |
| 空格 | 无声停顿 / 分词 | — |

### 符号 — 直接念出单词

| 输入 | 念法 | 实测 |
|---|---|---|
| `"` | quote | `"HELLO"` → `KWOW4T- /HEHLOW -AH5NKWOWT-` |
| `#` | number | `#1` → `NAH4MBER WAH4N` |
| `$` | dollar | `$5` → `DAA4LER FAY4V` |
| `%` | percent | `50%` → `FAY4V ZIY4ROW PERSEH4NT` |
| `&` | and | `A&B` → `AH AEND BIY4` |
| `*` | asterisk | — |
| `+` | plus | `1+1=2` → `WAH4N PLAH4S WAH4N IY4KWULZ TUW4` |
| `/` | slash | — |
| `=` | equals | 同上 `1+1=2` |
| `<` | less than | — |
| `>` | greater than | — |
| `@` | at | — |
| `^` | caret | — |

### 未识别字符

手册: "如果 RECITER 不理解的字符, 直接不传给 S.A.M." (相当于无声停顿忽略);
`(` `)` `[` `]` `\` `_` 等均落在此类。

## 音素模式输入

> 音素代码**连写不用空格** (解析器先匹配 2 字符、失败再匹配单字符); 大小写不敏感。
> 手册示例: `/HEHLOW` = 平调 hello, `/HEH3LOW` = 带重音 hello (`/H` 是清辅音 h,
> 如 "ahead" 的起音)。

### 重音数字 1..8 (手册: stress markers)

> 手册: "There are eight stress markers that can be used simply by inserting a
> number (1-8) **after the vowel to be stressed**." 数字让 S.A.M. 升高/降低音高
> 并拉长对应元音。逐级含义 (手册原表):

| 数字 | 含义 |
|---|---|
| 1 | very emotional stress (非常情绪化) |
| 2 | very emphatic stress (非常强调) |
| 3 | rather strong stress (较强) |
| 4 | **ordinary stress** (普通) |
| 5 | light stress (轻) |
| 6 | **neutral, no pitch change** (中性, 音高不变) |
| 7 | pitch-dropping stress (降调) |
| 8 | extreme pitch-dropping stress (强降调) |

### 音素模式标点 (手册: 仅 4 种)

| 输入 | 作用 (手册原文语义) |
|---|---|
| `-` | 短停顿, 标记子句边界 (short pause) |
| `,` | 停顿约为连字符两倍, 标记短语边界 |
| `.` | 停顿 + 音调**下降** |
| `?` | 停顿 + 音调**上升** |

> 手册另注: 音素串超过约 2.5 秒时 S.A.M. 每 2.5 秒自动插一次短断句, 且总是
> 在标点处断句。

### 音素表

- 单元音: `IY IH EH AE AA AH AO UH AX IX ER UX OH`
- 双元音: `EY AY OY AW OW UW` ・ 成音节: `UL UM UN`
- 双字母辅音: `SH TH /H /X ZH DH CH WH NX DX GX KX RX LX WX YX` (H 写作 `/H`)
- 单字母辅音: `R L W Y M N Q S F Z V J B D G P T K`

---

# DECtalk 引擎参考(对照 DECTalk 5.01 说明书)

> 本节整理 DECtalk 在 Narrator 插件里可用的**文本控制码**与**音色参数**,内容对照
> [assets/DECtalk501.pdf](DECTalk 5.01-E1 User Guide, Fonix 2007) 编写,并逐条在
> 本仓库 vendored 的 DECtalk 4.x 引擎 (`src/dsp/dectalk/`, 版本串 4.99) 上实测验证。
>
> 标注约定:
> - ✅ **实测生效** 在 4.x 引擎上输出有可测量变化 (采样数/字节级差异);
> - ⚠️ **可解析无听感变化** 命令被识别 (不触发错误停顿), 但对该场景无明显听感;
> - ❌ **本引擎不识别** 输入会产生 ~1.5s 错误停顿 (说明书 5.01 有, 4.x 没有, 或参数错误);
> - — 未单独复测 (继承 5.01 说明)。

## 1. 文本控制码 (in-line commands)

控制码内嵌在文本流中, 语法 `[:命令 参数]`, 一行可连续多个, 每个须自成一组括号:
`[:rate 150] [:nb] Hello.` 命令是异步的 (要同步用 `[:sync]`)。**支持首字母缩写**
(说明书 §In-line commands): `[:c]`=comma, `[:pi]`=pitch, `[:pu]`=punct — 注意
`[:p]` 是第一条 p 开头命令 Period Pause。命令之间/参数以空格或 Tab 分隔; 冲突时
"后写的生效"; 参数越界取最近合法值。

| 命令 | 语法 / 参数 | 范围 / 默认 | 本引擎实测 |
|---|---|---|---|
| `[:name]` 音色 | `[:name paul]` 或 `[:n<名字首字母>]` | 9 个内置音色, 默认 PAUL | ✅ 音色切换逐字节验证; `[:nb]` `[:nv]` 均可用 |
| `[:rate]` 语速 | `[:rate DD]` | 75..600 wpm (说明书默认 200; 本引擎原生 ≈ **180**) | ✅ 语速变化验证 (400→0.54× 时长) |
| `[:phoneme]` 音素模式 | `[:phoneme arpabet speak on]` / `[:phoneme on]` / `[:phoneme silent on]` / `[:phoneme off]` | 默认 off; 音素文本必须用 `[ ]` 括起 | ✅ `[:phoneme on]` 生效; ⚠️ 方括号是关键, 见 §4 |
| `[:phone]` | 老写法同 `[:phoneme]` | — | ✅ 可解析 |
| `[:pitch]` 字母音高差 | `[:pitch DD]` | 默认 35 Hz | ⚠️ 只作用于**打字模式的字母大小写音高差** (说明书原话), 对普通文本无可听效果 — 调平均音高请用 `[:dv ap]` / API `ap` |
| `[:dv]` 设计音色 | `[:dv 选项 值 ... save]` | 音色参数见 §2 | ✅ `[:dv ap 200]` 与 API `SetVoiceParamEx("ap",200)` **逐字节一致**; 多选项 `[:dv ap 160 pr 50]`、`[:dv save]` 均正常 |
| `[:rate]`/`[:name]` 缩写 | `[:np]` = name paul, `[:nb]` = name betty | 首字母缩写法 | ✅ (UI 音色段代码 np/nb/nh/nf/nd/nk/nu/nr/nw 同此约定) |
| `[:comma]` 逗号停顿 | `[:comma DD]` / 缩写 `[:cp DD]` | -280..30000 ms (加到默认 280ms 上), `[:cp 0]` 还原 | ✅ `[:comma 500]` 时长增加 ~0.5s |
| `[:period]` 句号停顿 | `[:period DD]` / `[:pp DD]` | -420..30000 ms (默认 640ms) | ✅ `[:period 500]` 时长增加 |
| `[:punct]` 标点朗读 | `[:punct none\|some\|all\|pass]` | — | ✅ `[:punct some]` 可解析 |
| `[:pronounce]` 读音 | `[:pronounce alternate\|primary\|name\|noun\|verb\|adj\|function\|interjection]` | — | ✅ 可解析 (同形词主/次读音切换) |
| `[:mode]` 解读模式 | `[:mode math\|europe\|spell\|name\|citation\|table] [on\|off\|set]` | 全关; `set` 打开一项并关其余 | ✅ `[:mode spell on]` 逐字母朗读验证 |
| `[:say]` 起播时机 | `[:say clause\|word\|letter\|line]` (+`filtered letter`) | 默认 clause | ✅ 可解析 |
| `[:skip]` 跳过预处理 | `[:skip punct\|rule\|all\|parser\|off\|cpg\|none]` | 默认 none | ✅ `[:skip none]` 可解析 |
| `[:tone]` 音调 | `[:tone 频率, 时长]` (两者皆可用空格分隔) | 时长 ms / 频率 Hz | ✅ `[:tone 440 100]` 输出多出恰 ~100ms 440Hz 音 |
| `[:dial]` 拨号音 | `[:dial DD]` | — | ✅ 可解析 (输出有变化) |
| `[:error]` 错误模式 | `[:error ignore\|text\|escape\|speak\|tone]` | 默认 on | ✅ `[:error ignore]` 可解析 |
| `[:sync]` | 裸形式无参数 | — | ✅ 裸 `[:sync]` 可解析; ⚠️ 带参数 (`[:sync 1]`) 反而触发错误停顿 |
| `[:volume]` | `[:volume set\|up\|down\|lset\|rup\|rdown\|sset\|att ...]` | — | ❌ `[:volume up]` 输出 0 样本 (静音), `[:volume 70]` 错误停顿 — **本引擎不可用**, 音量请在 DAW 侧调 |
| `[:pz]` 音高范围 | `[:pz DD]` | 5.01 定义 | ❌ 本引擎错误停顿 — 音高范围用 `[:dv pr DD]` |
| `[:pr]` `[:ap]` `[:am]` `[:ho]` `[:gv]` `[:gn]` `[:hs]` `[:fn]` `[:fx]` `[:age]` `[:gender]` `[:lang]` 等 | 全部 | 4.x 里不是独立命令 | ❌ 均为 `[:dv 选项 值]` 或 API `SetVoiceParamEx` 才有效 |
| `[:loadv]` `[:setv]` | 命令组存取 | — | — 5.01-E1 仅快速列表提及, 未逐测 |
| `[:debug]` `[:sp]` `[:ra]` | 4.x 遗留命令 (不在 5.01 快速列表) | — | ✅ `[:debug]` `[:sp]` 裸形式可解析; ⚠️ `[:ra]` 有输出变化但语义未明 (勿依赖) |
| 未列出的任意文本 | — | — | ❌ 触发 ~1.5s 错误停顿 (先念错误提示再继续), 引擎因此只注入验证过的前缀 |

## 2. 音色参数 (`[:dv 选项]` 与 API SetVoiceParamEx)

说明书 §Voice Definitions 定义了 20 个选项 (5.01-E1 只附 Paul/Wendy 默认值);
本引擎的 `define_options` 表 (API 可识别名) 有 **37 个** — 多出的 `f7 f8 gf gh gv gn
g1..g5 ft ago agvo aguo chink oq` 是 4.x 残留 (说明书注明: "4.6.4 起 g1..gv 已从
[:dv] 移除, 改为自动整定" — 本 4.99 引擎 API 侧仍接受且实测改变输出)。

默认值 (Hz/%, 说明书 §Voice Definitions; 内置音色都是 Paul 与 Betty 的缩放变体):

| 选项 | 含义 | Paul | Wendy | 本引擎实测 (值=100 时输出) |
|---|---|---|---|---|
| `ap` | 平均音高 (Hz) | 112 | 195 | ✅ 改变 (== 插件 kDectalkPitch) |
| `pr` | 音高范围 (%) | 100 | 100 | 改变幅度小 (说明书: pr 影响 f0 摆幅) |
| `sx` | 性别 1=男 0=女 | 1 | 0 | ✅ 改变 (切换男/女目标表) |
| `hs` | 头型大小 (%) | 100 | 100 | ✅ 改变 (变调最剧烈的参数之一) |
| `br` | 气息度 (dB) | 0 | 45 | ✅ 改变 (0..70 dB; 55 + `gv 56` 模拟 Dennis) |
| `ri` | 丰满度 (%) | 70 | 70 | ✅ 改变 (0..90 有力 / 低则柔和) |
| `sm` | 平滑度 (%) | 30 | 20 | ✅ 改变 (0..100, 高=更平滑暗淡) |
| `nf` | 声门开放固定样本数 | 10 | 15 | ✅ 改变 (最短 10 ≈ 1ms) |
| `la` | 喉化 (%) | 0 | 0 | ✅ 改变 (0..100, 嘎裂声) |
| `lx` | 懒气息 (%) | 0 | 80 | ✅ 改变 (0..100) |
| `qu` | 快速性 (%) | 40 | 20 | ✅ 改变 (70% 响应时间常数 ≈100ms/10 值) |
| `as` | 断言度 (%) | 100 | 55 | 改变小 |
| `bf` | 基线下降 (Hz) | 18 | 10 | 改变小 (句内 f0 基线从 +bf/2 以 16Hz/s 落到 -bf/2) |
| `hr` | 帽形上升 (Hz) | 18 | 18 | 改变小 (重音音节帽形抬升) |
| `sr` | 重音上升 (Hz) | 25 | 22 | 改变小 (说明书正文示例写 32, 与表冲突, 以表为准) |
| `f4` `f5` | 第 4/5 共振峰频率 (Hz) | 3300/3650 | 4600/2500 | ✅ 改变 (限制: f5≥f4+300; 男 f4≥3250, 女 f4≥3700, ×hs/100) |
| `b4` `b5` | 第 4/5 共振峰带宽 (Hz) | 280/330 | 300/2048 | ✅ b5 改变 (b4 表现弱) |
| `save` | 存为 Val 的音色 | — | — | ✅ 可解析 (`[:dv ... save]`, 之后 `[:nv]` 切回) |
| `f7` `f8` | 第 7/8 共振峰 | 4.x 残留 | — | — 未单独测 (输出理论上改变) |
| `gf` `gh` `gv` `gn` `g1`..`g5` | 声门增益/展宽/噪声等 | 4.x 残留 (4.6.4 起自动整定) | — | ✅ 实测改变输出 (g1/g4/gf/gh/gv/gn) |
| `ft` | 颤音 | 4.x 残留 | — | ✅ 改变 |
| `oq` `chink` `ago` `agvo` `aguo` | 开放商/缺口/声门开放 | 4.x 残留 | — | ⚠️ 可识别, 值=100 未见变化 (可能取值为 0..60 类范围) |

**音高换算公式** (说明书 §Changing Pitch and Intonation):
f0′ = ap + ((f0 − 120) × pr) / 100,结果钳位在 **50..500 Hz** (插件 kDectalkPitch
同取 50..500)。`ap` 整体平移音高曲线, `pr` 围绕 120Hz 缩放摆幅, `pr 0` = 单调。
音色音高从低到高大致: Harry < ... < Kit。

## 3. 内置音色

说明书: "DECtalk comes with nine built-in voices"; 5.01-E1 只详列 Paul (默认男声)
与 Wendy (气声女声), 其余为 Paul/Betty 的缩放变体。本引擎 9 音色与 `[:name]`/
`[:nX]` 缩写 (与插件 UI 段代码一致):

| 缩写 | 名称 | 说明 (说明书/常识) |
|---|---|---|
| `[:np]` | paul | 默认成年男声 (sx=1, ap=112) |
| `[:nb]` | betty | 女声基准原型 |
| `[:nh]` | harry | 低沉大嗓门 (hs=115, ap 最低) |
| `[:nf]` | frank | 平直音高范围 (pr 相对平) |
| `[:nd]` | dennis | 气声男声 (br≈55) |
| `[:nk]` | kit | 小男孩, ap 最高, qu=50 快 |
| `[:nu]` | ursula | — |
| `[:nr]` | rita | — |
| `[:nw]` | wendy | 气声女声 (br=45, sx=0) |
| `[:nv]` | val | 用户定制槽 (`[:dv ... save]` 写入) |

## 4. 音素模式 (phoneme input)

- 开启: 文本前缀 `[:phoneme on]` (等价 `[:phoneme arpabet speak on]`)。
- **音素串必须放在方括号里**: `[:phoneme on] [HX EH L OW]` — 实测本引擎
  带括号输出 9656 采样, 不带括号 15194 且音频不同 (被当普通文本处理)。
  ⚠️ 当前插件音素直通把用户串以裸文本 + 前缀送入, **未加括号** — 建议后续改为
  自动包裹 `[ ]` (引擎行为已按说明书验证)。
- 括号缺失/不成对时, 方括号后的全部文本都会被当音素念 (说明书 §Avoiding common
  errors), 文本开头放一个 `]` 是保险做法。
- Arpabet 是 2 字符系统, 单字符音素后必须有空格 (如 `[* w 'ayt hxowr s]` 的 t)。
- 静音: `[_<300>]` (时长 ms); 唱歌/音调: `[hxae<300,10> ...]` — `<时长, 音高编号>`,
  音高编号见音调表。

**US 英语 Arpabet 表** (说明书 §Phonetic symbols - US English; 大写用词为示例词):

| 输入 | Arpabet | 示例 | | 输入 | Arpabet | 示例 |
|---|---|---|---|---|---|---|
| `_` | 静音 | — | | `l` | ll | Lad |
| `i` | iy | bEAn | | `h` | hx | Had |
| `I` | ih | pIt | | `R` | rx | fiRe |
| `e` | ey | bAY | | `l` (30) | lx | untiL (成音节 l) |
| `E` | eh | pEt | | `m` | m | Mad |
| `@` | ae | pAt | | `n` | n | Nat |
| `a` | aa | pOt | | `G` | nx | baNG |
| `A` | ay | bUY | | `L` | el | dangLe |
| `W` | aw | brOW | | `D` (35) | dz | wiDth |
| `^` | ah | pUtt | | `N` | en | burdeN |
| `c` | ao | bOUght | | `f` | f | Fat |
| `o` | ow | nO | | `v` | v | Vat |
| `O` | oy | bOY | | `T` | th | THin |
| `U` | uh | pUt | | `D` (40) | dh | THen |
| `u` | uw | bOOn | | `s` | s | Sap |
| `R` (15) | rr | anothER | | `z` | z | Zap |
| `Y` | yu | cUte | | `S` | sh | SHeep |
| `x` | ax | About | | `Z` | zh | meaSure |
| `\|` | ix | kissEs | | `p` | p | Pat |
| `B` | ir | pEEr | | `b` | b | Bad |
| `K` | er | pAir | | `t` | t | Tack |
| `P` | ar | bARn | | `d` | d | Dad |
| `M` | or | bOrn | | `k` | k | Cad |
| `j` (23) | ur | pOOr | | `g` | g | Game |
| `w` | w | Why | | `Q` | tx | baTTen |
| `y` | yx | Yank | | `q` | q | (喉塞) |
| `r` | r | Rat | | `C` | ch | CHeap |
| — | — | — | | `J` | jh | Jeep |
| — | — | — | | `F` | df | wriTer (弹舌) |

**重音/句法符号** (须开音素模式; 说明书 §Stress and syntactic symbols):
`'` 主重音 / `` ` `` 次重音 / `"` 强调重音 / `/` 升调 `\` 降调 (音素模式内);
`-` 音节界 / `*` 词素界 / `#` 复合名词 / `(` 介词短语起 `)` 动词短语起 /
`,` 子句界 / `.` `?` `!` 句末。

**音调表** (说明书 §Tone table, 唱歌用音高编号 `<>`):

| 编号 | 音名 | Hz | | 编号 | 音名 | Hz |
|---|---|---|---|---|---|---|
| 1 | C2 | 65 | | 13 | C3 | 130 |
| 2 | C# | 69 | | 14 | C# | 138 |
| 3 | D | 73 | | 15 | D | 146 |
| 4 | D# | 77 | | 16 | D# | 155 |
| 5 | E | 82 | | 17 | E | 164 |
| 6 | F | 87 | | 18 | F | 174 |
| 7 | F# | 92 | | 19 | F# | 185 |
| 8 | G | 98 | | 20 | G | 196 |
| 9 | G# | 103 | | 21 | G# | 207 |
| 10 | A | 110 | | 22 | A | 220 |
| 11 | A# | 116 | | 23 | A# | 233 |
| 12 | B | 123 | | 24 | B | 247 |
| | | | | 25 | C4 | 261 (向上继续) |

## 5. 引擎差异与踩坑小结 (4.x vs 5.01)

- **默认语速**: 说明书 200 wpm; 本 4.x 引擎原生 ≈180 (实测 `rate 180` 与不设 rate
  逐字节一致) — 插件默认取 180。
- **`[:pitch]` 不是平均音高** (说明书: 打字模式下大写字母的音高增量, 默认 35 Hz);
  早期"死代码"判断修正为"场景差异"。平均音高走 `[:dv ap N]` / API `ap`。
- **`[:ap]` `[:pr]` 等不是独立文本命令** — 只能作 `[:dv]` 选项或 API 参数。
- **未知命令 → ~1.5s 错误停顿** (先念错误提示)。想抑制可前缀 `[:error ignore]`;
  引擎代码只注入验证过的前缀, 未过滤用户文本里的任意 `[:...]` — 文案层应提示。
- **`[:volume]` 在本引擎不可用** (实测静音/错误), 音量由宿主控制。
- 本参考表除标注"—"外均已在本引擎实测; 音素括号用法 (第 4 节) 是本次对照说明书
  验证的新发现, 插件音素直通尚未加括号。
