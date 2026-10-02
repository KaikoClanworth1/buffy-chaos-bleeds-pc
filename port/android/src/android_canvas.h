/* A CPU picture for the Android screens drawn over or instead of the game
 * (touch controls, the installer): premultiplied BGRA, drawn with simple
 * antialiased shapes and a 5x7 font, shown through the GPU layer
 * (android_canvas.c). Coordinates are screen pixels; the picture may be
 * smaller than the screen (cv_s: picture pixels per screen pixel). */
#pragma once
#include <stdint.h>
#include "gpu.h"

typedef struct { float x0, y0, x1, y1; } CvBox;

extern uint32_t *cv_px;
extern int cv_w, cv_h;
extern float cv_s;

int  cv_alloc(int w, int h, float scale);     /* (again when the size changes) */
void cv_clear(uint32_t argb);                 /* premultiplied */
void cv_disc(float cx, float cy, float r, float ring, uint32_t rgb, float a);
void cv_rbox(const CvBox *b, float rad, float ring, uint32_t rgb, float a);
void cv_arrow(float cx, float cy, float size, int dir, uint32_t rgb, float a);   /* 0 up 1 down 2 left 3 right */
float cv_text_w(const char *t, float h);
void cv_text(const char *t, float cx, float cy, float h, uint32_t rgb, float a);  /* centred */

/* The picture, uploaded and blended over (or onto) a swap chain's back
 * buffer -- the whole of it, stretched. */
typedef struct {
    GpuShader *vs, *ps;
    GpuBlendState *bs;
    GpuRasterState *rs;
    GpuSampler *samp;
    GpuTexture *tex;
    int tw, th;
} CvGpu;
int  cv_upload(CvGpu *g);
void cv_draw(CvGpu *g, GpuTexture *bb, uint32_t bw, uint32_t bh);
