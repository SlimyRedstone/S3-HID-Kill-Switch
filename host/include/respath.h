/*
 * Finding the files that ship with the program.
 *
 * Everything used to be opened as "resources/<name>", which only works when the
 * working directory happens to be the source tree. A desktop launcher starts
 * the program in the user's home directory instead, so the icon and the fonts
 * silently went missing. Paths are therefore resolved against the executable's
 * own location, with the installed layout and the old relative form as
 * fallbacks.
 *
 * Not thread safe; call from one thread.
 */

#ifndef RESPATH_H
#define RESPATH_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Locate a file that ships with the program.
 *
 * Search order: $KILLSWITCH_RESOURCES, beside the executable, one level up (the
 * build directory's sibling), the installed share directory, and finally
 * "resources/<name>" relative to the working directory.
 *
 * @param name Name inside the resources directory, e.g. "icon.png".
 * @param out  Receives the first candidate that exists, or "" when none does.
 * @param cap  Size of @p out.
 * @return false when the file was not found anywhere.
 */
bool respath_find(const char *name, char *out, size_t cap);

/**
 * @param out Receives the directory holding the running executable, or "".
 * @param cap Size of @p out.
 */
bool respath_program_dir(char *out, size_t cap);

#endif /* RESPATH_H */
