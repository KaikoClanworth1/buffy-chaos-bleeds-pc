/**
 * PC entries in the game's own book menus.
 *
 *   Options page:  Resolution (render scale) and VSync -- after Sound
 *                  Volume, changed with Left / Right / A. (Fullscreen is
 *                  Alt+Enter / F11; a third line would cover the prompt.)
 *   Main menu:     Exit -- after Extras.
 *
 * The menus are HUD scripts: each line is an XHudScriptButton the script
 * builds through XHudScriptWnd::AddButton(template). A button's place on the
 * page is its animator (+0x24, an EXItemAnimator_Script placed in the book
 * scene); its label is a string id (+0x9C) or text set with SetText; what it
 * does is its type (+0x64), dispatched in XHudScriptButton::OnPress.
 *
 * When the script adds the last line of a page (Sound Volume, Extras) this
 * adds more lines built from that line's template: the same look, a copy of
 * its animator moved down by the page's own line spacing, a label of our own,
 * and a type the game does not use (0x46FF00xx), which OnPress hands to us.
 * The copied animators are kept in step with the line they were copied from
 * each frame (page turns, fades) before the button draws.
 *
 * Button layout (0xE4 bytes): +0x1C window, +0x20 item, +0x24 animator,
 * +0x5C/+0x60 type data, +0x64 type, +0x68 flags (bit 0 jump to +0x70, bit 1
 * popup +0x6C, bit 2 has text), +0x90 text display, +0x9C label string id.
 * Window: selectable buttons at +0x190 (count) / +0x194 (array).
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp/gen/recomp_types.h"
#include "buffy_settings.h"

void XHudScriptWnd_AddButton_00051590_orig(void);
void XHudScriptButton_OnPress_0004D2E0_orig(void);
void XHudScriptButton_Draw_00047DC0_orig(void);
void XHudScriptButton_SetText_000486D0(void);     /* SetText(unsigned short *) */
void XPlaySound_00091780(void);
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
uint32_t xbox_ContiguousAllocatedBytes(void);

#define BTN_SIZE        0xE4
#define ANIM_SIZE       0x110              /* EXItemAnimator_Script */
#define TYPE_SOUND_VOL  0x46000084u
#define TYPE_MUSIC_VOL  0x46000085u
#define LABEL_EXTRAS    0x450004D7u        /* main menu "Extras" */

enum { E_RESOLUTION, E_VSYNC, E_FULLSCREEN, E_EXIT, E_COOP, E_P1CHANGE, E_COUNT };
#define TYPE_OURS(e)    (0x46FF0001u + (uint32_t)(e))

#define PAD_LEFT   0x40u
#define PAD_RIGHT  0x80u
#define PAD_A      0x2000u

typedef struct {
    uint32_t anim;          /* our copy of an animator (guest VA, allocated once) */
    uint32_t src_anim;      /* the animator it follows */
    float    dx, dy, dz;    /* offset from src_anim */
    uint32_t text;          /* guest buffer for the label */
    DWORD    last_press;
} Entry;

static Entry s_e[E_COUNT];
static uint32_t s_tmpl;     /* guest scratch for a button template */
static void gothic(uint32_t btn);
static uint32_t s_coop_wnd; /* the pause page that has the Co-op line */
static int s_last_line = 4; /* the lowest line added to it (E_COOP / E_P1CHANGE) */

/* ── calling guest code ──────────────────────────────────────────────── */

static void push32(uint32_t v)
{
    g_esp -= 4;
    MEM32(g_esp) = v;
}

/* thiscall f(this, a): callee pops the argument (ret 4). */
static uint32_t call_this1(void (*f)(void), uint32_t self, uint32_t a)
{
    uint32_t esp0 = g_esp;
    push32(a);
    push32(0);                              /* return address */
    g_ecx = self;
    f();
    g_esp = esp0;
    return g_eax;
}

/* cdecl f(a) */
static void call_cdecl1(void (*f)(void), uint32_t a)
{
    uint32_t esp0 = g_esp;
    push32(a);
    push32(0);
    f();
    g_esp = esp0;
}

