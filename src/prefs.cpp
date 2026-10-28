// Preferences page for the JOC decoder.
//
// Implemented directly on preferences_page_v3 / preferences_page_instance with a
// plain Win32 modeless dialog.  The SDK's preferences_page_impl<> helper would
// pull in the ATL/WTL dialog framework, which the SDK does not ship; nothing in
// this page needs it.
//
// Every visible string comes from src/lang.cpp: the dialog resource carries the
// Chinese text as the design-time default, and the page replaces all of it once it
// knows which language is selected, so changing the language combo takes effect
// without reopening the page.

#include <SDK/foobar2000-lite.h>

#include <SDK/core_api.h>
#include <SDK/preferences_page.h>

#include <windows.h>
#include <commdlg.h>
#include <uxtheme.h>

#include <cstdio>
#include <string>
#include <vector>

#include "joc_decode.h"
#include "lang.h"
#include "prefs.h"
#include "log.h"
#include "resource.h"
#include "settings.h"

namespace {

constexpr GUID kPrefsGuid = {0x71d4a2f8, 0x9c35, 0x4e60, {0x8b, 0x12, 0x7d, 0xa4, 0x63, 0xf0, 0x2c, 0x95}};

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                        needed);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                        needed, nullptr, nullptr);
    return out;
}

std::string get_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, buffer.data(), length + 1);
    buffer.resize(static_cast<std::size_t>(length));
    return narrow(buffer);
}

void set_text(HWND control, const std::string& text) {
    SetWindowTextW(control, wide(text).c_str());
}

// A drop-down list's items are rebuilt whenever the language changes, and the
// selection has to survive that.
void fill_combo(HWND combo, const std::vector<std::string>& items, int selected) {
    const int current =
        selected >= 0 ? selected
                      : static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const std::string& item : items) {
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(wide(item).c_str()));
    }
    if (current >= 0 && current < static_cast<int>(items.size())) {
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(current), 0);
    }
}

// The binaural mode combo lists the three modes in the order they widen, with the
// default (mid) in the middle.
joc_lang::Text mode_text(unsigned index) {
    switch (index) {
        case 0: return joc_lang::Text::kComboModeNear;
        case 2: return joc_lang::Text::kComboModeFar;
        default: return joc_lang::Text::kComboModeMid;
    }
}

unsigned mode_index(unsigned mode) {
    switch (mode) {
        case 1: return 0;
        case 2: return 2;
        default: return 1;
    }
}

unsigned mode_value(unsigned index) {
    switch (index) {
        case 0: return 1;
        case 2: return 2;
        default: return 3;
    }
}

int language_index(joc_lang::Language language) {
    switch (language) {
        case joc_lang::Language::kChineseSimplified: return 1;
        case joc_lang::Language::kEnglish: return 2;
        default: return 0;
    }
}

joc_lang::Language language_value(int index) {
    switch (index) {
        case 1: return joc_lang::Language::kChineseSimplified;
        case 2: return joc_lang::Language::kEnglish;
        default: return joc_lang::Language::kSystem;
    }
}

std::vector<std::string> mode_items(joc_lang::Language language) {
    std::vector<std::string> items;
    for (unsigned i = 0; i < 3; ++i) {
        items.emplace_back(joc_lang::get(language, mode_text(i)));
    }
    return items;
}

std::vector<std::string> language_items(joc_lang::Language language) {
    return {joc_lang::get(language, joc_lang::Text::kLanguageSystem),
            joc_lang::get(language, joc_lang::Text::kLanguageChinese),
            joc_lang::get(language, joc_lang::Text::kLanguageEnglish)};
}

bool pick_file(HWND owner, std::string& path, const wchar_t* title,
               const wchar_t* filter) {    wchar_t buffer[4096] = {};
    const std::wstring current = wide(path);
    if (!current.empty() && current.size() < 4090) {
        std::wcsncpy(buffer, current.c_str(), 4090);
    }
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = 4096;
    dialog.lpstrTitle = title;
    dialog.lpstrFilter = filter;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog) == FALSE) return false;
    path = narrow(buffer);
    return true;
}

