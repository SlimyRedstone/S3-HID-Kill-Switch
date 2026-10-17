#include "inputblock.h"

#ifdef _WIN32

#include <windows.h>
#include <stdio.h>

/*
 * The hooks live on their own thread with a message loop, because the system
 * delivers low-level hook callbacks through the installing thread's message
 * queue and drops a hook whose thread does not service it promptly. Keeping the
 * hooks installed for the whole run and toggling a flag avoids install/remove
 * races; the callback only checks the flag and, when set, returns 1 to swallow
 * the event.
 */
static volatile LONG s_block_kb;
static volatile LONG s_block_mouse;
static volatile LONG s_allow_virtual = 1;   /* let injected (virtual) input through */
static volatile LONG s_debug;               /* log swallowed-event flags to stderr */

static HHOOK  s_kb_hook;
static HHOOK  s_mouse_hook;
static HANDLE s_thread;
static DWORD  s_thread_id;

/*
 * Distinguishing physical input from virtual (software-synthesised) input.
 *
 * SendInput -- used by Unified Remote, AutoHotkey, the on-screen keyboard and
 * the like -- sets the injected flag. When the injecting process runs at a
 * LOWER integrity level than this one (this app is elevated; Unified Remote's
 * server is an ordinary medium-IL process), the event gets
 * LLKHF_LOWER_IL_INJECTED / LLMHF_LOWER_IL_INJECTED and the plain INJECTED bit
 * is NOT guaranteed, so both bits must be checked or the virtual input looks
 * physical and gets swallowed.
 */
#define KB_INJECTED_MASK    (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)
#define MOUSE_INJECTED_MASK (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)

/* Log a flags value once per distinct value per kind, so a stream of events
 * does not flood the debug console; only active while debug is on. */
static void debug_flags(const char *kind, DWORD flags, bool injected, bool pass)
{
    static DWORD last_kb = 0xFFFFFFFF, last_mouse = 0xFFFFFFFF;
    if (!InterlockedCompareExchange(&s_debug, 0, 0)) {
        return;
    }
    DWORD *last = (kind[0] == 'k') ? &last_kb : &last_mouse;
    if (flags == *last) {
        return;
    }
    *last = flags;
    fprintf(stderr, "[inputblock] %s flags=0x%02lX injected=%d -> %s\n",
            kind, (unsigned long)flags, injected ? 1 : 0, pass ? "pass" : "block");
    fflush(stderr);
}

static LRESULT CALLBACK kb_proc(int code, WPARAM wparam, LPARAM lparam)
{
    if (code == HC_ACTION && InterlockedCompareExchange(&s_block_kb, 0, 0)) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lparam;
        bool injected = (k->flags & KB_INJECTED_MASK) != 0;
        bool pass = injected && InterlockedCompareExchange(&s_allow_virtual, 0, 0);
        debug_flags("kb", k->flags, injected, pass);
        if (!pass) {
            return 1;                   /* consume: physical input is dropped */
        }
    }
    return CallNextHookEx(NULL, code, wparam, lparam);
}

static LRESULT CALLBACK mouse_proc(int code, WPARAM wparam, LPARAM lparam)
{
    if (code == HC_ACTION && InterlockedCompareExchange(&s_block_mouse, 0, 0)) {
        const MSLLHOOKSTRUCT *m = (const MSLLHOOKSTRUCT *)lparam;
        bool injected = (m->flags & MOUSE_INJECTED_MASK) != 0;
        bool pass = injected && InterlockedCompareExchange(&s_allow_virtual, 0, 0);
        debug_flags("mouse", m->flags, injected, pass);
        if (!pass) {
            return 1;
        }
    }
    return CallNextHookEx(NULL, code, wparam, lparam);
}

static DWORD WINAPI thread_main(LPVOID arg)
{
    (void)arg;
    HINSTANCE hinst = GetModuleHandleW(NULL);

    s_kb_hook    = SetWindowsHookExW(WH_KEYBOARD_LL, kb_proc, hinst, 0);
    s_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, hinst, 0);

    /* The hooks are serviced inside GetMessage; the loop body does nothing. */
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        /* no dispatch needed for low-level hooks */
    }

    if (s_kb_hook) {
        UnhookWindowsHookEx(s_kb_hook);
        s_kb_hook = NULL;
    }
    if (s_mouse_hook) {
        UnhookWindowsHookEx(s_mouse_hook);
        s_mouse_hook = NULL;
    }
    return 0;
}

void inputblock_init(void)
{
    if (s_thread) {
        return;
    }
    s_thread = CreateThread(NULL, 0, thread_main, NULL, 0, &s_thread_id);
}

bool inputblock_available(void)
{
    return true;
}

void inputblock_set(bool keyboard, bool mouse)
{
    InterlockedExchange(&s_block_kb, keyboard ? 1 : 0);
    InterlockedExchange(&s_block_mouse, mouse ? 1 : 0);
}

void inputblock_set_allow_virtual(bool allow)
{
    InterlockedExchange(&s_allow_virtual, allow ? 1 : 0);
}

void inputblock_set_debug(bool on)
{
    InterlockedExchange(&s_debug, on ? 1 : 0);
}

void inputblock_shutdown(void)
{
    inputblock_set(false, false);
    if (s_thread) {
        PostThreadMessageW(s_thread_id, WM_QUIT, 0, 0);
        WaitForSingleObject(s_thread, 1000);
        CloseHandle(s_thread);
        s_thread = NULL;
    }
}

#else /* !_WIN32 */

void inputblock_init(void)                      {}
bool inputblock_available(void)                 { return false; }
void inputblock_set(bool keyboard, bool mouse)  { (void)keyboard; (void)mouse; }
void inputblock_set_allow_virtual(bool allow)   { (void)allow; }
void inputblock_set_debug(bool on)              { (void)on; }
void inputblock_shutdown(void)                  {}

#endif /* _WIN32 */
