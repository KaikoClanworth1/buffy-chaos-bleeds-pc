/**
 * Mods, as the launcher sets them up.
 *
 * Each mod is a folder mods\<name>\ beside the exe with a mod.ini and, for
 * mods that replace game files, a files\ folder laid out like the game
 * folder. The launcher's Mods tab writes the ticked ones, in order, to
 * mods\.launcher\enabled.txt; here each one's files\ becomes an overlay on
 * the game folder (xbox_path_add_overlay, kernel_path.c): a game file
 * present in a ticked mod is read from the mod instead, the lowest ticked
 * mod winning. Nothing in the game folder is copied or changed, so
 * unticking a mod takes it out completely.
 *
 * Changes a file cannot make -- what the game works out at run time -- are
 * built-in patches, switched on by a ticked mod's mod.ini:
 *
 *   [Patches]
 *   UnlockAllMultiplayerCharacters = 1
 *   UnlockAllMultiplayerArenas = 1
 *   UnlockAllWillowSpells = 1
 *   AlwaysPlayAsWillow = 1
 *   StoryCoop = 1, StoryCoopScreens = 2
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp/gen/recomp_types.h"
#include "buffy_settings.h"

void buffy_menu_set_text(uint32_t btn, const wchar_t *w);
void nv2a_gpu_set_split(int on);
void nv2a_gpu_show_second_window(int show);
void nv2a_gpu_enable_second_window(void);

void xbox_path_add_overlay(const wchar_t *dir);

static wchar_t s_dirs[32][MAX_PATH];     /* each ticked mod's files\ folder */
static int     s_count;

/* ── patches ───────────────────────────────────────────────────────────── */

static int s_unlock_mp_chars, s_unlock_mp_arenas, s_unlock_spells, s_always_willow;
static int s_story_coop, s_p1_change;     /* StoryCoop, Player1ChangeCharacter */
static int s_screens_cfg = 1;              /* StoryCoopScreens */
void nv2a_gpu_enable_second_window(void);

static void coop_settings_load(void);
static int s_split_last;

static void read_patches(const wchar_t *mod_dir)
{
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, MAX_PATH, L"%s\\mod.ini", mod_dir);
    if (GetPrivateProfileIntW(L"Patches", L"UnlockAllMultiplayerCharacters", 0, ini)) {
        s_unlock_mp_chars = 1;
        fprintf(stderr, "[MODS]   patch: every multiplayer character unlocked\n");
    }
    if (GetPrivateProfileIntW(L"Patches", L"UnlockAllWillowSpells", 0, ini)) {
        s_unlock_spells = 1;
        fprintf(stderr, "[MODS]   patch: Willow starts with every spell\n");
    }
    if (GetPrivateProfileIntW(L"Patches", L"AlwaysPlayAsWillow", 0, ini)) {
        s_always_willow = 1;
        fprintf(stderr, "[MODS]   patch: every story level is played as Willow\n");
    }
    if (GetPrivateProfileIntW(L"Patches", L"StoryCoop", 0, ini)) {
        s_story_coop = 1;
        fprintf(stderr, "[MODS]   patch: story co-op (player 2 joins with Start on controller 2)\n");
        s_screens_cfg = (int)GetPrivateProfileIntW(L"Patches", L"StoryCoopScreens", 1, ini);
        if (s_screens_cfg >= 2)
            fprintf(stderr, "[MODS]   patch: story co-op on two screens (player 2's opens as they join)\n");
        coop_settings_load();
    }
    if (GetPrivateProfileIntW(L"Patches", L"Player1ChangeCharacter", 0, ini)) {
        s_p1_change = 1;
        fprintf(stderr, "[MODS]   patch: player 1's pause menu has Change Character\n");
    }
    if (GetPrivateProfileIntW(L"Patches", L"UnlockAllMultiplayerArenas", 0, ini)) {
        s_unlock_mp_arenas = 1;
        fprintf(stderr, "[MODS]   patch: every multiplayer arena unlocked\n");
    }
}

/* void XMemcardManager::UnlockBonusesFromSaves(XBuffySave *, int, int,
 * EXWString *, int *, int *) -- wrapped.
 *
 * Multiplayer characters are unlocked by story progress. The character
 * select page (XHudScriptButton::OnInit / OnUpdate, portrait type
 * 0x46000005, index at +0x5C) shows character n when bit n of the memcard
 * manager's mask at +0x2D8 is set. The mask is not saved: it is zeroed and
 * rebuilt from the saves' level progress by this function each time the
 * saves are read, so setting the bits after it changes no save file, and
 * unticking the mod brings the game's own unlocks back.
 *
 * The next mask, +0x2DC, is the other unlocks: bits 0-8 the arenas on the
 * Arena Select page (button type 0x46000045, index at +0x5C), bits 9 and up
 * the Extras (types 0x4600007D/7E, index + 9), which are left alone. */
void XMemcardManager_UnlockBonusesFromSaves_0005E8D0_orig(void);

void XMemcardManager_UnlockBonusesFromSaves_0005E8D0(void)
{
    uint32_t self = g_ecx;
    XMemcardManager_UnlockBonusesFromSaves_0005E8D0_orig();
    if (s_unlock_mp_chars)
        MEM32(self + 0x2D8) |= 0x00FFFFFFu;         /* the 24 characters */
    if (s_unlock_mp_arenas)
        MEM32(self + 0x2DC) |= 0x000001FFu;         /* the arenas */
}

/* ── Willow's spells ──────────────────────────────────────────────────────
 *
 * Spells are inventory items 0x86-0x93 (item group 6 -- the group byte at
 * +0x192 of the item table, 0x1F0 bytes an item, which the spell book asks
 * XInventory::AvailibleToSelect for). A player keeps one inventory per
 * story character, at handler +0x9A4 + n * 0x1B04, character 1 being
 * Willow; an item is held while the inventory's count for it (0x2C bytes an
 * item, at +0x28) is above zero. XInventory::SetInitialInventoryContents(n)
 * builds inventory n at the start of every level and match, giving Willow
 * the spells the story has reached (0x86 always, more by level).
 *
 * UnlockAllWillowSpells: after that, and after a checkpoint restores
 * player 1's inventories (ReStoreInventoryForContinuePoints), Willow's
 * inventory gets every spell -- added the way the game adds its own. */
#define SPELL_FIRST     0x86u
#define SPELL_LAST      0x93u
#define CHAR_WILLOW     1
#define INV_STRIDE      0x1B04u

void XInventory_AddItemToInventory_00057630(void);
void XInventory_SetInitialInventoryContents_00057DD0_orig(void);
void XInventory_ReStoreInventoryForContinuePoints_00055B80_orig(void);

static void give_spells(uint32_t inv)
{
    uint32_t id;
    int added = 0;
    for (id = SPELL_FIRST; id <= SPELL_LAST; id++) {
        uint32_t esp0 = g_esp;
        if ((int16_t)MEM16(inv + id * 0x2C + 0x28) > 0)
            continue;                                    /* already known */
        /* AddItemToInventory(item, count, -100.0f): thiscall, callee pops. */
        g_esp -= 4; MEM32(g_esp) = 0xC2C80000u;
        g_esp -= 4; MEM32(g_esp) = 1;
        g_esp -= 4; MEM32(g_esp) = id;
        g_esp -= 4; MEM32(g_esp) = 0;
        g_ecx = inv;
        XInventory_AddItemToInventory_00057630();
        g_esp = esp0;
        added++;
    }
    if (added && getenv("BUFFY_MODS_LOG")) {
        int held = 0;
        for (id = SPELL_FIRST; id <= SPELL_LAST; id++)
            held += (int16_t)MEM16(inv + id * 0x2C + 0x28) > 0;
        fprintf(stderr, "[MODS] Willow was given %d spell(s); she now holds %d of %d\n", added, held,
                (int)(SPELL_LAST - SPELL_FIRST + 1));
    }
}

/* void XInventory::SetInitialInventoryContents(unsigned char character) -- wrapped. */
void XInventory_SetInitialInventoryContents_00057DD0(void)
{
    uint32_t inv = g_ecx, eax;
    int who = (int)MEM8(g_esp + 4);
    XInventory_SetInitialInventoryContents_00057DD0_orig();
    eax = g_eax;
    if (s_unlock_spells && who == CHAR_WILLOW)
        give_spells(inv);
    g_eax = eax;
}

/* void XInventory::ReStoreInventoryForContinuePoints(unsigned, unsigned) -- wrapped. */
void XInventory_ReStoreInventoryForContinuePoints_00055B80(void)
{
    uint32_t eax, player, handler;
    XInventory_ReStoreInventoryForContinuePoints_00055B80_orig();
    eax = g_eax;
    player = MEM32(0x26DC54);
    handler = player ? MEM32(player + 0x14C) : 0;
    if (s_unlock_spells && handler)
        give_spells(handler + 0x9A4 + CHAR_WILLOW * INV_STRIDE);
    g_eax = eax;
}

/* ── always Willow ────────────────────────────────────────────────────────
 *
 * A story level's player comes from its player-start trigger:
 * XTrigger_PlayerStart::Init reads the character from the trigger (+0x70,
 * 1-based: 1 Buffy, 2 Willow, 3 Xander, 4 Spike, 5 Faith), loads that row
 * of the character sheet (model, sounds) and makes the player with it; the
 * player item's +0x16C is then the character, which also picks which of the
 * player's six inventories is in use. Some levels change character part way
 * through with XItemHandler_Player::SwapCharacter(n 1-based, unload).
 *
 * AlwaysPlayAsWillow: outside multiplayer (XApp +0x234 == 3), a trigger that
 * starts a playable character starts Willow instead, and a swap asks for Willow --
 * which SwapCharacter treats as no change when she is already playing. */
#define CHAR_WILLOW_1BASED 2
#define START_TABLE        0x1B8E68u
#define START_WILLOW       11


static int in_multiplayer(void)
{
    uint32_t app = MEM32(0x26D868);
    return app && MEM32(app + 0x234) == 3;
}

void XTrigger_PlayerStart_Init_0009F3A0_orig(void);
void XItemHandler_Player_SwapCharacter_0007B200_orig(void);

/* void XTrigger_PlayerStart::Init(...) -- wrapped. */
/* ── story co-op ──────────────────────────────────────────────────────────
 *
 * StoryCoop: a second player in story levels. In story mode the player-start
 * trigger only ever makes player slot 0 (the table at 0x26DC54: players 0-3,
 * then the camera); when slot 0 is taken it just moves that player to the
 * start. So once the game has made player 1, the trigger is run once more
 * with slot 0 briefly empty, a playable-character entry for player 2 and a
 * position beside player 1: the game builds the whole character (model,
 * sounds, inventories, physics, lists) exactly as for player 1. The new
 * player goes to slot 1 with its player index (handler +0x719) set to 1, and
 * player 1 is put back. Player 2's character (a character-sheet row, 0
 * Buffy, 1 Willow, 2 Xander, 3 Spike, 4 Faith, 5 the sixth) is picked when
 * they join (below), and kept: every later level start makes them again. */
static const uint32_t k_start_entry[6] = { 16, 11, 12, 13, 14, 15 };   /* by character row */
static uint32_t s_trig, s_trig_args[3];   /* player 1's start trigger, this level */
static int      s_p2_row = -1;            /* player 2's character row; -1 until they join */

