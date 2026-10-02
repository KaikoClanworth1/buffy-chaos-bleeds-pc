/**
 * Native replacements for guest CRT routines the lifter cannot translate.
 *
 * Each function here is listed in manual_functions.json, so the recompiler
 * skips its generated body and this definition links in its place. Guest
 * calling convention: on entry [esp] is the guest return address and cdecl
 * arguments follow it; returning pops the return address.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "recomp/gen/recomp_types.h"

/* memmove(dst, src, n) -- 0x0012EBD0.
 *
 * MSVC's hand-written memmove dispatches its tail copies through
 * `jmp [ecx*4 + TrailDownVec_end]` with a negative index, which the lifter
 * cannot recognise as a local switch; at runtime it becomes an indirect jump
 * into the middle of the function and fails. */
void memmove_0012EBD0(void)
{
    uint32_t dst = MEM32(g_esp + 4);
    uint32_t src = MEM32(g_esp + 8);
    uint32_t n   = MEM32(g_esp + 12);

    if (n)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), n);
    g_eax = dst;
    g_esp += 4;
}

/* xbMovieIsPlaying is the game's own again: movies play through the XMV
 * decoder replacement in buffy_movie.c. */

/* DWORD GetTickCount(void) -- 0x001289F7, XAPI.
 *
 * Reads the kernel's KeTickCount, which on the console the timer interrupt
 * advances. Nothing here is an interrupt: the milliseconds come from Windows
 * directly, the same unit and origin the kernel's copy was kept at. */
#if defined(_WIN32)
__declspec(dllimport) unsigned long __stdcall GetTickCount(void);
#else
#include <windows.h>                        /* (xboxrecomp's POSIX shim) */
#endif

void GetTickCount_001289F7(void)
{
    g_eax = (uint32_t)GetTickCount();
    g_esp += 4;
}

/* DWORD XGetLanguage(void) -- 0x00128A36, XAPI: the dashboard's language,
 * which picks the game's text. Here the launcher's Language setting
 * (buffy_settings.c). */
int buffy_settings_language_code(void);

void XGetLanguage_00128A36(void)
{
    g_eax = (uint32_t)buffy_settings_language_code();
    g_esp += 4;
}
