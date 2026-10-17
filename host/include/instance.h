/*
 * One running copy at a time.
 *
 * The first process to start owns the instance; every later launch hands its
 * request to that owner and exits, so clicking the launcher twice raises the
 * window already running instead of opening a second one.
 *
 * Windows uses a named event, which the kernel scopes to the login session.
 * Elsewhere a Unix socket in the runtime directory serves as both the lock and
 * the channel: binding it succeeds for exactly one process.
 *
 * Not thread safe; call from one thread.
 */

#ifndef INSTANCE_H
#define INSTANCE_H

#include <stdbool.h>

/**
 * Claim the single-instance slot.
 *
 * @return true when this process is the owner and should carry on starting.
 *         false when another copy is already running, in which case it has
 *         been asked to come forward and this process should exit quietly.
 */
bool instance_acquire(void);

/**
 * Whether another launch has asked this process to show itself since the last
 * call. Poll once per frame.
 */
bool instance_show_requested(void);

/**
 * Bring this process's window to the front.
 *
 * @param window_handle raylib's GetWindowHandle(), which is NULL on Linux;
 *                      the window is recovered from GLFW there instead.
 */
void instance_raise(void *window_handle);

void instance_release(void);

#endif /* INSTANCE_H */
