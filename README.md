# foo_input_joc — foobar2000 的 E-AC-3 JOC 输入组件

[English](README.en.md) · [安装](#安装) · [设置](#设置) · [构建](#构建) · [已知限制](#已知限制)

foobar2000 输入组件，播放 **E-AC-3 JOC（Dolby Atmos）** 文件：从 E-AC-3 同步帧里取出 JOC
对象与 OAMD 元数据，与 ffmpeg 解出的 5.1 核心 PCM 配对后实时渲染，输出双耳（HRTF）或最多
7.1 的扬声器布局。渲染内核直接编译进组件，除 `foo_input_joc.dll` 之外不需要安装任何东西。

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

Preferences → Tools → **JOC decoder**：

* **Output** —— 双耳，或 2.0 到 7.1 的扬声器布局；
* **Binaural mode**（near / mid / far）与房间 **tail** 秒数；
* **HRTF source** —— **SOFA** 文件或 **Rosella** `.personalized_headphone` 模型。路径留空表示
  用默认位置 `<组件目录>\HRTF\` 下的 `binaural.sofa` 或 `binaural.personalized_headphone`；
* **Gain** —— 开关加 dB 值。双耳渲染在核心混音不削顶的素材上也可能超过满刻度，衰减放在这里；
* **ffmpeg** 可执行文件路径。

页面上的控件都不禁用，状态行会说明当前生效的是什么。

### HRTF 数据

SOFA 测量集或个性化耳机模型由使用组件的人自己提供，并且写在 `.gitignore` 里，避免误提交。
扬声器布局不需要 HRTF。双耳渲染缺少 HRTF 时会报错并指出它找的是哪个文件。

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
自检与交叉核对、渲染比对、设置页布局检查、容器探测检查），`tools\` 是构建与测试床脚本。设置项
另有 `JOC_*` 环境变量覆盖（仅用于开发运行），清单与含义在 `src\settings.cpp`。

排查问题看 DLL 旁边的 `joc_decoder.log`；组件启动时会把自己的版本、核心版本、日志路径写在
里面。

## 已知限制

* 不实现 ADM BWF 输出。
* 容器只有在它排在内置容器读取器之前时才会被接管（见[安装](#容器需要调一次解码器顺序)）；核心
  不允许某个解码器去要一个已经被别的条目拿走的文件。这类文件的标签也仍旧归那个读取器。
* 裸流 `.eac3` / `.ec3` 的标签（流前面的 ID3v2，或后面的 APEv2/ID3v1）**能读不能写**：没有
  组件声明可以写裸 E-AC-3，为插入标签重写整个文件也不是本组件该做的事。
* 传输流（`.ts`、`.m2ts`）不接管。
* 播放长度严格等于文件时长。双耳渲染器仍会算出房间尾音，但它不作为文件本身没有的播放时间交付。
* 跳转会从包含目标位置的那个帧重新进入码流，而不是把前面的内容全部解码一遍——这是跳转代价与
  目标位置无关的原因。位置精确、不漂移；样本是同一段波形交给了从该处开始的解码器，与从头播放
  相比差一个很低的噪声底（−59 dBFS 或更低）。

## 许可

`LICENSE` 是上游 MIT 许可，原样复制；`kernel/` 是上游渲染内核源码的副本，保留其声明。见
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
