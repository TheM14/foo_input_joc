// User-interface text for the component, in the languages the page offers.
//
// Only strings a user can see belong here: labels, buttons, the status lines under
// the controls, and the messages an error is reported with.  Log lines and engine
// diagnostics stay English, so a support log reads the same whatever the page is
// set to.
//
// The Chinese column is the source: the English column translates it rather than
// wording the feature independently, and the two must mean the same thing.
#pragma once

#include <string>

namespace joc_lang {

enum class Language {
    kSystem = 0,        // follow the Windows user-interface language
    kChineseSimplified = 1,
    kEnglish = 2,
};

enum class Text {
    // preferences page
    kPageName,
    kGroupOutput,
    kRadioBinaural,
    kRadioSpeaker,
    kLabelMode,
    kComboModeNear,
    kComboModeMid,
    kComboModeFar,
    kLabelTail,
    kCheckInt24,
    kInt24Hint,
    kSpeakerStatus,
    kSpeakerStatusAsio,
    kGroupHrtf,
    kRadioSofa,
    kRadioRosella,
    kLabelHrtfFile,
    kButtonBrowse,
    kHrtfHint,
    kHrtfSourceSofa,
    kHrtfSourceRosella,
    kHrtfDefault,
    kHrtfDefaultUnknown,
    kGroupGain,
    kCheckGain,
    kLabelGainUnit,
    kGainStatusOn,
    kGainStatusOff,
    kGainHint,
    kGroupOther,
    kLabelLanguage,
    kLabelFfmpeg,
    kLanguageSystem,
    kLanguageChinese,
    kLanguageEnglish,
    // "SOFA": the separator between a label and the value after it
    kSeparator,
    // messages shown when decoding fails
    kErrorHrtfNeeded,
    kErrorRenderStream,
    kErrorSeek,
};

// The language actually used: kSystem resolves to the Windows user-interface
// language (Chinese if that language is Chinese, English otherwise).
Language resolve(Language language);

// Text for an already resolved language (pass resolve()'s result).
const char* get(Language language, Text id);

}  // namespace joc_lang
