#include "watchdog.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

/* How long the loop may sit in one phase before it counts as hung. */
#define WATCHDOG_TIMEOUT_MS 4000
#define WATCHDOG_POLL_MS    500

/*
 * Written by the main loop, read by the watching thread. Neither needs to be
 * exact: a torn read costs at most one extra poll, and the phase pointer is
 * always a string literal, so it is valid whatever value is observed.
 */
static const char *volatile s_phase = "starting";
static volatile unsigned long s_beat;

static char s_log_path[260];
static volatile bool s_running;
static bool s_reported;

static void report(const char *phase, unsigned seconds)
{
    FILE *f = fopen(s_log_path, "a");
    if (f == NULL) {
        return;
    }

    time_t now = time(NULL);
    char when[32];

    if (strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S",
                 localtime(&now)) == 0) {
        when[0] = '\0';
    }

    fprintf(f, "%s  HUNG for %us in phase: %s\n", when, seconds, phase);
    fclose(f);

    fprintf(stderr, "watchdog: hung for %us in phase: %s\n", seconds, phase);
}

/* Body of the watching thread, shared by both platforms. */
static void watchdog_loop(void)
{
    unsigned long last_beat = s_beat;
    unsigned stalled_ms = 0;

    while (s_running) {
#ifdef _WIN32
        Sleep(WATCHDOG_POLL_MS);
#else
        struct timespec ts = { WATCHDOG_POLL_MS / 1000,
                               (WATCHDOG_POLL_MS % 1000) * 1000000L };
        nanosleep(&ts, NULL);
#endif
        if (s_beat != last_beat) {
            last_beat = s_beat;
            stalled_ms = 0;
            s_reported = false;
            continue;
        }

        stalled_ms += WATCHDOG_POLL_MS;

        if (stalled_ms >= WATCHDOG_TIMEOUT_MS && !s_reported) {
            s_reported = true;      /* once per episode, not once per poll */
            report(s_phase, stalled_ms / 1000);
        }
    }
}

void watchdog_phase(const char *name)
{
    s_phase = name;
    s_beat++;
}

#ifdef _WIN32

static HANDLE s_thread;

static DWORD WINAPI watchdog_thread(LPVOID arg)
{
    (void)arg;
    watchdog_loop();
    return 0;
}

bool watchdog_start(const char *log_path)
{
    if (s_running) {
        return true;
    }

    snprintf(s_log_path, sizeof(s_log_path), "%s", log_path);
    s_running = true;

    s_thread = CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
    if (s_thread == NULL) {
        s_running = false;
        return false;
    }
    return true;
}

void watchdog_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;

    /* It sleeps in short slices, so this returns promptly. */
    WaitForSingleObject(s_thread, 2000);
    CloseHandle(s_thread);
    s_thread = NULL;
}

#else /* !_WIN32 */

static pthread_t s_thread;
static bool s_thread_valid;

static void *watchdog_thread(void *arg)
{
    (void)arg;
    watchdog_loop();
    return NULL;
}

bool watchdog_start(const char *log_path)
{
    if (s_running) {
        return true;
    }

    snprintf(s_log_path, sizeof(s_log_path), "%s", log_path);
    s_running = true;

    if (pthread_create(&s_thread, NULL, watchdog_thread, NULL) != 0) {
        s_running = false;
        return false;
    }
    s_thread_valid = true;
    return true;
}

void watchdog_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;

    if (s_thread_valid) {
        pthread_join(s_thread, NULL);
        s_thread_valid = false;
    }
}

#endif /* _WIN32 */