static float fget(uint32_t va) { uint32_t u = MEM32(va); float f; memcpy(&f, &u, 4); return f; }
static void  fset(uint32_t va, float f) { uint32_t u; memcpy(&u, &f, 4); MEM32(va) = u; }

/* ── labels ──────────────────────────────────────────────────────────── */

static void label_for(int e, wchar_t *out, size_t n)
{
    switch (e) {
    case E_RESOLUTION:
        swprintf(out, n, L"Resolution  %d x %d", buffy_settings_res_width(), buffy_settings_res_height());
        break;
    case E_VSYNC:      swprintf(out, n, L"VSync  %ls", buffy_settings_vsync() ? L"On" : L"Off"); break;
    case E_FULLSCREEN: swprintf(out, n, L"Fullscreen  %ls", buffy_settings_fullscreen() ? L"On" : L"Off"); break;
    case E_COOP:       swprintf(out, n, L"           Co-op"); break;   /* (spaces: under the lines' centre) */
    case E_P1CHANGE:   swprintf(out, n, L"                          Change Character"); break;
    default:           swprintf(out, n, L"Exit"); break;
    }
}

static void set_label(int e, uint32_t btn)
{
    wchar_t w[64];
    int i;
    if (!s_e[e].text)
        s_e[e].text = xbox_HeapAlloc(64 * 2, 16);
    if (!s_e[e].text)
        return;
    label_for(e, w, 64);
    for (i = 0; i < 63 && w[i]; i++)
        MEM16(s_e[e].text + i * 2) = (uint16_t)w[i];
    MEM16(s_e[e].text + i * 2) = 0;
    call_this1(XHudScriptButton_SetText_000486D0, btn, s_e[e].text);
}

/* ── animators ───────────────────────────────────────────────────────── */

/* Copy the source animator into ours and move it by the entry's offset.
 * Position is the translation at +0xAC and the matrix's fourth row at +0x9C;
 * the list links (+4, +8) point at the copy itself, so nothing that walks or
 * unlinks the scene's list can reach it. */
static void sync_anim(Entry *en)
{
    uint32_t a = en->anim, s = en->src_anim;
    int i;
    if (!a || !s)
        return;
    for (i = 0; i < ANIM_SIZE; i += 4)
        MEM32(a + i) = MEM32(s + i);
    MEM32(a + 4) = a;
    MEM32(a + 8) = a;
    fset(a + 0xAC, fget(s + 0xAC) + en->dx);
    fset(a + 0xB0, fget(s + 0xB0) + en->dy);
    fset(a + 0xB4, fget(s + 0xB4) + en->dz);
    fset(a + 0x9C, fget(s + 0x9C) + en->dx);
    fset(a + 0xA0, fget(s + 0xA0) + en->dy);
    fset(a + 0xA4, fget(s + 0xA4) + en->dz);
}

/* ── adding the lines ────────────────────────────────────────────────── */

/* Add entry `e` to window `wnd`, `steps` page lines below the template's line,
 * using `prev_anim` (the line above the template) for the spacing. */