static void coop_spawn(uint32_t trig, uint32_t a1, uint32_t a2, uint32_t a3, int row)
{
    uint32_t p1 = MEM32(0x26DC54), p2, save[0x30], esp0 = g_esp, h, borrowed;
    int i;
    float x;
    if (!p1 || MEM32(0x26DC58))
        return;
    for (i = 0; i < 0x30; i++)
        save[i] = MEM32(trig + i * 4);                   /* +0x00..+0xBC */
    /* Rows 0-5 have their own start entries; a multiplayer character (6-23)
     * borrows Buffy's entry with its row written in for the call. */
    MEM32(trig + 0x40) = row <= 5 ? k_start_entry[row] : k_start_entry[0];
    borrowed = MEM32(START_TABLE + k_start_entry[0] * 16 + 4);
    if (row > 5)
        MEM32(START_TABLE + k_start_entry[0] * 16 + 4) = (uint32_t)row;
    MEM32(trig + 0x70) = 0;
    memcpy(&x, (const void *)XBOX_PTR(trig + 0x0C), 4);
    x += 1.0f;                                         /* beside player 1 */
    memcpy((void *)XBOX_PTR(trig + 0x0C), &x, 4);
    MEM32(0x26DC54) = 0;
    g_esp -= 4; MEM32(g_esp) = a3;
    g_esp -= 4; MEM32(g_esp) = a2;
    g_esp -= 4; MEM32(g_esp) = a1;
    g_esp -= 4; MEM32(g_esp) = 0;                      /* return address */
    g_ecx = trig;
    XTrigger_PlayerStart_Init_0009F3A0_orig();
    g_esp = esp0;
    MEM32(START_TABLE + k_start_entry[0] * 16 + 4) = borrowed;
    p2 = MEM32(0x26DC54);
    MEM32(0x26DC54) = p1;
    for (i = 0; i < 0x30; i++)
        MEM32(trig + i * 4) = save[i];
    if (!p2 || p2 == p1) {
        fprintf(stderr, "[MODS] co-op: player 2 could not be made\n");
        return;
    }
    MEM32(0x26DC58) = p2;
    h = MEM32(p2 + 0x14C);
    if (h)
        MEM8(h + 0x719) = 1;
    /* The game pad object (0x26EBB8) maps players to controllers at +0xCA8
     * (story mode: player 0 -> controller 0, the rest -1): player 2 uses
     * controller 2. */
    if (MEM32(0x26EBB8))
        MEM32(MEM32(0x26EBB8) + 0xCA8 + 4) = 1;
    {
        uint32_t gp = MEM32(0x26EBB8);
        if (gp && getenv("BUFFY_MODS_LOG"))
            fprintf(stderr, "[MODS] co-op: pad map before %d %d %d %d, masks %08X %08X\n",
                    (int)MEM32(gp + 0xCA8), (int)MEM32(gp + 0xCAC), (int)MEM32(gp + 0xCB0), (int)MEM32(gp + 0xCB4),
                    MEM32(gp + 0x64), MEM32(gp + 0x68));
    }
    fprintf(stderr, "[MODS] co-op: player 2 (character %d) is item %08X\n", (int)MEM32(p2 + 0x16C), p2);
}

static int s_cam2_valid;                  /* (the two-screens section's) */

/* Put player 2 beside player 1, facing the same way; player 2's camera
 * starts again from player 1's. */
static void coop_follow(void)
{
    s_cam2_valid = 0;
    uint32_t p1 = MEM32(0x26DC54), p2 = MEM32(0x26DC58);
    float x;
    int i;
    for (i = 0; i < 4; i++) {
        MEM32(p2 + 0xAC + i * 4) = MEM32(p1 + 0xAC + i * 4);
        MEM32(p2 + 0xBC + i * 4) = MEM32(p1 + 0xBC + i * 4);
    }
    memcpy(&x, (const void *)XBOX_PTR(p2 + 0xAC), 4);
    x += 1.0f;
    memcpy((void *)XBOX_PTR(p2 + 0xAC), &x, 4);
    MEM8(p2 + 0x69) = 0;                      /* as the trigger does after moving */
    if (getenv("BUFFY_MODS_LOG"))
        fprintf(stderr, "[MODS] co-op: player 2 moved beside player 1\n");
}

/* void XTrigger_PlayerStart::Init(a, b, c) -- wrapped. */
void XTrigger_PlayerStart_Init_0009F3A0(void)
{
    uint32_t trig = g_ecx;
    uint32_t a1 = MEM32(g_esp + 4), a2 = MEM32(g_esp + 8), a3 = MEM32(g_esp + 12);
    uint32_t had_player = MEM32(0x26DC54);
    /* +0x40 picks a 16-byte entry of the start table (0x1B8E68): kind at +0
     * (2 = a playable character), character-sheet row at +4. The character
     * entries are 11 Willow, 12 Xander, 13 Spike, 14-15 the others, 16
     * Buffy; +0x70, when set, overrides the row (1-based). */
    uint32_t kind = MEM32(START_TABLE + MEM32(trig + 0x40) * 16);
    int playable = !in_multiplayer() && MEM32(trig + 0x40) < 64 && kind == 2;
    if (s_always_willow && playable) {
        if (getenv("BUFFY_MODS_LOG") && MEM32(trig + 0x40) != START_WILLOW)
            fprintf(stderr, "[MODS] level starts as start entry %u; playing as Willow\n", MEM32(trig + 0x40));
        MEM32(trig + 0x40) = START_WILLOW;
        if ((int32_t)MEM32(trig + 0x70) > 0)
            MEM32(trig + 0x70) = CHAR_WILLOW_1BASED;
    }
    {
        uint32_t t40 = MEM32(trig + 0x40), t70 = MEM32(trig + 0x70);
        XTrigger_PlayerStart_Init_0009F3A0_orig();
        if (getenv("BUFFY_MODS_LOG")) {
            uint32_t item = MEM32(trig + 0xBC);
            fprintf(stderr, "[MODS] player start: +40 %u +70 %d -> item %08X char %d (+170 %d)\n", t40, (int)t70,
                    item, item ? (int)MEM32(item + 0x16C) : -1, item ? (int)MEM32(item + 0x170) : -1);
        }
    }
    /* Player 1 was just made (not moved): remember the trigger for player 2
     * joining later, and if player 2 has joined already (an earlier level, a
     * restart), make them beside player 1 now. */
    if (s_story_coop && playable && !had_player && MEM32(0x26DC54)) {
        uint32_t eax = g_eax;
        s_trig = trig;
        s_trig_args[0] = a1; s_trig_args[1] = a2; s_trig_args[2] = a3;
        if (s_p2_row >= 0)
            coop_spawn(trig, a1, a2, a3, s_p2_row);
        g_eax = eax;
    }
    /* Player 1 was moved to a start point (the start of play after the
     * intro, a checkpoint): bring player 2 along, the way the trigger moves
     * player 1 -- position (+0xAC) and facing (+0xBC) written directly. */
    else if (s_story_coop && had_player && MEM32(0x26DC54) && MEM32(0x26DC58))
        coop_follow();
}

/* char XItemHandler_Player::SwapCharacter(unsigned char n, char unload) -- wrapped. */
void XItemHandler_Player_SwapCharacter_0007B200(void)
{
    uint32_t p1 = MEM32(0x26DC54);
    if (s_always_willow && !in_multiplayer() && (!p1 || MEM32(p1 + 0x14C) == g_ecx)) {   /* player 1 only */
        if (getenv("BUFFY_MODS_LOG") && MEM8(g_esp + 4) != CHAR_WILLOW_1BASED)
            fprintf(stderr, "[MODS] level swaps to character %u; staying Willow\n", MEM8(g_esp + 4));
        MEM32(g_esp + 4) = CHAR_WILLOW_1BASED;
    }
    XItemHandler_Player_SwapCharacter_0007B200_orig();
}

/* ── loading ───────────────────────────────────────────────────────────── */

void buffy_mods_load(void)
{
    wchar_t base[MAX_PATH], list[MAX_PATH], *slash;
    FILE *f;
    char line[512];

    GetModuleFileNameW(NULL, base, MAX_PATH);
    slash = wcsrchr(base, L'\\');
    if (!slash)
        return;
    *slash = 0;
    swprintf_s(list, MAX_PATH, L"%s\\mods\\.launcher\\enabled.txt", base);
    if (_wfopen_s(&f, list, L"r") || !f)
        return;
    while (fgets(line, sizeof line, f) && s_count < 32) {
        wchar_t name[256], dir[MAX_PATH];
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
            line[--n] = 0;
        if (!n || line[0] == '#' || strchr(line, '\\') || strchr(line, '/') || strstr(line, ".."))
            continue;
        MultiByteToWideChar(CP_UTF8, 0, line, -1, name, 256);
        swprintf_s(dir, MAX_PATH, L"%s\\mods\\%s", base, name);
        if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES)
            continue;
        fprintf(stderr, "[MODS] %s\n", line);
        read_patches(dir);
        wcscat_s(dir, MAX_PATH, L"\\files");
        if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES)
            continue;                                   /* a patch-only mod */
        wcscpy_s(s_dirs[s_count++], MAX_PATH, dir);
        xbox_path_add_overlay(dir);
    }
    fclose(f);
}

/* A file under the game folder (relative path), as a ticked mod replaces
 * it: the host path of the mod's copy, or 0 if no ticked mod has one. */
int buffy_mods_find(const char *rel, char *out, size_t cap)
{
    int k;
    for (k = s_count - 1; k >= 0; k--) {
        char dir[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, s_dirs[k], -1, dir, MAX_PATH, NULL, NULL);
        sprintf_s(out, cap, "%s\\%s", dir, rel);
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES)
            return 1;
    }
    return 0;
}

/* Once a frame (buffy_frame.c). BUFFY_COOP_LOG=1: both players' positions
 * every half second, for the co-op work. */
static void buffy_coop_log_cameras(void);
void buffy_coop_item_dump(void);
void buffy_coop_font_dump(void);

void buffy_mods_frame(void)
{
    buffy_coop_item_dump();
    buffy_coop_font_dump();
    static DWORD last;
    int i;
    if (!getenv("BUFFY_COOP_LOG") || GetTickCount() - last < 500)
        return;
    last = GetTickCount();
    for (i = 0; i < 2; i++) {
        uint32_t it = MEM32(0x26DC54 + i * 4);
        float x = 0, y = 0, z = 0;
        if (!it)
            continue;
        memcpy(&x, (const void *)XBOX_PTR(it + 0xAC), 4);
        memcpy(&y, (const void *)XBOX_PTR(it + 0xB0), 4);
        memcpy(&z, (const void *)XBOX_PTR(it + 0xB4), 4);
        fprintf(stderr, "[COOP] t=%lu p%d %.2f %.2f %.2f\n", GetTickCount() / 100 % 100000, i + 1, x, y, z);
    }
    buffy_coop_log_cameras();
    {
        void buffy_dsound_p2_report(void);
        buffy_dsound_p2_report();
    }
}

/* ── memory for joining ───────────────────────────────────────────────────
 *
 * Player 2 picks a character on the multiplayer Character Select, which lives
 * in the front end's files (0x01000116: the book, portraits, its scripts --
 * about 8.4 MB) and a level has no room for on a retail Xbox's 64 MB. So with
 * story co-op on, the Xbox is given a development kit's 128 MB of contiguous
 * memory and the game's main heap (38 MB, sized by the word at 0x1B7F84,
 * read once at boot) grows by COOP_EXTRA_HEAP. */
#define COOP_EXTRA_HEAP (24u * 1024u * 1024u)
void xbox_SetContigSize(uint32_t bytes);

void buffy_coop_memory_early(void)
{
    if (s_story_coop || s_p1_change)
        xbox_SetContigSize(128u * 1024u * 1024u);
}

void buffy_coop_memory_patch(void)
{
    if ((!s_story_coop && !s_p1_change) || getenv("BUFFY_NO_HEAP_PATCH"))
        return;
    fprintf(stderr, "[MODS] co-op: main heap %u MB -> %u MB\n", MEM32(0x1B7F84) >> 20,
            (MEM32(0x1B7F84) + COOP_EXTRA_HEAP) >> 20);
    MEM32(0x1B7F84) += COOP_EXTRA_HEAP;
}

/* void *XPhysicalAlloc(size, physical address or -1, alignment, protect) --
 * wrapped. The grown heap goes above the retail 64 MB (physical 0x04000000,
 * VA 0x84000000), so everything allocated after it -- the GPU's push buffer
 * and surfaces -- stays where a 64 MB console would put it. */
void XPhysicalAlloc_0012A2AC_orig(void);
void XPhysicalAlloc_0012A2AC(void)
{
    if ((s_story_coop || s_p1_change) && !getenv("BUFFY_NO_HEAP_PATCH") && MEM32(g_esp + 4) == 0x2600000u + COOP_EXTRA_HEAP
            && MEM32(g_esp + 8) == 0xFFFFFFFFu)
        MEM32(g_esp + 8) = 0x04000000u;
    XPhysicalAlloc_0012A2AC_orig();
}