// Shortens a path to the width a control has, keeping its start and the file name
// and cutting out the middle.  The status line under the group is two lines tall,
// and a long user-components path would otherwise push the file name out of view.
std::string elide_path(HWND control, const std::string& path, int max_width) {
    HDC dc = GetDC(control);
    if (dc == nullptr) return path;
    const auto font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    HGDIOBJ previous = font != nullptr ? SelectObject(dc, font) : nullptr;
    auto fits = [&](const std::wstring& text) {
        SIZE size{};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
        return size.cx <= max_width;
    };

    const std::wstring wide_path = wide(path);
    std::string result = path;
    if (!fits(wide_path)) {
        const std::size_t slash = wide_path.find_last_of(L"\\/");
        const std::wstring tail = slash == std::wstring::npos ? wide_path
                                                             : wide_path.substr(slash);
        const std::wstring head = slash == std::wstring::npos ? std::wstring()
                                                             : wide_path.substr(0, slash);
        result = narrow(tail);
        for (std::size_t keep = head.size(); keep > 0; --keep) {
            const std::wstring candidate = head.substr(0, keep) + L"…" + tail;
            if (fits(candidate)) {
                result = narrow(candidate);
                break;
            }
        }
    }
    if (previous != nullptr) SelectObject(dc, previous);
    ReleaseDC(control, dc);
    return result;
}

class prefs_instance : public preferences_page_instance {
public:
    prefs_instance(HWND parent, preferences_page_callback::ptr callback)
        : m_callback(callback), m_initial(joc_settings::read()) {
        m_hwnd = CreateDialogParamW(core_api::get_my_instance(), MAKEINTRESOURCE(IDD_JOC_PREFS),
                                    parent, &prefs_instance::dialog_proc,
                                    reinterpret_cast<LPARAM>(this));
        if (m_hwnd == nullptr) {
            joc_log::line("prefs: CreateDialogParamW failed (error %lu)", GetLastError());
        } else {
            joc_log::line("prefs: page created, hwnd=%p", static_cast<void*>(m_hwnd));
        }
    }

    ~prefs_instance() {
        if (m_hwnd != nullptr) DestroyWindow(m_hwnd);
    }

    t_uint32 get_state() override {
        t_uint32 state = preferences_state::resettable | preferences_state::dark_mode_supported;
        if (changed()) state |= preferences_state::changed;
        return state;
    }

    HWND get_wnd() override { return m_hwnd; }

    void apply() override {
        const joc_settings::Values values = read_controls();
        joc_settings::write(values);
        m_initial = values;
        joc_log::line("prefs: applied %s", joc_settings::describe(values).c_str());
        apply_texts();
        notify();
    }

