/*
 * Clay + raylib front end for the kill switch.
 *
 * A header with the connection badge and Save/Reload, a status banner that
 * says whether the switch is engaged with the button that flips it, and two
 * tabs: "Devices" lists the HID keyboards and mice present and ticks the ones
 * the switch controls; "Cards" lists the authorised UIDs and adds the one last
 * scanned. The traffic log sits below, and only when "debug" is set.
 */

#ifndef UI_H
#define UI_H

#include "app.h"

/* Fixed window: the layout is designed for exactly this, and resizing is off.
   With debug on, the Traffic log needs more room, so the window opens taller
   (and only then becomes resizable) -- everything still fits in 800x600 when
   debug is off. */
#define UI_WINDOW_WIDTH         800
#define UI_WINDOW_HEIGHT        600
#define UI_WINDOW_HEIGHT_DEBUG  820

/* How often the bus is swept for the device while none is connected. */
#define UI_CONNECT_SCAN_SECONDS 3.0

/* How often the present HID list is refreshed while nothing forces it. */
#define UI_HID_REFRESH_SECONDS  4.0

#define UI_FALLBACK_FPS   60

/** Open the window, run the loop until closed, clean up; returns exit status. */
int ui_run(app_t *app);

#endif /* UI_H */