/* ── two screens ──────────────────────────────────────────────────────────
 *
 * StoryCoopScreens = 2: player 2 gets their own window, with the story
 * camera following them. The renderer has a second window
 * (nv2a_gpu_enable_second_window) and sends each D3D Swap to window 1,
 * window 2 or both, in order (nv2a_gpu_queue_flip, from buffy_frame.c).
 *
 * A frame is EXApp::MainUpdate: Update (game logic -- every item, the
 * camera too), Render (EXBaseApp::Render: the display's windows, the world
 * then the HUD), then EXDisplay::Present (the D3D Swap). After it, player 2's
 * frame: the camera's state is swapped for player 2's (its handler, 0x664
 * bytes at camera item +0x14C; the render camera it drives, handler +0x410,
 * 0x80 bytes; the camera item's placement), players 1 and 2 swap table
 * slots so the story camera -- which follows the player in slot 0 -- runs
 * one update on player 2, and the frame is drawn and presented again, to
 * window 2. Then player 1's camera is put back. Game logic runs once; only
 * the drawing is done twice.
 *
 * Menus, cutscenes (a camera mode change pending) and movies are not drawn
 * twice: that frame goes to both windows. */
int  nv2a_gpu_has_second_window(void);
int  buffy_movie_active(void);
void EXApp_MainUpdate_000BD240_orig(void);
void XItemHandler_Camera_DoUpdate_0003B930(void);
void EXBaseApp_Render_000BDE00(void);
void EXDisplay_Present_000D9880(void);

#define CAM_HANDLER_SIZE 0x664
#define EXCAM_SIZE       0x80
#define s_screens s_screens_cfg
static int      s_split_off;
static uint8_t  s_cam2[CAM_HANDLER_SIZE], s_excam2[EXCAM_SIZE], s_camitem2[0x30];
static int      s_cam2_valid;
static volatile int s_in_pass2, s_split_frame;
static const uint32_t k_cam_ptrs[] = { 0x04, 0x410, 0x4BC, 0x4C0, 0x4C8, 0x4D0 };   /* owned/linked pointers */

int buffy_coop_in_pass2(void) { return s_in_pass2; }

static int s_teleport_on_back = 1, s_friendly_fire, s_respawn_secs = 3;
static int s_share_inventory = 1, s_hud_p2, s_hud_p2_update = 1;
static void coop_inventory_sync(void);
static uint32_t s_inv_1, s_inv_2;          /* the inventories the base was taken from */
static uint32_t player_inventory(uint32_t item);
static int16_t inv_count(uint32_t inv, int id);
static void inv_set(uint32_t inv, int id, int16_t want);
static void hud_for_p2(int on);

/* ── joining ──────────────────────────────────────────────────────────────
 *
 * Player 2 joins by pressing Start on controller 2 during a story level.
 * Their window opens (StoryCoopScreens = 2) showing the multiplayer
 * Character Select (script 0x040000F9 of the front end's file), taking only
 * controller 2, while player 1 plays on. Picking a story character (A on a
 * portrait) makes player 2 beside player 1; B backs out.
 *
 * The page lives in the front end's files, which the game unloads for a
 * level: they are loaded for the page (the language's script/text file at
 * 0x1B7F68 and its other file at 0x1B7F78, plus 0x01000043 and 0x010000C0;
 * 0x01000055, the character sheet, stays loaded in play) and unloaded after.
 * The window is made as XApp::CreatePauseMenu makes the pause menu:
 * EXAlloc(0x21C), XHudScriptWnd(script, file), SetWindowOrder(0xC), its
 * Update; RestrictToPad(1) gives it to controller 2. +0x1B4 points at a word
 * the window clears when it is destroyed.
 *
 * A portrait's roster index (+0x5C) maps to a character-sheet row through
 * the table the multiplayer start uses (0x19CB18); only rows 0-5, the story
 * characters, have story inventories, so those are the ones player 2 can
 * take. */
#define CS_SCRIPT       0x040000F9u
#define BTN_PORTRAIT    0x46000005u
#define PAD_A_GAME      0x2000u            /* the game's pad bits, as OnPress gets them */
#define XI_START        0x0010u
#define XI_BACK         0x0020u

void EXAlloc_000BD070(void);
void XHudScriptWnd_ctor_00052CF0(void);
void XHudScriptWnd_SetWindowOrder_00051810(void);
void XHudScriptWnd_RestrictToPad_00051A50(void);
void XHudScriptWnd_KillNextFrame_00050280(void);
void EXGeoFile_LoadGeoFile_000C69C0(void);
void EXGeoFile_DeLoadGeoFile_000C6B50(void);
uint16_t buffy_input_buttons(int port);
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

static uint32_t s_join_wnd;                /* the Character Select window while choosing */
static uint32_t s_join_owner;              /* guest word the window clears as it goes */
static int      s_join_pick = -1, s_join_cancel, s_join_closing;
static uint16_t s_pad2_prev;
static uint32_t s_xapp;                    /* the XApp (EXApp::MainUpdate's this) */
static int      s_join_change;             /* the page is changing player 2's character */
static int      s_join_who = 1;            /* the page is player 1's (0) or player 2's (1) */
static int      s_p1_action;
static int      s_p2_rebar;
static void health_bar_p2(void);               /* player 1 chose Change Character in their pause menu */
#define ALL_ROSTER_BITS 0x00FFFFFFu
static int      s_p2_pause, s_p2_action;   /* player 2's pause menu is up / what they chose */
enum { P2_NONE, P2_CHANGE, P2_DROP };
#define TYPE_CONTINUE   0x46000078u        /* pause menu lines */
#define TYPE_RESTART    0x46000031u
#define TYPE_OPTIONS    0x460000BDu
#define TYPE_QUIT       0x4600001Au
#define TYPE_P2_CHANGE  0x46FF0010u        /* ours: player 2's lines */
#define TYPE_COOP_LINE  0x46FF0020u        /* ours: the co-op page's lines */
#define TEXT_STYLE_GOTHIC 0x18u            /* template +0x84: the book menus' lettering (0x28 plain) */
static uint32_t s_hidden_wnd, s_hidden[16];  /* page artwork folded away while it draws */
static uint32_t s_hide_wnd, s_hide[8];      /* player 2's pause page: captions hidden while it draws */
static int s_hide_n;

/* A text line in the book menus' lettering: font 0x07000005 of the front
 * end's text file (template +0x94 / +0x98; 0x0100005C stands for the
 * language's front-end file). In play the Options page's lines take the
 * in-game text file, whose font is plain; the front-end files stay loaded
 * once player 2 has joined, and are loaded here if they are not. */
static uint32_t call_cdecl1_ret(void (*f)(void), uint32_t a)
{
    uint32_t esp0 = g_esp;
    g_esp -= 4; MEM32(g_esp) = a;
    g_esp -= 4; MEM32(g_esp) = 0;
    f();
    g_esp = esp0;
    return g_eax;
}

static void coop_gothic(uint32_t tmpl) { (void)tmpl; }   /* (the font is set on the made line: buffy_menu.c) */
static int s_hidden_n;
#define COOP_LINES      6
#define PAD_LEFT_GAME   0x44u               /* stick or d-pad */
#define PAD_RIGHT_GAME  0x88u
static int s_coop_page;                    /* the Options page being opened is the co-op page */
static int coop_line_press(uint32_t btn, int line, uint32_t mask);
static void coop_line_text(int line, wchar_t *out, size_t n);
#define PAUSE_PAGE      0x04000184u
#define OPTIONS_PAGE    0x0400039Bu
#define TYPE_P2_DROP    0x46FF0011u
void XApp_CreatePauseMenu_0002D640(void);
void XItemHandler_Player_SwapCharacter_0007B200(void);
void nv2a_gpu_show_second_window(int show);
static void join_finish(int row);
static void p1_swap_to(int row);
static void coop_drop_out(void);
static uint32_t s_mask_saved;
#define MC_CHAR_MASK      (0x26E638u + 0x2D8u)
#define STORY_ROSTER_BITS 0x0001100Fu     /* roster 0-3, 12, 16: rows 0-5 */

static void call_this0(void (*f)(void), uint32_t self)
{
    uint32_t esp0 = g_esp;
    g_esp -= 4; MEM32(g_esp) = 0;
    g_ecx = self;
    f();
    g_esp = esp0;
}

static void call_this1(void (*f)(void), uint32_t self, uint32_t a)
{
    uint32_t esp0 = g_esp;
    g_esp -= 4; MEM32(g_esp) = a;
    g_esp -= 4; MEM32(g_esp) = 0;
    g_ecx = self;
    f();
    g_esp = esp0;
}

static uint32_t call_cdecl2(void (*f)(void), uint32_t a, uint32_t b)
{
    uint32_t esp0 = g_esp;
    g_esp -= 4; MEM32(g_esp) = b;
    g_esp -= 4; MEM32(g_esp) = a;
    g_esp -= 4; MEM32(g_esp) = 0;
    f();
    g_esp = esp0;
    return g_eax;
}

static void fe_files(int load)
{
    uint32_t f[4];
    int i;
    f[0] = MEM32(0x1B7F68); f[1] = MEM32(0x1B7F78); f[2] = 0x01000043u; f[3] = 0x010000C0u;
    for (i = 0; i < 4; i++)
        call_cdecl2(load ? EXGeoFile_LoadGeoFile_000C69C0 : EXGeoFile_DeLoadGeoFile_000C6B50, f[i], 0);
}

static int hud_visible(void)
{
    uint32_t app = MEM32(0x26D868), hud = app ? MEM32(app + 0x230) : 0;
    return hud && MEM8(hud + 0x11) && MEM8(hud + 0x18);
}

static void join_open(void)
{
    uint32_t w, esp0, gp = MEM32(0x26EBB8);
    if (s_screens == 2 && s_join_who == 1)
        nv2a_gpu_enable_second_window();
    fe_files(1);
    if (!s_join_owner)
        s_join_owner = xbox_HeapAlloc(4, 4);
    w = call_cdecl2(EXAlloc_000BD070, 0x21C, 0);
    if (!w || !s_join_owner) {
        fe_files(0);
        return;
    }
    esp0 = g_esp;
    g_esp -= 4; MEM32(g_esp) = MEM32(0x1B7F68);            /* the front end's script file */
    g_esp -= 4; MEM32(g_esp) = CS_SCRIPT;
    g_esp -= 4; MEM32(g_esp) = 0;
    g_ecx = w;
    XHudScriptWnd_ctor_00052CF0();
    g_esp = esp0;
    MEM32(s_join_owner) = w;
    MEM32(w + 0x1B4) = s_join_owner;
    call_this1(XHudScriptWnd_SetWindowOrder_00051810, w, 0xC);
    {
        recomp_func_t update = recomp_lookup(MEM32(MEM32(w) + 0x24));   /* its Update, as the pause menu gets */
        if (update)
            call_this0(update, w);
    }
    call_this1(XHudScriptWnd_RestrictToPad_00051A50, w, s_join_who == 1 ? 1u : (gp ? MEM32(gp + 0xCA8) : 0u));
    s_join_wnd = w;
    s_join_pick = -1;
    if (s_screens == 2 && s_join_who == 1)
        nv2a_gpu_show_second_window(1);
    s_join_cancel = 0;
    s_join_closing = 0;
    /* The portraits show the characters the memcard manager's mask (0x26E638
     * +0x2D8, bit per roster index) has unlocked; it is only built in the
     * front end. The story characters are lit while the page is up. */
    s_mask_saved = MEM32(MC_CHAR_MASK);
    MEM32(MC_CHAR_MASK) |= ALL_ROSTER_BITS;                  /* every character, for either player */
    fprintf(stderr, "[MODS] co-op: player %d is choosing a character\n", s_join_who + 1);
}

