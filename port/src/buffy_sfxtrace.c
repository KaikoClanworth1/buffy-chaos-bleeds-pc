#include <stdio.h>
#include <stdlib.h>
#include "recomp/gen/recomp_types.h"

static int sfx_log(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("BUFFY_SFX_LOG") != NULL;
    return v;
}

/* ── sound tracing (BUFFY_SFX_LOG): the SFX calls around music and streams ── */

void SFXPauseMusic_000DF220_orig(void);
void SFXPauseMusic_000DF220(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] SFXPauseMusic(%08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp));
    SFXPauseMusic_000DF220_orig();
}

void SFXUnPauseMusic_000DF280_orig(void);
void SFXUnPauseMusic_000DF280(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] SFXUnPauseMusic(%08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp));
    SFXUnPauseMusic_000DF280_orig();
}

void SFXMuteMusic_000DE250_orig(void);
void SFXMuteMusic_000DE250(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] SFXMuteMusic(%08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp));
    SFXMuteMusic_000DE250_orig();
}

void SFXUnMuteMusic_000DE280_orig(void);
void SFXUnMuteMusic_000DE280(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] SFXUnMuteMusic(%08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp));
    SFXUnMuteMusic_000DE280_orig();
}

void SFXStopAllStreams_000E2780_orig(void);
void SFXStopAllStreams_000E2780(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] SFXStopAllStreams(%08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp));
    SFXStopAllStreams_000E2780_orig();
}

void SFXKillStreamed_000E0340_orig(void);
void SFXKillStreamed_000E0340(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] SFXKillStreamed(%08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp));
    SFXKillStreamed_000E0340_orig();
}

void SFXKillStreamedNonLooping_000E02F0_orig(void);
void SFXKillStreamedNonLooping_000E02F0(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] SFXKillStreamedNonLooping(%08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp));
    SFXKillStreamedNonLooping_000E02F0_orig();
}

void SFXFadeDown_000DDF80_orig(void);
void SFXFadeDown_000DDF80(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] SFXFadeDown(%08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp));
    SFXFadeDown_000DDF80_orig();
}

void SFXFadeUp_000DDFC0_orig(void);
void SFXFadeUp_000DDFC0(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] SFXFadeUp(%08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp));
    SFXFadeUp_000DDFC0_orig();
}

void SFXStopStream_000E1120_orig(void);
void SFXStopStream_000E1120(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] SFXStopStream(%08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp));
    SFXStopStream_000E1120_orig();
}

void SFXMuteAll_000DE270_orig(void);
void SFXMuteAll_000DE270(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] SFXMuteAll(%08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp));
    SFXMuteAll_000DE270_orig();
}

void SFXUnMuteAll_000DE2A0_orig(void);
void SFXUnMuteAll_000DE2A0(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] SFXUnMuteAll(%08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp));
    SFXUnMuteAll_000DE2A0_orig();
}

void SFXPause_000E21A0_orig(void);
void SFXPause_000E21A0(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] SFXPause(%08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp));
    SFXPause_000E21A0_orig();
}

void SFXUnPause_000E2230_orig(void);
void SFXUnPause_000E2230(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] SFXUnPause(%08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp));
    SFXUnPause_000E2230_orig();
}

void SFXMusicSetVolume_000DF3C0_orig(void);
void SFXMusicSetVolume_000DF3C0(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] SFXMusicSetVolume(%08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp));
    SFXMusicSetVolume_000DF3C0_orig();
}

void SFXStart_000E2C80_orig(void);
void SFXStart_000E2C80(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] SFXStart(%08X, %08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12), MEM32(g_esp));
    SFXStart_000E2C80_orig();
}

void SFXStart3D_000E2460_orig(void);
void SFXStart3D_000E2460(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] SFXStart3D(%08X, %08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12), MEM32(g_esp));
    SFXStart3D_000E2460_orig();
}

void ES_InitialiseStream_000E1440_orig(void);
void ES_InitialiseStream_000E1440(void)
{
    if (sfx_log())
        fprintf(stderr, "[SFX] ES_InitialiseStream(%08X, %08X, %08X) from %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12), MEM32(g_esp));
    ES_InitialiseStream_000E1440_orig();
}
