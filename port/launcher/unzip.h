/* Buffy the Vampire Slayer: Chaos Bleeds PC launcher - zip extraction without tar.exe
 * (which Wine / Proton lacks). */

#pragma once

#include <wchar.h>

/* Extracts every file of the zip `zip` into the existing folder `to` (store
 * and deflate; no zip64 or encryption). 1 on success. */
int unzip_file(const wchar_t *zip, const wchar_t *to);