/* At the start of each frame, outside the game's update. */
static void join_frame(void)
{
    uint16_t b, pressed;
    {
        /* BUFFY_COOP_SCREENS_AT=secs:n (testing): Screens changed to n. */
        static DWORD ts0;
        static int tsdone;
        void coop_set_screens_test(int v);
        const char *sa = getenv("BUFFY_COOP_SCREENS_AT");
        if (!ts0)
            ts0 = GetTickCount();
        if (sa && !tsdone && strchr(sa, ':') && GetTickCount() - ts0 > (DWORD)atoi(sa) * 1000) {
            tsdone = 1;
            coop_set_screens_test(atoi(strchr(sa, ':') + 1));
        }
    }
    {
        /* BUFFY_COOP_GOD=1 (testing): both players stay at full health. */
        int k;
        for (k = 0; k < 2 && getenv("BUFFY_COOP_GOD"); k++) {
            uint32_t it = MEM32(0x26DC54 + k * 4), hh = it ? MEM32(it + 0x14C) : 0;
            if (hh)
                MEM32(hh + 0x538) = MEM32(hh + 0x53C);
        }
    }
    {
        /* BUFFY_COOP_GIVE_P2=secs:item (testing): player 2 picks up an item. */
        static DWORD t0;
        static int done;
        const char *g = getenv("BUFFY_COOP_GIVE_P2");
        if (!t0)
            t0 = GetTickCount();
        if (g && !done && s_inv_2 && MEM32(0x26DC58) && GetTickCount() - t0 > (DWORD)atoi(g) * 1000 && strchr(g, ':')) {
            uint32_t inv2 = player_inventory(MEM32(0x26DC58));
            int id = (int)strtol(strchr(g, ':') + 1, NULL, 16);
            done = 1;
            if (inv2) {
                inv_set(inv2, id, (int16_t)(inv_count(inv2, id) < 0 ? 1 : inv_count(inv2, id) + 1));
                fprintf(stderr, "[MODS] co-op: (test) player 2 picked up item %02X\n", id);
            }
        }
    }
    coop_inventory_sync();
    if (s_p2_rebar && !--s_p2_rebar)
        health_bar_p2();                                  /* (the old bar's window went last frame) */
    if (s_join_wnd) {
        if (!MEM32(s_join_owner)) {
            /* The window has gone: unload its files; make player 2 if one
             * was picked. */
            s_join_wnd = 0;
            /* the front-end files stay loaded: unloading them loses text the
             * game looks up in play ("HashCode Not Found" on the pause page) */
            MEM32(MC_CHAR_MASK) = s_mask_saved;
            if (s_join_pick >= 0)
                join_finish(s_join_pick);
            else
                fprintf(stderr, "[MODS] co-op: player 2 backed out\n");
            return;
        }
        if ((s_join_pick >= 0 || s_join_cancel) && !s_join_closing) {
            s_join_closing = 1;
            call_this0(XHudScriptWnd_KillNextFrame_00050280, s_join_wnd);
        }
        return;
    }
    {
        /* BUFFY_COOP_KILL_P2=secs (testing): player 2 falls, once. */
        static DWORD t0;
        static int done;
        const char *k = getenv("BUFFY_COOP_KILL_P2");
        const char *k1 = getenv("BUFFY_COOP_KILL_P1");
        static int done1;
        if (!t0)
            t0 = GetTickCount();
        if (k1 && !done1 && MEM32(0x26DC54) && GetTickCount() - t0 > (DWORD)atoi(k1) * 1000) {
            void XItemHandler_Player_KillPlayer_00081850(void);
            done1 = 1;
            call_this1(XItemHandler_Player_KillPlayer_00081850, MEM32(MEM32(0x26DC54) + 0x14C), 0);
            fprintf(stderr, "[MODS] co-op: (test) player 1 killed\n");
        }
        if (!t0)
            t0 = GetTickCount();
        if (k && !done && MEM32(0x26DC58) && GetTickCount() - t0 > (DWORD)atoi(k) * 1000) {
            void XItemHandler_Player_KillPlayer_00081850(void);
            done = 1;
            call_this1(XItemHandler_Player_KillPlayer_00081850, MEM32(MEM32(0x26DC58) + 0x14C), 0);
            fprintf(stderr, "[MODS] co-op: (test) player 2 killed\n");
        }
    }
    if (s_p1_action && s_xapp && !MEM32(s_xapp + 0x84) && MEM32(0x26DC54)) {
        /* player 1 chose Change Character; the pause has ended */
        s_p1_action = 0;
        s_join_who = 0;
        s_join_change = 1;
        join_open();
        return;
    }
    if (!s_story_coop)
        return;
    b = buffy_input_buttons(1);
    pressed = (uint16_t)(b & ~s_pad2_prev);
    s_pad2_prev = b;
    if ((pressed & XI_BACK) && MEM32(0x26DC54) && MEM32(0x26DC58) && s_teleport_on_back) {
        /* Back: player 2 to player 1 -- for when they are stuck, or left
         * behind by a scripted scene. */
        coop_follow();
        fprintf(stderr, "[MODS] co-op: player 2 came to player 1 (Back)\n");
        return;
    }
    {
        uint32_t app = s_xapp;                  /* XApp: +0x84 its pause menu */
        if (app && !MEM32(app + 0x84)) {
            /* not paused: carry out what player 2 chose in their menu */
            int act = s_p2_action;
            s_coop_page = 0;
            s_p2_pause = 0;
            s_p2_action = 0;
            if (act == P2_DROP)
                coop_drop_out();
            else if (act == P2_CHANGE && MEM32(0x26DC58)) {
                s_join_change = 1;
                s_join_who = 1;
                join_open();
                return;
            }
        }
    }
    if ((pressed & XI_START) && getenv("BUFFY_MODS_LOG"))
        fprintf(stderr, "[MODS] co-op: pad 2 Start (xapp %08X +84 %08X, p2 %08X, hud %d)\n", s_xapp,
                s_xapp ? MEM32(s_xapp + 0x84) : 0, MEM32(0x26DC58), hud_visible());
    if ((pressed & XI_START) && s_xapp && !MEM32(s_xapp + 0x84) && MEM32(0x26DC58) && hud_visible()
            && !buffy_movie_active()) {
        /* Player 2 pauses. The game only opens the pause menu for player
         * 1's controller in a story level; this is its own call, for pad 1
         * (the wrapper below makes it player 2's menu). */
        call_this1(XApp_CreatePauseMenu_0002D640, s_xapp, 1);
        return;
    }
    if (!(pressed & XI_START))
        return;
    if (MEM32(0x26DC58) || !MEM32(0x26DC54) || !s_trig || in_multiplayer() || buffy_movie_active()
            || !hud_visible())
        return;
    s_join_change = 0;
    s_join_who = 1;
    join_open();
}

/* From XHudScriptButton::OnPress (buffy_menu.c): a press on the join page.
 * Returns 1 when handled here (the game's own handling is skipped). */
int buffy_coop_menu_press(uint32_t btn, uint32_t mask)
{
    static const uint8_t k_roster_row[24] = { 0, 3, 1, 2, 9, 13, 14, 8, 18, 22, 19, 11,
                                              4, 12, 17, 20, 5, 16, 10, 15, 21, 23, 7, 6 };
    uint32_t idx, type = MEM32(btn + 0x64);
    if (type == TYPE_OPTIONS)
        s_coop_page = 0;                                  /* the real Options page */
    if (type >= TYPE_COOP_LINE && type < TYPE_COOP_LINE + COOP_LINES)
        return coop_line_press(btn, (int)(type - TYPE_COOP_LINE), mask);
    if ((type == TYPE_P2_CHANGE || type == TYPE_P2_DROP || (s_p2_pause && type == TYPE_CONTINUE))
            && (mask & PAD_A_GAME) && s_p2_pause) {
        /* Player 2's pause menu: note what they chose and close the menu
         * (the pause ends with its window); it is carried out once play
         * resumes (join_frame). */
        s_p2_action = type == TYPE_P2_CHANGE ? P2_CHANGE : type == TYPE_P2_DROP ? P2_DROP : P2_NONE;
        if (MEM32(btn + 0x1C))
            call_this0(XHudScriptWnd_KillNextFrame_00050280, MEM32(btn + 0x1C));
        return 1;
    }
    if (!s_join_wnd || MEM32(btn + 0x1C) != s_join_wnd)
        return 0;
    if (MEM32(btn + 0x64) != BTN_PORTRAIT || !(mask & PAD_A_GAME))
        return 1;                                       /* nothing else on the page applies here */
    idx = MEM32(btn + 0x5C);
    /* player 2: anyone on the page; player 1: the story characters (the
     * game's character swap takes rows 0-5) */
    if (idx < 24 && s_join_pick < 0) {
        s_join_pick = k_roster_row[idx];
        fprintf(stderr, "[MODS] co-op: player %d picked roster %u (character row %d)\n", s_join_who + 1, idx,
                s_join_pick);
    }
    return 1;
}

/* From XHudScriptWnd::SetScript (buffy_menu.c): the join page moving to
 * another page is backing out (B). Returns 1 to skip the change. */
int buffy_coop_set_script(uint32_t wnd, uint32_t script)
{
    if (!s_join_wnd || wnd != s_join_wnd || script == CS_SCRIPT)
        return 0;
    s_join_cancel = 1;
    return 1;
}

/* At player 1's Swap: will player 2's frame follow? -1 no second window,
 * 0 no (show this frame on both), 1 yes. */
int buffy_coop_decide(void)
{
    uint32_t cam_item, h;
    s_split_last = 0;
    if (s_screens < 2 || !nv2a_gpu_has_second_window())
        return s_split_frame = 0, -1;
    s_split_frame = 0;
    if (s_join_wnd && !s_join_closing && !in_multiplayer() && s_join_who == 1) {
        s_split_frame = 2;                               /* window 2: the join page */
        return 1;
    }
    cam_item = MEM32(0x26DC64);
    h = cam_item ? MEM32(cam_item + 0x14C) : 0;
    if (s_split_off || !MEM32(0x26DC54) || !MEM32(0x26DC58) || !h || !MEM32(h + 0x410)
            || in_multiplayer() || buffy_movie_active() || MEM32(h + 0x400) != MEM32(h + 0x404))
        return 0;
    /* Only in play: the HUD (XApp +0x230, visible when +0x11 and +0x18 are
     * set -- XHudWnd::ElementsVisible) is hidden for cutscenes, which a
     * script camera films, and for the pause menu. Those show on both. */
    if (!hud_visible())
        return 0;
    s_split_frame = 1;
    s_split_last = 1;
    return 1;
}

static void cam_save(uint32_t h, uint32_t item, uint8_t *hb, uint8_t *eb, uint8_t *ib)
{
    memcpy(hb, (const void *)XBOX_PTR(h), CAM_HANDLER_SIZE);
    memcpy(eb, (const void *)XBOX_PTR(MEM32(h + 0x410)), EXCAM_SIZE);
    memcpy(ib, (const void *)XBOX_PTR(item + 0xAC), 0x30);
}

static void cam_load(uint32_t h, uint32_t item, const uint8_t *hb, const uint8_t *eb, const uint8_t *ib)
{
    uint32_t excam = MEM32(h + 0x410);
    memcpy((void *)XBOX_PTR(h), hb, CAM_HANDLER_SIZE);
    memcpy((void *)XBOX_PTR(excam), eb, EXCAM_SIZE);
    memcpy((void *)XBOX_PTR(item + 0xAC), ib, 0x30);
}

/* Draw and present the frame again, for window 2. */
static int s_pass2_any;                    /* anything of player 2's pass is running */

/* buffy_dsound.c: player 2's pass must not move the one listener or start
 * sounds (it re-runs the camera and the drawing, not the game). */
int buffy_coop_pass2_active(void)
{
    return s_pass2_any || s_in_pass2;
}

static void draw_again(uint32_t app)
{
    s_in_pass2 = 1;
    if (MEM8(0x1B987C))                               /* rendering on, as MainUpdate checks */
        call_this0(EXBaseApp_Render_000BDE00, app);
    call_this0(EXDisplay_Present_000D9880, MEM32(0x26F11C));
    s_in_pass2 = 0;
}

/* ── keeping the two cameras apart ────────────────────────────────────────
 *
 * There is one camera (item 0x26DC64); player 2's is a snapshot swapped in
 * whenever player 2 is the one using it:
 *   - its update, in player 2's pass (coop_pass2);
 *   - player 2's own update (XItemHandler_Player::DoUpdate, below): the
 *     stick turns into a direction by adding the camera's heading (camera item
 *     +0xC0, GetAnalogStickSector), and player actions call into the camera
 *     (lock-on, first person), which must reach player 2's camera, not 1's.
 * The camera reads its controller through the player-0 entry of the pad map
 * (XGamePad +0xCA8: right stick for looking, the centring button), so during
 * player 2's camera update the map's entries 0 and 1 are swapped too.
 *
 * The handler owns a few buffers (k_cam_ptrs); both snapshots share player
 * 1's. If a swapped-in update changes them (a mode that allocates), the two
 * states can no longer be kept apart safely: the camera is left as it is
 * (never put back to pointers that may have been freed) and both windows show
 * the one camera from then on (s_split_off). */
