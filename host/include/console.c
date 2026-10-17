#include "console.h"

#ifdef _WIN32

#include <stdio.h>
#include <windows.h>

/* Only a console this process created is ours to close. */
static bool s_allocated;
static bool s_ready;

void console_init(bool debug)
{
    if (!debug || s_ready) {
        return;
    }

    /*
     * Started from a terminal: write into that one. A GUI subsystem process
     * does not inherit it automatically, but it may still attach to it, which
     * is better than opening a second window on top.
     */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        s_allocated = false;
    } else if (AllocConsole()) {
        s_allocated = true;
        SetConsoleTitleA("KillSwitch debug console");
    } else {
        return;
    }

    /*
     * The CRT captured its handles at startup, when there was no console, so
     * printf would still write nowhere. Reopening the streams on the console
     * device repoints them.
     */
    FILE *out = freopen("CONOUT$", "w", stdout);
    FILE *err = freopen("CONOUT$", "w", stderr);
    FILE *in  = freopen("CONIN$",  "r", stdin);

    (void)out;
    (void)err;
    (void)in;

    setvbuf(stdout, NULL, _IOLBF, 0);
    s_ready = true;
}

void console_shutdown(void)
{
    if (s_allocated) {
        FreeConsole();
        s_allocated = false;
    }
    s_ready = false;
}

#else /* !_WIN32 */

/*
 * Nothing to do: the terminal a program was launched from is already its
 * stdout, and a desktop launcher starts it with Terminal=false and no
 * terminal at all.
 */
void console_init(bool debug) { (void)debug; }
void console_shutdown(void)   {}

#endif /* _WIN32 */