static void add_entry(uint32_t wnd, uint32_t src_tmpl, uint32_t prev_anim, int e, float steps,
                      uint32_t flags_clear)
{
    Entry *en = &s_e[e];
    uint32_t src_anim = MEM32(src_tmpl + 0x24), n, btn;
    int i;

    if (!src_anim || !prev_anim)
        return;
    if (!s_tmpl)
        s_tmpl = xbox_HeapAlloc(BTN_SIZE, 16);
    if (!en->anim)
        en->anim = xbox_HeapAlloc(ANIM_SIZE, 16);
    if (!s_tmpl || !en->anim)
        return;
    en->src_anim = src_anim;
    en->dx = (fget(src_anim + 0xAC) - fget(prev_anim + 0xAC)) * steps;
    en->dy = (fget(src_anim + 0xB0) - fget(prev_anim + 0xB0)) * steps;
    en->dz = (fget(src_anim + 0xB4) - fget(prev_anim + 0xB4)) * steps;
    sync_anim(en);

    for (i = 0; i < BTN_SIZE; i += 4)
        MEM32(s_tmpl + i) = MEM32(src_tmpl + i);
    MEM32(s_tmpl + 0x24) = en->anim;
    MEM32(s_tmpl + 0x64) = TYPE_OURS(e);
    MEM32(s_tmpl + 0x68) &= ~flags_clear;
    MEM32(s_tmpl + 0x5C) = 0;
    MEM32(s_tmpl + 0x60) = 0;              /* volume lines point at their bar */
    MEM32(s_tmpl + 0x6C) = 0;
    MEM32(s_tmpl + 0x70) = 0;
    MEM32(s_tmpl + 0x9C) = 0;
    /* +0x7C: the pad buttons the line reacts to. The volume line this is
     * copied from takes only Left/Right; ours take A as well. */
    MEM32(s_tmpl + 0x7C) |= PAD_A;
    if (e == E_COOP) {
        /* the pause page's lines are drawn artwork: this one is text, and
         * opens the Options page as a popup (buffy_mods.c makes it the
         * co-op page) */
        MEM32(s_tmpl + 0x68) |= 0x4u | 0x2u | 0x8000u | 0x40000u;   /* text, popup; fit to the line's box, centred */
        MEM32(s_tmpl + 0x6C) = 0x0400039Bu;
    }
    if (e == E_P1CHANGE)
        MEM32(s_tmpl + 0x68) |= 0x4u | 0x8000u | 0x40000u;          /* text, fit to the line's box, centred */

    n = MEM32(wnd + 0x190);
    call_this1(XHudScriptWnd_AddButton_00051590_orig, wnd, s_tmpl);
    if (MEM32(wnd + 0x190) != n + 1)
        return;
    btn = MEM32(MEM32(wnd + 0x194) + n * 4);
    if (e == E_COOP || e == E_P1CHANGE)
        gothic(btn);
    set_label(e, btn);
    if (e == E_COOP && MEM32(btn + 0x90))
        MEMF(MEM32(btn + 0x90) + 0x38) = 0.8f;      /* the text's scale: the page's lines' size */
    if (getenv("BUFFY_MENU_LOG"))
        fprintf(stderr, "[MENU] added entry %d as %08X (offset %.3f %.3f %.3f)\n", e, btn, en->dx, en->dy, en->dz);
}

/* The last selectable line before this one, for the page's line spacing. */
static uint32_t prev_line_anim(uint32_t wnd)
{
    uint32_t n = MEM32(wnd + 0x190);
    return n ? MEM32(MEM32(MEM32(wnd + 0x194) + (n - 1) * 4) + 0x24) : 0;
}

/* void XHudScriptWnd::AddButton(XHudScriptButton *tmpl) -- wrapped. */
int buffy_coop_pause_button(uint32_t wnd, uint32_t tmpl, const wchar_t **text);
int buffy_coop_menu_line(void);
int buffy_coop_p1_change_line(void);
void buffy_coop_p1_change_pressed(uint32_t btn);
void buffy_coop_page_opening(void);

/* A line in the book menus' lettering: font 0x07000005 of the front end's
 * text file ([0x1B7F68]) -- in play the pages use the in-game file's plain
 * font. Set on the made button, before its text (SetText hands the button's
 * font to its text display); set in the template, the line's setup fails.
 * Only while the front end's files are loaded (after player 2 has joined). */
static void gothic(uint32_t btn)
{
    void EXGeoFile_GetGeoFile_000C6960(void);
    uint32_t esp0 = g_esp, file = MEM32(0x1B7F68);
    g_esp -= 4; MEM32(g_esp) = file;
    g_esp -= 4; MEM32(g_esp) = 0;
    EXGeoFile_GetGeoFile_000C6960();
    g_esp = esp0;
    if (!g_eax || getenv("BUFFY_NO_GOTHIC"))
        return;
    MEM32(btn + 0x94) = 0x07000005u;
    MEM32(btn + 0x98) = file;
    if (MEM32(btn + 0x1C) && MEM32(MEM32(btn + 0x1C) + 0x174) == 0x04000184u) {
        /* on the pause page: the captions' cream, not the lettering's gold
         * (the line's colour scale, +0x48.. red, green, blue; the result is
         * held to 255) */
        float t[4] = { 0.88f, 1.0f, 1.065f, 1.31f };
        if (getenv("BUFFY_TEXT_TINT"))
            sscanf(getenv("BUFFY_TEXT_TINT"), "%f,%f,%f,%f", &t[0], &t[1], &t[2], &t[3]);
        MEMF(btn + 0x48) = t[0];
        MEMF(btn + 0x4C) = t[1];
        MEMF(btn + 0x50) = t[2];
        MEMF(btn + 0x54) = t[3];
    }
}

