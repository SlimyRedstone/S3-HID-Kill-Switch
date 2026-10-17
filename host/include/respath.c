#include "respath.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Strip the last path component in place, leaving the directory. */
static void keep_directory(char *path)
{
    char *cut = NULL;

    for (char *p = path; *p; p++) {
        if (*p == '/' || *p == 0x5C) {   /* forward slash or backslash */
            cut = p;
        }
    }
    if (cut) {
        *cut = 0;
    }
}

bool respath_program_dir(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return false;
    }
    out[0] = 0;

#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap) {
        out[0] = 0;
        return false;
    }
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) {
        out[0] = 0;
        return false;
    }
    out[n] = 0;
#endif

    keep_directory(out);
    return out[0] != 0;
}

static bool exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fclose(f);
    return true;
}

/* Join the pieces into @p out and report whether that file is there. */
static bool candidate(char *out, size_t cap, const char *dir,
                      const char *sub, const char *name)
{
    if (sub != NULL && sub[0] != 0) {
        snprintf(out, cap, "%s/%s/%s", dir, sub, name);
    } else {
        snprintf(out, cap, "%s/%s", dir, name);
    }
    return exists(out);
}

bool respath_find(const char *name, char *out, size_t cap)
{
    if (name == NULL || out == NULL || cap == 0) {
        return false;
    }
    out[0] = 0;

    const char *env = getenv("KILLSWITCH_RESOURCES");
    if (env != NULL && env[0] != 0 && candidate(out, cap, env, NULL, name)) {
        return true;
    }

    char dir[512];
    if (respath_program_dir(dir, sizeof(dir))) {
        /* Beside the executable when installed or copied, then one level up,
           which is where the build directory finds the source tree's copy. */
        if (candidate(out, cap, dir, "resources", name)) {
            return true;
        }
        if (candidate(out, cap, dir, "../resources", name)) {
            return true;
        }
        if (candidate(out, cap, dir, "../share/KillSwitch", name)) {
            return true;
        }
    }

#ifndef _WIN32
    if (candidate(out, cap, "/usr/local/share/KillSwitch", NULL, name)) {
        return true;
    }
    if (candidate(out, cap, "/usr/share/KillSwitch", NULL, name)) {
        return true;
    }
#endif

    /* How build.sh and build.bat launch it: from the directory above build/. */
    snprintf(out, cap, "resources/%s", name);
    if (exists(out)) {
        return true;
    }

    out[0] = 0;
    return false;
}
