#include "window.h"

#include "raylib.h"

void window_hide(void)
{
    SetWindowState(FLAG_WINDOW_HIDDEN);
}

void window_show(void)
{
    ClearWindowState(FLAG_WINDOW_HIDDEN);

    /* Un-hiding does not raise it, so ask for the focus separately. */
    SetWindowFocused();
}