/* A line's text, from a string of ours (kept in a guest buffer per line). */
void buffy_menu_set_text(uint32_t btn, const wchar_t *w);
static void set_text(uint32_t btn, const wchar_t *w) { buffy_menu_set_text(btn, w); }
void buffy_menu_set_text(uint32_t btn, const wchar_t *w)
{
    static uint32_t bufs[16];
    static uint32_t owners[16];
    uint32_t buf = 0;
    int i, k;
    for (k = 0; k < 16 && owners[k] && owners[k] != btn; k++)
        ;
    if (k == 16)
        k = 15;
    if (!bufs[k])
        bufs[k] = xbox_HeapAlloc(64 * 2, 16);
    owners[k] = btn;
    buf = bufs[k];
    if (!buf)
        return;
    {
        /* the book lettering's space is narrow: two for one */
        int wide = MEM32(btn + 0x94) == 0x07000005u, j = 0;
        for (i = 0; j < 62 && w[i]; i++) {
            MEM16(buf + j++ * 2) = (uint16_t)w[i];
            if (wide && w[i] == L' ')
                MEM16(buf + j++ * 2) = (uint16_t)' ';
        }
        i = j;
    }
    MEM16(buf + i * 2) = 0;
    call_this1(XHudScriptButton_SetText_000486D0, btn, buf);
}