    void reset() override {
        load_values(joc_settings::defaults());
        joc_log::line("prefs: reset to defaults");
        notify();
    }

private:
    static INT_PTR CALLBACK dialog_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        prefs_instance* self = nullptr;
        if (message == WM_INITDIALOG) {
            self = reinterpret_cast<prefs_instance*>(lparam);
            self->m_hwnd = hwnd;
            SetWindowLongPtrW(hwnd, DWLP_USER, lparam);
        } else {
            self = reinterpret_cast<prefs_instance*>(GetWindowLongPtrW(hwnd, DWLP_USER));
        }
        if (self == nullptr) return FALSE;
        return self->handle(message, wparam, lparam);
    }

    INT_PTR handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_INITDIALOG:
                load_values(joc_settings::read());
                apply_dark_mode();
                return TRUE;
            case WM_COMMAND: {
                const int id = LOWORD(wparam);
                const int code = HIWORD(wparam);
                if (id == IDC_BROWSE_HRTF) {
                    std::string path = get_text(GetDlgItem(m_hwnd, IDC_EDIT_HRTF));
                    // The filter follows the selected source, because that is what
                    // decides the format the file has to be in.
                    const wchar_t* filter = L"SOFA (*.sofa)\0*.sofa\0所有文件 (*.*)\0*.*\0\0";
                    const wchar_t* title = L"选择 SOFA HRTF 文件";
                    if (IsDlgButtonChecked(m_hwnd, IDC_RADIO_HRTF_ROSELLA) == BST_CHECKED) {
                        filter = L"Rosella 模型 (*.personalized_headphone)\0"
                                 L"*.personalized_headphone\0所有文件 (*.*)\0*.*\0\0";
                        title = L"选择 Rosella 个性化模型";
                    }
                    if (pick_file(m_hwnd, path, title, filter)) {
                        set_text(GetDlgItem(m_hwnd, IDC_EDIT_HRTF), path);
                        update_status();
                        notify();
                    }
                    return TRUE;
                }
                if (id == IDC_RADIO_HRTF_SOFA || id == IDC_RADIO_HRTF_ROSELLA) {
                    update_status();
                    notify();
                    return TRUE;
                }
                if (id == IDC_BROWSE_FFMPEG) {
                    std::string path = get_text(GetDlgItem(m_hwnd, IDC_EDIT_FFMPEG));
                    if (pick_file(m_hwnd, path, L"选择 ffmpeg.exe",
                                  L"可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0\0")) {
                        set_text(GetDlgItem(m_hwnd, IDC_EDIT_FFMPEG), path);
                        notify();
                    }
                    return TRUE;
                }
                if (id == IDC_RADIO_BINAURAL || id == IDC_RADIO_SPEAKER) {
                    update_status();
                    notify();
                    return TRUE;
                }
                if (id == IDC_CHECK_GAIN || id == IDC_CHECK_INT24) {
                    update_status();
                    notify();
                    return TRUE;
                }
                if (code == CBN_SELCHANGE) {
                    if (id == IDC_COMBO_LANGUAGE) {
                        // The language applies to the page itself, so every text is
                        // replaced at once.  The value is stored when the page is
                        // applied, like the other controls.
                        apply_texts();
                    } else if (id == IDC_COMBO_LAYOUT || id == IDC_COMBO_MODE) {
                        update_status();
                    }
                    notify();
                    return TRUE;
                }
                if (code == EN_CHANGE) {
                    if (id == IDC_EDIT_HRTF || id == IDC_EDIT_GAIN) {
                        update_status();
                    }
                    notify();
                    return TRUE;
                }
                break;
            }
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN: {
                if (!m_dark) break;
                HDC dc = reinterpret_cast<HDC>(wparam);
                SetTextColor(dc, RGB(0xF0, 0xF0, 0xF0));
                SetBkColor(dc, RGB(0x20, 0x20, 0x20));
                return reinterpret_cast<INT_PTR>(m_dark_brush);
            }
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX: {
                if (!m_dark) break;
                HDC dc = reinterpret_cast<HDC>(wparam);
                SetTextColor(dc, RGB(0xF0, 0xF0, 0xF0));
                SetBkColor(dc, RGB(0x2B, 0x2B, 0x2B));
                return reinterpret_cast<INT_PTR>(m_dark_edit_brush);
            }
            case WM_ERASEBKGND: {
                if (!m_dark) break;
                RECT area{};
                GetClientRect(m_hwnd, &area);
                FillRect(reinterpret_cast<HDC>(wparam), &area, m_dark_brush);
                return TRUE;
            }
            case WM_DESTROY:
                if (m_dark_brush != nullptr) DeleteObject(m_dark_brush);
                if (m_dark_edit_brush != nullptr) DeleteObject(m_dark_edit_brush);
                m_dark_brush = m_dark_edit_brush = nullptr;
                m_hwnd = nullptr;
                return TRUE;
            default:
                break;
        }
        return FALSE;
    }

    // ---- language ---------------------------------------------------------

    joc_lang::Language selected_language() const {
        const int index = static_cast<int>(
            SendDlgItemMessageW(m_hwnd, IDC_COMBO_LANGUAGE, CB_GETCURSEL, 0, 0));
        return language_value(index);
    }

    // Replaces every visible string with the one for the language the combo has
    // selected.  Safe to call before the combos have items: their lists are
    // (re)built here from the same language, keeping the current selection.
    void apply_texts() {
        const joc_lang::Language language = joc_lang::resolve(selected_language());
        auto label = [&](int control, joc_lang::Text text) {
            set_text(GetDlgItem(m_hwnd, control), joc_lang::get(language, text));
        };
        label(IDC_GROUP_OUTPUT, joc_lang::Text::kGroupOutput);
        label(IDC_RADIO_BINAURAL, joc_lang::Text::kRadioBinaural);
        label(IDC_RADIO_SPEAKER, joc_lang::Text::kRadioSpeaker);
        label(IDC_LABEL_MODE, joc_lang::Text::kLabelMode);
        label(IDC_LABEL_TAIL, joc_lang::Text::kLabelTail);
        label(IDC_CHECK_INT24, joc_lang::Text::kCheckInt24);
        label(IDC_LABEL_INT24_HINT, joc_lang::Text::kInt24Hint);
        label(IDC_GROUP_HRTF, joc_lang::Text::kGroupHrtf);
        label(IDC_RADIO_HRTF_SOFA, joc_lang::Text::kRadioSofa);
        label(IDC_RADIO_HRTF_ROSELLA, joc_lang::Text::kRadioRosella);
        label(IDC_LABEL_HRTF, joc_lang::Text::kLabelHrtfFile);
        label(IDC_BROWSE_HRTF, joc_lang::Text::kButtonBrowse);
        label(IDC_LABEL_HRTF_HINT, joc_lang::Text::kHrtfHint);
        label(IDC_GROUP_GAIN, joc_lang::Text::kGroupGain);
        label(IDC_CHECK_GAIN, joc_lang::Text::kCheckGain);
        label(IDC_LABEL_GAIN, joc_lang::Text::kLabelGainUnit);
        label(IDC_LABEL_GAIN_HINT, joc_lang::Text::kGainHint);
        label(IDC_GROUP_MISC, joc_lang::Text::kGroupOther);
        label(IDC_LABEL_LANGUAGE, joc_lang::Text::kLabelLanguage);
        label(IDC_LABEL_FFMPEG, joc_lang::Text::kLabelFfmpeg);
        label(IDC_BROWSE_FFMPEG, joc_lang::Text::kButtonBrowse);

        fill_combo(GetDlgItem(m_hwnd, IDC_COMBO_MODE), mode_items(language), -1);
        fill_combo(GetDlgItem(m_hwnd, IDC_COMBO_LANGUAGE), language_items(language), -1);
        update_status();
    }

    // ---- control values ---------------------------------------------------

    void load_values(const joc_settings::Values& values) {
        CheckRadioButton(m_hwnd, IDC_RADIO_BINAURAL, IDC_RADIO_SPEAKER,
                         values.output == 0 ? IDC_RADIO_BINAURAL : IDC_RADIO_SPEAKER);
        CheckRadioButton(m_hwnd, IDC_RADIO_HRTF_SOFA, IDC_RADIO_HRTF_ROSELLA,
                         values.hrtf_source == joc_settings::HrtfSource::kRosella
                             ? IDC_RADIO_HRTF_ROSELLA
                             : IDC_RADIO_HRTF_SOFA);

        set_text(GetDlgItem(m_hwnd, IDC_EDIT_HRTF), values.hrtf_file);
        set_text(GetDlgItem(m_hwnd, IDC_EDIT_FFMPEG), values.ffmpeg_path);

        char text[64] = {};
        std::snprintf(text, sizeof(text), "%.2f", values.tail_seconds);
        set_text(GetDlgItem(m_hwnd, IDC_EDIT_TAIL), text);
        std::snprintf(text, sizeof(text), "%.2f", values.gain_db);
        set_text(GetDlgItem(m_hwnd, IDC_EDIT_GAIN), text);

        SendMessageW(GetDlgItem(m_hwnd, IDC_CHECK_GAIN), BM_SETCHECK,
                     values.gain_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(m_hwnd, IDC_CHECK_INT24), BM_SETCHECK,
                     values.force_int24 ? BST_CHECKED : BST_UNCHECKED, 0);

        // The item lists are language-dependent, so they are built (in the stored
        // language) before the selections are restored, and the texts are applied
        // last: apply_texts() reads the language from the combo, so a selection set
        // afterwards would leave the page in the other language.
        const joc_lang::Language language = joc_lang::resolve(values.language);
        fill_combo(GetDlgItem(m_hwnd, IDC_COMBO_LANGUAGE),
                   language_items(language), language_index(values.language));
        fill_combo(GetDlgItem(m_hwnd, IDC_COMBO_MODE), mode_items(language),
                   static_cast<int>(mode_index(values.binaural_mode)));

        std::size_t layout_count = 0;
        const joc_decode::LayoutSpec* layouts = joc_decode::layout_table(&layout_count);
        std::vector<std::string> names;
        int layout_index = 0;
        for (std::size_t i = 0; i < layout_count; ++i) {
            names.emplace_back(layouts[i].name);
            if (values.speaker_layout == layouts[i].name) {
                layout_index = static_cast<int>(i);
            }
        }
        // The list comes from the core-facing table, so the page cannot offer a
        // layout the decoder rejects.
        fill_combo(GetDlgItem(m_hwnd, IDC_COMBO_LAYOUT), names, layout_index);
        apply_texts();
    }

    joc_settings::Values read_controls() const {
        joc_settings::Values values;
        values.output = IsDlgButtonChecked(m_hwnd, IDC_RADIO_SPEAKER) == BST_CHECKED ? 1 : 0;

        const int layout_index =
            static_cast<int>(SendDlgItemMessageW(m_hwnd, IDC_COMBO_LAYOUT, CB_GETCURSEL, 0, 0));
        std::size_t layout_count = 0;
        const joc_decode::LayoutSpec* layouts = joc_decode::layout_table(&layout_count);
        if (layout_index >= 0 && static_cast<std::size_t>(layout_index) < layout_count) {
            values.speaker_layout = layouts[layout_index].name;
        }
        const int mode_sel =
            static_cast<int>(SendDlgItemMessageW(m_hwnd, IDC_COMBO_MODE, CB_GETCURSEL, 0, 0));
        values.binaural_mode = mode_value(mode_sel < 0 ? 1u : static_cast<unsigned>(mode_sel));

        values.hrtf_source = IsDlgButtonChecked(m_hwnd, IDC_RADIO_HRTF_ROSELLA) == BST_CHECKED
                                 ? joc_settings::HrtfSource::kRosella
                                 : joc_settings::HrtfSource::kSofa;
        values.hrtf_file = get_text(GetDlgItem(m_hwnd, IDC_EDIT_HRTF));
        values.ffmpeg_path = get_text(GetDlgItem(m_hwnd, IDC_EDIT_FFMPEG));
        values.tail_seconds =
            std::atof(get_text(GetDlgItem(m_hwnd, IDC_EDIT_TAIL)).c_str());
        values.gain_db = std::atof(get_text(GetDlgItem(m_hwnd, IDC_EDIT_GAIN)).c_str());
        values.gain_enabled =
            IsDlgButtonChecked(m_hwnd, IDC_CHECK_GAIN) == BST_CHECKED;
        values.force_int24 =
            IsDlgButtonChecked(m_hwnd, IDC_CHECK_INT24) == BST_CHECKED;
        values.language = selected_language();
        // Advanced values are not on this page; keep whatever is stored.
        const joc_settings::Values stored = joc_settings::read();
        values.object_delay_samples = stored.object_delay_samples;
        values.native_threads = stored.native_threads;
        return values;
    }

    // Updates the lines that describe what is in effect.  Nothing on this page is
    // disabled: a speaker layout chosen while binaural output is active simply has
    // no effect until the output is switched back, and the status line says which
    // layout and how many channels that would be.
    void update_status() {
        const joc_lang::Language language = joc_lang::resolve(selected_language());
        const joc_settings::Values values = read_controls();

        std::string output_status;
        if (values.output != 0) {
            char text[192] = {};
            std::snprintf(text, sizeof(text),
                          joc_lang::get(language, joc_lang::Text::kSpeakerStatus),
                          values.speaker_layout.c_str());
            output_status = text;
        }
        set_text(GetDlgItem(m_hwnd, IDC_LABEL_OUTPUT_STATUS), output_status);

        joc_decode::Settings effective = joc_settings::current();
        effective.hrtf_source = static_cast<joc_decode::HrtfSource>(values.hrtf_source);
        effective.hrtf_file = values.hrtf_file;
        const std::string file = joc_decode::resolve_hrtf_file(effective);
        const joc_lang::Text source = values.hrtf_source == joc_settings::HrtfSource::kRosella
                                          ? joc_lang::Text::kHrtfSourceRosella
                                          : joc_lang::Text::kHrtfSourceSofa;
        std::string hrtf_status = joc_lang::get(language, source);
        hrtf_status += joc_lang::get(language, joc_lang::Text::kSeparator);
        if (values.hrtf_file.empty()) {
            hrtf_status += joc_lang::get(language, joc_lang::Text::kHrtfDefault);
        }
        hrtf_status += "\n";
        if (file.empty()) {
            hrtf_status += joc_lang::get(language, joc_lang::Text::kHrtfDefaultUnknown);
        } else {
            RECT status_rect{};
            GetClientRect(GetDlgItem(m_hwnd, IDC_LABEL_STATUS), &status_rect);
            hrtf_status += elide_path(GetDlgItem(m_hwnd, IDC_LABEL_STATUS), file,
                                      status_rect.right - 4);
        }
        set_text(GetDlgItem(m_hwnd, IDC_LABEL_STATUS), hrtf_status);

        char gain[96] = {};
        std::snprintf(
            gain, sizeof(gain),
            joc_lang::get(language, values.gain_enabled ? joc_lang::Text::kGainStatusOn
                                                        : joc_lang::Text::kGainStatusOff),
            values.gain_db);
        set_text(GetDlgItem(m_hwnd, IDC_LABEL_GAIN_STATUS), gain);
    }

    bool changed() const {
        const joc_settings::Values now = read_controls();
        if (now.output != m_initial.output) return true;
        if (now.speaker_layout != m_initial.speaker_layout) return true;
        if (now.hrtf_source != m_initial.hrtf_source) return true;
        if (now.hrtf_file != m_initial.hrtf_file) return true;
        if (now.binaural_mode != m_initial.binaural_mode) return true;
        if (now.gain_enabled != m_initial.gain_enabled) return true;
        if (now.force_int24 != m_initial.force_int24) return true;
        if (now.gain_db != m_initial.gain_db) return true;
        if (now.tail_seconds != m_initial.tail_seconds) return true;
        if (now.ffmpeg_path != m_initial.ffmpeg_path) return true;
        if (now.language != m_initial.language) return true;
        return false;
    }

    void notify() {
        if (m_callback.is_valid()) m_callback->on_state_changed();
    }

    // Dark mode without the SDK's ATL helper: ask uxtheme for the app mode and
    // theme the dialog and its children.  Ordinals 132/133/135 are undocumented
    // but have been stable since Windows 1809 and are what every themed dialog
    // uses; failing to resolve them is not fatal, the page just stays light.
    void apply_dark_mode() {
        HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
        if (uxtheme == nullptr) return;
        using should_apps_use_dark_mode_t = BOOL(WINAPI*)();
        using allow_dark_mode_for_window_t = BOOL(WINAPI*)(HWND, BOOL);
        auto should_dark = reinterpret_cast<should_apps_use_dark_mode_t>(
            GetProcAddress(uxtheme, MAKEINTRESOURCEA(132)));
        auto allow_dark = reinterpret_cast<allow_dark_mode_for_window_t>(
            GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)));
        if (should_dark == nullptr || should_dark() == FALSE) return;

        if (allow_dark != nullptr) allow_dark(m_hwnd, TRUE);
        SetWindowTheme(m_hwnd, L"DarkMode_Explorer", nullptr);
        EnumChildWindows(
            m_hwnd,
            [](HWND child, LPARAM) -> BOOL {
                SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
                return TRUE;
            },
            0);
        m_dark_brush = CreateSolidBrush(RGB(0x20, 0x20, 0x20));
        m_dark_edit_brush = CreateSolidBrush(RGB(0x2B, 0x2B, 0x2B));
        m_dark = true;
        joc_log::line("prefs: dark mode applied");
    }

    HWND m_hwnd = nullptr;
    preferences_page_callback::ptr m_callback;
    joc_settings::Values m_initial;
    bool m_dark = false;
    HBRUSH m_dark_brush = nullptr;
    HBRUSH m_dark_edit_brush = nullptr;
};

class prefs_page : public preferences_page_v3 {
public:
    // The page name follows the language setting; foobar2000 asks for it when it
    // builds the tree, so a change shows up the next time the page is opened.
    const char* get_name() override {
        static std::string name;
        name = joc_lang::get(joc_lang::resolve(joc_settings::read().language),
                             joc_lang::Text::kPageName);
        return name.c_str();
    }
    GUID get_guid() override { return kPrefsGuid; }
    GUID get_parent_guid() override { return preferences_page::guid_tools; }

    preferences_page_instance::ptr instantiate(HWND parent,
                                               preferences_page_callback::ptr callback) override {
        return fb2k::service_new<prefs_instance>(parent, callback);
    }
};

static preferences_page_factory_t<prefs_page> g_prefs_page_factory;

}  // namespace

namespace joc_prefs {

GUID page_guid() { return kPrefsGuid; }

}  // namespace joc_prefs
