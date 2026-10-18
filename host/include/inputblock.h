/*
 * Blocking keyboard and/or mouse input at the OS level.
 *
 * Disabling a device node (hidctl) is the clean way, but Windows marks many
 * keyboards -- built-in laptop ones especially -- non-disableable, and nothing
 * overrides that. This blocks the *input* instead, with low-level hooks
 * (WH_KEYBOARD_LL / WH_MOUSE_LL) that swallow events while engaged. It works on
 * any keyboard or mouse whatever the hardware, flips instantly, and is removed
 * by Windows the moment this process exits -- so a crash can never leave the
 * machine locked out.
 *
 * The block is global, not per device: it stops all keyboard input, all mouse
 * input, or both. With "allow virtual" on (the default), only *physical* input
 * is dropped -- software-synthesised events (SendInput, used by Unified Remote,
 * AutoHotkey, the on-screen keyboard...) carry an injected flag and pass
 * through, so a virtual keyboard/mouse keeps working while its physical
 * counterpart is blocked. With it off, everything is blocked. The secure
 * desktop (Ctrl+Alt+Del, UAC, the lock screen) is always exempt, which is the
 * escape hatch of last resort.
 *
 * Elsewhere this is a stub: inputblock_available() is false and nothing is
 * blocked.
 */

#ifndef INPUTBLOCK_H
#define INPUTBLOCK_H

#include <stdbool.h>

/** Start the hook thread. Safe to call once; a second call is a no-op. */
void inputblock_init(void);

/** True where input blocking is implemented (Windows only). */
bool inputblock_available(void);

/**
 * Block or release keyboard and mouse input independently.
 *
 * @param keyboard swallow all keyboard input while true.
 * @param mouse    swallow all mouse input while true.
 */
void inputblock_set(bool keyboard, bool mouse);

/**
 * Whether injected (software-synthesised) input passes through while blocked.
 *
 * @param allow true  -> swallow only physical input; virtual input (SendInput:
 *                       Unified Remote, AutoHotkey, on-screen keyboard) works.
 *              false -> swallow everything, physical and virtual alike.
 *
 * Takes effect immediately; defaults to true before the first call.
 */
void inputblock_set_allow_virtual(bool allow);

/**
 * Log each swallowed event's raw flags to stderr (debug console), once per
 * distinct value, so the injected bit on virtual input can be inspected.
 * Off unless enabled.
 */
void inputblock_set_debug(bool on);

/**
 * Raw flags of the most recent event each hook saw while blocking was engaged,
 * for surfacing physical-vs-injected input. Each is set to 0xFFFFFFFF if no
 * event has been seen yet. Either pointer may be NULL.
 */
void inputblock_last_flags(unsigned *kb_flags, unsigned *mouse_flags);

/** Release everything and stop the hook thread. */
void inputblock_shutdown(void);

#endif /* INPUTBLOCK_H */