void XHudScriptWnd_AddButton_00051590(void)
{
    uint32_t wnd = g_ecx, tmpl = MEM32(g_esp + 4);
    uint32_t type = MEM32(tmpl + 0x64), label = MEM32(tmpl + 0x9C);
    uint32_t prev = prev_line_anim(wnd), eax;
    int options = type == TYPE_SOUND_VOL && prev;
    int mainmenu = label == LABEL_EXTRAS && (MEM32(tmpl + 0x68) & 1) && prev;
    int pause = MEM32(wnd + 0x174) == 0x04000184u && type == 0x4600001Au && prev
                && (buffy_coop_menu_line() || buffy_coop_p1_change_line());
    uint32_t saved[BTN_SIZE / 4];
    int i;

    if (getenv("BUFFY_MENU_LOG"))
        fprintf(stderr, "[MENU] button: window %08X script %08X type %08X label %08X flags %08X jump %08X popup %08X\n",
                wnd, MEM32(wnd + 0x174), type, label, MEM32(tmpl + 0x68), MEM32(tmpl + 0x70), MEM32(tmpl + 0x6C));
    {
        /* co-op: player 2's pause menu has its own lines */
        const wchar_t *text = NULL;
        switch (buffy_coop_pause_button(wnd, tmpl, &text)) {
        case 1:
            g_esp += 4 + 4;                     /* left out: ret 4 */
            g_eax = 0;
            return;
        case 2: {
            uint32_t n = MEM32(wnd + 0x190);
            g_ecx = wnd;
            XHudScriptWnd_AddButton_00051590_orig();
            eax = g_eax;
            if (MEM32(wnd + 0x190) == n + 1)
            {
                uint32_t b = MEM32(MEM32(wnd + 0x194) + n * 4);
                gothic(b);
                set_text(b, text);
            }
            g_eax = eax;
            return;
        }
        }
    }
    if (getenv("BUFFY_MENU_DUMP") && (type == 0x4600001Au || type == 0x4600003Fu || type == 0x4600007Cu || type == 0x46000078u
                                      || type == 0x46000088u || type == 0x46000087u)) {
        int k;
        fprintf(stderr, "[MENU] template type %08X label %08X:", type, label);
        for (k = 0; k < BTN_SIZE; k += 4)
            fprintf(stderr, "%s%08X", k % 32 ? " " : "\n   ", MEM32(tmpl + k));
        fprintf(stderr, "\n");
    }
    if (options || mainmenu || pause)       /* the template lives in the caller's frame */
        for (i = 0; i < BTN_SIZE / 4; i++)
            saved[i] = MEM32(tmpl + i * 4);
    XHudScriptWnd_AddButton_00051590_orig();
    if (!options && !mainmenu && !pause)
        return;
    eax = g_eax;
    if (!s_tmpl)
        s_tmpl = xbox_HeapAlloc(BTN_SIZE, 16);
    if (s_tmpl) {
        /* Build from the saved copy: AddButton may have touched the original. */
        static uint32_t src;
        if (!src)
            src = xbox_HeapAlloc(BTN_SIZE, 16);
        if (src) {
            for (i = 0; i < BTN_SIZE / 4; i++)
                MEM32(src + i * 4) = saved[i];
            if (pause) {
                /* under Quit Game: Co-op (story co-op), Change Character
                 * (its own mod), each a line lower */
                float step = 1.0f;
                if (buffy_coop_menu_line()) {
                    add_entry(wnd, src, prev, E_COOP, step, 0x1u);
                    s_last_line = E_COOP;
                    step += 1.0f;
                }
                if (buffy_coop_p1_change_line()) {
                    add_entry(wnd, src, prev, E_P1CHANGE, step, 0x3u);
                    s_last_line = E_P1CHANGE;
                }
                s_coop_wnd = wnd;
            } else if (options) {
                /* Two lines fit between Sound Volume and the page's
                 * Select / Back prompt at 3/4 of the page's spacing;
                 * fullscreen stays on Alt+Enter / F11. */
                add_entry(wnd, src, prev, E_RESOLUTION, 0.75f, 0);
                add_entry(wnd, src, prev, E_VSYNC, 1.5f, 0);
            } else {
                void buffy_input_menu_ready(void);
                add_entry(wnd, src, prev, E_EXIT, 1.0f, 1);   /* no jump: we handle A */
                buffy_input_menu_ready();
            }
        }
    }
    g_eax = eax;
}

/* ── pressing them ───────────────────────────────────────────────────── */

static void press(int e, uint32_t btn, uint32_t mask)
{
    DWORD now = GetTickCount();
    /* Held Left/Right repeats every frame (the volume bars want that). */
    if (now - s_e[e].last_press < 250 || !(mask & (PAD_LEFT | PAD_RIGHT | PAD_A)))
        return;
    s_e[e].last_press = now;
    if (getenv("BUFFY_MENU_WATCH")) {
        void buffy_menu_watch(uint32_t guest_va);
        buffy_menu_watch(MEM32(btn + 0x1C) + 0x1A8);
    }
    if (getenv("BUFFY_MENU_LOG"))
        fprintf(stderr, "[MENU] entry %d pressed, mask %08X\n", e, mask);
    switch (e) {
    case E_RESOLUTION:
        buffy_settings_step_res((mask & PAD_LEFT) ? -1 : 1);
        break;
    case E_VSYNC:
        buffy_settings_set_vsync(!buffy_settings_vsync());
        break;
    case E_FULLSCREEN:
        buffy_settings_set_fullscreen(!buffy_settings_fullscreen());
        break;
    case E_EXIT:
        if (!(mask & PAD_A))
            return;
        call_cdecl1(XPlaySound_00091780, 0x1A00013Eu);
        fprintf(stderr, "[MENU] Exit chosen: quitting\n");
        fflush(stderr);
        buffy_settings_save();
        Sleep(150);
        ExitProcess(0);
        return;
    }
    call_cdecl1(XPlaySound_00091780, 0x1A00013Du);
    set_label(e, btn);
}

/* char XHudScriptButton::OnPress(int pad, int, unsigned mask) -- wrapped. */
int buffy_coop_menu_press(uint32_t btn, uint32_t mask);