static uint8_t  s_cam1[CAM_HANDLER_SIZE], s_excam1[EXCAM_SIZE], s_camitem1[0x30];
static uint32_t s_cam_ptrs_live[6];

static void cam_enter_p2(uint32_t h, uint32_t item)
{
    int i;
    cam_save(h, item, s_cam1, s_excam1, s_camitem1);
    for (i = 0; i < 6; i++)
        s_cam_ptrs_live[i] = MEM32(h + k_cam_ptrs[i]);
    if (s_cam2_valid) {
        for (i = 0; i < 6; i++)
            memcpy(s_cam2 + k_cam_ptrs[i], &s_cam_ptrs_live[i], 4);
        cam_load(h, item, s_cam2, s_excam2, s_camitem2);
    }
}

/* Returns 0 if the two cameras could not be kept apart. */
static int cam_leave_p2(uint32_t h, uint32_t item, const char *who)
{
    int i, bad = 0;
    for (i = 0; i < 6; i++)
        bad |= MEM32(h + k_cam_ptrs[i]) != s_cam_ptrs_live[i];
    if (bad) {
        fprintf(stderr, "[MODS] co-op: the camera changed mode in %s; one camera for both screens from here\n", who);
        s_split_off = 1;
        return 0;
    }
    cam_save(h, item, s_cam2, s_excam2, s_camitem2);
    s_cam2_valid = 1;
    cam_load(h, item, s_cam1, s_excam1, s_camitem1);
    return 1;
}

static void coop_pass2(uint32_t app)
{
    uint32_t cam_item = MEM32(0x26DC64), h = MEM32(cam_item + 0x14C), gp = MEM32(0x26EBB8);
    uint32_t p1 = MEM32(0x26DC54), p2 = MEM32(0x26DC58);
    uint32_t regs[4] = { g_eax, g_ebx, g_esi, g_edi }, map0 = 0, map1 = 0;
    int ok;

    s_pass2_any = 1;
    cam_enter_p2(h, cam_item);
    MEM32(0x26DC54) = p2;                         /* the story camera follows slot 0 */
    MEM32(0x26DC58) = p1;
    if (gp) {
        map0 = MEM32(gp + 0xCA8);                 /* ...and reads slot 0's controller */
        map1 = MEM32(gp + 0xCAC);
        MEM32(gp + 0xCA8) = map1;
        MEM32(gp + 0xCAC) = map0;
    }
    call_this0(XItemHandler_Camera_DoUpdate_0003B930, h);
    if (gp) {
        MEM32(gp + 0xCA8) = map0;
        MEM32(gp + 0xCAC) = map1;
    }
    MEM32(0x26DC54) = p1;
    MEM32(0x26DC58) = p2;
    {
        /* draw with player 2's camera, then keep it */
        int i, bad = 0;
        for (i = 0; i < 6; i++)
            bad |= MEM32(h + k_cam_ptrs[i]) != s_cam_ptrs_live[i];
        if (!bad) {
            hud_for_p2(1);
            draw_again(app);
            hud_for_p2(0);
        }
    }
    ok = cam_leave_p2(h, cam_item, "player 2's pass");
    s_pass2_any = 0;
    (void)ok;
    g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
}

/* char XItemHandler_Player::DoUpdate() -- wrapped: player 2 plays with
 * player 2's camera. */
void XItemHandler_Player_DoUpdate_0007C5C0_orig(void);
void XItemHandler_Player_DoUpdate_0007C5C0(void)
{
    uint32_t h = g_ecx, cam_item = MEM32(0x26DC64), ch, eax;
    uint32_t p2 = MEM32(0x26DC58);
    if (!s_story_coop || s_screens < 2 || s_split_off || !s_cam2_valid || !p2 || MEM32(p2 + 0x14C) != h
            || !cam_item || !(ch = MEM32(cam_item + 0x14C)) || !MEM32(ch + 0x410) || in_multiplayer()
            || !nv2a_gpu_has_second_window()) {
        XItemHandler_Player_DoUpdate_0007C5C0_orig();
        return;
    }
    {
        static DWORD last;
        float before, seen, after;
        int log = getenv("BUFFY_COOP_LOG") && GetTickCount() - last >= 500;
        memcpy(&before, (const void *)XBOX_PTR(cam_item + 0xC0), 4);
        cam_enter_p2(ch, cam_item);
        memcpy(&seen, (const void *)XBOX_PTR(cam_item + 0xC0), 4);
        g_ecx = h;
        XItemHandler_Player_DoUpdate_0007C5C0_orig();
        eax = g_eax;
        cam_leave_p2(ch, cam_item, "player 2's update");
        g_eax = eax;
        memcpy(&after, (const void *)XBOX_PTR(cam_item + 0xC0), 4);
        if (log) {
            last = GetTickCount();
            fprintf(stderr, "[COOP] player 2 update: camera 1 heading %.3f, player 2 moved by heading %.3f, camera 1 after %.3f\n",
                    before, seen, after);
        }
    }
}

/* char EXApp::MainUpdate() -- wrapped: joining, and player 2's frame after
 * player 1's. */
void EXApp_MainUpdate_000BD240(void)
{
    uint32_t app = g_ecx, eax, regs[4];
    s_xapp = app;
    if ((s_story_coop || s_p1_change) && !in_multiplayer()) {
        regs[0] = g_ebx; regs[1] = g_esi; regs[2] = g_edi; regs[3] = g_ecx;
        join_frame();
        g_ebx = regs[0]; g_esi = regs[1]; g_edi = regs[2]; g_ecx = regs[3];
    }
    {
        /* A player choosing a character: their pad-map entry (XGamePad
         * +0xCA8 + player * 4) is pointed at an unused pad for the game's
         * update, so their character and camera stand still; the page reads
         * its controller directly (RestrictToPad). */
        uint32_t gp = MEM32(0x26EBB8), slot = 0, saved = 0;
        int who = s_join_wnd && !s_join_closing && (s_join_who == 0 || MEM32(0x26DC58)) ? s_join_who : -1;
        if (gp && who >= 0) {
            slot = gp + 0xCA8 + (uint32_t)who * 4;
            saved = MEM32(slot);
            MEM32(slot) = 3;
        }
        EXApp_MainUpdate_000BD240_orig();
        if (slot)
            MEM32(slot) = saved;
    }
    if (!s_split_frame)
        return;
    eax = g_eax;
    if (s_split_frame == 2 && s_join_wnd) {
        /* Window 2 while player 2 chooses: the same view, with the page. */
        draw_again(app);
    } else if (s_split_frame == 1)
        coop_pass2(app);
    s_split_frame = 0;
    g_eax = eax;
}

/* char EXBaseDisplay::RedrawWindow(EXBaseWnd *) -- wrapped: with two screens
 * the join page is drawn only in window 2's pass. (The window's own "hidden"
 * bit, +0x64 & 0x8000, would stop its updates as well.) */
void EXBaseDisplay_RedrawWindow_000D7290_orig(void);
void EXBaseDisplay_RedrawWindow_000D7290(void)
{
    if (s_join_wnd && MEM32(g_esp + 4) == s_join_wnd && s_screens >= 2 && nv2a_gpu_has_second_window()
            && (s_join_who == 1 ? !s_in_pass2 : s_in_pass2)) {
        g_eax = 1;
        g_esp += 4;                                 /* cdecl: just the return */
        return;
    }
    /* (both health bars show in both windows, player 1's over player 2's,
     * so each can keep an eye on the other) */
    EXBaseDisplay_RedrawWindow_000D7290_orig();
}

/* ── player 2 dying ───────────────────────────────────────────────────────
 *
 * A player at no health goes to game mode 0x2F and runs
 * XItemHandler_Player::HandlePlayerDeath each frame. In a story level that
 * counts +0x70C to 120 frames and opens the game-over page; in multiplayer
 * it waits 300 and brings the player back where they are (SetFadeIn, then
 * InitializeSpecificPlayer(1): health, state, weapons). Player 2 gets the
 * multiplayer way, beside player 1, after P2_RESPAWN_FRAMES -- player 2
 * going down never ends player 1's game. */
#define P2_RESPAWN_FRAMES (s_respawn_secs * 60)

void XItemHandler_Player_HandlePlayerDeath_00082BA0_orig(void);
void XItemCharacterHandler_SetFadeIn_000AA790(void);
void XItemHandler_Player_InitializeSpecificPlayer_0007A5D0(void);

void XItemHandler_Player_HandlePlayerDeath_00082BA0(void)
{
    uint32_t h = g_ecx, item = MEM32(h + 4), t, regs[3];
    if (!s_story_coop || in_multiplayer() || !item || item != MEM32(0x26DC58)) {
        XItemHandler_Player_HandlePlayerDeath_00082BA0_orig();
        return;
    }
    g_esp += 4;                                     /* ret */
    t = MEM32(h + 0x70C);
    if (t < P2_RESPAWN_FRAMES) {
        MEM32(h + 0x70C) = t + 1;
        return;
    }
    if (!MEM32(0x26DC54))
        return;
    regs[0] = g_ebx; regs[1] = g_esi; regs[2] = g_edi;
    MEM32(h + 0x70C) = 0;
    coop_follow();
    call_this0(XItemCharacterHandler_SetFadeIn_000AA790, h);
    call_this1(XItemHandler_Player_InitializeSpecificPlayer_0007A5D0, h, 1);
    g_ebx = regs[0]; g_esi = regs[1]; g_edi = regs[2];
    fprintf(stderr, "[MODS] co-op: player 2 is back beside player 1\n");
}

/* BUFFY_COOP_LOG: both cameras' headings (camera item +0xC0; player 2's from
 * its snapshot) and modes, beside the positions. */
static void buffy_coop_log_cameras(void)
{
    uint32_t ci = MEM32(0x26DC64), h = ci ? MEM32(ci + 0x14C) : 0, m2 = 0;
    float y1, y2 = 0;
    if (!h)
        return;
    memcpy(&y1, (const void *)XBOX_PTR(ci + 0xC0), 4);
    if (s_cam2_valid) {
        memcpy(&y2, s_camitem2 + (0xC0 - 0xAC), 4);
        memcpy(&m2, s_cam2 + 0x404, 4);
    }
    fprintf(stderr, "[COOP] cam1 yaw %.3f mode %u | cam2 yaw %.3f mode %u%s\n", y1, MEM32(h + 0x404), y2, m2,
            s_split_off ? " (split off)" : "");
    {
        /* each player's sticks, through the pad map (left +0x84/+0x88, right +0x8C/+0x90) */
        uint32_t gp = MEM32(0x26EBB8);
        int i;
        for (i = 0; gp && i < 2; i++) {
            int pad = (int)MEM32(gp + 0xCA8 + i * 4);
            float v[4] = { 0, 0, 0, 0 };
            if (pad >= 0 && pad < 4)
                memcpy(v, (const void *)XBOX_PTR(gp + pad * 64 + 0x84), 16);
            fprintf(stderr, "[COOP] player %d pad %d left %.2f %.2f right %.2f %.2f\n", i + 1, pad, v[0], v[1], v[2],
                    v[3]);
        }
    }
}

/* BUFFY_ITEM_DUMP=secs (testing): once, every item's group and both players'
 * counts. */
void buffy_coop_item_dump(void)
{
    static DWORD t0;
    static int done;
    const char *e = getenv("BUFFY_ITEM_DUMP");
    uint32_t tab = MEM32(0x1B8010), i1 = 0, i2 = 0, p;
    int id;
    if (!e || done)
        return;
    if (!t0)
        t0 = GetTickCount();
    if (GetTickCount() - t0 < (DWORD)atoi(e) * 1000 || !MEM32(0x26DC58))
        return;
    done = 1;
    p = MEM32(0x26DC54);
    if (p && MEM32(p + 0x14C))
        i1 = MEM32(p + 0x14C) + 0x9A4 + (MEM32(p + 0x16C) > 5 ? 0 : MEM32(p + 0x16C)) * 0x1B04;
    p = MEM32(0x26DC58);
    if (p && MEM32(p + 0x14C))
        i2 = MEM32(p + 0x14C) + 0x9A4 + (MEM32(p + 0x16C) > 5 ? 0 : MEM32(p + 0x16C)) * 0x1B04;
    for (id = 1; id < 0x9C; id++) {
        uint32_t it = tab + id * 0x1F0;
        fprintf(stderr, "[ITEM] %02X group %u b190 %02X %02X %02X %02X p1 %d p2 %d hash %08X\n", id, MEM8(it + 0x192),
                MEM8(it + 0x190), MEM8(it + 0x191), MEM8(it + 0x193), MEM8(it + 0x194),
                i1 ? (int16_t)MEM16(i1 + id * 0x2C + 0x28) : -9, i2 ? (int16_t)MEM16(i2 + id * 0x2C + 0x28) : -9,
                MEM32(it));
    }
}

