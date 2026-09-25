/**
 * The title's GPU waits, bracketed so the runtime's GPU threads know when
 * someone is waiting on them (xbox_gpu_hurry, xbox_memory_layout.c).
 *
 * Xbox D3D waits for the GPU by spinning on its registers -- KickOff sets
 * the flush bit and loops until it clears, IsBusy is polled in loops, the
 * Block* functions wait on fences. The runtime's pushbuffer and register
 * threads answer those. They now sleep when nobody is waiting; these
 * wrappers wake them for exactly as long as the title is inside a wait.
 * The wrapped functions keep their own arguments and return values: each
 * wrapper only runs the original between the two calls.
 */
#include "recomp/gen/recomp_types.h"

void xbox_gpu_hurry(int delta);

#define GPU_WAIT(name)                  \
    void name##_orig(void);             \
    void name(void)                     \
    {                                   \
        xbox_gpu_hurry(1);              \
        name##_orig();                  \
        xbox_gpu_hurry(-1);             \
    }

GPU_WAIT(D3D_CDevice_KickOff_00135510)
GPU_WAIT(D3D_CDevice_ReentrantKickOffAndWait_001355D0)
GPU_WAIT(D3D_KickOffAndWaitForIdle_00135A10)
GPU_WAIT(D3DDevice_IsBusy_00138F20)
GPU_WAIT(D3D_BlockOnTime_001356F0)
GPU_WAIT(D3DDevice_BlockUntilIdle_00138570)
GPU_WAIT(D3D_BusyLoop_00135140)
