# Native renderer

**Goal:** draw the game with Direct3D 11 from its Direct3D 8 calls, instead of emulating the Xbox GPU (NV2A) from the command stream. The emulated renderer stays in as a fallback and as the reference to compare against.

## Status

Phases 1 to 5 are done. **Native is the default renderer.** The emulated one is still selectable: the launcher's **Renderer** setting, `[Display] Renderer=emulated` or `BUFFY_RENDERER=emulated`. It is also used automatically when no Direct3D 11 device comes up.

A/B runs match the emulated renderer for:

- the legal and title screens, all menus, text and the save dialog;
- the intro movies and in-engine cutscenes;
- Magic Box gameplay, including the reflective floor (render-to-texture), HUD, hints and fire particles;
- the pause menu and its co-op pages;
- multiplayer;
- story co-op in two windows and in split screen.

Uncapped, native runs at about 700 to 750 fps where emulated manages 150 to 240. In play the game stays at 60 fps (see below).

The code:

- `xboxrecomp/src/kernel/nv2a_native.inc` is the draw backend. It handles vertex programs to HLSL (with packed-normal inputs), fixed-function and register-combiner pixel shaders, state objects, GPU vertex buffers cached by address and content, and textures through the emulated renderer's texture cache.
- `port/src/buffy_native.c` is the game side. It has the renderer switch, the hooks on the draw, constant, texture, viewport, render-target and push-buffer calls, render-to-texture targets and present.

Depth matches the NV2A. 3D draws ask for W-buffering (`D3DZB_USEW`), so depth is W mapped through D3D's clip range, the same as in the emulated renderer. The range is read from the pushbuffer state and kept from when it last showed W mode (`BUFFY_NATIVE_NOW=1` turns W off, for comparison).

The native renderer reads vertices with its own fetch. The pushbuffer executor's `fetch_attr` depends on that thread's batch state, and calling it from the game thread raced with inline-array batches. Vertices then fell back to (0,0,0,1): the main menu's transparent fade quad flashed dark, and meshes stretched across the screen. `BUFFY_FLASHTRACE=1` keeps the per-draw trace, detects one-frame flashes and dumps the flashing frame beside the one before it. `BUFFY_FLASH_SHOTS=<folder>` also saves both frames as images.

Not done yet: the phase 6 extras.

Debug switches for native: `BUFFY_NATIVE_WLOG` (depth modes seen), `BUFFY_NATIVE_VBCHECK` (vertex data changed after its per-frame check), `BUFFY_FLIPLOG` (where each frame is shown), plus the `BUFFY_NATIVE_*` switches in `nv2a_native.inc`.

## Today: the emulated renderer

```
game code ─► Xbox D3D8 library (translated) ─► NV2A pushbuffer ─► nv2a_pb_exec / nv2a_pb_d3d11.inc ─► D3D11
                                                 (GPU commands)     (decodes the commands again)
```

## Target: the native renderer

```
game code ─► Xbox D3D8 library (translated) ──► pushbuffer (bookkeeping only: fences, no drawing)
                 │
                 └── draw calls hooked ─► native backend: reads the D3D state ─► D3D11
```

This is a *draw-time state capture* design. The Xbox D3D8 library keeps running as it does today, so resource headers, fences, Lock/Unlock and device internals all stay correct. Only drawing moves:

- The draw functions are wrapped.
- At each draw, the native backend reads the current state straight from the library's own globals and issues a D3D11 draw.
- The pushbuffer executor keeps servicing fences and semaphores, but skips its draws and flips.

The state it reads, with addresses from the game's linker map, relocated:

| Global | Guest VA | Contents |
|---|---|---|
| `D3D__TextureState` | `0x145040` | 4 stages × texture stage states |
| `D3D__RenderState` | `0x145240` | render states, including the register-combiner (pixel shader) registers |
| `D3D__DirtyFlags` | `0x145038` | what changed since the last draw |
| `D3D__pDevice` | `0x1454D8` | the device: bound textures (`+0xB68`), streams, vertex shader, render targets |

## The draw paths to cover

These come from the call inventory of the translated code.

| Path | Callers | Used for |
|---|---|---|
| `D3DDevice_DrawIndexedVertices` | `EXGeoEntity_DrawDirectX` | all level and character geometry (vertex shaders) |
| `D3DDevice_DrawVerticesUP` | 15 engine routines | 2D, HUD, fonts, lines, particles, swooshes |
| `D3DDevice_Begin` / `SetVertexData*` / `End` | a few | immediate-mode quads |
| `D3DDevice_BeginPush` / `EndPush` | `EXParticleSys_DrawQuadParticles` | particles written as raw commands |
| `D3DDevice_Clear`, `SetRenderTarget`, `Swap` | | frame structure, render-to-texture |

## Phases

The plan as it was followed:

1. **Foundation.**
   - A renderer switch (`[Display] Renderer = emulated | native`).
   - Hooks on the draw calls.
   - State capture and a per-frame state log, to check that the capture agrees with what the emulated renderer draws.
2. **2D first.** In native mode:
   - `DrawVerticesUP` with fixed-function and pass-through shaders.
   - Textures from guest memory: swizzle, DXT and palette decoding, a content-hashed cache that reuses the texture-pack code.
   - Clear, and present through the existing window, split-screen and two-window code.

   **Milestone:** menus, fonts and the HUD render natively.
3. **3D.** `DrawIndexedVertices`:
   - vertex buffers and index data from guest memory, cached by address and content;
   - Xbox vertex shader microcode to HLSL, reusing the emulated renderer's translator;
   - register combiners to HLSL, reusing the existing combiner compiler;
   - fog and alpha test.

   **Milestone:** levels render natively.
4. **Render targets and effects.** Render-to-texture (reflections, shadows, screen effects), `BeginPush` particles, movies and gamma.
5. **Parity and switch-over.**
   - An A/B harness: the same scripted run through both renderers, compared screenshot by screenshot.
   - Fix the differences, then make native the default.
6. **Afterwards.** Features that are hard in the emulated renderer:
   - frame interpolation for above-60 fps (the game logic stays at its fixed 60 Hz step; see below);
   - MSAA;
   - texture packs keyed by texture object;
   - widescreen fixes.

## Frame rate: what the native renderer does and doesn't do

The game's logic advances one fixed step per frame; uncapped, it runs too fast. My notes record that it ran at 400 fps with everything sped up. So a faster renderer alone doesn't give more than 60 fps in play.

What it does give:

- far less CPU per frame (no command decoding and no second copy of the state);
- no emulation stalls;
- headroom for higher resolutions and effects.

Going above 60 fps is a separate step: logic at 60 Hz, with the renderer interpolating transforms between logic steps.

## Comparing the two renderers

- **Runtime switch:** `BUFFY_RENDERER=native|emulated` (environment) or `[Display] Renderer` (ini).
- **Test runs:** scripted pad input with `BUFFY_SHOTS`, taking screenshots at the same moments in both modes.
