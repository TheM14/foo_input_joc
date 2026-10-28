// See lang.h: one row per string, Chinese first because it is the source text.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "lang.h"

namespace joc_lang {

namespace {

struct Entry {
    Text id;
    const char* zh;
    const char* en;
};

constexpr Entry kTexts[] = {
    {Text::kPageName, "JOC 解码器", "JOC decoder"},
    {Text::kGroupOutput, "输出", "Output"},
    {Text::kRadioBinaural, "双耳（HRTF）", "Binaural (HRTF)"},
    {Text::kRadioSpeaker, "扬声器布局", "Speaker layout"},
    {Text::kLabelMode, "双耳模式", "Binaural mode"},
    {Text::kComboModeNear, "near（近场）", "near"},
    {Text::kComboModeMid, "mid（默认）", "mid (default)"},
    {Text::kComboModeFar, "far（远场）", "far"},
    {Text::kLabelTail, "房间尾音（秒）", "Room tail (s)"},
    {Text::kCheckInt24, "强制 24bit 输出（int24）", "Force 24-bit output (int24)"},
    {Text::kInt24Hint, "切换后需刷新文件信息", "reload file info after switching"},
    {Text::kSpeakerStatus, "扬声器布局 %s", "Speaker layout %s"},
    {Text::kGroupHrtf, "HRTF", "HRTF"},
    {Text::kRadioSofa, "SOFA 文件", "SOFA file"},
    {Text::kRadioRosella, "Rosella 个性化模型", "Rosella personalized model"},
    {Text::kLabelHrtfFile, "HRTF 文件", "HRTF file"},
    {Text::kButtonBrowse, "浏览…", "Browse…"},
    {Text::kHrtfHint,
     "留空时读组件目录下 HRTF\\binaural.sofa 或 HRTF\\binaural.personalized_headphone。",
     "Empty means HRTF\\binaural.sofa or binaural.personalized_headphone next to "
     "the component."},
    {Text::kHrtfSourceSofa, "SOFA", "SOFA"},
    {Text::kHrtfSourceRosella, "Rosella 个性化模型", "Rosella personalized model"},
    {Text::kHrtfDefault, "默认", "default"},
    {Text::kHrtfDefaultUnknown, "无法确定默认路径",
     "cannot resolve the default location"},
    {Text::kGroupGain, "增益", "Gain"},
    {Text::kCheckGain, "应用增益", "Apply gain"},
    {Text::kLabelGainUnit, "dB", "dB"},
    {Text::kGainStatusOn, "增益开 %.2f dB", "gain on, %.2f dB"},
    {Text::kGainStatusOff, "增益关（输出不衰减）", "gain off (no attenuation)"},
    {Text::kGainHint, "双耳渲染后可能超过 0 dBFS，可用负值衰减。",
     "Binaural rendering can exceed 0 dBFS; attenuate with a negative value."},
    {Text::kGroupOther, "其它", "Other"},
    {Text::kLabelLanguage, "界面语言", "Language"},
    {Text::kLabelFfmpeg, "ffmpeg.exe", "ffmpeg.exe"},
    {Text::kLanguageSystem, "跟随系统", "Follow system"},
    {Text::kLanguageChinese, "简体中文", "简体中文"},
    {Text::kLanguageEnglish, "English", "English"},
    {Text::kSeparator, "：", ": "},
    {Text::kErrorHrtfNeeded,
     "JOC：双耳渲染需要 HRTF 数据文件，请在 Preferences → Tools → JOC 解码器 里指定，"
     "或把输出改为扬声器布局。",
     "JOC: binaural rendering needs an HRTF data file. Set one in Preferences → "
     "Tools → JOC decoder, or switch the output to a speaker layout."},
    {Text::kErrorRenderStream, "JOC：无法创建渲染流。",
     "JOC: cannot create the render stream."},
    {Text::kErrorSeek, "JOC：无法跳转到目标位置。",
     "JOC: cannot seek to the target position."},
};

}  // namespace

Language resolve(Language language) {
    if (language == Language::kChineseSimplified || language == Language::kEnglish) {
        return language;
    }
    // PRIMARYLANGID stays the same across the Chinese variants, so a Traditional
    // Chinese Windows reads the simplified column rather than English.
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE
               ? Language::kChineseSimplified
               : Language::kEnglish;
}

const char* get(Language language, Text id) {
    const bool english = resolve(language) == Language::kEnglish;
    for (const Entry& entry : kTexts) {
        if (entry.id == id) return english ? entry.en : entry.zh;
    }
    return "";
}

}  // namespace joc_lang
