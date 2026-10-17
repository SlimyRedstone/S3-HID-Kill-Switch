#include "instance.h"

#include <stdio.h>

#include "window.h"

#ifdef _WIN32

#include <windows.h>

/* "Local\" scopes the name to the login session, so two users may each run a
   copy without colliding. */
#define INSTANCE_EVENT "Local\\KillSwitchShowWindow"

static HANDLE s_event;

bool instance_acquire(void)
{
    s_event = CreateEventA(NULL, FALSE, FALSE, INSTANCE_EVENT);
    if (s_event == NULL) {
        return true;        /* cannot arbitrate, so do not block startup */
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* Somebody else owns it: ask them to surface, then step aside. */
        SetEvent(s_event);
        CloseHandle(s_event);
        s_event = NULL;
        return false;
    }
    return true;
}

bool instance_show_requested(void)
{
    if (s_event == NULL) {
        return false;
    }
    return WaitForSingleObject(s_event, 0) == WAIT_OBJECT_0;
}

void instance_raise(void *window_handle)
{
    HWND hwnd = (HWND)window_handle;
    if (hwnd == NULL) {
        return;
    }

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else {
        ShowWindow(hwnd, SW_SHOW);
    }

    /*
     * A process that does not own the foreground cannot simply take it.
     * Briefly sharing the input queue of whichever window does own it lifts
     * that restriction for this call.
     */
    DWORD self = GetCurrentThreadId();
    DWORD owner = GetWindowThreadProcessId(GetForegroundWindow(), NULL);

    if (owner != 0 && owner != self) {
        AttachThreadInput(self, owner, TRUE);
        SetForegroundWindow(hwnd);
        AttachThreadInput(self, owner, FALSE);
    } else {
        SetForegroundWindow(hwnd);
    }
    SetActiveWindow(hwnd);
}

void instance_release(void)
{
    if (s_event) {
        CloseHandle(s_event);
        s_event = NULL;
    }
}

#else /* !_WIN32 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static int s_listener = -1;
static char s_path[108];

/* Runtime directory if there is one, otherwise the home directory. */
static void socket_path(char *out, size_t cap)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");

    if (dir == NULL || dir[0] == 0) {
        dir = getenv("HOME");
    }
    if (dir == NULL || dir[0] == 0) {
        dir = "/tmp";
    }
    snprintf(out, cap, "%s/.KillSwitch.sock", dir);
}

/* accept() is called every frame, so it must never wait for a caller. */
static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static void fill_address(struct sockaddr_un *addr, const char *path)
{
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    snprintf(addr->sun_path, sizeof(addr->sun_path), "%s", path);
}

bool instance_acquire(void)
{
    socket_path(s_path, sizeof(s_path));

    struct sockaddr_un addr;
    fill_address(&addr, s_path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return true;        /* cannot arbitrate, so do not block startup */
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        listen(fd, 4);
        set_nonblocking(fd);
        s_listener = fd;
        return true;
    }

    if (errno != EADDRINUSE) {
        close(fd);
        return true;
    }

    /*
     * The name exists. If nothing is listening it is a leftover from a copy
     * that was killed, so it is removed and the bind retried; otherwise the
     * connection itself is the request to come forward.
     */
    close(fd);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return true;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        close(fd);
        return false;
    }
    close(fd);

    unlink(s_path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return true;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        listen(fd, 4);
        set_nonblocking(fd);
        s_listener = fd;
    } else {
        close(fd);
    }
    return true;
}

bool instance_show_requested(void)
{
    if (s_listener < 0) {
        return false;
    }

    bool asked = false;

    /* Drain every pending connection: each one is one launch. */
    for (;;) {
        int fd = accept(s_listener, NULL, NULL);
        if (fd < 0) {
            break;          /* EAGAIN once the backlog is empty */
        }
        close(fd);
        asked = true;
    }
    return asked;
}

void instance_raise(void *window_handle)
{
    (void)window_handle;    /* NULL on Linux */

    /* A window manager may still refuse to raise a window that did not come
       from the user's own interaction; there is nothing more to be done. */
    window_show();
}

void instance_release(void)
{
    if (s_listener >= 0) {
        close(s_listener);
        s_listener = -1;
        unlink(s_path);
    }
}

#endif /* _WIN32 */
