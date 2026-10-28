// Measures every user-visible string of the preferences page against the control
// it is shown in, in both languages.
//
//   ui_text_fit_test <path to foo_input_joc.dll>
//
// The page is bilingual, and the wider language is not always the translated one:
// the English label is sometimes the longer one.  This check creates the dialog from
// the component's own resource (the same way the component does), then measures
// each string with the control's own font and compares it with the space that
// control has.  Multi-line static controls are wrapped at word boundaries, so a
// hint that needs one line too many is reported as well.
//
// The text itself comes from src/lang.cpp, so the check covers both languages with
// the strings that ship; the mapping below mirrors what the page applies.
//
// The component imports the SDK helpers from the host's shared.dll, so that file
// has to sit beside this executable (and beside the component).

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../src/lang.h"
#include "../src/joc_decode.h"
#include "../src/resource.h"

namespace {

int g_failures = 0;

std::string narrow(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::wstring widen(const char* text) {
    if (text == nullptr || *text == '\0') return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), needed);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

class Measurer {
public:
    Measurer() {
        dc_ = CreateCompatibleDC(nullptr);
        SetMapMode(dc_, MM_TEXT);
    }
    ~Measurer() {
        if (dc_ != nullptr) DeleteDC(dc_);
    }

    int width(const std::wstring& text, HFONT font) {
        HGDIOBJ previous = nullptr;
        if (font != nullptr) previous = SelectObject(dc_, font);
        SIZE size{};
        GetTextExtentPoint32W(dc_, text.c_str(), static_cast<int>(text.size()), &size);
        if (previous != nullptr) SelectObject(dc_, previous);
        return size.cx;
    }

    int line_height(HFONT font) {
        HGDIOBJ previous = nullptr;
        if (font != nullptr) previous = SelectObject(dc_, font);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc_, &metrics);
        if (previous != nullptr) SelectObject(dc_, previous);
        return metrics.tmHeight + metrics.tmExternalLeading;
    }

    // Greedy word wrap: how many lines the text needs at this width.
    int lines(const std::wstring& text, HFONT font, int available) {
        int count = 1;
        int used = 0;
        std::wstring word;
        for (std::size_t i = 0; i <= text.size(); ++i) {
            const wchar_t ch = i < text.size() ? text[i] : L' ';
            if (ch == L' ' || ch == L'\n' || i == text.size()) {
                if (!word.empty()) {
                    const int word_width = width(word, font);
                    if (used != 0 && used + width(L" ", font) + word_width > available) {
                        ++count;
                        used = word_width;
                    } else {
                        used += (used != 0 ? width(L" ", font) : 0) + word_width;
                    }
                    word.clear();
                }
                if (ch == L'\n') {
                    ++count;
                    used = 0;
                }
            } else {
                word.push_back(ch);
            }
        }
        return count;
    }

private:
    HDC dc_ = nullptr;
};

struct Binding {
    int control;
    joc_lang::Text text;
};

const Binding kBindings[] = {
    {IDC_GROUP_OUTPUT, joc_lang::Text::kGroupOutput},
    {IDC_RADIO_BINAURAL, joc_lang::Text::kRadioBinaural},
    {IDC_RADIO_SPEAKER, joc_lang::Text::kRadioSpeaker},
    {IDC_LABEL_MODE, joc_lang::Text::kLabelMode},
    {IDC_LABEL_TAIL, joc_lang::Text::kLabelTail},
    {IDC_CHECK_INT24, joc_lang::Text::kCheckInt24},
    {IDC_LABEL_INT24_HINT, joc_lang::Text::kInt24Hint},
    {IDC_GROUP_HRTF, joc_lang::Text::kGroupHrtf},
    {IDC_RADIO_HRTF_SOFA, joc_lang::Text::kRadioSofa},
    {IDC_RADIO_HRTF_ROSELLA, joc_lang::Text::kRadioRosella},
    {IDC_LABEL_HRTF, joc_lang::Text::kLabelHrtfFile},
    {IDC_BROWSE_HRTF, joc_lang::Text::kButtonBrowse},
    {IDC_LABEL_HRTF_HINT, joc_lang::Text::kHrtfHint},
    {IDC_GROUP_GAIN, joc_lang::Text::kGroupGain},
    {IDC_CHECK_GAIN, joc_lang::Text::kCheckGain},
    {IDC_LABEL_GAIN, joc_lang::Text::kLabelGainUnit},
    {IDC_LABEL_GAIN_HINT, joc_lang::Text::kGainHint},
    {IDC_GROUP_MISC, joc_lang::Text::kGroupOther},
    {IDC_LABEL_LANGUAGE, joc_lang::Text::kLabelLanguage},
    {IDC_LABEL_FFMPEG, joc_lang::Text::kLabelFfmpeg},
    {IDC_BROWSE_FFMPEG, joc_lang::Text::kButtonBrowse},
};

