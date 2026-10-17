#include "tray.h"

#include <stdio.h>

#ifdef _WIN32

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

#define TRAY_MESSAGE   (WM_APP + 1)
#define TRAY_ICON_ID   1

#define MENU_SHOW      100
#define MENU_QUIT      101

#define TRAY_TITLE     "KillSwitch"

static HWND  s_app_window;      /* raylib's window */
static HWND  s_helper;          /* message-only window that owns the tray icon */
static HICON s_icon;
static NOTIFYICONDATAA s_nid;

static bool s_ready;
static bool s_minimized;
static bool s_quit_requested;

/* What the close button does. Read by app_window_proc() below, which is the
   only place WM_CLOSE is ever seen. */
static bool s_close_hides = true;

/* GLFW's own window procedure, chained to for everything we do not handle. */
static WNDPROC s_original_proc;

/*
 * The tray icon needs a window to deliver its notifications to, and raylib's
 * window belongs to GLFW. Subclassing that would mean intercepting GLFW's own
 * message handling, so a private message-only window is used instead: it costs
 * nothing and keeps the two event loops apart.
 */
static LRESULT CALLBACK tray_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg != TRAY_MESSAGE) {
        if (msg == WM_COMMAND) {
            switch (LOWORD(wparam)) {
            case MENU_SHOW: tray_restore(); return 0;
            case MENU_QUIT: s_quit_requested = true; return 0;
            default: break;
            }
        }
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }

    switch (LOWORD(lparam)) {
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
        tray_restore();
        break;

    case WM_RBUTTONUP: {
        HMENU menu = CreatePopupMenu();
        if (menu == NULL) {
            break;
        }
        AppendMenuA(menu, MF_STRING, MENU_SHOW, "Show " TRAY_TITLE);
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(menu, MF_STRING, MENU_QUIT, "Close " TRAY_TITLE);

        POINT at;
        GetCursorPos(&at);

        /* Required so the menu closes when focus moves elsewhere. */
        SetForegroundWindow(hwnd);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, at.x, at.y, 0, hwnd, NULL);
        PostMessageA(hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
        break;
    }

    default:
        break;
    }
    return 0;
}

/*
 * Stop Windows from standing in for the window while the loop is busy.
 *
 * A top-level window that has not taken a message for five seconds counts as
 * hung, and the desktop hides it behind a look-alike of class "Ghost" titled
 * "... (Not Responding)" so the rest of the screen stays usable. When the real
 * window answers again the ghost is destroyed and the real one comes back, and
 * that swap is the flicker: nothing has actually crashed.
 *
 * The interface earns this honestly, and not by being slow. Both the tray menu
 * below and dragging the window run a modal loop of the system's own inside
 * the message pump, and neither returns until the user is finished with it --
 * so no frame is drawn, and past five seconds the ghost appears. There is
 * nothing to fix in the frame itself, so the stand-in is what goes.
 *
 * Turning it off leaves a genuinely wedged window simply frozen instead of
 * captioned, which is the trade: the watchdog in watchdog.h is what says where
 * a real hang was, and it says it to a file rather than to a title bar.
 *
 * Fetched rather than called outright -- MinGW's import library does not
 * always carry it, and a missing stand-in is not worth failing to start over.
 */
static void disable_ghosting(void)
{
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32 == NULL) {
        return;
    }

    typedef void (WINAPI *disable_fn)(void);
    disable_fn disable = (disable_fn)(void (*)(void))
        GetProcAddress(user32, "DisableProcessWindowsGhosting");

    if (disable != NULL) {
        disable();
    }
}

/*
 * The close button must not end the program: it hides to the notification area
 * instead, and only the tray menu really quits.
 *
 * raylib offers no way to clear the close flag once GLFW has set it, so WM_CLOSE
 * is intercepted before GLFW ever sees it.
 */
static LRESULT CALLBACK app_window_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                        LPARAM lparam)
{
    if (msg == WM_CLOSE && s_ready && s_close_hides) {
        tray_minimize();
        return 0;
    }
    return CallWindowProcA(s_original_proc, hwnd, msg, wparam, lparam);
}