void XHudScriptButton_OnPress_0004D2E0(void)
{
    uint32_t btn = g_ecx, type = MEM32(btn + 0x64), mask = MEM32(g_esp + 12);
    int e = (int)(type - TYPE_OURS(0));

    if (getenv("BUFFY_MENU_LOG"))
        fprintf(stderr, "[MENU] press: button %08X type %08X mask %08X args %08X %08X\n", btn, type, mask,
                MEM32(g_esp + 4), MEM32(g_esp + 8));
    switch (buffy_coop_menu_press(btn, mask)) {   /* co-op pages */
    case 1:
        g_esp += 4 + 12;                        /* ret 0Ch */
        g_eax = 0;
        return;
    case 2:
        XHudScriptButton_OnPress_0004D2E0_orig();   /* the game's, as the line now is */
        return;
    }
    if (type < TYPE_OURS(0) || e >= E_COUNT) {
        XHudScriptButton_OnPress_0004D2E0_orig();
        return;
    }
    if (e == E_P1CHANGE) {
        /* player 1's Change Character: the pause closes and Character
         * Select opens (buffy_mods.c) */
        if (MEM32(g_esp + 12) & PAD_A)
            buffy_coop_p1_change_pressed(btn);
        g_esp += 4 + 12;
        g_eax = 0;
        return;
    }
    if (e == E_COOP) {
        /* Co-op: the game opens the line's popup (the Options page), which
         * buffy_mods.c turns into the co-op page */
        if (MEM32(g_esp + 12) & PAD_A) {
            buffy_coop_page_opening();
            XHudScriptButton_OnPress_0004D2E0_orig();
            return;
        }
        g_esp += 4 + 12;
        g_eax = 0;
        return;
    }
    g_esp += 4 + 12;                        /* ret 0Ch */
    press(e, btn, mask);
    /* Returned last: the guest calls above leave their own results in eax.
     * Nonzero means "this line was activated", and the page script then
     * closes the page -- with no page to go back to, the front end restarts. */
    g_eax = 0;
}

/* void XHudScriptButton::Draw(EXMatrix, EXRenderInfo) -- wrapped: keep our
 * animator copies moving with the lines they were copied from. */
int buffy_coop_hide_line(uint32_t btn);
void XHudScriptButton_Draw_00047DC0(void)
{
    uint32_t btn = g_ecx, type = MEM32(btn + 0x64);
    if (buffy_coop_hide_line(btn)) {
        g_esp += 4 + 0x60;                      /* not drawn: ret 60h (EXMatrix, EXRenderInfo) */
        g_eax = 1;
        return;
    }
    int e = (int)(type - TYPE_OURS(0));
    if (type >= TYPE_OURS(0) && e < E_COUNT && MEM32(btn + 0x24) == s_e[e].anim)
        sync_anim(&s_e[e]);
    if (type == 0x4600007Cu && s_coop_wnd && MEM32(btn + 0x1C) == s_coop_wnd && MEM32(btn + 0x24)) {
        /* the pause page's "Press START to continue", a line lower to make
         * room for Co-op */
        uint32_t a = MEM32(btn + 0x24);
        float dx = s_e[s_last_line].dx, dy = s_e[s_last_line].dy, dz = s_e[s_last_line].dz;
        fset(a + 0xAC, fget(a + 0xAC) + dx); fset(a + 0xB0, fget(a + 0xB0) + dy); fset(a + 0xB4, fget(a + 0xB4) + dz);
        fset(a + 0x9C, fget(a + 0x9C) + dx); fset(a + 0xA0, fget(a + 0xA0) + dy); fset(a + 0xA4, fget(a + 0xA4) + dz);
        XHudScriptButton_Draw_00047DC0_orig();
        fset(a + 0xAC, fget(a + 0xAC) - dx); fset(a + 0xB0, fget(a + 0xB0) - dy); fset(a + 0xB4, fget(a + 0xB4) - dz);
        fset(a + 0x9C, fget(a + 0x9C) - dx); fset(a + 0xA0, fget(a + 0xA0) - dy); fset(a + 0xA4, fget(a + 0xA4) - dz);
        return;
    }
    XHudScriptButton_Draw_00047DC0_orig();
}