/* ── one inventory, two hands ─────────────────────────────────────────────
 *
 * Each player carries their own XInventory (handler +0x9A4 + row * 0x1B04:
 * +0 the selected item, +4 the equipped one, item n's count at +0x28 + n *
 * 0x2C, -1 when not held). The story asks player 1's (XGetInventory(0)) --
 * a door checks player 1 for its key -- and the HUD shows player 1's.
 *
 * So the contents are shared: once a frame each player's change since the
 * last frame is applied to both (count = last + change 1 + change 2), with
 * the game's own XInventory::AddItemToInventory / RemoveItemFromInventory.
 * Selection stays each player's own. Items that belong to one character are
 * left alone: item group (item table [0x1B8010] + id * 0x1F0 + 0x192) 0 the
 * fists, 4 each character's own weapon, 6 Willow's spells. When player 2
 * joins (or either changes character) player 2 takes player 1's items. */
#define INV_ITEMS 0x9C

void XInventory_RemoveItemFromInventory_00056B50(void);

static int16_t  s_inv_base[INV_ITEMS];

static uint32_t player_inventory(uint32_t item)
{
    uint32_t h = item ? MEM32(item + 0x14C) : 0, row;
    if (!h)
        return 0;
    row = MEM32(item + 0x16C);
    return h + 0x9A4 + (row > 5 ? 0 : row) * 0x1B04;
}

static int inv_shared(int id)
{
    uint8_t g = MEM8(MEM32(0x1B8010) + id * 0x1F0 + 0x192);
    return g != 0 && g != 4 && g != 6;
}

static int16_t inv_count(uint32_t inv, int id) { return (int16_t)MEM16(inv + id * 0x2C + 0x28); }

static void inv_set(uint32_t inv, int id, int16_t want)
{
    int16_t cur = inv_count(inv, id);
    uint32_t esp0 = g_esp;
    if (want == cur)
        return;
    if (want < 0) {
        g_esp -= 4; MEM32(g_esp) = 0;
        g_esp -= 4; MEM32(g_esp) = (uint32_t)id;
        g_esp -= 4; MEM32(g_esp) = 0;
        g_ecx = inv;
        XInventory_RemoveItemFromInventory_00056B50();
    } else if (want > cur) {
        g_esp -= 4; MEM32(g_esp) = 0xC2C80000u;            /* -100.0f, as the game passes */
        g_esp -= 4; MEM32(g_esp) = (uint32_t)(uint16_t)(want - (cur < 0 ? 0 : cur));
        g_esp -= 4; MEM32(g_esp) = (uint32_t)id;
        g_esp -= 4; MEM32(g_esp) = 0;
        g_ecx = inv;
        XInventory_AddItemToInventory_00057630();
    }
    g_esp = esp0;
    if (inv_count(inv, id) != want)
        MEM16(inv + id * 0x2C + 0x28) = (uint16_t)want;   /* used up, or past the add's cap */
}

static void coop_inventory_sync(void)
{
    uint32_t inv1 = player_inventory(MEM32(0x26DC54)), inv2 = player_inventory(MEM32(0x26DC58));
    uint32_t regs[4] = { g_eax, g_ebx, g_esi, g_edi };
    int id, changed = 0;
    if (!inv1 || !inv2 || !s_share_inventory) {
        s_inv_1 = s_inv_2 = 0;
        return;
    }
    if (inv1 != s_inv_1 || inv2 != s_inv_2) {
        /* new pairing: player 2 takes player 1's items */
        for (id = 1; id < INV_ITEMS; id++)
            if (inv_shared(id)) {
                s_inv_base[id] = inv_count(inv1, id);
                inv_set(inv2, id, s_inv_base[id]);
            }
        s_inv_1 = inv1;
        s_inv_2 = inv2;
        fprintf(stderr, "[MODS] co-op: player 2 shares player 1's inventory\n");
    } else {
        for (id = 1; id < INV_ITEMS; id++) {
            int16_t b, c1, c2, n;
            if (!inv_shared(id))
                continue;
            b = s_inv_base[id];
            c1 = inv_count(inv1, id);
            c2 = inv_count(inv2, id);
            if (c1 == b && c2 == b)
                continue;
            n = (int16_t)(b + (c1 - b) + (c2 - b));
            if (n < -1)
                n = -1;
            if (n == 0 && (c1 < 0 || c2 < 0))
                n = -1;                                    /* one used the last: gone for both */
            inv_set(inv1, id, n);
            inv_set(inv2, id, n);
            s_inv_base[id] = inv_count(inv1, id);
            changed++;
            if (getenv("BUFFY_MODS_LOG"))
                fprintf(stderr, "[MODS] co-op: item %02X %d -> %d (player 1 %d, player 2 %d)\n", id, b, s_inv_base[id],
                        c1, c2);
        }
    }
    (void)changed;
    g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
}

/* XInventory *XGetInventory(unsigned char player) -- wrapped: while window 2's
 * HUD is brought up to date, "player 1's inventory" is player 2's. */
void XGetInventory_00025980_orig(void);
void XGetInventory_00025980(void)
{
    if (s_hud_p2 && MEM8(g_esp + 4) == 0)
        MEM32(g_esp + 4) = 1;
    XGetInventory_00025980_orig();
}

/* Window 2's HUD: the inventory window (XHudWnd +0x50) updated once more,
 * reading player 2's inventory, just before window 2 is drawn. */
static void hud_for_p2(int on)
{
    uint32_t app = MEM32(0x26D868), hud = app ? MEM32(app + 0x230) : 0, w = hud ? MEM32(hud + 0x50) : 0;
    s_hud_p2 = on;
    if (on && w && s_hud_p2_update) {
        recomp_func_t update = recomp_lookup(MEM32(MEM32(w) + 0x1C));
        if (update)
            call_this0(update, w);
    }
}

/* buffy_input.c: while player 2 is in a story level, controller 2's Back is
 * ours (it brings player 2 to player 1) and the game does not see it. */
int buffy_coop_owns_back(void)
{
    return s_story_coop && s_teleport_on_back && MEM32(0x26DC58) && !in_multiplayer();
}

/* ── player 2 in and out ──────────────────────────────────────────────── */

static void health_bar_p2(void)
{
    void XHudWnd_SetupPlayerHealthBar_000548C0(void);
    uint32_t app = MEM32(0x26D868), hud = app ? MEM32(app + 0x230) : 0, esp0 = g_esp;
    if (!hud || MEM32(hud + 0x24) || !MEM32(0x26DC58))
        return;
    g_esp -= 4; MEM32(g_esp) = 1;
    g_esp -= 4; MEM32(g_esp) = MEM32(0x26DC58);
    g_esp -= 4; MEM32(g_esp) = 0;
    g_ecx = hud;
    XHudWnd_SetupPlayerHealthBar_000548C0();
    g_esp = esp0;
}

/* Player 2 becomes character `row` (0-5): the game's own character swap. The
 * old model's file is unloaded only if player 1 is not using it. */
static void p2_respawn_as(int row);

static void p2_swap_to(int row)
{
    uint32_t p2 = MEM32(0x26DC58), h = p2 ? MEM32(p2 + 0x14C) : 0, esp0 = g_esp;
    uint32_t old = p2 ? MEM32(p2 + 0x16C) : 0, p1 = MEM32(0x26DC54);
    int unload = p1 && MEM32(p1 + 0x16C) != old;
    if (!h || old == (uint32_t)row)
        return;
    if (row > 5 || old > 5) {
        p2_respawn_as(row);                  /* the swap takes story characters only */
        return;
    }
    g_esp -= 4; MEM32(g_esp) = (uint32_t)unload;
    g_esp -= 4; MEM32(g_esp) = (uint32_t)(row + 1);
    g_esp -= 4; MEM32(g_esp) = 0;
    g_ecx = h;
    XItemHandler_Player_SwapCharacter_0007B200_orig();
    g_esp = esp0;
    fprintf(stderr, "[MODS] co-op: player 2 is now character %d\n", (int)MEM32(p2 + 0x16C));
}

static void join_finish(int row)
{
    uint32_t regs[4] = { g_eax, g_ebx, g_esi, g_edi };
    if (s_join_who == 0) {
        p1_swap_to(row);                                     /* player 1's Change Character */
        s_join_who = 1;
        s_join_change = 0;
        g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
        return;
    }
    if (s_join_change && MEM32(0x26DC58)) {
        p2_swap_to(row);                                     /* Change Character */
        s_p2_row = row;
    } else if (s_trig && MEM32(0x26DC54) && !MEM32(0x26DC58)) {
        s_p2_row = row;
        coop_spawn(s_trig, s_trig_args[0], s_trig_args[1], s_trig_args[2], row);
        if (MEM32(0x26DC58)) {
            coop_follow();
            health_bar_p2();
        }
    }
    s_join_change = 0;
    g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
}

/* Player 2 as a character the game's swap does not take (the multiplayer
 * ones): the old character removed (as Drop Out does) and a new one made
 * where they stood. */
static void p2_respawn_as(int row)
{
    uint32_t p2 = MEM32(0x26DC58), app = MEM32(0x26D868), hud = app ? MEM32(app + 0x230) : 0, place[8];
    int i;
    if (!p2 || !s_trig || !MEM32(0x26DC54))
        return;
    for (i = 0; i < 8; i++)
        place[i] = MEM32(p2 + 0xAC + i * 4);              /* position, facing */
    if (hud && MEM32(hud + 0x24))
        call_this0(XHudScriptWnd_KillNextFrame_00050280, MEM32(hud + 0x24));
    MEM8(p2 + 0x10) |= 0x10;                              /* the world deletes it */
    MEM8(p2 + 0x69) = 0;
    MEM32(0x26DC58) = 0;
    coop_spawn(s_trig, s_trig_args[0], s_trig_args[1], s_trig_args[2], row);
    p2 = MEM32(0x26DC58);
    if (!p2)
        return;
    for (i = 0; i < 8; i++)
        MEM32(p2 + 0xAC + i * 4) = place[i];
    MEM8(p2 + 0x69) = 0;
    s_cam2_valid = 0;
    s_inv_1 = s_inv_2 = 0;
    s_p2_rebar = 2;                                       /* their health bar again once the old one has gone */
    fprintf(stderr, "[MODS] co-op: player 2 is now character %d (made again)\n", (int)MEM32(p2 + 0x16C));
}

/* The model file (geometry hash) of character-sheet row `row`: the sheet
 * the player start reads (0x14000007 in file 0x01000055), 0x1C bytes a row,
 * the hash first. Read once. */
void XSpreadSheet_ctor_000BBFB0(void);
void XSpreadSheet_LoadSpreadSheet_000BC1F0(void);
void XSpreadSheet_GetDataSheet_000BC060(void);
void XItemHandler_Player_InitializeSpecificPlayer_0007A5D0(void);
#define STORY_MODELS 0x1B82C0u                 /* the six story characters' model files */

static uint32_t sheet_model(int row)
{
    static uint32_t models[32];
    static int read;
    if (!read) {
        uint32_t obj = xbox_HeapAlloc(16, 4), esp0 = g_esp, data;
        int r;
        read = 1;
        if (!obj)
            return 0;
        call_this0(XSpreadSheet_ctor_000BBFB0, obj);
        g_esp -= 4; MEM32(g_esp) = 0x14000007u;
        g_esp -= 4; MEM32(g_esp) = 0x01000055u;
        g_esp -= 4; MEM32(g_esp) = 0;
        g_ecx = obj;
        XSpreadSheet_LoadSpreadSheet_000BC1F0();
        g_esp = esp0;
        call_this1(XSpreadSheet_GetDataSheet_000BC060, obj, 0);
        data = g_eax;
        for (r = 0; data && r < 24; r++)
            models[r] = MEM32(data + (uint32_t)r * 0x1C);
    }
    return row >= 0 && row < 24 ? models[row] : 0;
}

