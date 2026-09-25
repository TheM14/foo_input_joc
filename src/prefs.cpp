// Preferences page for the JOC decoder.
//
// Implemented directly on preferences_page_v3 / preferences_page_instance with a
// plain Win32 modeless dialog.  The SDK's preferences_page_impl<> helper would
// pull in the ATL/WTL dialog framework, which the SDK does not ship; nothing in
// this page needs it.

#include <SDK/foobar2000-lite.h>

#include <SDK/core_api.h>
#include <SDK/preferences_page.h>

#include <windows.h>
#include <commdlg.h>
#include <uxtheme.h>

#include <cstdio>
#include <string>

#include "joc_decode.h"
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

// Layouts are offered up to 7.1 on purpose: output chains handle eight channels
// comfortably, and wider layouts are only meaningful when writing a file.
struct LayoutChoice {
    const char* name;
    const char* label;
};
const LayoutChoice kLayoutChoices[] = {
    {"2.0", "2.0 (立体声)"}, {"3.0", "3.0"},   {"3.1", "3.1"},   {"4.0", "4.0"},
    {"5.0", "5.0"},         {"5.1", "5.1"},   {"6.1", "6.1"},   {"7.0", "7.0"},
    {"7.1", "7.1"},
};

const char* const kModeNames[] = {"near", "far", "mid"};
const char* const kModeLabels[] = {"near（近场）", "far（远场）", "mid（默认）"};

unsigned mode_index(unsigned mode) {
    switch (mode) {
        case 1: return 0;
        case 2: return 1;
        default: return 2;
    }
}

unsigned mode_value(unsigned index) {
    switch (index) {
        case 0: return 1;
        case 1: return 2;
        default: return 3;
    }
}

bool pick_file(HWND owner, std::string& path, const wchar_t* title,
               const wchar_t* filter) {
    wchar_t buffer[4096] = {};
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
        update_status();
        notify();
    }

    void reset() override {
        load_controls(joc_settings::defaults());
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
                load_controls(joc_settings::read());
                apply_dark_mode();
                update_status();
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
                        notify();
                    }
                    return TRUE;
                }
                if (id == IDC_RADIO_HRTF_SOFA || id == IDC_RADIO_HRTF_ROSELLA) {
                    update_enabled_state();
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
                    update_enabled_state();
                    notify();
                    return TRUE;
                }
                if (id == IDC_CHECK_GAIN) {
                    update_enabled_state();
                    notify();
                    return TRUE;
                }
                if (code == EN_CHANGE || code == CBN_SELCHANGE) {
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

    void load_controls(const joc_settings::Values& values) {
        CheckRadioButton(m_hwnd, IDC_RADIO_BINAURAL, IDC_RADIO_SPEAKER,
                         values.output == 0 ? IDC_RADIO_BINAURAL : IDC_RADIO_SPEAKER);

        HWND layout = GetDlgItem(m_hwnd, IDC_COMBO_LAYOUT);
        SendMessageW(layout, CB_RESETCONTENT, 0, 0);
        int selected = 0;
        for (unsigned i = 0; i < sizeof(kLayoutChoices) / sizeof(kLayoutChoices[0]); ++i) {
            SendMessageW(layout, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(wide(kLayoutChoices[i].label).c_str()));
            if (values.speaker_layout == kLayoutChoices[i].name) selected = static_cast<int>(i);
        }
        SendMessageW(layout, CB_SETCURSEL, selected, 0);

        HWND mode = GetDlgItem(m_hwnd, IDC_COMBO_MODE);
        SendMessageW(mode, CB_RESETCONTENT, 0, 0);
        for (unsigned i = 0; i < 3; ++i) {
            SendMessageW(mode, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(wide(kModeLabels[i]).c_str()));
        }
        SendMessageW(mode, CB_SETCURSEL, mode_index(values.binaural_mode), 0);

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
        update_enabled_state();
    }

    joc_settings::Values read_controls() const {
        joc_settings::Values values;
        values.output = IsDlgButtonChecked(m_hwnd, IDC_RADIO_SPEAKER) == BST_CHECKED ? 1 : 0;

        const int layout_index =
            static_cast<int>(SendDlgItemMessageW(m_hwnd, IDC_COMBO_LAYOUT, CB_GETCURSEL, 0, 0));
        if (layout_index >= 0 &&
            layout_index < static_cast<int>(sizeof(kLayoutChoices) / sizeof(kLayoutChoices[0]))) {
            values.speaker_layout = kLayoutChoices[layout_index].name;
        }
        const int mode_sel =
            static_cast<int>(SendDlgItemMessageW(m_hwnd, IDC_COMBO_MODE, CB_GETCURSEL, 0, 0));
        values.binaural_mode = mode_value(mode_sel < 0 ? 2u : static_cast<unsigned>(mode_sel));

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
        // Advanced values are not on this page; keep whatever is stored.
        const joc_settings::Values stored = joc_settings::read();
        values.object_delay_samples = stored.object_delay_samples;
        values.native_threads = stored.native_threads;
        return values;
    }

    // Nothing on this page is ever disabled: a greyed control reads as an option
    // that cannot be used, and none of these settings is invalid in the other
    // mode.  A speaker layout chosen while binaural output is active simply has no
    // effect until the output is switched back, and the gain field describes what
    // the switch would apply.  What is in effect is stated in the status line.
    void update_enabled_state() { update_status(); }

    void update_status() {
        const joc_settings::Values values = joc_settings::read();
        std::string status;
        if (values.output != 0) {
            char text[96] = {};
            const unsigned channels = joc_decode::speaker_channels(values.speaker_layout);
            std::snprintf(text, sizeof(text), "扬声器布局 %s（%u 声道）",
                          values.speaker_layout.c_str(), channels);
            status = text;
        } else {
            // Show what will actually be read, so an empty box is not a mystery.
            joc_decode::Settings effective = joc_settings::current();
            effective.hrtf_source =
                static_cast<joc_decode::HrtfSource>(values.hrtf_source);
            effective.hrtf_file = values.hrtf_file;
            const std::string file = joc_decode::resolve_hrtf_file(effective);
            status = std::string(joc_settings::hrtf_source_name(values.hrtf_source)) + "：" +
                     (file.empty() ? std::string("无法确定默认路径")
                                   : (values.hrtf_file.empty() ? "默认 " + file : file));
        }
        // Say what is actually in effect, including the parts that do not apply
        // to the selected output mode, so nothing has to be greyed out.
        char gain[96] = {};
        if (values.gain_enabled) {
            std::snprintf(gain, sizeof(gain), "\n增益开 %.2f dB", values.gain_db);
        } else {
            std::snprintf(gain, sizeof(gain), "\n增益关（输出不衰减）");
        }
        status += gain;
        set_text(GetDlgItem(m_hwnd, IDC_LABEL_STATUS), status);
    }

    bool changed() const {
        const joc_settings::Values now = read_controls();
        if (now.output != m_initial.output) return true;
        if (now.speaker_layout != m_initial.speaker_layout) return true;
        if (now.hrtf_source != m_initial.hrtf_source) return true;
        if (now.hrtf_file != m_initial.hrtf_file) return true;
        if (now.binaural_mode != m_initial.binaural_mode) return true;
        if (now.gain_enabled != m_initial.gain_enabled) return true;
        if (now.gain_db != m_initial.gain_db) return true;
        if (now.tail_seconds != m_initial.tail_seconds) return true;
        if (now.ffmpeg_path != m_initial.ffmpeg_path) return true;
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
    const char* get_name() override { return "JOC 解码器"; }
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
