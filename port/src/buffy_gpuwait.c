/**
 * The Xbox D3D library without an Xbox GPU.
 *
 * The game draws through the native renderer (buffy_native.c); nothing
 * emulates the NV2A. The D3D library still runs -- it keeps the device
 * state the native renderer reads -- in its own no-GPU mode,
 * D3D__NullHardware (set at boot, main.c). These three functions finish
 * what that mode leaves to a GPU: its time (fences) and its vertical blank.
 * Every other wait (IsBusy, BlockUntilIdle, the kick-off-and-wait calls)
 * returns at once in that mode, or against the GPU registers, which read
 * idle (xbox_memory_layout.c, xbox_Nv2aAckStart).
 */
#include "recomp/gen/recomp_types.h"

#define D3D_NULL_HARDWARE 0x145030u      /* D3D__NullHardware */
#define D3D_DEVICE_PTR    0x1454D8u      /* g_pDevice */

/* No GPU, so the GPU is never behind: its time (the dword at device +0x30
 * points to, which a GPU would write at each fence) is D3D's own time
 * (device +0x2C). D3D's null path leaves it two fences short, and a wait
 * on those would wait for good. */
static void null_hw_catch_up(void)
{
    uint32_t dev = MEM32(D3D_DEVICE_PTR), t;
    if (MEM32(D3D_NULL_HARDWARE) && dev && (t = MEM32(dev + 0x30)) != 0)
        MEM32(t) = MEM32(dev + 0x2C);
}

/* KickOff (thiscall, the device): Xbox D3D takes its no-GPU path once the
 * device carries flag 0x2000 -- normally set after one real kickoff has
 * waited for the GPU to go idle. Set here before the first one, so not even
 * that one talks to the GPU: the pushbuffer is retired at once (GET = PUT in
 * D3D's own register block, g_NullHardwareGetPut). */
void D3D_CDevice_KickOff_00135510_orig(void);
void D3D_CDevice_KickOff_00135510(void)
{
    uint32_t dev = g_ecx;
    if (MEM32(D3D_NULL_HARDWARE) && dev && !(MEM32(dev + 8) & 0x2000u))
        MEM32(dev + 8) |= 0x2000u;
    D3D_CDevice_KickOff_00135510_orig();
    null_hw_catch_up();
}

/* void D3D::BlockOnTime(DWORD time, BOOL ...): returns at once when the GPU
 * time has reached `time` -- with no GPU, it always has. */
void D3D_BlockOnTime_001356F0_orig(void);
void D3D_BlockOnTime_001356F0(void)
{
    null_hw_catch_up();
    D3D_BlockOnTime_001356F0_orig();
}

/* void D3DDevice_BlockUntilVerticalBlank(void): waits for the display's next
 * vertical blank. There is no Xbox display; frames are paced at Swap
 * (buffy_frame.c), so it returns at once. */
void D3DDevice_BlockUntilVerticalBlank_00138580_orig(void);
void D3DDevice_BlockUntilVerticalBlank_00138580(void)
{
    if (MEM32(D3D_NULL_HARDWARE)) {
        g_esp += 4;                                      /* ret */
        return;
    }
    D3DDevice_BlockUntilVerticalBlank_00138580_orig();
}