/* Player 1 becomes character `row` (any of the 24). The game's swap
 * (XItemHandler_Player::SwapCharacter) loads the new model from the story
 * table (0x1B82C0, rows 0-5) and re-initialises the player; for a
 * multiplayer character, or back from one, a story slot other than the
 * current one is lent the wanted model for the call, then the player's row
 * (+0x16C) is set and the player initialised again. The old model file is
 * unloaded when player 2 is not using it. */
static void p1_swap_to(int row)
{
    uint32_t p1 = MEM32(0x26DC54), h = p1 ? MEM32(p1 + 0x14C) : 0, esp0, p2 = MEM32(0x26DC58);
    uint32_t old = p1 ? MEM32(p1 + 0x16C) : 0, cur = old > 5 ? 0 : old, k, lent = 0, want;
    int other_has_old = p2 && MEM32(p2 + 0x16C) == old, plain = old <= 5 && row <= 5;
    if (!h || old == (uint32_t)row || row < 0 || row > 23)
        return;
    want = row <= 5 ? MEM32(STORY_MODELS + (uint32_t)row * 4) : sheet_model(row);
    if (!want)
        return;
    k = (uint32_t)row;
    if (!plain) {
        k = row <= 5 && (uint32_t)row != cur ? (uint32_t)row : (cur == 1 ? 2u : 1u);
        lent = MEM32(STORY_MODELS + k * 4);
        MEM32(STORY_MODELS + k * 4) = want;
    }
    esp0 = g_esp;
    g_esp -= 4; MEM32(g_esp) = (uint32_t)(plain && !other_has_old);   /* unload: only the plain swap */
    g_esp -= 4; MEM32(g_esp) = k + 1;
    g_esp -= 4; MEM32(g_esp) = 0;
    g_ecx = h;
    XItemHandler_Player_SwapCharacter_0007B200_orig();
    g_esp = esp0;
    if (!plain) {
        MEM32(STORY_MODELS + k * 4) = lent;
        if (MEM32(p1 + 0x16C) != (uint32_t)row) {
            MEM32(p1 + 0x16C) = (uint32_t)row;
            call_this1(XItemHandler_Player_InitializeSpecificPlayer_0007A5D0, h, 0);
        }
        if (!other_has_old) {
            uint32_t oldm = old <= 5 ? MEM32(STORY_MODELS + old * 4) : sheet_model((int)old);
            if (oldm && oldm != want)
                call_cdecl2(EXGeoFile_DeLoadGeoFile_000C6B50, oldm, 0);
        }
    }
    s_inv_1 = s_inv_2 = 0;
    fprintf(stderr, "[MODS] player 1 is now character %d\n", (int)MEM32(p1 + 0x16C));
}

/* buffy_menu.c: player 1's pause menu has Change Character (its own mod). */
int buffy_coop_p1_change_line(void)
{
    return s_p1_change && !in_multiplayer() && !s_p2_pause && MEM32(0x26DC54);
}

/* ...and it was chosen: the pause closes, then the page opens. */
void buffy_coop_p1_change_pressed(uint32_t btn)
{
    s_p1_action = 1;
    if (MEM32(btn + 0x1C))
        call_this0(XHudScriptWnd_KillNextFrame_00050280, MEM32(btn + 0x1C));
}

/* Drop Out: player 2 leaves. Their character is removed the way multiplayer
 * removes an eliminated player (item +0x10 bit 0x10: the world deletes it)
 * and taken off the player table first, so nothing follows, targets or
 * shares with them; window 2 is hidden. Joining again makes a new one. */
static void coop_drop_out(void)
{
    uint32_t p2 = MEM32(0x26DC58), gp = MEM32(0x26EBB8), app = MEM32(0x26D868);
    uint32_t hud = app ? MEM32(app + 0x230) : 0;
    if (!p2)
        return;
    if (hud && MEM32(hud + 0x24))
        call_this0(XHudScriptWnd_KillNextFrame_00050280, MEM32(hud + 0x24));   /* their health bar */
    MEM8(p2 + 0x10) |= 0x10;
    MEM8(p2 + 0x69) = 0;
    MEM32(0x26DC58) = 0;
    if (gp)
        MEM32(gp + 0xCA8 + 4) = 0xFFFFFFFFu;
    s_p2_row = -1;
    s_cam2_valid = 0;
    s_inv_1 = s_inv_2 = 0;
    nv2a_gpu_show_second_window(0);
    fprintf(stderr, "[MODS] co-op: player 2 dropped out\n");
}

/* From XHudScriptWnd::AddButton (buffy_menu.c), for each line a page adds:
 * player 2's pause menu is the game's with its lines rewritten -- Restart
 * Level becomes Change Character, Options becomes Drop Out, Quit Game goes.
 * Returns 0 to add the line as it is, 1 to leave it out, 2 when rewritten
 * (its text: *text). */
int buffy_coop_pause_button(uint32_t wnd, uint32_t tmpl, const wchar_t **text)
{
    static uint32_t page;
    static int n;
    uint32_t type = MEM32(tmpl + 0x64), flags = MEM32(tmpl + 0x68);
    if (s_p2_pause && MEM32(wnd + 0x174) == PAUSE_PAGE) {
        /* Player 2's pause menu: the pause page, with Restart Level as
         * Change Character and Options as Drop Out, in the page's lettering,
         * and Quit Game gone. Those lines' captions are the page's own
         * artwork (their animators, objects of the page's scene): hidden
         * while it draws (XHudScriptWnd::Draw). */
        uint32_t type0 = MEM32(tmpl + 0x64);
        if (type0 != TYPE_RESTART && type0 != TYPE_OPTIONS && type0 != TYPE_QUIT)
            return 0;
        if (s_hide_wnd != wnd) {
            s_hide_wnd = wnd;
            s_hide_n = 0;
        }
        if (MEM32(tmpl + 0x24) && s_hide_n < 8)
            s_hide[s_hide_n++] = MEM32(tmpl + 0x24);
        if (type0 == TYPE_QUIT)
            return 1;
        MEM32(tmpl + 0x64) = type0 == TYPE_RESTART ? TYPE_P2_CHANGE : TYPE_P2_DROP;
        MEM32(tmpl + 0x68) = (MEM32(tmpl + 0x68) & ~0x3u) | 0x4u | 0x8000u | 0x40000u;   /* text, fitted, centred */
        MEM32(tmpl + 0x6C) = 0;
        MEM32(tmpl + 0x70) = 0;
        *text = type0 == TYPE_RESTART ? L"      Change Character" : L"      Drop Out";
        return 2;
    }
    if (MEM32(wnd + 0x174) != OPTIONS_PAGE || (!s_p2_pause && !s_coop_page))
        return 0;
    if (page != wnd) {
        page = wnd;
        n = 0;
        s_hidden_n = 0;
    }
    if ((flags & 0x00100000u) && (flags & 0x4u)) {
        /* a selectable text line: the first three are player 2's */
        static const uint32_t types[3] = { TYPE_CONTINUE, TYPE_P2_CHANGE, TYPE_P2_DROP };
        static const wchar_t *labels[3] = { L"Continue", L"Change Character", L"Drop Out" };
        if (n >= (s_p2_pause ? 3 : COOP_LINES)) {
            if (MEM32(tmpl + 0x24) && s_hidden_n < 16) {
                s_hidden_wnd = wnd;
                s_hidden[s_hidden_n++] = MEM32(tmpl + 0x24);
            }
            return 1;
        }
        if (!s_p2_pause) {
            /* player 1's co-op page */
            static wchar_t text_buf[COOP_LINES][64];
            coop_line_text(n, text_buf[n], 64);
            MEM32(tmpl + 0x64) = TYPE_COOP_LINE + n;
            MEM32(tmpl + 0x68) &= ~0x3u;
            MEM32(tmpl + 0x6C) = 0;
            MEM32(tmpl + 0x70) = 0;
            MEM32(tmpl + 0x9C) = 0;
            MEM32(tmpl + 0x5C) = 0;
            MEM32(tmpl + 0x60) = 0;
            MEM32(tmpl + 0x7C) |= PAD_A_GAME | PAD_LEFT_GAME | PAD_RIGHT_GAME;
            coop_gothic(tmpl);
            *text = text_buf[n++];
            return 2;
        }
        MEM32(tmpl + 0x64) = types[n];
        MEM32(tmpl + 0x68) &= ~0x3u;                      /* no jump, no popup */
        MEM32(tmpl + 0x6C) = 0;
        MEM32(tmpl + 0x70) = 0;
        MEM32(tmpl + 0x9C) = 0;
        MEM32(tmpl + 0x5C) = 0;
        MEM32(tmpl + 0x60) = 0;
        MEM32(tmpl + 0x7C) |= PAD_A_GAME;                 /* option lines take only Left / Right */
        coop_gothic(tmpl);
        *text = labels[n++];
        return 2;
    }
    if (type == 0x46000087u || type == 0x46000088u) {
        if (MEM32(tmpl + 0x24) && s_hidden_n < 16) {
            s_hidden_wnd = wnd;
            s_hidden[s_hidden_n++] = MEM32(tmpl + 0x24);   /* one of the page's objects (Draw walks from it) */
        }
        return 1;                                         /* the volume bars */
    }                                         /* the volume bars: added, not drawn (buffy_coop_hide_line) */
    if (type == 0 || (type >= 0x460000BFu && type <= 0x460000C3u) || (flags & 0x00100000u)) {
        /* the options' headings, values, volume bars and further lines: left
         * out -- and as their animators are part of the page's scene (the
         * bars' frames are drawn from them), those are folded away while the
         * page draws (XHudScriptWnd::Draw below) */
        if (MEM32(tmpl + 0x24) && s_hidden_n < 16) {
            s_hidden_wnd = wnd;
            s_hidden[s_hidden_n++] = MEM32(tmpl + 0x24);
        }
        return 1;
    }
    return 0;
}

/* void XApp::CreatePauseMenu(int pad) -- wrapped: the game opens the pause
 * menu for whichever joined player pressed Start; when that is player 2's
 * controller it is their menu (buffy_coop_pause_button rewrites its lines),
 * and only their controller works it. */
void XApp_CreatePauseMenu_0002D640_orig(void);
void XApp_CreatePauseMenu_0002D640(void)
{
    uint32_t app = g_ecx, pad = MEM32(g_esp + 4), gp = MEM32(0x26EBB8);
    int p2 = s_story_coop && MEM32(0x26DC58) && gp && pad == MEM32(gp + 0xCA8 + 4) && pad != MEM32(gp + 0xCA8)
             && !in_multiplayer();
    s_p2_pause = p2;
    if ((s_story_coop || s_p1_change) && !in_multiplayer()) {
        /* the co-op lines are in the book lettering, a font of the front
         * end's files: loaded (once; they then stay) before the page is */
        uint32_t regs[4] = { g_ecx, g_ebx, g_esi, g_edi };
        fe_files(1);
        g_ecx = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
    }
    if (getenv("BUFFY_MODS_LOG"))
        fprintf(stderr, "[MODS] pause menu for pad %d (player 2's pad %d)\n", (int)pad, gp ? (int)MEM32(gp + 0xCAC) : -9);
    XApp_CreatePauseMenu_0002D640_orig();
    if (p2 && MEM32(app + 0x84)) {
        uint32_t eax = g_eax;
        call_this1(XHudScriptWnd_RestrictToPad_00051A50, MEM32(app + 0x84), pad);
        g_eax = eax;
        fprintf(stderr, "[MODS] co-op: player 2 paused\n");
    }
}

/* ── the co-op page (player 1's pause menu: Co-op) ────────────────────────
 *
 * Kept in buffy_settings.ini, [Coop]. The page is the Options page with its
 * lines rewritten (buffy_coop_pause_button); Left / Right / A change a line. */
int buffy_coop_menu_line(void)
{
    return s_story_coop && !in_multiplayer() && !s_p2_pause;
}

void buffy_coop_page_opening(void)
{
    s_coop_page = 1;
}

static const wchar_t *k_views[4] = { L"", L"One Screen", L"Two Windows", L"Split Screen" };

