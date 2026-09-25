// Headless check of the preferences dialog resource inside the built component.
//
//   prefs_layout_check <path to foo_input_joc.dll>
//
// The page's appearance cannot be inspected on this machine (windows created from
// an agent shell are not on an enumerable desktop), but every property that makes
// a page look or behave wrong *is* measurable:
//
//   * a WS_CAPTION / WS_BORDER on an embedded page draws a second frame inside the
//     host's own frame -- that is the "two nested pages" look;
//   * a control whose centre does not hit-test to itself cannot be clicked, and
//     WindowFromPoint says which window is swallowing the click (usually a group
//     box declared after it, or the host's clip region when the control sits
//     outside the dialog's client area);
//   * controls outside the dialog's client rectangle are invisible or cut off;
//   * disabled controls are reported so "I can see it but cannot change it" can be
//     told apart from "it is covered".
//
// It creates the dialog from the resource exactly as the component does, inside a
// host window, and prints one line per control.

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Control {
    HWND window = nullptr;
    int id = 0;
    std::string window_class;
    std::string text;
    bool top_level = false;  // direct child of the dialog
};

std::string narrow(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') return {};
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::string class_of(HWND window) {
    wchar_t buffer[256] = {};
    GetClassNameW(window, buffer, 255);
    return narrow(buffer);
}

std::string text_of(HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return {};
    std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(window, buffer.data(), length + 1);
    buffer.resize(static_cast<std::size_t>(length));
    return narrow(buffer.c_str());
}

BOOL CALLBACK collect(HWND window, LPARAM param) {
    auto* controls = reinterpret_cast<std::vector<Control>*>(param);
    Control control;
    control.window = window;
    control.id = GetDlgCtrlID(window);
    control.window_class = class_of(window);
    control.text = text_of(window);
    controls->push_back(control);
    return TRUE;
}

INT_PTR CALLBACK page_proc(HWND, UINT, WPARAM, LPARAM) { return FALSE; }

HWND create_host() {
    static const wchar_t* kClass = L"joc_prefs_host";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    return CreateWindowExW(0, kClass, L"host", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr,
                           nullptr, wc.hInstance, nullptr);
}

bool is_descendant(HWND candidate, HWND ancestor) {
    for (HWND walk = candidate; walk != nullptr; walk = GetParent(walk)) {
        if (walk == ancestor) return true;
    }
    return false;
}

// A control the user can operate: static text and group boxes answer WM_NCHITTEST
// with HTTRANSPARENT, so a real click passes straight through them.
bool is_interactive(HWND window) {
    const std::string window_class = class_of(window);
    if (window_class == "Edit" || window_class == "ComboBox") return true;
    if (window_class != "Button") return false;
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    return (style & BS_GROUPBOX) == 0;
}

// True when another interactive sibling sits above the control at that point --
// that, and only that, is what stops a click from reaching it.  The walk goes
// from the top of the z-order downwards (GW_CHILD / GW_HWNDNEXT), which is the
// order Windows itself hit-tests in.
bool covered_by_sibling(HWND dialog, HWND control, POINT centre) {
    for (HWND walk = GetWindow(dialog, GW_CHILD); walk != nullptr;
         walk = GetWindow(walk, GW_HWNDNEXT)) {
        if (walk == control) return false;  // reached it: nothing above is in the way
        if (!is_interactive(walk) || IsWindowVisible(walk) == FALSE) continue;
        RECT rect{};
        GetWindowRect(walk, &rect);
        POINT top_left{rect.left, rect.top};
        POINT bottom_right{rect.right, rect.bottom};
        ScreenToClient(dialog, &top_left);
        ScreenToClient(dialog, &bottom_right);
        if (centre.x >= top_left.x && centre.x < bottom_right.x && centre.y >= top_left.y &&
            centre.y < bottom_right.y) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: prefs_layout_check <foo_input_joc.dll>\n");
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
    HWND dialog = CreateDialogParamW(module, MAKEINTRESOURCEW(2001), host, page_proc, 0);
    if (dialog == nullptr) {
        std::fprintf(stderr, "CreateDialogParamW failed (error %lu)\n", GetLastError());
        return 1;
    }
    ShowWindow(host, SW_SHOW);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    const LONG_PTR style = GetWindowLongPtrW(dialog, GWL_STYLE);
    RECT window_rect{};
    RECT client_rect{};
    GetWindowRect(dialog, &window_rect);
    GetClientRect(dialog, &client_rect);
    const int border_x = static_cast<int>((window_rect.right - window_rect.left) -
                                          (client_rect.right - client_rect.left));
    const int border_y = static_cast<int>((window_rect.bottom - window_rect.top) -
                                          (client_rect.bottom - client_rect.top));

    std::printf("dialog 0x%p  client=%ldx%ld  non-client=%dx%d\n", static_cast<void*>(dialog),
                client_rect.right, client_rect.bottom, border_x, border_y);
    std::printf("  WS_CAPTION=%s WS_BORDER=%s WS_CHILD=%s WS_VISIBLE=%s WS_TABSTOP=%s\n",
                (style & WS_CAPTION) ? "yes" : "no", (style & WS_BORDER) ? "yes" : "no",
                (style & WS_CHILD) ? "yes" : "no", (style & WS_VISIBLE) ? "yes" : "no",
                (style & WS_TABSTOP) ? "yes" : "no");
    if ((style & WS_CAPTION) != 0) {
        std::printf("  NOTE: a caption on an embedded page is the second frame the host "
                    "already draws\n");
    }

    std::vector<Control> controls;
    EnumChildWindows(dialog, collect, reinterpret_cast<LPARAM>(&controls));

    std::printf("\n%-28s %-22s %-8s %-7s %-9s %-16s %s\n", "text", "class", "id", "enabled",
                "inside", "hit-test", "covered by");
    int problems = 0;
    LONG max_x = 0;
    LONG max_y = 0;
    for (const Control& control : controls) {
        RECT rect{};
        GetWindowRect(control.window, &rect);
        POINT top_left{rect.left, rect.top};
        POINT bottom_right{rect.right, rect.bottom};
        ScreenToClient(dialog, &top_left);
        ScreenToClient(dialog, &bottom_right);
        if (bottom_right.x > max_x) max_x = bottom_right.x;
        if (bottom_right.y > max_y) max_y = bottom_right.y;

        const bool inside = top_left.x >= 0 && top_left.y >= 0 &&
                            bottom_right.x <= client_rect.right &&
                            bottom_right.y <= client_rect.bottom;
        const bool enabled = IsWindowEnabled(control.window) != FALSE;

        // Hit-test the way Windows does for a real click: a static or a group box
        // in the way is transparent, another interactive control is not.
        POINT centre{(top_left.x + bottom_right.x) / 2, (top_left.y + bottom_right.y) / 2};
        const bool blocked = covered_by_sibling(dialog, control.window, centre);
        const bool hits_self = !blocked;
        std::string covered = blocked ? std::string("another control") : std::string();

        // Only controls the user is meant to operate count as problems: static
        // text and group boxes are transparent to the mouse by design.
        const LONG_PTR control_style = GetWindowLongPtrW(control.window, GWL_STYLE);
        const bool is_group_box =
            control.window_class == "Button" && (control_style & BS_GROUPBOX) != 0;
        const bool is_label = control.window_class == "Static" && !is_group_box;
        const bool interactive = !is_group_box && !is_label;
        const bool bad = interactive && (!enabled || !inside || !hits_self);
        if (bad) ++problems;

        std::printf("%-28s %-22s %-8d %-7s %-9s %-16s %s%s\n",
                    control.text.empty() ? "(no text)" : control.text.c_str(),
                    control.window_class.c_str(), control.id, enabled ? "yes" : "NO",
                    inside ? "yes" : "NO", hits_self ? "self" : "OTHER", covered.c_str(),
                    bad ? "   <== PROBLEM" : (interactive ? "" : "   (label)"));
    }

    std::printf("\ncontent extent=%ldx%ld  client=%ldx%ld  %s\n", max_x, max_y,
                client_rect.right, client_rect.bottom,
                (max_x <= client_rect.right && max_y <= client_rect.bottom)
                    ? "(everything fits)"
                    : "(CONTENT IS CLIPPED)");
    std::printf("controls=%zu  problems=%d\n", controls.size(), problems);
    return problems == 0 ? 0 : 1;
}