// The host sizes the page window to its own area and clips whatever is below it,
// without a scrollbar, so an oversized page silently loses its lower groups.  The
// area measured inside a 125 percent foobar2000 window is 456 px tall, i.e. about
// 273 dialog units; 268 leaves room for the window chrome at both 96 and 125.
void check_page_size(HWND dialog) {
    RECT client{};
    GetClientRect(dialog, &client);
    RECT budget{0, 0, 330, 268};
    MapDialogRect(dialog, &budget);
    if (client.right > budget.right || client.bottom > budget.bottom) {
        ++g_failures;
        std::printf("PAGE TOO LARGE: %ldx%ld px, budget %ldx%ld px\n", client.right,
                    client.bottom, budget.right, budget.bottom);
    } else {
        std::printf("page size %ldx%ld px, budget %ldx%ld px\n", client.right, client.bottom,
                    budget.right, budget.bottom);
    }
}

void report(const std::string& language, const char* what, const std::string& text, int needed,
            int available) {
    if (needed <= available) return;
    ++g_failures;
    std::printf("OVERFLOW [%s] %s: %d px needed, %d available: %s\n", language.c_str(), what,
                needed, available, text.c_str());
}

std::size_t index_of(joc_lang::Text text) {
    std::size_t index = 0;
    for (const Binding& binding : kBindings) {
        if (binding.text == text) return index;
        ++index;
    }
    return static_cast<std::size_t>(-1);
}

void check_control(Measurer& measurer, HWND dialog, const Binding& binding,
                   const std::string& language) {
    HWND control = GetDlgItem(dialog, binding.control);
    if (control == nullptr) {
        ++g_failures;
        std::printf("MISSING [%s] control %d\n", language.c_str(), binding.control);
        return;
    }
    RECT rect{};
    GetClientRect(control, &rect);
    const int rect_width = rect.right - rect.left;
    const int rect_height = rect.bottom - rect.top;
    const auto font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    const std::wstring text = widen(joc_lang::get(
        language == "en" ? joc_lang::Language::kEnglish : joc_lang::Language::kChineseSimplified,
        binding.text));

    wchar_t class_name[64] = {};
    GetClassNameW(control, class_name, 64);
    const std::string window_class = narrow(class_name);

    int available = rect_width;
    if (window_class == "Button") {
        const LONG_PTR style = GetWindowLongPtrW(control, GWL_STYLE);
        const LONG_PTR type = style & BS_TYPEMASK;
        // A check box or radio button draws its box inside the control rectangle,
        // and a push button its frame: the text has to fit in what is left.
        available -= (type == BS_AUTOCHECKBOX || type == BS_AUTORADIOBUTTON) ? 17 : 8;
    }
    const int line_height = measurer.line_height(font);
    if (rect_height >= 2 * line_height) {
        const int lines = measurer.lines(text, font, available);
        report(language, "wrapped text", narrow(text.c_str()), lines * line_height,
               rect_height);
    } else {
        report(language, "text width", narrow(text.c_str()), measurer.width(text, font),
               available);
    }
}

void check_line(Measurer& measurer, HWND control, const std::string& language,
                const std::string& text) {
    if (control == nullptr) {
        ++g_failures;
        std::printf("MISSING [%s] status control\n", language.c_str());
        return;
    }
    RECT rect{};
    GetClientRect(control, &rect);
    const int rect_height = rect.bottom - rect.top;
    const auto font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    const std::wstring wide_text = widen(text.c_str());
    const int line_height = measurer.line_height(font);
    if (rect_height >= 2 * line_height) {
        const int lines = measurer.lines(wide_text, font, rect.right - rect.left);
        report(language, "status lines", text, lines * line_height, rect_height);
    } else {
        report(language, "status width", text, measurer.width(wide_text, font), rect.right);
    }
}

void check_combo(Measurer& measurer, HWND control, const std::string& language,
                 const std::vector<std::string>& items) {
    if (control == nullptr) {
        ++g_failures;
        std::printf("MISSING [%s] combo\n", language.c_str());
        return;
    }
    RECT rect{};
    GetClientRect(control, &rect);
    // Drop-down list: the closed control shows the selected item next to the arrow.
    const int available = rect.right - 17;
    const auto font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    for (const std::string& item : items) {
        report(language, "combo item", item, measurer.width(widen(item.c_str()), font),
               available);
    }
}

std::string format(const char* pattern, const char* layout) {
    char buffer[256] = {};
    std::snprintf(buffer, sizeof(buffer), pattern, layout);
    return buffer;
}

INT_PTR CALLBACK stub_proc(HWND, UINT, WPARAM, LPARAM) { return FALSE; }

