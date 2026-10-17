/*
 * Finding out where the interface stopped responding.
 *
 * The main loop marks which phase it is in. A separate thread watches that
 * marker, and if it has not moved for a few seconds the loop is stuck: the
 * name of the last phase entered is the call that blocked, and it is written
 * to a file where it survives the process being killed.
 *
 * This exists because a hang cannot report itself: whatever blocked the loop
 * also blocked any code that might have logged it.
 */

#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdbool.h>

/**
 * Start watching.
 *
 * @param log_path Where a hang is recorded, appended to. Relative paths land
 *                 beside config.json, since both use the working directory.
 * @return false if the watching thread could not be started, after which the
 *         phase calls are harmless no-ops.
 */
bool watchdog_start(const char *log_path);

/**
 * Mark the phase the loop is entering.
 *
 * Cheap by design: one pointer store and one counter bump, so it can sit
 * between every step of the frame.
 *
 * @param name Static string, used after the fact by the watching thread.
 */
void watchdog_phase(const char *name);

void watchdog_stop(void);

#endif /* WATCHDOG_H */