static void coop_settings_save(void)
{
    const char *ini = buffy_settings_path();
    char v[16];
    if (!ini || !ini[0])
        return;
    sprintf_s(v, sizeof v, "%d", s_screens_cfg);
    WritePrivateProfileStringA("Coop", "Screens", v, ini);
    WritePrivateProfileStringA("Coop", "FriendlyFire", s_friendly_fire ? "1" : "0", ini);
    WritePrivateProfileStringA("Coop", "SharedInventory", s_share_inventory ? "1" : "0", ini);
    WritePrivateProfileStringA("Coop", "BackTeleport", s_teleport_on_back ? "1" : "0", ini);
    sprintf_s(v, sizeof v, "%d", s_respawn_secs);
    WritePrivateProfileStringA("Coop", "RespawnSeconds", v, ini);
}

static void coop_settings_load(void)
{
    const char *ini = buffy_settings_path();
    int v;
    if (!ini || !ini[0])
        return;
    v = (int)GetPrivateProfileIntA("Coop", "Screens", 0, ini);
    if (v >= 1 && v <= 3)
        s_screens_cfg = v;                                   /* the page's choice beats the mod's */
    s_friendly_fire = GetPrivateProfileIntA("Coop", "FriendlyFire", 0, ini) != 0;
    s_share_inventory = GetPrivateProfileIntA("Coop", "SharedInventory", 1, ini) != 0;
    s_teleport_on_back = GetPrivateProfileIntA("Coop", "BackTeleport", 1, ini) != 0;
    v = (int)GetPrivateProfileIntA("Coop", "RespawnSeconds", 3, ini);
    s_respawn_secs = v < 1 ? 1 : v > 30 ? 30 : v;
    nv2a_gpu_set_split(s_screens_cfg == 3);
}

static void coop_line_text(int line, wchar_t *out, size_t n)
{
    switch (line) {
    case 0: swprintf(out, n, L"Screens  %ls", k_views[s_screens_cfg]); break;
    case 1: swprintf(out, n, L"Friendly Fire  %ls", s_friendly_fire ? L"On" : L"Off"); break;
    case 2: swprintf(out, n, L"Shared Inventory  %ls", s_share_inventory ? L"On" : L"Off"); break;
    case 3: swprintf(out, n, L"Back Brings Player 2  %ls", s_teleport_on_back ? L"On" : L"Off"); break;
    case 4: swprintf(out, n, L"Player 2 Respawn  %d s", s_respawn_secs); break;
    default:
        swprintf(out, n, L"%ls", MEM32(0x26DC58) ? L"Drop Out Player 2" : L"Player 2 not in"); break;
    }
}

/* Switch between one screen, two windows and split screen. */
static void coop_set_screens(int v)
{
    s_screens_cfg = v;
    s_cam2_valid = 0;
    s_split_off = 0;
    nv2a_gpu_set_split(v == 3);
    if (v == 2 && MEM32(0x26DC58)) {
        nv2a_gpu_enable_second_window();
        nv2a_gpu_show_second_window(1);
    } else
        nv2a_gpu_show_second_window(0);
}

static int coop_line_press(uint32_t btn, int line, uint32_t mask)
{
    static DWORD last;
    wchar_t w[64];
    int dir = (mask & PAD_LEFT_GAME) ? -1 : 1;
    if (!(mask & (PAD_A_GAME | PAD_LEFT_GAME | PAD_RIGHT_GAME)) || GetTickCount() - last < 250)
        return 1;
    last = GetTickCount();
    switch (line) {
    case 0: coop_set_screens((s_screens_cfg - 1 + dir + 3) % 3 + 1); break;
    case 1: s_friendly_fire = !s_friendly_fire; break;
    case 2: s_share_inventory = !s_share_inventory; s_inv_1 = s_inv_2 = 0; break;
    case 3: s_teleport_on_back = !s_teleport_on_back; break;
    case 4: s_respawn_secs = s_respawn_secs + dir * (s_respawn_secs >= 10 ? 5 : 1);
            if (s_respawn_secs < 1) s_respawn_secs = 30;
            if (s_respawn_secs > 30) s_respawn_secs = 1;
            break;
    default:
        if ((mask & PAD_A_GAME) && MEM32(0x26DC58))
            s_p2_action = P2_DROP;                            /* on resuming */
        break;
    }
    coop_line_text(line, w, 64);
    if (line == 5 && s_p2_action == P2_DROP)
        swprintf(w, 64, L"Player 2 drops out on Continue");
    buffy_menu_set_text(btn, w);
    coop_settings_save();
    return 1;
}

/* char XItemHandler_Player::ApplyHit(HitData) -- wrapped: one player
 * hitting the other only lands with Friendly Fire on. HitData is on the
 * stack by value (0x98 bytes, ret 0x98); its first word is the attacker. */
void XItemHandler_Player_ApplyHit_00082E00_orig(void);
void XItemHandler_Player_ApplyHit_00082E00(void)
{
    uint32_t h = g_ecx, me = MEM32(h + 4), by = MEM32(g_esp + 4);
    uint32_t p1 = MEM32(0x26DC54), p2 = MEM32(0x26DC58);
    if (s_story_coop && p2 && by && me && by != me && (by == p1 || by == p2) && (me == p1 || me == p2)
            && !in_multiplayer()) {
        if (getenv("BUFFY_MODS_LOG"))
            fprintf(stderr, "[MODS] co-op: player %d hit player %d (friendly fire %s)\n", by == p1 ? 1 : 2,
                    me == p1 ? 1 : 2, s_friendly_fire ? "on" : "off");
        if (!s_friendly_fire) {
            g_eax = 0;
            g_esp += 4 + 0x98;
            return;
        }
    }
    XItemHandler_Player_ApplyHit_00082E00_orig();
}

/* ── split screen ─────────────────────────────────────────────────────────
 *
 * Screens = Split Screen: both players' frames in window 1, side by side
 * (nv2a_gpu_set_split). Each half is narrower than the 4:3 picture the game
 * draws, so while a split frame is drawn the camera's projection is made for
 * a half: EXCamera::UpdateDisplay scales the picture's width by the constant
 * at 0x196948 (0.75) when widescreen (0x27EC21) is on -- that is how 16:9
 * is done -- and for these frames it scales by 4/3 over the half's shape
 * instead. The rest of the game sees widescreen off (buffy_settings.c), so
 * the HUD keeps its 4:3 layout. */
float nv2a_gpu_split_half_aspect(void);
static int s_split_last;                   /* the last frame was drawn split */

int buffy_coop_split_active(void)
{
    return s_story_coop && s_screens_cfg == 3 && MEM32(0x26DC58) && !in_multiplayer();
}

void EXCamera_UpdateDisplay_000E7080_orig(void);
void EXCamera_UpdateDisplay_000E7080(void)
{
    if (s_split_last && buffy_coop_split_active()) {
        uint8_t wide = MEM8(0x27EC21);
        uint32_t k = MEM32(0x196948);
        float f = (4.0f / 3.0f) / nv2a_gpu_split_half_aspect();
        MEM8(0x27EC21) = 1;
        memcpy((void *)XBOX_PTR(0x196948), &f, 4);
        EXCamera_UpdateDisplay_000E7080_orig();
        MEM32(0x196948) = k;
        MEM8(0x27EC21) = wide;
        return;
    }
    {
        /* Widescreen, the original side-to-side view: the story camera's
         * picture zoomed by 4/3 (camera +0x48 scales the projection), so with
         * the game's 16:9 squeeze the width seen is 4:3's and the top and
         * bottom are trimmed -- nothing appears at the sides that the game's
         * 4:3 culling and camera collision did not expect. Other cameras
         * (the HUD's, the menus') are left alone. */
        uint32_t ci = MEM32(0x26DC64), h = ci ? MEM32(ci + 0x14C) : 0, cam = g_ecx;
        if (MEM8(0x27EC21) && !buffy_settings_widescreen_wide() && h && cam == MEM32(h + 0x410)
                && !in_multiplayer()) {
            float z = MEMF(cam + 0x48);
            MEMF(cam + 0x48) = z * (4.0f / 3.0f);
            EXCamera_UpdateDisplay_000E7080_orig();
            MEMF(cam + 0x48) = z;
            return;
        }
    }
    EXCamera_UpdateDisplay_000E7080_orig();
}

/* char XHudScriptWnd::Draw() -- wrapped: on player 2's menu and the co-op
 * page, the Options page's volume-bar frames (animators of the page's own
 * scene) are folded to nothing while the page is drawn, then put back. */
void XHudScriptWnd_Draw_0004FEA0_orig(void);
/* char XHudScriptWnd::Draw() -- wrapped. Player 2's menu and the co-op page
 * are the Options page without its volume lines; the bars' frames are
 * objects of the page's script (model 0x82000113), drawn with the page
 * whatever lines it has. While the page draws they are marked not drawn
 * (object +0x10 bit 0, which the scene's draw checks), then put back. The
 * script's objects are a list linked through object +4 (next node at +0,
 * previous at +4), walked from one of the page's objects. */
#define MODEL_BAR_FRAME 0x82000113u

void XHudScriptWnd_Draw_0004FEA0_orig(void);
void XHudScriptWnd_Draw_0004FEA0(void)
{
    uint32_t hid[16], wnd = g_ecx;
    int i, n = 0;
    if (wnd == s_hidden_wnd && (s_p2_pause || s_coop_page) && s_hidden_n) {
        uint32_t start = s_hidden[0] + 4;
        int dir;
        for (dir = 0; dir < 2; dir++) {
            uint32_t node = dir ? MEM32(start + 4) : start;
            int guard = 0;
            while (node && guard++ < 256 && n < 8) {
                uint32_t an = node - 4;
                if (MEM32(an + 0x24) == MODEL_BAR_FRAME && (MEM8(an + 0x10) & 1)) {
                    MEM8(an + 0x10) &= (uint8_t)~1u;
                    hid[n++] = an;
                }
                node = MEM32(node + (dir ? 4 : 0));
            }
        }
    }
    if (wnd == s_hide_wnd && s_p2_pause)
        for (i = 0; i < s_hide_n && n < 8; i++)
            if (MEM8(s_hide[i] + 0x10) & 1) {
                MEM8(s_hide[i] + 0x10) &= (uint8_t)~1u;
                hid[n++] = s_hide[i];
            }
    XHudScriptWnd_Draw_0004FEA0_orig();
    for (i = 0; i < n; i++)
        MEM8(hid[i] + 0x10) |= 1;
}

/* BUFFY_FONT_DUMP=1 (testing): the fonts in the in-game and front-end text
 * files (geometry header +0xB4: count, then a relative offset to 16-byte
 * entries, the font's hash code first). */
void buffy_coop_font_dump(void)
{
    static int done;
    uint32_t files[3], i, k;
    void EXGeoFile_GetGeoFile_000C6960(void);
    if (done || !getenv("BUFFY_FONT_DUMP") || !MEM32(0x26DC58))
        return;
    done = 1;
    files[0] = MEM32(0x1B7F70); files[1] = MEM32(0x1B7F68); files[2] = MEM32(0x1B7F78);
    for (i = 0; i < 3; i++) {
        uint32_t geo = call_cdecl1_ret(EXGeoFile_GetGeoFile_000C6960, files[i]), hdr = geo ? MEM32(geo + 0x28) : 0;
        uint32_t arr, n, off, e;
        if (!hdr) {
            fprintf(stderr, "[FONT] file %08X not loaded\n", files[i]);
            continue;
        }
        arr = hdr + 0xB4;
        n = MEM32(arr);
        off = MEM32(arr + 4);
        e = off ? arr + 4 + off : 0;
        fprintf(stderr, "[FONT] file %08X: %u fonts\n", files[i], n);
        for (k = 0; e && k < n && k < 32; k++)
            fprintf(stderr, "[FONT]   %08X %08X %08X %08X\n", MEM32(e + k * 16), MEM32(e + k * 16 + 4),
                    MEM32(e + k * 16 + 8), MEM32(e + k * 16 + 12));
    }
}

/* buffy_menu.c, drawing a line (nothing to leave undrawn now: the bars are
 * left out and their frames hidden with the page). */
int buffy_coop_hide_line(uint32_t btn)
{
    (void)btn;
    return 0;
}

void coop_set_screens_test(int v)
{
    fprintf(stderr, "[MODS] co-op: (test) Screens -> %d\n", v);
    coop_set_screens(v);
}