// The page is an embedded WS_CHILD dialog, so it needs a parent window even when
// it is only measured.
HWND create_host() {
    static const wchar_t* kClass = L"joc_text_fit_host";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kClass;
    RegisterClassExW(&window_class);
    return CreateWindowExW(0, kClass, L"host", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr,
                           nullptr, window_class.hInstance, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ui_text_fit_test <foo_input_joc.dll>\n");
        return 2;
    }
    const std::wstring dll_path = [&] {
        const int needed = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
        std::wstring out(static_cast<std::size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, out.data(), needed);
        if (!out.empty() && out.back() == L'\0') out.pop_back();
        return out;
    }();

    HMODULE module = LoadLibraryExW(dll_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        std::fprintf(stderr, "cannot load %s (error %lu)\n", argv[1], GetLastError());
        return 1;
    }
    HWND host = create_host();
    if (host == nullptr) {
        std::fprintf(stderr, "cannot create the host window (error %lu)\n", GetLastError());
        return 1;
    }
    HWND dialog = CreateDialogParamW(module, MAKEINTRESOURCEW(IDD_JOC_PREFS), host,
                                     stub_proc, 0);
    if (dialog == nullptr) {
        std::fprintf(stderr, "CreateDialogParamW failed (error %lu)\n", GetLastError());
        return 1;
    }

    check_page_size(dialog);

    Measurer measurer;
    for (const std::string language : {"zh", "en"}) {
        for (const Binding& binding : kBindings) {
            check_control(measurer, dialog, binding, language);
        }

        const joc_lang::Language resolved = language == "en"
                                                ? joc_lang::Language::kEnglish
                                                : joc_lang::Language::kChineseSimplified;

        // Combo boxes.
        std::vector<std::string> modes;
        for (unsigned i = 0; i < 3; ++i) {
            const joc_lang::Text id = i == 0 ? joc_lang::Text::kComboModeNear
                                             : i == 1 ? joc_lang::Text::kComboModeFar
                                                      : joc_lang::Text::kComboModeMid;
            modes.emplace_back(joc_lang::get(resolved, id));
        }
        check_combo(measurer, GetDlgItem(dialog, IDC_COMBO_MODE), language, modes);
        check_combo(measurer, GetDlgItem(dialog, IDC_COMBO_LANGUAGE), language,
                    {joc_lang::get(resolved, joc_lang::Text::kLanguageSystem),
                     joc_lang::get(resolved, joc_lang::Text::kLanguageChinese),
                     joc_lang::get(resolved, joc_lang::Text::kLanguageEnglish)});

        std::size_t layout_count = 0;
        const joc_decode::LayoutSpec* layouts = joc_decode::layout_table(&layout_count);
        std::vector<std::string> names;
        for (std::size_t i = 0; i < layout_count; ++i) names.emplace_back(layouts[i].name);
        check_combo(measurer, GetDlgItem(dialog, IDC_COMBO_LAYOUT), language, names);

        // Status lines the page writes at run time.
        const HWND output_status = GetDlgItem(dialog, IDC_LABEL_OUTPUT_STATUS);
        for (std::size_t i = 0; i < layout_count; ++i) {
            check_line(measurer, output_status, language,
                       format(joc_lang::get(resolved, joc_lang::Text::kSpeakerStatus),
                              layouts[i].name));
        }
        // The page shortens the path line before it is shown, so only the two-line
        // envelope of the status field is measured: the label, and the longest text
        // the elision can leave behind (the file name on its own).
        check_line(measurer, GetDlgItem(dialog, IDC_LABEL_STATUS), language,
                   std::string(joc_lang::get(resolved, joc_lang::Text::kHrtfSourceSofa)) +
                       joc_lang::get(resolved, joc_lang::Text::kSeparator) +
                       joc_lang::get(resolved, joc_lang::Text::kHrtfDefault) + "\n" +
                       "binaural.personalized_headphone");
        check_line(measurer, GetDlgItem(dialog, IDC_LABEL_STATUS), language,
                   std::string(
                       joc_lang::get(resolved, joc_lang::Text::kHrtfSourceRosella)) +
                       joc_lang::get(resolved, joc_lang::Text::kSeparator) + "\n" +
                       joc_lang::get(resolved, joc_lang::Text::kHrtfDefaultUnknown));
        char gain[128] = {};
        std::snprintf(gain, sizeof(gain),
                      joc_lang::get(resolved, joc_lang::Text::kGainStatusOn), -12.00);
        check_line(measurer, GetDlgItem(dialog, IDC_LABEL_GAIN_STATUS), language, gain);
        check_line(measurer, GetDlgItem(dialog, IDC_LABEL_GAIN_STATUS), language,
                   joc_lang::get(resolved, joc_lang::Text::kGainStatusOff));
    }

    DestroyWindow(dialog);
    if (g_failures == 0) {
        std::printf("ui_text_fit_test: both languages fit their controls\n");
        return 0;
    }
    std::printf("ui_text_fit_test: %d string(s) do not fit\n", g_failures);
    return 1;
}
