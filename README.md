# foo_input_joc — foobar2000 的 E-AC-3 JOC 输入组件

[English](README.en.md) · [安装](#安装) · [设置](#设置) · [构建](#构建) · [已知限制](#已知限制)

foobar2000 输入组件，播放 **E-AC-3 JOC（Dolby Atmos）** 文件：从 E-AC-3 同步帧里取出 JOC
对象与 OAMD 元数据，与 ffmpeg 解出的核心 PCM（按 $joc\_dmx\_config\_idx$ 为 5.1 或 7 通道）配对后实时渲染，输出双耳（HRTF）或
2.0 到 9.1.6 的扬声器布局。渲染内核直接编译进组件，除 `foo_input_joc.dll` 之外不需要安装任何东西。

输入是裸流 `.eac3` / `.ec3`，或容器里的 E-AC-3 JOC 轨道（`.mp4` `.m4a` `.m4b` `.m4p`
`.m4r` `.mov` `.mkv` `.mka` `.webm`）。文件里没有 JOC 就不接管：普通 AC-3 / E-AC-3、音频轨道
是 AAC 的容器、传输流都交还 foobar2000，由它原本的解码器播放。

## 环境要求

* foobar2000 1.6（32 位）或 2.x（32 位与 64 位）。
* 一个 `ffmpeg` 可执行文件，默认从 `PATH` 找，设置页可以指定路径。
* 双耳输出需要一份 HRTF 数据文件。**本仓库不附带**，见 [HRTF 数据](#hrtf-数据)。

## 安装

从 [Releases](../../releases) 下载对应架构的包（`v*` 标签触发的 CI 会把两个架构都附上去），
或按[构建](#构建)自行打出 `dist\` 下的产物；文件名都是
`foo_input_joc-<版本>-<架构>.fb2k-component`。把它拖到 foobar2000 上，或用
Preferences → Components → Install 安装，然后重启。1.6 和 2.x 32 位用 `-x86`，2.x 64 位用
`-x64`。

手工安装就把 `foo_input_joc.dll` 放进当前版本会读取的 `user-components` 子目录：

| foobar2000 | 目录 |
|---|---|
| 1.6 | `<profile>\user-components\foo_input_joc\` |
| 2.x | `<app>\user-components\foo_input_joc\`（portable 模式同样如此） |

子目录是必须的——DLL 直接躺在 `user-components\` 下不会被扫描；放进当前版本不读的目录，或
架构不对，都会**静默忽略**，不会有任何提示。

`tools/deploy.ps1 -TestBed <portable foobar2000>` 是上面手工步骤的脚本版；
`tools/run.ps1 -TestBed <路径> -Play <文件>` 可以无人值守播放并把日志打出来。让 foobar2000
通过 `/exit` 正常退出：被强杀的实例会在 `<profile>\running` 留下标记，下次启动会拒绝加载任何
用户组件。

### 容器需要调一次解码器顺序

foobar2000 按 Preferences → **Decoding** 里的顺序询问解码器，内置的容器读取器也在那张表里。
如果它排在前面接到 MP4 / Matroska 文件，文件就被它拿走，JOC 对象随之丢失——于是听起来只是
普通 E-AC-3。

所以要把 **JOC decoder (E-AC-3 JOC)** 提到 **foobar2000 MP4 Demuxer** 与 **foobar2000
Matroska/WebM Reader** 之前。裸流 `.eac3` / `.ec3` 不受这个顺序影响。

## 设置

Preferences → Tools → **JOC 解码器**：

* **输出** —— **双耳（HRTF）**，或 **扬声器布局**：2.0 到 9.1.6，布局名与声道顺序见[下表](#扬声器布局与声道顺序)；
* **双耳模式**（near / mid / far）与房间**尾音**秒数；
* **HRTF** —— **SOFA 文件**或 **Rosella 个性化模型**。路径留空表示
  用默认位置 `<组件目录>\HRTF\` 下的 `binaural.sofa` 或 `binaural.personalized_headphone`；
* **增益** —— 开关加 dB 值。双耳渲染在核心混音不削顶的素材上也可能超过满刻度，衰减放在这里；
* **强制 24bit 输出（int24）** —— 默认关闭，交付 float32。打开后，交付前按内核写 WAV 的 int24
  规则量化（截到 ±1 → ×8388607 → 截断），超范围的样本被截断到边缘、次数记进日志；组件同时把该
  文件的技术信息报成 24bit 定点。渲染本身仍是 float32，这一项说的是交付给 foobar2000 的采样精度，
  不是输出设备的位深（那个在 Preferences → Output 里设置）。**切换这个开关后要刷新文件信息**
  （Properties → Reload info，或把文件从媒体库移除后重新加入）：foobar2000 按文件缓存技术信息，
  缓存没刷新时 Properties 与转换器仍按旧的 32bit 浮点判断，会提示"编码器不支持浮点输入"。
* **ffmpeg.exe** 可执行文件路径。
* **界面语言** —— **跟随系统** / **简体中文** / **English**。设置页的全部文字、状态行和用户可见的
  错误信息都用这一项选定的语言，切换后立刻生效；选择存在组件自己的配置里。"跟随系统"按 Windows 的
  界面语言决定：中文（含繁体）用简体中文，其它语言用英文。这一项只影响界面文字，解码与渲染不读它。

页面上的控件都不禁用，状态行会说明当前生效的是什么。中文页面的控件名与上表一致，英文页面是同一些
控件的译文（例如 **Output**、**Speaker layout**、**Binaural mode**、**HRTF file**、**Apply gain**）。

### HRTF 数据

SOFA 测量集或个性化耳机模型由使用组件的人自己提供，并且写在 `.gitignore` 里，避免误提交。
扬声器布局不需要 HRTF。双耳渲染缺少 HRTF 时会报错并指出它找的是哪个文件。

## 扬声器布局与声道顺序

组件交给 foobar2000 的 PCM 与转换器写出的多声道 WAV 都按渲染内核的声道顺序排列：

| 布局 | 声道顺序 |
|---|---|
| 2.0 | L, R |
| 3.1 | L, R, C, LFE |
| 4.0 | L, R, Lrs, Rrs |
| 5.1 | L, R, C, LFE, Ls, Rs |
| 7.1 | L, R, C, LFE, Ls, Rs, Lrs, Rrs |
| 5.1.2 | L, R, C, LFE, Ls, Rs, Ltm, Rtm |
| 5.1.4 | L, R, C, LFE, Ls, Rs, Ltf, Rtf, Ltr, Rtr |
| 7.1.2 | L, R, C, LFE, Ls, Rs, Lrs, Rrs, Ltm, Rtm |
| 7.1.4 | L, R, C, LFE, Ls, Rs, Lrs, Rrs, Ltf, Rtf, Ltr, Rtr |
| 9.1.4 | L, R, C, LFE, Ls, Rs, Lrs, Rrs, Lw, Rw, Ltf, Rtf, Ltr, Rtr |
| 9.1.6 | L, R, C, LFE, Ls, Rs, Lrs, Rrs, Lw, Rw, Ltf, Rtf, Ltm, Rtm, Ltr, Rtr |

`4.0` 是本项目补充的 Quad 布局（传统 Quad 的 `FL FR BL BR`），声道为 `L R Lrs Rrs`：前后各一对，
既没有中置也没有 LFE——对象在正前方时由左右两只音箱合成幻像中置，输入里的 LFE 在这种布局下没有
输出声道，按内核规则不混入其它声道。除 `4.0` 外，其余布局及声道顺序均依据杜比相关规范定义。

**声道顺序与 foobar2000 的限制。** 组件把渲染内核的 PCM 原样交出，声道顺序就是上表；写进 WAV 时
（播放的 chunk 或转换器的产物），这 1–16 个声道的排列顺序与杜比规范严格对应。但 foobar2000 的
WAVE 写出只能用一套 Windows 扬声器掩码，它没有 `Ts`（top middle）和 `Ww`（wide）的位置，也无法
表达 7.1 `Ls Rs Lrs Rrs` 这种顺序，所以**声道的定义（掩码/扬声器位置）不保证完全对应**——这是
foobar2000 的限制，不是渲染顺序的问题。Properties 只声明声道数、不声明位置；播放时 chunk 携带
foobar2000 对该声道数的默认映射（未指定掩码会被判“无效的音频流规范”，播放直接失败）。

需要 `dwChannelMask = 0` 的多声道 WAV 时，请自行到
[github.com/TheM14/JustOneCacophony](https://github.com/TheM14/JustOneCacophony) 下载对应的 CLI
程序导出：它写 `WAVE_FORMAT_EXTENSIBLE` + `dwChannelMask = 0`，声道顺序同上表。

播放器、ASIO、DAW 或设备的输出映射要按声道编号自行完成；组件保证各 PCM 声道的内容和编号符合上表。

配置里的布局名按上表读取；无法识别的名字会记一条日志并回落到 `7.1`。

## 构建

```powershell
pwsh -File tools/setup_sdk.ps1          # 官方 SDK 拉进 SDK/，固定到 target 1.5/1.6
pwsh -File tools/build.ps1              # Win32 -> build\Win32\foo_input_joc.dll
pwsh -File tools/build.ps1 -Platform x64
pwsh -File tools/package.ps1            # 两个架构，打包到 dist\*.fb2k-component
```

配置固定为 `Release-Static`（静态 CRT，`/MT`）；`/fp:precise` 是逐字节验收的前提，不要改。
`foo_input_joc.vcxproj` 通过项目引用先构建 `kernel\joc_kernel.vcxproj`；`kernel/` 里的渲染内核
源码以 `JOC_STATIC` / `EJOC_STATIC` 编译，入口既不导入也不导出。`tests\` 是离线工具（码流
自检与交叉核对、渲染比对、布局表与频道顺序检查、设置页布局检查、中英文文字宽度检查、容器探测
检查），`tools\` 是构建与测试床脚本。设置项另有 `JOC_*` 环境变量覆盖（仅用于开发运行），清单与含义在 `src\settings.cpp`。

排查问题看 DLL 旁边的 `joc_decoder.log`；组件启动时会把自己的版本、核心版本、日志路径写在
里面。日志按**字节**封顶：单个文件写满 16 MiB 就滚到同目录的 `joc_decoder.log.1`（覆盖上一次
滚动，不累积），所以两份加起来不超过 32 MiB，最新的内容始终在 `joc_decoder.log`。滚动之后版本
信息会重写一遍。媒体库扫描会给库里每个容器写日志，体量主要来自这里。

## 已知限制

* 不实现 ADM BWF 输出。
* 扬声器布局到 9.1.6（16 声道）为止；内核输出上限就是 16 声道，没有 22.2，见
  [扬声器布局与声道顺序](#扬声器布局与声道顺序)。
* 多声道布局在 Properties 里不声明扬声器位置，播放时的块内映射只是 foobar2000 对该声道数的
  默认值；输出映射要按声道编号自行完成（同上）。
* $joc\_dmx\_config\_idx$ 全部五个档位都支持：取床按音频帧配对，床通道数由档位决定（0/3
  为 6 通道，1/2/4 为 8 通道），ffmpeg 只在 6 通道档位传 `-ac 6`。AC-3 核心 + 依赖子流
  （`strmtyp=1`）与"首个音频帧没有 JOC"都按正常情况处理。
* 容器只有在它排在内置容器读取器之前时才会被接管（见[安装](#容器需要调一次解码器顺序)）；核心
  不允许某个解码器去要一个已经被别的条目拿走的文件。这类文件的标签也仍旧归那个读取器。
* 裸流 `.eac3` / `.ec3` 的标签（流前面的 ID3v2，或后面的 APEv2/ID3v1）**能读不能写**：没有
  组件声明可以写裸 E-AC-3，为插入标签重写整个文件也不是本组件该做的事。
* 传输流（`.ts`、`.m2ts`）不接管。
* **强制 24bit 输出**只改变解码器交付的采样精度：交付的 int24 会被 foobar2000 转成它内部的
  浮点，输出设备用什么位深仍由 Preferences → Output 决定，组件不能替它选。超出 ±1 的样本按内核
  的 `continue` 策略截断（双耳渲染可能超过满刻度，需要衰减时用 **Gain**）。
* 播放长度严格等于文件时长。双耳渲染器仍会算出房间尾音，但它不作为文件本身没有的播放时间交付。
* 跳转会从包含目标位置的那个帧重新进入码流，而不是把前面的内容全部解码一遍——这是跳转代价与
  目标位置无关的原因。位置精确、不漂移；样本是同一段波形交给了从该处开始的解码器，与从头播放
  相比差一个很低的噪声底（−59 dBFS 或更低）。

## 许可

`LICENSE` 是上游 MIT 许可，原样复制；`kernel/` 是上游渲染内核源码的副本，保留其声明。见
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