bool tray_init(void *window_handle, const char *icon_path, const char *tooltip)
{
    s_app_window = (HWND)window_handle;

    /* Before the tray icon exists, since its menu is one of the two things
       that stall the loop long enough for a ghost to appear. */
    disable_ghosting();

    /* LR_LOADFROMFILE keeps this independent of the executable's resources,
       so the .ico simply ships beside the binary. */
    s_icon = (HICON)LoadImageA(NULL, icon_path, IMAGE_ICON, 0, 0,
                               LR_LOADFROMFILE | LR_DEFAULTSIZE | LR_SHARED);
    if (s_icon == NULL) {
        fprintf(stderr, "tray: could not load %s (%lu)\n",
                icon_path, (unsigned long)GetLastError());
    }

    if (s_icon && s_app_window) {
        SendMessageA(s_app_window, WM_SETICON, ICON_SMALL, (LPARAM)s_icon);
        SendMessageA(s_app_window, WM_SETICON, ICON_BIG, (LPARAM)s_icon);
    }

    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = tray_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "KillSwitchTray";

    if (RegisterClassExA(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    s_helper = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (s_helper == NULL) {
        return false;
    }

    ZeroMemory(&s_nid, sizeof(s_nid));
    s_nid.cbSize = sizeof(s_nid);
    s_nid.hWnd = s_helper;
    s_nid.uID = TRAY_ICON_ID;
    s_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    s_nid.uCallbackMessage = TRAY_MESSAGE;
    s_nid.hIcon = s_icon ? s_icon : LoadIconA(NULL, IDI_APPLICATION);
    snprintf(s_nid.szTip, sizeof(s_nid.szTip), "%s", tooltip ? tooltip : "");

    if (!Shell_NotifyIconA(NIM_ADD, &s_nid)) {
        DestroyWindow(s_helper);
        s_helper = NULL;
        return false;
    }

    if (s_app_window) {
        s_original_proc = (WNDPROC)SetWindowLongPtrA(s_app_window, GWLP_WNDPROC,
                                                     (LONG_PTR)app_window_proc);
    }

    s_ready = true;
    return true;
}

void tray_shutdown(void)
{
    if (s_app_window && s_original_proc) {
        SetWindowLongPtrA(s_app_window, GWLP_WNDPROC, (LONG_PTR)s_original_proc);
        s_original_proc = NULL;
    }
    if (s_ready) {
        Shell_NotifyIconA(NIM_DELETE, &s_nid);
    }
    s_minimized = false;

    if (s_helper) {
        DestroyWindow(s_helper);
        s_helper = NULL;
    }
    s_ready = false;
}

bool tray_available(void)
{
    return s_ready;
}

void tray_set_close_hides(bool hides)
{
    s_close_hides = hides;
}

void tray_minimize(void)
{
    if (!s_ready || s_minimized) {
        return;
    }

    /* The icon is already registered, so this only hides the window. */
    ShowWindow(s_app_window, SW_HIDE);
    s_minimized = true;
}

void tray_restore(void)
{
    if (!s_minimized) {
        return;
    }
    s_minimized = false;

    ShowWindow(s_app_window, SW_SHOW);
    ShowWindow(s_app_window, SW_RESTORE);
    SetForegroundWindow(s_app_window);
}

bool tray_is_minimized(void)
{
    return s_minimized;
}

void tray_notify(const char *title, const char *text)
{
    /* The balloon hangs off the tray icon, so there is nothing to show it from
       unless the icon is currently registered. */
    if (!s_ready || !s_minimized) {
        return;
    }

    NOTIFYICONDATAA balloon = s_nid;
    balloon.uFlags = NIF_INFO;
    balloon.dwInfoFlags = NIIF_INFO;
    snprintf(balloon.szInfoTitle, sizeof(balloon.szInfoTitle), "%s",
             title ? title : "");
    snprintf(balloon.szInfo, sizeof(balloon.szInfo), "%s", text ? text : "");

    Shell_NotifyIconA(NIM_MODIFY, &balloon);
}

bool tray_poll(void)
{
    if (!s_ready) {
        return false;
    }

    /* Our helper window is not pumped by GLFW, so drain it here. */
    MSG msg;
    while (PeekMessageA(&msg, s_helper, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    bool quit = s_quit_requested;
    s_quit_requested = false;
    return quit;
}

#elif defined(TRAY_HAVE_APPINDICATOR)

/*
 * Cinnamon, KDE and the GNOME extensions all show tray icons through
 * StatusNotifierItem over D-Bus; Mint runs xapp-sn-watcher for exactly that.
 * Ayatana's AppIndicator speaks it, so this uses that rather than the retired
 * XEmbed system tray, which current Cinnamon no longer hosts.
 */

#include <libayatana-appindicator/app-indicator.h>
#include <gtk/gtk.h>

#include <string.h>

#include "window.h"

static AppIndicator *s_indicator;
static bool s_ready;
static bool s_hidden;
static bool s_quit_requested;
static bool s_show_requested;

static void on_show_clicked(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    (void)user_data;
    s_show_requested = true;
}

static void on_quit_clicked(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    (void)user_data;
    s_quit_requested = true;
}

/*
 * AppIndicator looks its icon up by name in an icon theme rather than opening a
 * file, so the directory is registered as a search path and the name is the
 * file's stem. "icon.png" in resources/ therefore becomes the name "icon".
 */
static void split_icon_path(const char *path, char *dir, size_t dir_cap,
                            char *name, size_t name_cap)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;

    if (slash) {
        size_t n = (size_t)(slash - path);
        if (n >= dir_cap) {
            n = dir_cap - 1;
        }
        memcpy(dir, path, n);
        dir[n] = 0;
    } else {
        snprintf(dir, dir_cap, ".");
    }

    snprintf(name, name_cap, "%s", base);

    char *dot = strrchr(name, '.');
    if (dot) {
        *dot = 0;
    }
}

bool tray_init(void *window_handle, const char *icon_path, const char *tooltip)
{
    (void)window_handle;        /* NULL on Linux; the GLFW window is used */

    if (s_ready) {
        return true;
    }

    /* Fails when there is no display, in which case there is no tray either. */
    if (!gtk_init_check(NULL, NULL)) {
        fprintf(stderr, "tray: no display, so no tray icon\n");
        return false;
    }

    char dir[512];
    char name[128];

    if (icon_path && icon_path[0]) {
        split_icon_path(icon_path, dir, sizeof(dir), name, sizeof(name));
        s_indicator = app_indicator_new_with_path(
            "iomeeter", name, APP_INDICATOR_CATEGORY_APPLICATION_STATUS, dir);
    } else {
        s_indicator = app_indicator_new(
            "iomeeter", "audio-volume-high",
            APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    }

    if (s_indicator == NULL) {
        fprintf(stderr, "tray: could not create the status icon\n");
        return false;
    }

    GtkWidget *menu = gtk_menu_new();
    GtkWidget *show_item = gtk_menu_item_new_with_label("Show KillSwitch");
    GtkWidget *quit_item = gtk_menu_item_new_with_label("Close KillSwitch");

    g_signal_connect(show_item, "activate", G_CALLBACK(on_show_clicked), NULL);
    g_signal_connect(quit_item, "activate", G_CALLBACK(on_quit_clicked), NULL);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), show_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);
    gtk_widget_show_all(menu);

    app_indicator_set_menu(s_indicator, GTK_MENU(menu));
    app_indicator_set_status(s_indicator, APP_INDICATOR_STATUS_ACTIVE);

    if (tooltip && tooltip[0]) {
        app_indicator_set_title(s_indicator, tooltip);
    }

    s_ready = true;
    return true;
}

void tray_shutdown(void)
{
    if (s_indicator) {
        app_indicator_set_status(s_indicator, APP_INDICATOR_STATUS_PASSIVE);
        g_object_unref(s_indicator);
        s_indicator = NULL;
    }
    s_ready = false;
}

bool tray_available(void)
{
    return s_ready;
}

/* Nothing to remember here: X11 lets the close button through to the main
   loop as GLFW's should-close flag, so the choice is made where that is read
   rather than in a window procedure of our own. */
void tray_set_close_hides(bool hides)
{
    (void)hides;
}

void tray_minimize(void)
{
    if (!s_ready || s_hidden) {
        return;
    }

    window_hide();
    s_hidden = true;
}

void tray_restore(void)
{
    if (!s_ready || !s_hidden) {
        return;
    }

    window_show();
    s_hidden = false;
}

bool tray_is_minimized(void)
{
    return s_hidden;
}

/*
 * Notifications would mean a libnotify dependency for one line of text. The
 * interface already draws its own toast, so this stays a no-op.
 */
void tray_notify(const char *title, const char *text)
{
    (void)title;
    (void)text;
}

bool tray_poll(void)
{
    if (!s_ready) {
        return false;
    }

    /* Non-blocking: GTK shares the frame with raylib rather than owning a loop. */
    while (gtk_events_pending()) {
        gtk_main_iteration_do(FALSE);
    }

    if (s_show_requested) {
        s_show_requested = false;
        tray_restore();
    }

    if (s_quit_requested) {
        s_quit_requested = false;
        return true;
    }
    return false;
}

#else /* no tray backend */

/*
 * Built without a tray backend: no Windows shell and no AppIndicator. The
 * interface still runs, the close button quits normally, and the window icon
 * is whatever the UI set through raylib.
 */

bool tray_init(void *window_handle, const char *icon_path, const char *tooltip)
{
    (void)window_handle;
    (void)icon_path;
    (void)tooltip;
    return false;
}

void tray_shutdown(void)      {}
void tray_notify(const char *title, const char *text) { (void)title; (void)text; }
bool tray_available(void)     { return false; }
void tray_set_close_hides(bool hides) { (void)hides; }
void tray_minimize(void)      {}
void tray_restore(void)       {}
bool tray_is_minimized(void)  { return false; }
bool tray_poll(void)          { return false; }

#endif /* _WIN32 */