/* ── debugging: BUFFY_MENU_WATCH=1 puts a hardware write watchpoint on the
 * pressed line's window flags (+0x1A8) and names every function that writes
 * them afterwards. ─────────────────────────────────────────────────────── */
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static volatile LONG s_watch_hits;

static LONG CALLBACK watch_veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && (ep->ContextRecord->Dr6 & 1)) {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        DWORD64 disp = 0;
        memset(buf, 0, sizeof buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)ep->ExceptionRecord->ExceptionAddress, &disp, sym))
            fprintf(stderr, "[WATCH] write by %s+0x%llX, value now %08X\n", sym->Name,
                    (unsigned long long)disp, *(uint32_t *)(uintptr_t)ep->ContextRecord->Dr0);
        ep->ContextRecord->Dr6 = 0;
        if (InterlockedIncrement(&s_watch_hits) > 40)
            ep->ContextRecord->Dr7 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

typedef struct { HANDLE th; uintptr_t addr; } WatchReq;

static DWORD WINAPI watch_arm(LPVOID p)
{
    WatchReq *r = (WatchReq *)p;
    CONTEXT c;
    SuspendThread(r->th);
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(r->th, &c);
    c.Dr0 = r->addr;
    c.Dr7 = 1 | (1u << 16) | (3u << 18);        /* local, write, 4 bytes */
    SetThreadContext(r->th, &c);
    ResumeThread(r->th);
    return 0;
}

void buffy_menu_watch(uint32_t guest_va)
{
    static int armed;
    static WatchReq r;
    HANDLE t;
    if (armed++)
        return;
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, watch_veh);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &r.th, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    r.addr = (uintptr_t)XBOX_PTR(guest_va);
    t = CreateThread(NULL, 0, watch_arm, &r, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    fprintf(stderr, "[WATCH] armed on %08X\n", guest_va);
}

/* void XHudScriptWnd::SetScript(uint32_t script, uint32_t file) -- wrapped: the
 * page a script window shows (a script in the given geometry file). The
 * co-op join page backing out is stopped here (buffy_mods.c). */
void XHudScriptWnd_SetScript_000522A0_orig(void);
int  buffy_coop_set_script(uint32_t wnd, uint32_t script);

void XHudScriptWnd_SetScript_000522A0(void)
{
    if (getenv("BUFFY_MENU_LOG"))
        fprintf(stderr, "[MENU] t=%lu window %08X script %08X file %08X\n", GetTickCount() / 100 % 100000, g_ecx,
                MEM32(g_esp + 4), MEM32(g_esp + 8));
    if (buffy_coop_set_script(g_ecx, MEM32(g_esp + 4))) {
        g_esp += 4 + 8;                         /* ret 8 */
        return;
    }
    XHudScriptWnd_SetScript_000522A0_orig();
}

/* EXGeoFile *LoadGeoFile(uint32_t hash, char queue), char DeLoadGeoFile(uint32_t
 * hash, char now) -- wrapped (BUFFY_GEO_LOG: which files come and go). */
void EXGeoFile_LoadGeoFile_000C69C0_orig(void);
void EXGeoFile_DeLoadGeoFile_000C6B50_orig(void);

void EXGeoFile_LoadGeoFile_000C69C0(void)
{
    uint32_t hash = MEM32(g_esp + 4);
    EXGeoFile_LoadGeoFile_000C69C0_orig();
    if (getenv("BUFFY_GEO_LOG"))
        fprintf(stderr, "[GEO] t=%lu load %08X -> %08X (contiguous %u KB)\n", GetTickCount() / 100 % 100000, hash,
                g_eax, xbox_ContiguousAllocatedBytes() / 1024);
}

void EXGeoFile_DeLoadGeoFile_000C6B50(void)
{
    if (getenv("BUFFY_GEO_LOG"))
        fprintf(stderr, "[GEO] t=%lu unload %08X now %u\n", GetTickCount() / 100 % 100000, MEM32(g_esp + 4),
                MEM8(g_esp + 8));
    EXGeoFile_DeLoadGeoFile_000C6B50_orig();
}
