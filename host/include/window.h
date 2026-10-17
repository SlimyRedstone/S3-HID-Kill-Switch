/*
 * Showing and hiding the window.
 *
 * raylib.h and windows.h cannot share a translation unit -- windows.h defines
 * Rectangle, CloseWindow and ShowCursor -- so tray.c and instance.c cannot
 * call raylib directly. This is the one file that does, and it exposes the
 * three operations they need.
 *
 * Declaring GLFW's entry points by hand was the previous approach. It linked
 * wherever raylib re-exported them, and failed against a raylib built from
 * source on Linux, which does not.
 */

#ifndef WINDOW_H
#define WINDOW_H

#include <stdbool.h>

/** Take the window off the screen, leaving the process running. */
void window_hide(void);

/** Put it back and give it the keyboard focus. */
void window_show(void);

#endif /* WINDOW_H */
