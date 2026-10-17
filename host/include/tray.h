/*
 * Window icon and system tray.
 *
 * Implemented against the platform shell, so this header deliberately exposes
 * no windows.h and no raylib types: those two cannot share a translation unit,
 * because windows.h defines Rectangle, CloseWindow and ShowCursor.
 *
 * The window handle crosses the boundary as an opaque pointer, which is what
 * raylib's GetWindowHandle() returns.
 *
 * Windows uses the shell notification area. Linux uses Ayatana AppIndicator,
 * which speaks StatusNotifierItem over D-Bus, the protocol Cinnamon, KDE and
 * the GNOME extensions all listen on. Without either, every call is a no-op
 * and tray_available() reports false.
 */

#ifndef TRAY_H
#define TRAY_H

#include <stdbool.h>

/**
 * Attach to the window and load @p icon_path as its title-bar and taskbar icon.
 *
 * @param window_handle Native handle, from raylib's GetWindowHandle().
 * @param icon_path     .ico file, already resolved to a full path by the
 *                      caller; an empty string just leaves the default icon.
 * @param tooltip       Text shown when hovering the tray icon.
 * @return false if the platform has no tray support, or setup failed.
 */
bool tray_init(void *window_handle, const char *icon_path, const char *tooltip);

void tray_shutdown(void);

bool tray_available(void);

/**
 * Whether the close button hides to the tray instead of ending the program.
 *
 * Handed down rather than read where the loop sees it, because on Windows the
 * loop never sees it: WM_CLOSE is taken in tray.c's own window procedure
 * before GLFW is given the chance to set its close flag.
 *
 * @param hides false makes the close button quit, as it would without a tray.
 */
void tray_set_close_hides(bool hides);

void tray_minimize(void);

void tray_restore(void);

bool tray_is_minimized(void);

/**
 * Show a balloon notification from the tray icon.
 *
 * Only meaningful while the window is hidden; ignored otherwise, and on
 * platforms without tray support.
 */
void tray_notify(const char *title, const char *text);

/**
 * Service the tray's message queue. Call once per frame.
 *
 * @return true if the user asked to quit from the tray menu.
 */
bool tray_poll(void);

#endif /* TRAY_H */
