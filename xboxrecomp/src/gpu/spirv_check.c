/*
 * spirv_check - compiles HLSL files the renderer dumped (BUFFY_SHADER_DUMP)
 * to SPIR-V with the Vulkan backend's compiler, and reports each.
 *
 *   spirv_check <file.hlsl>...   (names end _vs_vs_4_0.hlsl or _ps_ps_4_0.hlsl)
 *   exit code: the number that failed
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_spirv.h"

int main(int argc, char **argv)
{
    int i, bad = 0, ok = 0;
    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        char *src, err[8192];
        long n;
        uint32_t *words;
        size_t count;
        int stage = strstr(argv[i], "_ps_") ? GPU_STAGE_PIXEL : GPU_STAGE_VERTEX;
        if (!f) {
            printf("FAIL %s: cannot open\n", argv[i]);
            bad++;
            continue;
        }
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        src = (char *)malloc((size_t)n + 1);
        fread(src, 1, (size_t)n, f);
        src[n] = 0;
        fclose(f);
        if (gpu_hlsl_to_spirv(src, stage == GPU_STAGE_PIXEL ? "ps" : "vs", stage, &words, &count, err, sizeof err)) {
            ok++;
            free(words);
        } else {
            printf("FAIL %s:\n%s\n", argv[i], err);
            bad++;
        }
        free(src);
    }
    printf("%d compiled, %d failed\n", ok, bad);
    return bad;
}
