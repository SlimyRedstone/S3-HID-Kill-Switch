/*
 * The console window.
 *
 * On Windows the program is linked as a GUI application, so it starts with no
 * console at all: that is what stops a black window appearing behind it when
 * it is launched from Explorer, the Start menu or the tray. stdout and stderr
 * then go nowhere until one is asked for, which is what "debug" in config.json
 * does.
 *
 * Everywhere else this is a no-op. A program started from a terminal keeps
 * that terminal, and one started from a desktop launcher never had one.
 *
 * Not thread safe; call from one thread.
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdbool.h>

/**
 * Give the program somewhere to print, if it was asked for.
 *
 * @param debug When true, write into the terminal the program was launched
 *              from, or open a console window when there was none. When false
 *              nothing happens and output is discarded.
 */
void console_init(bool debug);

/** Close a console opened by console_init(). Safe to call unconditionally. */
void console_shutdown(void);

#endif /* CONSOLE_H */
