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
#include <math.h>

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

/* ── character mods ──────────────────────────────────────────────────────
 *
 * A mod.ini with a [Character] section adds a character to the in-level
 * Character Select, after the six story characters:
 *   BaseCharacter = the story character (0-5) whose moves, spells, inventory
 *                   and story part they take (1: Willow);
 *   LookRow       = the character-sheet row (6-23) whose look they wear: its
 *                   alternate skin of the same model file, its voice lines and
 *                   sound set, and its portrait on the page (8: Tara).
 * The look is kept while that player is the base character. */
#define MAX_EXTRA 32
#define MAX_OUTFIT 32
typedef struct { uint32_t model; int skin; } OutfitDef;
/* hidden: a story character's own entry, only for its outfits (no page slot) */
typedef struct { int base, look, look_sounds, model_skin, portrait, rigid_hands; uint32_t model; wchar_t model_file[MAX_PATH];
                 OutfitDef outfit[MAX_OUTFIT]; int n_outfit, hidden; char name[64]; } ExtraChar;
int buffy_model_import(uint32_t hdr, uint32_t skin, const wchar_t *path);
static ExtraChar s_extra[MAX_EXTRA];
static int s_rigid_hands = 1;              /* (per mod: RigidHands) */
static int s_n_extra;
static int s_player_extra[2] = { -1, -1 }; /* the character mod each player plays, or -1 */
/* Outfits: each character mod's outfit 0 is its Model/ModelSkin (or LookRow),
 * then its [Character] Outfits (model:skin, model:first-last, ...); a story
 * character's extra outfits are a hidden entry's ([Outfits] Buffy = ...).
 * X on the Character Select steps the highlighted character's outfit, for
 * the player choosing. */
static int s_outfit[2][MAX_EXTRA];
static int s_worn_key[2] = { -1, -1 };     /* the (mod, outfit) worn: mod * 64 + outfit */
static int s_worn_look[2] = { -1, -1 };    /* ...its look row (sounds), or -1 */
static uint32_t s_worn_model[2];           /* ...its model file, or 0 */
static int s_worn_rigid[2] = { 1, 1 };     /* ...rigid hands */
static const wchar_t *const k_story_names[6] = { L"Buffy", L"Willow", L"Xander", L"Spike", L"Sid", L"Faith" };

/* "01000019:2-22, 0100003F:1" -> outfits */
static void parse_outfits(const wchar_t *v, ExtraChar *x)
{
    while (*v) {
        wchar_t *e;
        uint32_t m;
        long a, b;
        while (*v == L' ' || *v == L',' || *v == L'\t' || *v == L';')
            v++;
        if (!*v)
            break;
        m = (uint32_t)wcstoul(v, &e, 16);
        if (e == v || *e != L':')
            break;
        v = e + 1;
        a = wcstol(v, &e, 10);
        if (e == v)
            break;
        b = a;
        v = e;
        if (*v == L'-') {
            b = wcstol(v + 1, &e, 10);
            v = e;
        }
        for (; a <= b && x->n_outfit < MAX_OUTFIT; a++) {
            x->outfit[x->n_outfit].model = m;
            x->outfit[x->n_outfit].skin = (int)a;
            x->n_outfit++;
        }
    }
}

/* The hidden entry holding story row `row`'s outfits, or -1. */
static int story_outfit_entry(int row)
{
    int i;
    for (i = 0; i < s_n_extra; i++)
        if (s_extra[i].hidden && s_extra[i].base == row)
            return i;
    return -1;
}

/* What player `who` wears as character mod `e`: its chosen outfit. */
static void outfit_of(int who, int e, uint32_t *model, int *skin, int *look, const wchar_t **file, int *rigid)
{
    int o = who >= 0 && who < 2 ? s_outfit[who][e] : 0;
    if (o > 0 && o <= s_extra[e].n_outfit) {
        *model = s_extra[e].outfit[o - 1].model;
        *skin = s_extra[e].outfit[o - 1].skin;
        *look = -1;
        *file = NULL;
        *rigid = 1;
    } else {
        *model = s_extra[e].model;
        *skin = s_extra[e].model_skin;
        *look = s_extra[e].look;
        *file = s_extra[e].model_file[0] ? s_extra[e].model_file : NULL;
        *rigid = s_extra[e].rigid_hands;
    }
}
static const uint8_t k_roster_row_of[24] = { 0, 3, 1, 2, 9, 13, 14, 8, 18, 22, 19, 11,
                                             4, 12, 17, 20, 5, 16, 10, 15, 21, 23, 7, 6 };

static int roster_of_row(int row)
{
    int i;
    for (i = 0; i < 24; i++)
        if (k_roster_row_of[i] == row)
            return i;
    return -1;
}

/* The roster portrait (page slot) character mod `e` uses: its Portrait, its
 * look row's, or the first one no story character or earlier mod has. */
static int extra_roster(int e)
{
    int i, r, k;
    if (s_extra[e].hidden)
        return -1;
    if (s_extra[e].portrait >= 0 && s_extra[e].portrait < 24)
        return s_extra[e].portrait;
    if (s_extra[e].look >= 0)
        return roster_of_row(s_extra[e].look);
    for (r = 0; r < 24; r++) {
        int used = (0x0001100Fu >> r) & 1;
        for (k = 0; k < s_n_extra && !used; k++)
            if (k != e && (s_extra[k].portrait == r || (s_extra[k].portrait < 0 && s_extra[k].look >= 0 && roster_of_row(s_extra[k].look) == r)))
                used = 1;
        for (i = 0; i < e && !used; i++)
            if (s_extra[i].portrait < 0 && s_extra[i].look < 0 && extra_roster(i) == r)
                used = 1;
        if (!used)
            return r;
    }
    return -1;
}

static void read_patches(const wchar_t *mod_dir)
{
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, MAX_PATH, L"%s\\mod.ini", mod_dir);
    {
        int base = (int)GetPrivateProfileIntW(L"Character", L"BaseCharacter", -1, ini);
        int look = (int)GetPrivateProfileIntW(L"Character", L"LookRow", -1, ini);
        wchar_t mv[32];
        uint32_t model = 0;
        GetPrivateProfileStringW(L"Character", L"Model", L"", mv, 32, ini);
        if (mv[0])
            model = (uint32_t)wcstoul(mv, NULL, 16);           /* a model file's hash (hex) */
        if (base >= 0 || look >= 0 || model) {
            if (base < 0 || base > 5 || (!model && (look < 6 || look > 23)) || s_n_extra >= MAX_EXTRA)
                fprintf(stderr, "[MODS]   character: BaseCharacter %d / LookRow %d not usable\n", base, look);
            else {
                memset(&s_extra[s_n_extra], 0, sizeof s_extra[0]);
                s_extra[s_n_extra].base = base;
                s_extra[s_n_extra].look = look >= 6 && look <= 23 ? look : -1;
                /* Model: that file's skin ModelSkin, on the base character's
                 * skeleton (the rig converted); Portrait: the roster portrait
                 * its page slot borrows (else the look row's, else a free one) */
                s_extra[s_n_extra].model = model;
                s_extra[s_n_extra].model_skin = (int)GetPrivateProfileIntW(L"Character", L"ModelSkin", 0, ini);
                s_extra[s_n_extra].portrait = (int)GetPrivateProfileIntW(L"Character", L"Portrait", -1, ini);
                s_extra[s_n_extra].rigid_hands = (int)GetPrivateProfileIntW(L"Character", L"RigidHands", 1, ini);
                /* ModelFile: that model edited (a .glb from the Advanced tab's
                 * exporter): its bones and vertices replace the model's */
                {
                    wchar_t mf[MAX_PATH];
                    s_extra[s_n_extra].model_file[0] = 0;
                    GetPrivateProfileStringW(L"Character", L"ModelFile", L"", mf, MAX_PATH, ini);
                    if (mf[0] && model) {
                        if (mf[1] == L':' || mf[0] == L'\\')
                            wcscpy_s(s_extra[s_n_extra].model_file, MAX_PATH, mf);
                        else
                            swprintf_s(s_extra[s_n_extra].model_file, MAX_PATH, L"%s\\%s", mod_dir, mf);
                        if (GetFileAttributesW(s_extra[s_n_extra].model_file) == INVALID_FILE_ATTRIBUTES) {
                            fprintf(stderr, "[MODS]   character: model file %ls not found\n", s_extra[s_n_extra].model_file);
                            s_extra[s_n_extra].model_file[0] = 0;
                        } else
                            fprintf(stderr, "[MODS]   character: model file %ls\n", s_extra[s_n_extra].model_file);
                    }
                    /* an edited model's rig is fitted to the player's: its
                     * fingers follow the player's (unless RigidHands = 1) */
                    if (s_extra[s_n_extra].model_file[0])
                        s_extra[s_n_extra].rigid_hands = (int)GetPrivateProfileIntW(L"Character", L"RigidHands", 0, ini);
                }
                /* LookSounds = 1: the look row's voice and sound set; else the
                 * base character's (a multiplayer set has no story lines) */
                s_extra[s_n_extra].look_sounds = look >= 6 && (int)GetPrivateProfileIntW(L"Character", L"LookSounds", 0, ini);
                {
                    /* the outfits after the first; the name (for the outfit message) */
                    static wchar_t v[4096];
                    wchar_t nm[64];
                    GetPrivateProfileStringW(L"Character", L"Outfits", L"", v, 4096, ini);
                    parse_outfits(v, &s_extra[s_n_extra]);
                    GetPrivateProfileStringW(L"Mod", L"Name", L"", nm, 64, ini);
                    WideCharToMultiByte(CP_UTF8, 0, nm, -1, s_extra[s_n_extra].name, 64, NULL, NULL);
                    if (s_extra[s_n_extra].n_outfit)
                        fprintf(stderr, "[MODS]   character: %d more outfits\n", s_extra[s_n_extra].n_outfit);
                }
                s_n_extra++;
                if (model)
                    fprintf(stderr, "[MODS]   character: model %08X (skin %d) on story character %d\n", model,
                            s_extra[s_n_extra - 1].model_skin, base);
                else
                    fprintf(stderr, "[MODS]   character: sheet row %d's look on story character %d\n", look, base);
            }
        }
    }
    {
        /* [Outfits] Buffy = model:skin, ... -- more outfits for the story characters */
        static wchar_t v[4096];
        int row;
        for (row = 0; row < 6; row++) {
            int e;
            GetPrivateProfileStringW(L"Outfits", k_story_names[row], L"", v, 4096, ini);
            if (!v[0])
                continue;
            if ((e = story_outfit_entry(row)) < 0) {
                if (s_n_extra >= MAX_EXTRA)
                    continue;
                e = s_n_extra++;
                memset(&s_extra[e], 0, sizeof s_extra[0]);
                s_extra[e].base = row;
                s_extra[e].look = -1;
                s_extra[e].portrait = -1;
                s_extra[e].rigid_hands = 1;
                s_extra[e].hidden = 1;
                WideCharToMultiByte(CP_UTF8, 0, k_story_names[row], -1, s_extra[e].name, 64, NULL, NULL);
            }
            parse_outfits(v, &s_extra[e]);
            fprintf(stderr, "[MODS]   outfits: %ls has %d more\n", k_story_names[row], s_extra[e].n_outfit);
        }
    }
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

static void load_char_sounds(int row);
static void inv_carry(uint32_t from, uint32_t to);
static void set_char_voice(uint32_t item, int row);
static uint32_t sheet_model(int row);
static uint8_t s_sheet_b4[24], s_sheet_b5[24];   /* each sheet row's +4 (skin section), +5 (handler +0x71A) */

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
    load_char_sounds((int)MEM32(p2 + 0x16C));
    fprintf(stderr, "[MODS] co-op: player 2 (character %d) is item %08X\n", (int)MEM32(p2 + 0x16C), p2);
}

static int s_cam2_valid;                  /* (the two-screens section's) */

/* Where player 1 has been: a sample every 10 frames, the last 3 seconds. A
 * fixed step beside player 1 could be in a wall or over an edge (player 2
 * fell through the floor); ground player 1 just walked on is not. A jump in
 * position (a start point, the story moving them) starts it again. */
#define TRAIL_N 18
static float s_trail[TRAIL_N][3];
static int   s_trail_n, s_trail_at, s_trail_tick;

static void trail_sample(void)
{
    uint32_t p1 = MEM32(0x26DC54);
    float p[3], *last;
    if (!p1) {
        s_trail_n = 0;
        return;
    }
    if (++s_trail_tick < 10)
        return;
    s_trail_tick = 0;
    memcpy(p, (const void *)XBOX_PTR(p1 + 0xAC), 12);
    if (s_trail_n) {
        last = s_trail[(s_trail_at + TRAIL_N - 1) % TRAIL_N];
        if (fabsf(p[0] - last[0]) + fabsf(p[1] - last[1]) + fabsf(p[2] - last[2]) > 8.0f)
            s_trail_n = 0;                        /* moved, not walked */
    }
    memcpy(s_trail[s_trail_at], p, 12);
    s_trail_at = (s_trail_at + 1) % TRAIL_N;
    if (s_trail_n < TRAIL_N)
        s_trail_n++;
}

/* A vampire's grab-and-throw (player game mode 0x4C grabbed, 0x4D thrown,
 * 0x54 its other form): the player is carried along a point of their "thrown"
 * animation (animator datum 0x10000019, XItemHandler_Player::
 * HandleBeingThrown) and the animation ends the mode. The multiplayer
 * characters (sheet rows 6-23) have no such animation: they were held in the
 * mode for good -- no moving on, no action button (pickups, doors, items).
 * After 3 s they are let go the game's way (SetGameMode out of it clears the
 * hold) and the grab's flags are undone. */
void XItemHandler_Player_SetGameMode_000758E0(void);
void EXItem_GetAnimatorDatum_000CA7D0(void);
void XItemCharacterHandler_CheckForAnimModeInSet_000AED40(void);
static void call_this0(void (*f)(void), uint32_t self);
static int apply_skin(uint32_t item, int skin);
static uint32_t model_of_row(int row);
static uint32_t skin_data(uint32_t geo, uint32_t id);
static int apply_rig_remap(uint32_t item);
static void retarget_setup(uint32_t ro, uint32_t skin, uint32_t skel, const int16_t *owner);
static void extra_remaps(void);
static void dump_skeleton(const char *name, uint32_t data);
static void dump_player(int who, const char *why);
static void mesh_dump(uint32_t geo, uint32_t skinid);
static uint32_t model_geo(uint32_t hash);
static void p2_swap_to(int row);
static void extra_refresh(int who);
static void extra_set(int who, int e);
static int extra_sound_row(int who, int row);
static uint32_t cs_visible_bits(void);
static void call_this1(void (*f)(void), uint32_t self, uint32_t a);

static void unstick_thrown(int slot)
{
    static int frames[2];
    uint32_t item = MEM32(0x26DC54 + (uint32_t)slot * 4), h = item ? MEM32(item + 0x14C) : 0, phys;
    uint8_t mode = h ? MEM8(h + 0x749) : 0;
    uint32_t regs[4];
    if (!h || MEM32(item + 0x16C) <= 5 || in_multiplayer() || (mode != 0x4C && mode != 0x4D && mode != 0x54)) {
        frames[slot] = 0;
        return;
    }
    if (++frames[slot] < 180)
        return;
    frames[slot] = 0;
    regs[0] = g_eax; regs[1] = g_ebx; regs[2] = g_esi; regs[3] = g_edi;
    call_this1(XItemHandler_Player_SetGameMode_000758E0, h, 1);
    phys = MEM32(item + 0x150);
    if (phys)
        MEM32(phys + 0xC0) &= ~0x10u;
    MEM8(h + 0x5F4) &= (uint8_t)~0x40u;
    MEM8(item + 0x69) = 0;
    g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
    fprintf(stderr, "[MODS] player %d (character %u) let go after a throw (no thrown animation)\n", slot + 1,
            MEM32(item + 0x16C));
}

/* Put player 2 by player 1, facing the same way; player 2's camera starts
 * again from player 1's. The spot: where player 1 was a moment ago, 0.6 to 3
 * units away (the newest such); else player 1's own spot, a hair aside (the
 * two push apart). `fresh`: player 1 has just been put somewhere new -- the
 * trail is of the old place. */
static void coop_follow_at(int fresh)
{
    s_cam2_valid = 0;
    uint32_t p1 = MEM32(0x26DC54), p2 = MEM32(0x26DC58);
    float pos[3], x;
    int i, found = 0;
    for (i = 0; i < 4; i++) {
        MEM32(p2 + 0xAC + i * 4) = MEM32(p1 + 0xAC + i * 4);
        MEM32(p2 + 0xBC + i * 4) = MEM32(p1 + 0xBC + i * 4);
    }
    memcpy(pos, (const void *)XBOX_PTR(p1 + 0xAC), 12);
    if (fresh)
        s_trail_n = 0;
    for (i = 1; i <= s_trail_n && !found; i++) {
        const float *t = s_trail[(s_trail_at + TRAIL_N - i) % TRAIL_N];
        float dx = t[0] - pos[0], dy = t[1] - pos[1], dz = t[2] - pos[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d >= 0.6f && d <= 3.0f) {
            memcpy((void *)XBOX_PTR(p2 + 0xAC), t, 12);
            found = 1;
        }
    }
    if (!found) {
        memcpy(&x, (const void *)XBOX_PTR(p2 + 0xAC), 4);
        x += 0.25f;
        memcpy((void *)XBOX_PTR(p2 + 0xAC), &x, 4);
    }
    MEM8(p2 + 0x69) = 0;                      /* as the trigger does after moving */
    if (getenv("BUFFY_MODS_LOG"))
        fprintf(stderr, "[MODS] co-op: player 2 moved by player 1 (%s)\n",
                found ? "where player 1 was a moment ago" : "player 1's spot");
}

static void coop_follow(void)
{
    coop_follow_at(0);
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
        coop_follow_at(1);
    if (playable) {
        uint32_t eax = g_eax;
        extra_refresh(0);                                 /* a character mod's look again */
        extra_refresh(1);
        g_eax = eax;
    }
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
    {
        uint32_t eax = g_eax;                            /* the story made them someone: a mod's look if theirs */
        extra_refresh(0);
        extra_refresh(1);
        g_eax = eax;
    }
}

/* ── the Change Character page's music ────────────────────────────────────
 *
 * The page is the front end's Character Select, and its script starts the
 * menu music: changing character in a level played the menu tune until the
 * level's own came back. While the page is up in a level, starting music is
 * ignored (the level's keeps playing). Both overloads clean their own
 * arguments, so a skipped call pops them. */
static uint32_t s_join_wnd;                /* the Character Select window while choosing */
static uint32_t s_music[5];                /* the last track started outside the page: overload (1 or 4 args), args */

void EXSoundManager_StartMusic_00110990_orig(void);
void EXSoundManager_StartMusic_00110990(void)          /* StartMusic(track) */
{
    if (getenv("BUFFY_MUSIC_LOG"))
        fprintf(stderr, "[MODS] music start %08X (00110990) page %08X\n", MEM32(g_esp + 4), s_join_wnd);
    if (s_join_wnd) {
        if (getenv("BUFFY_MODS_LOG"))
            fprintf(stderr, "[MODS] menu music %08X held off (Change Character)\n", MEM32(g_esp + 4));
        g_esp += 4 + 4;
        return;
    }
    s_music[0] = 1;
    s_music[1] = MEM32(g_esp + 4);
    EXSoundManager_StartMusic_00110990_orig();
}

void EXSoundManager_StartMusic_00110A00_orig(void);
void EXSoundManager_StartMusic_00110A00(void)          /* StartMusic(track, a, b, c) */
{
    if (getenv("BUFFY_MUSIC_LOG"))
        fprintf(stderr, "[MODS] music start %08X (00110A00) page %08X\n", MEM32(g_esp + 4), s_join_wnd);
    if (s_join_wnd) {
        if (getenv("BUFFY_MODS_LOG"))
            fprintf(stderr, "[MODS] menu music %08X held off (Change Character)\n", MEM32(g_esp + 4));
        g_esp += 4 + 16;
        return;
    }
    s_music[0] = 4;
    s_music[1] = MEM32(g_esp + 4);
    s_music[2] = MEM32(g_esp + 8);
    s_music[3] = MEM32(g_esp + 12);
    s_music[4] = MEM32(g_esp + 16);
    EXSoundManager_StartMusic_00110A00_orig();
}

/* The page closed: the level's music again. (Opening the page stops it --
 * the level's stream is let run to its end marker -- and nothing in the
 * level starts it again: after a change of character the level was silent.) */
static void music_resume(void)
{
    uint32_t mgr = MEM32(0x26EB24), esp0 = g_esp, regs[4] = { g_eax, g_ebx, g_esi, g_edi };
    int i;
    if (!mgr || !s_music[0] || in_multiplayer())
        return;
    for (i = s_music[0]; i >= 1; i--) {
        g_esp -= 4;
        MEM32(g_esp) = s_music[i];
    }
    g_esp -= 4; MEM32(g_esp) = 0;                         /* return address */
    g_ecx = mgr;
    if (s_music[0] == 4)
        EXSoundManager_StartMusic_00110A00_orig();
    else
        EXSoundManager_StartMusic_00110990_orig();
    g_esp = esp0;
    g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
    if (getenv("BUFFY_MODS_LOG"))
        fprintf(stderr, "[MODS] level music %08X again (Change Character closed)\n", s_music[1]);
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

static int s_pc_page;                      /* the Options page being opened is the PC settings page (buffy_menu.c) */
static int s_pc_built;                     /* frames since the PC page's lines were made */
static int s_pc_sixth;                     /* the main menu page's five lines are done: add the sixth */

/* buffy_menu.c: after a line of the PC page is added -- 1 once, when the
 * page needs its sixth line added (the main menu page has five). */
int buffy_pc_take_sixth(void)
{
    int v = s_pc_sixth;
    s_pc_sixth = 0;
    return v;
}

void buffy_mods_frame(void)
{
    if (s_pc_page && s_pc_built && ++s_pc_built > 2) {
        s_pc_page = 0;                                    /* the popup is built: the next Options page is itself */
        s_pc_built = 0;
    }
    {
        void buffy_pc_page_back(void);
        buffy_pc_page_back();                             /* (B closes the PC settings page) */
    }
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

/* Given always now (not only with the co-op mods): a PC has the memory, and
 * a bigger heap keeps more of a level loaded. 128 MB is the most the Xbox GPU
 * can address, so it is also the ceiling. BUFFY_NO_HEAP_PATCH: retail 64 MB. */
static int big_memory(void)
{
    return !getenv("BUFFY_NO_HEAP_PATCH");
}

void buffy_coop_memory_early(void)
{
    if (big_memory())
        xbox_SetContigSize(128u * 1024u * 1024u);
}

void buffy_coop_memory_patch(void)
{
    if (!big_memory())
        return;
    fprintf(stderr, "[MODS] memory: main heap %u MB -> %u MB\n", MEM32(0x1B7F84) >> 20,
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
    if (big_memory() && MEM32(g_esp + 4) == 0x2600000u + COOP_EXTRA_HEAP
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
static int s_share_inventory = 1, s_hud_p2;
static void coop_inventory_sync(void);
static uint32_t s_inv_1, s_inv_2;          /* the inventories the base was taken from */
static uint32_t player_inventory(uint32_t item);
static int16_t inv_count(uint32_t inv, int id);
static void inv_set(uint32_t inv, int id, int16_t want);
static void hud_for_p2(int on);
static uint32_t hud_inv1(void), hud_inv2(void);
void XHudWnd_ShowInventory_000551B0(void);

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

static uint32_t s_join_owner;              /* guest word the window clears as it goes */
static int      s_join_pick = -1, s_join_cancel, s_join_closing;
static int      s_join_extra = -1;         /* the pick is character mod n */
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
static uint32_t s_pc_wnd;                  /* the PC settings page's window */
static uint32_t s_pc_parent;               /* ...the page it is a popup of */
#define PC_LINES        6
#define TYPE_PC_LINE    0x46FF0040u
void buffy_pc_line_text(int line, wchar_t *out, size_t n);
int  buffy_pc_line_press(uint32_t btn, int line, uint32_t mask, int left, int any);
static int coop_line_press(uint32_t btn, int line, uint32_t mask);
static void coop_line_text(int line, wchar_t *out, size_t n);
#define PAUSE_PAGE      0x04000184u
#define OPTIONS_PAGE    0x0400039Bu
#define OPTIONS_PAGE_FE 0x040000F5u        /* the front end's Options page */
#define MAIN_MENU_PAGE  0x0400011Cu        /* the front end's main menu: the PC settings page there */
#define TYPE_P2_DROP    0x46FF0011u
void XApp_CreatePauseMenu_0002D640(void);
void XItemHandler_Player_SwapCharacter_0007B200(void);
void nv2a_gpu_show_second_window(int show);
static void join_finish(int row);
static void p1_swap_to(int row);
static void stop_char_sounds(int row);
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

static uint32_t call_cdecl3(void (*f)(void), uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t esp0 = g_esp;
    g_esp -= 4; MEM32(g_esp) = c;
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

/* Mouse look (buffy_input.c) only while playing: a player, the HUD up, no
 * pause menu (XApp +0x84), no Character Select page. */
int buffy_mouse_look_allowed(void)
{
    if (getenv("BUFFY_MOUSE_LOG")) {
        static DWORD last;
        if (GetTickCount() - last > 3000) {
            uint32_t app = MEM32(0x26D868), hud = app ? MEM32(app + 0x230) : 0;
            last = GetTickCount();
            fprintf(stderr, "[INPUT] player %08X xapp %08X +84 %08X hud %08X +11 %u +18 %u page %08X\n", MEM32(0x26DC54), s_xapp,
                    s_xapp ? MEM32(s_xapp + 0x84) : 0, hud, hud ? MEM8(hud + 0x11) : 0, hud ? MEM8(hud + 0x18) : 0, s_join_wnd);
        }
    }
    return MEM32(0x26DC54) && s_xapp && !MEM32(s_xapp + 0x84) && hud_visible() && !s_join_wnd;
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
     * front end. While the page is up only the six story characters are lit:
     * the multiplayer ones lack animations and sounds the story needs
     * (grabbed by a vampire, picking up, the story's lines), so they are
     * greyed out and cannot be picked. */
    s_mask_saved = MEM32(MC_CHAR_MASK);
    MEM32(MC_CHAR_MASK) = (s_mask_saved & ~ALL_ROSTER_BITS)
                          | (getenv("BUFFY_CS_ORIGINAL") ? ALL_ROSTER_BITS : cs_visible_bits());   /* (debugging: all) */
    fprintf(stderr, "[MODS] co-op: player %d is choosing a character\n", s_join_who + 1);
}

/* The Character Select in a level: the six story characters on the first
 * line, in story order, and the other slots empty (buffy_coop_hide_line) --
 * room for character mods. The page's 24 portraits are its buttons, in grid
 * order (12 a line); moving the cursor follows that order, and each is drawn
 * where its animator is (translation +0xAC, the matrix's fourth row +0x9C).
 * So the buttons are put in the new order and each is given the position of
 * the slot it now has; a button keeps its roster index (+0x5C), so a pick is
 * still who it shows. */
static int s_cs_laid;                      /* the page open now has been laid out */
static int s_cs_count = 6;                 /* the portraits in use (the story's and the mods') */

static void cs_layout(void)
{
    static const uint8_t k_story_order[6] = { 0, 2, 3, 1, 12, 16 };   /* roster: rows 0-5 */
    uint32_t w = s_join_wnd, arr = MEM32(w + 0x194), btn[24], order[24], pos[24][6];
    int i, j, n = 0;
    if (MEM32(w + 0x190) != 24 || !arr)
        return;
    for (i = 0; i < 24; i++) {
        btn[i] = MEM32(arr + (uint32_t)i * 4);
        if (!btn[i] || MEM32(btn[i] + 0x64) != BTN_PORTRAIT || !MEM32(btn[i] + 0x24) || MEM32(btn[i] + 0x5C) != (uint32_t)i)
            return;                                      /* not the page as known */
        for (j = 0; j < 3; j++) {
            pos[i][j] = MEM32(MEM32(btn[i] + 0x24) + 0xAC + (uint32_t)j * 4);
            pos[i][3 + j] = MEM32(MEM32(btn[i] + 0x24) + 0x9C + (uint32_t)j * 4);
        }
    }
    for (i = 0; i < 6; i++)
        order[n++] = btn[k_story_order[i]];
    s_cs_count = 6;
    for (i = 0; i < s_n_extra; i++) {                   /* the character mods next */
        int r = extra_roster(i), k, dup = 0;
        for (k = 0; k < i; k++)
            dup |= extra_roster(k) == r;
        if (r >= 0 && !dup && !(STORY_ROSTER_BITS & (1u << r))) {
            order[n++] = btn[r];
            s_cs_count++;
        }
    }
    for (i = 0; i < 24; i++)
        if (!(cs_visible_bits() & (1u << i)))
            order[n++] = btn[i];
    for (i = 0; i < 24; i++) {
        uint32_t a = MEM32(order[i] + 0x24);
        MEM32(arr + (uint32_t)i * 4) = order[i];
        for (j = 0; j < 3; j++) {
            MEM32(a + 0xAC + (uint32_t)j * 4) = pos[i][j];
            MEM32(a + 0x9C + (uint32_t)j * 4) = pos[i][3 + j];
        }
    }
    s_cs_laid = 1;
    if (getenv("BUFFY_MODS_LOG"))
        fprintf(stderr, "[MODS] Character Select: story characters on one line\n");
}

/* The cursor moves through the grid (right: the next button, down: 12 on):
 * past the story line it would land on an empty slot. It is put back on the
 * story character it came from (the window keeps each pad's highlighted
 * button at +0x1C0 + pad * 4). */
void XHudScriptWnd_HighlightButton_000513E0(void);

static void cs_keep_cursor(void)
{
    static uint32_t last[4];
    uint32_t w = s_join_wnd, pad, idx, esp0, regs[4];
    for (pad = 0; pad < 4; pad++) {
        idx = MEM32(w + 0x1C0 + pad * 4);
        if (idx < (uint32_t)s_cs_count) {
            last[pad] = idx;
            continue;
        }
        if (idx >= 24)
            continue;                                    /* (no cursor for that pad) */
        regs[0] = g_eax; regs[1] = g_ebx; regs[2] = g_esi; regs[3] = g_edi;
        esp0 = g_esp;
        g_esp -= 4; MEM32(g_esp) = pad;
        g_esp -= 4; MEM32(g_esp) = 1;
        g_esp -= 4; MEM32(g_esp) = last[pad] < (uint32_t)s_cs_count ? last[pad] : 0;
        g_esp -= 4; MEM32(g_esp) = 0;
        g_ecx = w;
        XHudScriptWnd_HighlightButton_000513E0();
        g_esp = esp0;
        g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
    }
}

/* On the Character Select: X steps the highlighted character's outfit for
 * the player choosing (a message says which). */
void nv2a_gpu_toast(const char *text, int ms);
static void cs_outfit_input(void)
{
    static uint16_t prev[2];
    int who = s_join_who & 1, pad = who, e = -1, i, n, o;
    uint16_t b = buffy_input_buttons(pad), press = (uint16_t)(b & ~prev[who]);
    uint32_t w = s_join_wnd, idx, arr, bt, roster;
    char msg[160];
    prev[who] = b;
    if (!(press & 0x4000u) || !w)
        return;
    if (getenv("BUFFY_MODS_LOG"))
        fprintf(stderr, "[MODS] X on the page (pad %d, highlight %u)\n", pad, MEM32(w + 0x1C0 + (uint32_t)pad * 4));
    idx = MEM32(w + 0x1C0 + (uint32_t)pad * 4);
    arr = MEM32(w + 0x194);
    if (!arr || idx >= MEM32(w + 0x190) || idx >= 64)
        return;
    bt = MEM32(arr + idx * 4);
    if (!bt || MEM32(bt + 0x64) != BTN_PORTRAIT || (roster = MEM32(bt + 0x5C)) >= 24)
        return;
    if (STORY_ROSTER_BITS & (1u << roster))
        e = story_outfit_entry(k_roster_row_of[roster]);
    else
        for (i = 0; i < s_n_extra; i++)
            if (!s_extra[i].hidden && extra_roster(i) == (int)roster) {
                e = i;
                break;
            }
    if (e < 0 || !s_extra[e].n_outfit) {
        sprintf_s(msg, sizeof msg, "Player %d: no other outfits", who + 1);
        nv2a_gpu_toast(msg, 1500);
        return;
    }
    n = s_extra[e].n_outfit + 1;
    o = (s_outfit[who][e] + 1) % n;
    s_outfit[who][e] = o;
    sprintf_s(msg, sizeof msg, "Player %d  -  %s: outfit %d of %d", who + 1, s_extra[e].name[0] ? s_extra[e].name : "Character", o + 1, n);
    nv2a_gpu_toast(msg, 2500);
    fprintf(stderr, "[MODS] %s\n", msg);
}

/* At the start of each frame, outside the game's update. */
static void join_frame(void)
{
    uint16_t b, pressed;
    if (s_join_wnd && !s_cs_laid && !getenv("BUFFY_CS_ORIGINAL"))
        cs_layout();
    if (!s_join_wnd)
        s_cs_laid = 0;
    else if (s_cs_laid) {
        cs_keep_cursor();
        cs_outfit_input();
    }
    if (getenv("BUFFY_CS_DUMP") && s_join_wnd) {
        /* (debugging) the Character Select's buttons, once, a second in */
        static int frames;
        if (++frames == 60) {
            uint32_t w = s_join_wnd, n = MEM32(w + 0x190), arr = MEM32(w + 0x194), i, k;
            fprintf(stderr, "[CS] window %08X: %u buttons\n", w, n);
            for (i = 0; i < n && i < 64; i++) {
                uint32_t bt = MEM32(arr + i * 4), a = bt ? MEM32(bt + 0x24) : 0;
                float x = 0, y = 0, z = 0;
                if (a) { memcpy(&x, (const void *)XBOX_PTR(a + 0xAC), 4); memcpy(&y, (const void *)XBOX_PTR(a + 0xB0), 4); memcpy(&z, (const void *)XBOX_PTR(a + 0xB4), 4); }
                fprintf(stderr, "[CS] %2u btn %08X type %08X +4 %08X +8 %08X +5C %d +68 %08X +74 %d +A0 %08X +A4 %08X anim %08X pos %.3f %.3f %.3f |", i, bt,
                        MEM32(bt + 0x64), MEM32(bt + 4), MEM32(bt + 8), (int)MEM32(bt + 0x5C), MEM32(bt + 0x68), (int)MEM32(bt + 0x74),
                        MEM32(bt + 0xA0), MEM32(bt + 0xA4), a, x, y, z);
                for (k = 0x28; k < 0x5C; k += 4)
                    fprintf(stderr, " %X", MEM32(bt + k));
                fprintf(stderr, " | 6C %X 70 %X 78 %X 7C %X 80 %X 84 %X 88 %X 8C %X\n", MEM32(bt + 0x6C), MEM32(bt + 0x70), MEM32(bt + 0x78),
                        MEM32(bt + 0x7C), MEM32(bt + 0x80), MEM32(bt + 0x84), MEM32(bt + 0x88), MEM32(bt + 0x8C));
            }
        }
    }
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
        {
            /* BUFFY_TEST_WEAK=secs (testing): from then the other characters
             * (the head-track list, [0x26E9D8]: node +8 item, +4 next) stay
             * at 5% health -- ready to be staked */
            static DWORD tw0;
            const char *wk = getenv("BUFFY_TEST_WEAK");
            if (!tw0)
                tw0 = GetTickCount();
            if (wk && MEM32(0x26E9D8) && GetTickCount() - tw0 > (DWORD)atoi(wk) * 1000) {
                uint32_t node = MEM32(MEM32(0x26E9D8)), guard = 0;
                for (; node && guard < 512; node = MEM32(node + 4), guard++) {
                    uint32_t it = MEM32(node + 8), hh = it ? MEM32(it + 0x14C) : 0;
                    float hp, mx;
                    if (!hh || it == MEM32(0x26DC54) || it == MEM32(0x26DC58))
                        continue;
                    memcpy(&hp, (const void *)XBOX_PTR(hh + 0x538), 4);
                    memcpy(&mx, (const void *)XBOX_PTR(hh + 0x53C), 4);
                    if (mx > 0.0f && mx < 100000.0f && hp > mx * 0.05f) {
                        hp = mx * 0.05f;
                        memcpy((void *)XBOX_PTR(hh + 0x538), &hp, 4);
                    }
                }
            }
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
    trail_sample();
    if (getenv("BUFFY_WEAPON_LOG") && MEM32(0x26DC54)) {
        /* (debugging) each weapon type's attach point, and whether each
         * player's character has it (EXItem::GetAnimatorDatum) */
        static DWORD last;
        uint32_t tab = MEM32(0x1B8998), t, k;
        if (tab && GetTickCount() - last > 10000) {
            static const uint32_t modes[] = { 0x9000054u, 0x90000ADu, 0x900017Cu, 0x900008Cu, 0x90001F4u };
            last = GetTickCount();
            for (k = 0; k < 2; k++) {
                uint32_t it = MEM32(0x26DC54 + k * 4), hh = it ? MEM32(it + 0x14C) : 0, m;
                if (!hh)
                    continue;
                fprintf(stderr, "[WPN] player %u (character %u) weapon type %u set %08X finisher modes:", k + 1, MEM32(it + 0x16C),
                        MEM8(hh + 0x6B8), MEM32(hh + 0x550));
                for (m = 0; m < 5; m++) {
                    uint32_t esp0 = g_esp, regs[4] = { g_eax, g_ebx, g_esi, g_edi };
                    g_esp -= 4; MEM32(g_esp) = MEM32(hh + 0x550);
                    g_esp -= 4; MEM32(g_esp) = modes[m];
                    g_esp -= 4; MEM32(g_esp) = 0;
                    g_ecx = hh;
                    XItemCharacterHandler_CheckForAnimModeInSet_000AED40();
                    fprintf(stderr, " %08X:%s", modes[m], (g_eax & 0xFF) ? "yes" : "NO");
                    g_esp = esp0;
                    g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
                }
                fprintf(stderr, "\n");
            }
            for (t = 1; t <= 24; t++) {
                uint32_t e = tab + (t - 1) * 0x3C, datum = MEM32(e + 8);
                fprintf(stderr, "[WPN] type %2u file %08X geo %08X datum %08X:", t, MEM32(e), MEM32(e + 4), datum);
                for (k = 0; k < 2; k++) {
                    uint32_t it = MEM32(0x26DC54 + k * 4), buf, esp0 = g_esp, regs[4] = { g_eax, g_ebx, g_esi, g_edi };
                    if (!it)
                        continue;
                    buf = xbox_HeapAlloc(0x80, 16);
                    g_esp -= 4; MEM32(g_esp) = 0;
                    g_esp -= 4; MEM32(g_esp) = 0;
                    g_esp -= 4; MEM32(g_esp) = buf;
                    g_esp -= 4; MEM32(g_esp) = datum;
                    g_esp -= 4; MEM32(g_esp) = 0;
                    g_ecx = it;
                    EXItem_GetAnimatorDatum_000CA7D0();
                    fprintf(stderr, " p%u(char %u) %s", k + 1, MEM32(it + 0x16C), g_eax ? "has" : "LACKS");
                    g_esp = esp0;
                    g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
                }
                fprintf(stderr, "\n");
            }
        }
    }
    {
        /* BUFFY_TEST_P1_EQUIP=secs:item (testing): player 1 equips that item; the held weapon logged */
        static DWORD t0e;
        static int done_e, logs_e;
        const char *eq = getenv("BUFFY_TEST_P1_EQUIP") ? getenv("BUFFY_TEST_P1_EQUIP") : getenv("BUFFY_TEST_P2_EQUIP");
        uint32_t p1e = MEM32(getenv("BUFFY_TEST_P1_EQUIP") ? 0x26DC54 : 0x26DC58), h1e = p1e ? MEM32(p1e + 0x14C) : 0, inv1e = p1e ? player_inventory(p1e) : 0;
        if (!t0e)
            t0e = GetTickCount();
        if (eq && strchr(eq, ':') && inv1e && !done_e && GetTickCount() - t0e > (DWORD)atoi(eq) * 1000) {
            void XInventory_EquipItem_00055EC0(void);
            uint32_t esp0 = g_esp, regs[4] = { g_eax, g_ebx, g_esi, g_edi };
            done_e = 1;
            g_esp -= 4; MEM32(g_esp) = 0;
            g_esp -= 4; MEM32(g_esp) = (uint32_t)strtol(strchr(eq, ':') + 1, NULL, 16);
            g_esp -= 4; MEM32(g_esp) = 0;
            g_ecx = inv1e;
            XInventory_EquipItem_00055EC0();
            g_esp = esp0;
            g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
        }
        if (done_e && h1e && logs_e < 6) {
            static DWORD tl;
            if (GetTickCount() - tl > 500) {
                tl = GetTickCount();
                logs_e++;
                fprintf(stderr, "[MODS] (test) player 1 (character %u): equipped %02X, weapon %08X type %u\n",
                        MEM32(p1e + 0x16C), MEM32(inv1e + 4), MEM32(h1e + 0x6BC), MEM8(h1e + 0x6B8));
            }
        }
    }
    if (getenv("BUFFY_ITEM_DUMP") && MEM32(0x26DC54)) {
        /* (debugging) the item table, once */
        static int done;
        uint32_t tab = MEM32(0x1B8010), id, k;
        if (!done++ && tab)
            for (id = 0; id < 0x9C; id++) {
                uint32_t e = tab + id * 0x1F0;
                fprintf(stderr, "[ITEM] %02X g%d:", id, MEM8(e + 0x192));
                for (k = 0x180; k < 0x1F0; k += 4)
                    fprintf(stderr, " %08X", MEM32(e + k));
                fprintf(stderr, " |");
                for (k = 0; k < 0x40; k += 4)
                    fprintf(stderr, " %08X", MEM32(e + k));
                fprintf(stderr, "\n");
            }
    }
    unstick_thrown(0);
    unstick_thrown(1);
    extra_remaps();
    if (getenv("BUFFY_MODE_LOG")) {
        static uint8_t last[2] = { 0xFF, 0xFF };
        int k;
        for (k = 0; k < 2; k++) {
            uint32_t it = MEM32(0x26DC54 + (uint32_t)k * 4), hh = it ? MEM32(it + 0x14C) : 0, wpn = hh ? MEM32(hh + 0x6BC) : 0;
            if (hh && MEM8(hh + 0x749) != last[k]) {
                last[k] = MEM8(hh + 0x749);
                fprintf(stderr, "[MODS] player %d (character %u) mode %02X (anim mode %08X) weapon %08X holder %d\n", k + 1,
                        MEM32(it + 0x16C), last[k], MEM32(hh + 0x3FC), wpn, wpn ? (int)MEM8(wpn + 0x60) : -1);
            }
        }
    }
    if (s_p2_rebar && !--s_p2_rebar)
        health_bar_p2();                                  /* (the old bar's window went last frame) */
    if (s_join_wnd) {
        if (!MEM32(s_join_owner)) {
            /* The window has gone: unload its files; make player 2 if one
             * was picked. */
            s_join_wnd = 0;
            music_resume();
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
    {
        /* BUFFY_TEST_P1_SWAP=secs:row (testing): player 1 becomes character
         * row, directly; BUFFY_TEST_P1_CHANGE=secs opens their Change
         * Character page as the pause menu line does. */
        static DWORD t0;
        static int done_swap, done_page;
        const char *sw = getenv("BUFFY_TEST_P1_SWAP"), *pg = getenv("BUFFY_TEST_P1_CHANGE");
        if (!t0)
            t0 = GetTickCount();
        {
            /* BUFFY_TEST_GIVE_P1=secs:item[,item...] (testing): player 1 picks up items */
            static int done_give;
            const char *gv = getenv("BUFFY_TEST_GIVE_P1");
            uint32_t inv1 = player_inventory(MEM32(0x26DC54));
            if (gv && !done_give && strchr(gv, ':') && inv1 && GetTickCount() - t0 > (DWORD)atoi(gv) * 1000) {
                char *q = strchr(gv, ':') + 1;
                done_give = 1;
                for (;;) {                                    /* secs:id[,id...] (hex) */
                    int id = (int)strtol(q, &q, 16);
                    inv_set(inv1, id, (int16_t)(inv_count(inv1, id) < 0 ? 1 : inv_count(inv1, id) + 1));
                    fprintf(stderr, "[MODS] (test) player 1 picked up item %02X: count %d\n", id, inv_count(inv1, id));
                    if (*q++ != ',')
                        break;
                }
            }
        }
        {
            /* BUFFY_TEST_DROP_P1=secs:item (testing): that item dropped as a
             * pickup at player 1's feet; their count of it logged each second */
            static int done_drop, drop_id, logs;
            static DWORD tlog;
            const char *dp = getenv("BUFFY_TEST_DROP_P1");
            uint32_t p1 = MEM32(0x26DC54), inv1 = player_inventory(p1);
            if (dp && !done_drop && strchr(dp, ':') && inv1 && GetTickCount() - t0 > (DWORD)atoi(dp) * 1000) {
                void XInventory_GeneratePickup_000562C0(void);
                uint32_t esp0 = g_esp, k;
                done_drop = 1;
                drop_id = (int)strtol(strchr(dp, ':') + 1, NULL, 16);
                g_esp -= 4; MEM32(g_esp) = 0;                          /* float */
                g_esp -= 16;
                for (k = 0; k < 4; k++)
                    MEM32(g_esp + k * 4) = MEM32(p1 + 0xAC + k * 4);   /* position */
                g_esp -= 4; MEM32(g_esp) = (uint32_t)drop_id;
                g_esp -= 4; MEM32(g_esp) = 0;                          /* return address */
                g_ecx = inv1;
                XInventory_GeneratePickup_000562C0();
                g_esp = esp0;
                fprintf(stderr, "[MODS] (test) item %02X dropped at player 1 (character %d, count %d): pickup %08X\n",
                        drop_id, (int)MEM32(p1 + 0x16C), inv_count(inv1, drop_id), g_eax);
                tlog = GetTickCount();
            }
            if (done_drop && logs < 400 && inv1) {
                static int16_t last = -2;
                static uint16_t lastb = 0xFFFF;
                uint16_t b = buffy_input_buttons(0);
                if (inv_count(inv1, drop_id) != last || b != lastb) {
                    logs++;
                    fprintf(stderr, "[MODS] (test) t+%lu player 1 (character %d) holds item %02X x%d, pad %04X\n",
                            GetTickCount() - tlog, (int)MEM32(p1 + 0x16C), drop_id, inv_count(inv1, drop_id), b);
                    last = inv_count(inv1, drop_id);
                    lastb = b;
                }
            }
        }
        {
            /* BUFFY_TEST_PICKUP_CYCLE=secs:item (testing): every 2 s the item is
             * dropped at player 1's feet and one button held for 0.3 s */
            extern const char *volatile g_test_button;
            static const char *btns_all[] = { "A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT" };
            static const char *btns_y[] = { "Y", "Y", "Y", "Y", "Y", "Y", "Y", "Y" };
            const char *const *btns = getenv("BUFFY_TEST_PICKUP_Y") ? btns_y : btns_all;
            static int step = -1;
            static DWORD ts;
            const char *pc = getenv("BUFFY_TEST_PICKUP_CYCLE");
            uint32_t p1 = MEM32(0x26DC54), inv1 = player_inventory(p1);
            if (pc && strchr(pc, ':') && inv1 && step < 8 && GetTickCount() - t0 > (DWORD)atoi(pc) * 1000) {
                int id = (int)strtol(strchr(pc, ':') + 1, NULL, 16);
                if (step < 0 || GetTickCount() - ts > 2000) {
                    if (step >= 0)
                        fprintf(stderr, "[MODS] (test) after %s: item %02X x%d\n", btns[step], id, inv_count(inv1, id));
                    step++;
                    ts = GetTickCount();
                    if (step < 8) {
                        void XInventory_GeneratePickup_000562C0(void);
                        uint32_t esp0 = g_esp, k;
                        g_esp -= 4; MEM32(g_esp) = 0;
                        g_esp -= 16;
                        for (k = 0; k < 4; k++)
                            MEM32(g_esp + k * 4) = MEM32(p1 + 0xAC + k * 4);
                        if (getenv("BUFFY_TEST_DROP_AHEAD")) {
                            /* that far in front (facing: +0xC0, a yaw) */
                            float yaw, pos[3], d = (float)atof(getenv("BUFFY_TEST_DROP_AHEAD"));
                            memcpy(&yaw, (const void *)XBOX_PTR(p1 + 0xC0), 4);
                            memcpy(pos, (const void *)XBOX_PTR(g_esp), 12);
                            pos[0] += sinf(yaw) * d;
                            pos[2] += cosf(yaw) * d;
                            memcpy((void *)XBOX_PTR(g_esp), pos, 12);
                            fprintf(stderr, "[MODS] (test) yaw %.2f\n", yaw);
                        }
                        g_esp -= 4; MEM32(g_esp) = (uint32_t)id;
                        g_esp -= 4; MEM32(g_esp) = 0;
                        g_ecx = inv1;
                        XInventory_GeneratePickup_000562C0();
                        g_esp = esp0;
                    }
                }
                g_test_button = (step < 8 && GetTickCount() - ts > 500 && GetTickCount() - ts < 800) ? btns[step] : NULL;
                if (g_test_button && getenv("BUFFY_PICKUP_LOG")) {
                    uint32_t h1 = MEM32(p1 + 0x14C), gp = MEM32(0x26EBB8);
                    fprintf(stderr, "[MODS] (test) %s held: mode %02X, pressed %08X, multi-use mask %08X\n", g_test_button,
                            MEM8(h1 + 0x749), gp ? MEM32(gp + MEM32(gp + 0xCA8) * 4 + 0x44) : 0, MEM32(0x1B89B0));
                }
            }
        }
        {
            /* BUFFY_TEST_P1_FOREIGN=secs:file:index (testing): player 1 wears skin
             * `index` of another model file (hex hash), on their own skeleton */
            static int done_f;
            const char *fo = getenv("BUFFY_TEST_P1_FOREIGN");
            if (fo && !done_f && strchr(fo, ':') && MEM32(0x26DC54) && GetTickCount() - t0 > (DWORD)atoi(fo) * 1000) {
                const char *q = strchr(fo, ':') + 1;
                uint32_t hash = (uint32_t)strtoul(q, (char **)&q, 16), geo, hdr, it = MEM32(0x26DC54), r, ro;
                int idx = *q == ':' ? atoi(q + 1) : 0;
                uint32_t regs[4] = { g_eax, g_ebx, g_esi, g_edi };
                done_f = 1;
                geo = call_cdecl2(EXGeoFile_LoadGeoFile_000C69C0, hash, 0);
                g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
                hdr = geo ? MEM32(geo + 0x28) : 0;
                r = MEM32(it + 0x144);
                ro = r ? MEM32(r - 4 + 0xE0) : 0;
                fprintf(stderr, "[MODS] (test) file %08X: geo %08X, %d skins; own geo %08X skins %d\n", hash, geo,
                        hdr ? (int)(int16_t)MEM16(hdr + 0x74) : -1, MEM32(it + 0x20),
                        MEM32(it + 0x20) && MEM32(MEM32(it + 0x20) + 0x28) ? (int)(int16_t)MEM16(MEM32(MEM32(it + 0x20) + 0x28) + 0x74) : -1);
                if (geo && hdr && ro && idx < (int)(int16_t)MEM16(hdr + 0x74)) {
                    MEM32(ro + 0x4C) = geo;
                    MEM32(ro + 0x50) = 0x8D000000u + (uint32_t)idx;
                    MEM32(ro + 0x60) = 0;
                    MEM32(ro + 0x64) = 0;
                    fprintf(stderr, "[MODS] (test) player 1 wears skin %d of %08X\n", idx, hash);
                    dump_skeleton("foreign", skin_data(geo, 0x8D000000u + (uint32_t)idx));
                    dump_skeleton("player", skin_data(MEM32(ro + 0x30), MEM32(ro + 0x34)));
                }
            }
        }
        {
            /* (with BUFFY_TEST_P1_FOREIGN) the remap the renderer built, 3 s later */
            static int done_rm;
            const char *fo = getenv("BUFFY_TEST_P1_FOREIGN");
            uint32_t it = MEM32(0x26DC54), r = it ? MEM32(it + 0x144) : 0, ro = r ? MEM32(r - 4 + 0xE0) : 0;
            if (fo && !done_rm && ro && GetTickCount() - t0 > (DWORD)(atoi(fo) + 3) * 1000) {
                uint32_t tab, k;
                done_rm = 1;
                if (!getenv("BUFFY_TEST_NO_REMAP"))
                    apply_rig_remap(it);
                tab = MEM32(ro + 0x80 + MEM32(ro + 0x5C) * 4);
                fprintf(stderr, "[SKEL] remap (slot %u, anim %08X/%08X, skin %08X/%08X) %08X:", MEM32(ro + 0x5C), MEM32(ro + 0x30),
                        MEM32(ro + 0x34), MEM32(ro + 0x60 + MEM32(ro + 0x5C) * 8), MEM32(ro + 0x64 + MEM32(ro + 0x5C) * 8), tab);
                for (k = 0; tab && k < 80; k++)
                    fprintf(stderr, " %u", MEM8(tab + k));
                fprintf(stderr, "\n");
            }
        }
        {
            /* (debugging) BUFFY_GEO_DUMP=secs:hash:bytes(hex) -- a loaded model
             * file's block (its header, file +0x28) to geo_<hash>.bin; its fields
             * are offsets from themselves, so it reads on its own */
            static int done_gd;
            const char *gd = getenv("BUFFY_GEO_DUMP");
            if (gd && !done_gd && strchr(gd, ':') && MEM32(0x26DC54) && GetTickCount() - t0 > (DWORD)atoi(gd) * 1000) {
                const char *q = strchr(gd, ':') + 1;
                uint32_t hash = (uint32_t)strtoul(q, (char **)&q, 16), len = *q == ':' ? (uint32_t)strtoul(q + 1, NULL, 16) : 0x400000u;
                uint32_t geo = model_geo(hash), hdr = geo ? MEM32(geo + 0x28) : 0;
                char fn[64];
                FILE *f;
                done_gd = 1;
                sprintf_s(fn, sizeof fn, "geo_%08X.bin", hash);
                if (hdr && !fopen_s(&f, fn, "wb") && f) {
                    fwrite((const void *)XBOX_PTR(hdr), 1, len, f);
                    fclose(f);
                    fprintf(stderr, "[MODS] model %08X: header %08X, %u bytes to %s\n", hash, hdr, len, fn);
                }
            }
        }
        {
            static int done_md;
            const char *md = getenv("BUFFY_MESH_DUMP");
            if (md && !done_md && strchr(md, ':') && MEM32(0x26DC54) && GetTickCount() - t0 > (DWORD)atoi(md) * 1000) {
                const char *q = strchr(md, ':') + 1;
                uint32_t hash = (uint32_t)strtoul(q, (char **)&q, 16), geo;
                uint32_t skin = *q == ':' ? (uint32_t)strtoul(q + 1, NULL, 16) : 0x8D000000u;
                done_md = 1;
                geo = model_geo(hash);
                mesh_dump(geo, skin);
            }
        }
        {
            /* BUFFY_TEST_P1_SKIN=secs:skin (testing): player 1 wears that skin */
            static int done_skin;
            const char *sk = getenv("BUFFY_TEST_P1_SKIN");
            if (sk && !done_skin && strchr(sk, ':') && MEM32(0x26DC54) && GetTickCount() - t0 > (DWORD)atoi(sk) * 1000) {
                done_skin = 1;
                apply_skin(MEM32(0x26DC54), atoi(strchr(sk, ':') + 1));
            }
        }
        {
            /* BUFFY_TEST_P2_SWAP=secs:row[,secs:row...] (testing): player 2 becomes that character */
            static int step2;
            const char *s2 = getenv("BUFFY_TEST_P2_SWAP");
            int k;
            const char *q = s2;
            for (k = 0; q && k < step2; k++)
                q = strchr(q, ',') ? strchr(q, ',') + 1 : NULL;
            if (q && *q && strchr(q, ':') && MEM32(0x26DC58) && GetTickCount() - t0 > (DWORD)atoi(q) * 1000) {
                step2++;
                dump_player(0, "before");
                dump_player(1, "before");
                p2_swap_to(atoi(strchr(q, ':') + 1));
                extra_set(1, -1);
                dump_player(0, "after player 2's swap");
                dump_player(1, "after player 2's swap");
            }
        }
        if (sw && !done_swap && strchr(sw, ':') && MEM32(0x26DC54) && GetTickCount() - t0 > (DWORD)atoi(sw) * 1000) {
            done_swap = 1;
            p1_swap_to(atoi(strchr(sw, ':') + 1));
        }
        {
            /* BUFFY_TEST_P1_EXTRA=secs:n[:outfit] (testing): player 1 becomes
             * character mod n (in load order) in that outfit, as picking it
             * on the page does; BUFFY_TEST_P1_EXTRA2 the same later (a change) */
            static int done_x[2], again_x[2];
            static const char *const names_x[2] = { "BUFFY_TEST_P1_EXTRA", "BUFFY_TEST_P1_EXTRA2" };
            int k;
            for (k = 0; k < 2; k++) {
                const char *xs = getenv(names_x[k]);
                if (xs && strchr(xs, ':') && MEM32(0x26DC54)) {
                    int e = atoi(strchr(xs, ':') + 1);
                    DWORD t = GetTickCount() - t0;
                    if (!done_x[k] && t > (DWORD)atoi(xs) * 1000 && e >= 0 && e < s_n_extra) {
                        const char *oq = strchr(strchr(xs, ':') + 1, ':');
                        done_x[k] = 1;
                        s_outfit[0][e] = oq ? atoi(oq + 1) : 0;
                        extra_set(0, e);
                        if (MEM32(MEM32(0x26DC54) + 0x16C) != (uint32_t)s_extra[e].base)
                            p1_swap_to(s_extra[e].base);
                        fprintf(stderr, "[MODS] (test) player 1 becomes character mod %d (%s), outfit %d\n", e, s_extra[e].name,
                                s_outfit[0][e]);
                    } else if (done_x[k] && !again_x[k] && t > (DWORD)(atoi(xs) + 3) * 1000) {
                        again_x[k] = 1;
                        extra_refresh(0);
                    }
                }
            }
        }
        if (pg && !done_page && MEM32(0x26DC54) && GetTickCount() - t0 > (DWORD)atoi(pg) * 1000) {
            done_page = 1;
            s_p1_action = 1;
        }
        {
            /* BUFFY_TEST_P1_CHANGE2=secs (testing): the page once more */
            static int done2;
            const char *pg2 = getenv("BUFFY_TEST_P1_CHANGE2");
            if (pg2 && !done2 && MEM32(0x26DC54) && GetTickCount() - t0 > (DWORD)atoi(pg2) * 1000) {
                done2 = 1;
                s_p1_action = 1;
            }
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
    if (type >= TYPE_PC_LINE && type < TYPE_PC_LINE + PC_LINES)
        return buffy_pc_line_press(btn, (int)(type - TYPE_PC_LINE), mask, (mask & PAD_LEFT_GAME) != 0,
                                   (mask & (PAD_A_GAME | PAD_LEFT_GAME | PAD_RIGHT_GAME)) != 0);
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
    /* the story characters and the character mods only (the others are empty) */
    if (idx < 24 && !(cs_visible_bits() & (1u << idx)))
        return 1;
    if (idx < 24 && s_join_pick < 0) {
        int i;
        s_join_pick = k_roster_row[idx];
        s_join_extra = -1;
        for (i = 0; i < s_n_extra && !(STORY_ROSTER_BITS & (1u << idx)); i++)
            if (extra_roster(i) == (int)idx) {
                s_join_extra = i;                        /* a character mod: its base character */
                s_join_pick = s_extra[i].base;
                break;
            }
        if (s_join_extra < 0 && s_join_pick >= 0 && s_join_pick <= 5) {
            int h = story_outfit_entry(s_join_pick);
            if (h >= 0 && s_outfit[s_join_who][h] > 0)
                s_join_extra = h;                        /* a story character in another outfit */
        }
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
/* Player 2 as player 0, for the length of their own update. The story's
 * player code was written for one player: it looks up "the player" in
 * slot 0 (0x26DC54) -- often inline, as Willow's magic does to check the
 * spell is in her inventory -- and asks for inventory 0 (XGetInventory(0):
 * picking up, the stake, the inventory's selection). With player 2's
 * update run as player 0 -- the two slots swapped, the players' indices
 * (handler +0x719) and the pad map's first two entries (XGamePad +0xCA8)
 * swapped to match, so player 2 still reads their own controller --
 * everything they do uses their own character, inventory and pad. It is all
 * put back afterwards. */
typedef struct { uint32_t p1, p2, h1, h2, map0, map1; int on; } AsP0;
static uint32_t s_asp0_p2;                 /* player 2's item while their update runs as player 0 */
static int      s_p2_respawn_due;          /* player 2 is to come back once their update is over */
static void p2_respawn_now(void);

/* Player 2's item -- in slot 0x26DC58, or in slot 0 while their update runs
 * as player 0. */
static uint32_t coop_p2_item(void)
{
    return s_asp0_p2 ? s_asp0_p2 : MEM32(0x26DC58);
}

static void as_p0_enter(AsP0 *st)
{
    uint32_t gp = MEM32(0x26EBB8);
    st->on = 0;
    st->p1 = MEM32(0x26DC54);
    st->p2 = MEM32(0x26DC58);
    if (!st->p1 || !st->p2 || !(st->h1 = MEM32(st->p1 + 0x14C)) || !(st->h2 = MEM32(st->p2 + 0x14C)) || !gp
            || getenv("BUFFY_COOP_NO_ASP0"))
        return;
    st->map0 = MEM32(gp + 0xCA8);
    st->map1 = MEM32(gp + 0xCA8 + 4);
    MEM32(0x26DC54) = st->p2;
    MEM32(0x26DC58) = st->p1;
    MEM8(st->h2 + 0x719) = 0;
    MEM8(st->h1 + 0x719) = 1;
    /* the players' weapons (handler +0x6BC) keep their holder's index at
     * +0x60 and find the holder by it: swapped too, so a weapon made during
     * player 2's update (a stake) ends up theirs */
    if (MEM32(st->h2 + 0x6BC)) MEM8(MEM32(st->h2 + 0x6BC) + 0x60) = 0;
    if (MEM32(st->h1 + 0x6BC)) MEM8(MEM32(st->h1 + 0x6BC) + 0x60) = 1;
    MEM32(gp + 0xCA8) = st->map1;
    MEM32(gp + 0xCA8 + 4) = st->map0;
    st->on = 1;
    s_asp0_p2 = st->p2;
}

static void as_p0_leave(AsP0 *st)
{
    uint32_t gp = MEM32(0x26EBB8), s0, s1;
    if (!st->on)
        return;
    s0 = MEM32(0x26DC54);
    s1 = MEM32(0x26DC58);
    MEM32(0x26DC54) = s1;                                 /* swapped back as they now are */
    MEM32(0x26DC58) = s0;
    MEM8(st->h2 + 0x719) = 1;
    MEM8(st->h1 + 0x719) = 0;
    if (MEM32(st->h2 + 0x6BC)) MEM8(MEM32(st->h2 + 0x6BC) + 0x60) = 1;
    if (MEM32(st->h1 + 0x6BC)) MEM8(MEM32(st->h1 + 0x6BC) + 0x60) = 0;
    if (gp) {
        MEM32(gp + 0xCA8) = st->map0;
        MEM32(gp + 0xCA8 + 4) = st->map1;
    }
    st->on = 0;
    s_asp0_p2 = 0;
}

void XItemHandler_Player_DoUpdate_0007C5C0(void)
{
    uint32_t h = g_ecx, cam_item = MEM32(0x26DC64), ch, eax;
    uint32_t p2 = MEM32(0x26DC58);
    AsP0 st;
    st.on = 0;
    if (s_story_coop && p2 && MEM32(p2 + 0x14C) == h && !in_multiplayer()) {
        uint32_t regs[3] = { g_ebx, g_esi, g_edi };
        as_p0_enter(&st);
        g_ebx = regs[0]; g_esi = regs[1]; g_edi = regs[2];
        g_ecx = h;
    }
    if (!s_story_coop || s_screens < 2 || s_split_off || !s_cam2_valid || !p2 || MEM32(p2 + 0x14C) != h
            || !cam_item || !(ch = MEM32(cam_item + 0x14C)) || !MEM32(ch + 0x410) || in_multiplayer()
            || !nv2a_gpu_has_second_window()) {
        XItemHandler_Player_DoUpdate_0007C5C0_orig();
        eax = g_eax;
        as_p0_leave(&st);
        if (s_p2_respawn_due)
            p2_respawn_now();
        g_eax = eax;
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
        as_p0_leave(&st);
        if (s_p2_respawn_due)
            p2_respawn_now();
        g_eax = eax;
        memcpy(&after, (const void *)XBOX_PTR(cam_item + 0xC0), 4);
        if (log) {
            last = GetTickCount();
            fprintf(stderr, "[COOP] player 2 update: camera 1 heading %.3f, player 2 moved by heading %.3f, camera 1 after %.3f\n",
                    before, seen, after);
        }
    }
}

/* Player 2's animation script events -- a spell's release
 * (XItemCharacterHandler::ScriptEventInvokeMagic), the stake's and other
 * weapons' moments (XItemHandler_Player::DoScriptCmdEvent) -- run with the
 * animations, outside player 2's update, and check "the player" (slot 0,
 * inventory 0) as that does: run as player 0 too. Not when already so
 * (inside player 2's update, player 2 is in slot 0). */
static int is_p2_handler(uint32_t h)
{
    uint32_t p2 = MEM32(0x26DC58);
    return s_story_coop && p2 && h && MEM32(p2 + 0x14C) == h && !in_multiplayer();
}

void XItemCharacterHandler_ScriptEventInvokeMagic_000AB1E0_orig(void);
void XItemCharacterHandler_ScriptEventInvokeMagic_000AB1E0(void)
{
    AsP0 st;
    uint32_t h = g_ecx, eax;
    if (!is_p2_handler(h)) {
        XItemCharacterHandler_ScriptEventInvokeMagic_000AB1E0_orig();
        return;
    }
    {
        uint32_t regs[3] = { g_ebx, g_esi, g_edi };
        as_p0_enter(&st);
        g_ebx = regs[0]; g_esi = regs[1]; g_edi = regs[2];
    }
    g_ecx = h;
    XItemCharacterHandler_ScriptEventInvokeMagic_000AB1E0_orig();
    eax = g_eax;
    as_p0_leave(&st);
    g_eax = eax;
}

void XItemHandler_Player_DoScriptCmdEvent_00077AF0_orig(void);
void XItemHandler_Player_DoScriptCmdEvent_00077AF0(void)
{
    AsP0 st;
    uint32_t h = g_ecx, eax;
    if (!is_p2_handler(h)) {
        XItemHandler_Player_DoScriptCmdEvent_00077AF0_orig();
        return;
    }
    {
        uint32_t regs[3] = { g_ebx, g_esi, g_edi };
        as_p0_enter(&st);
        g_ebx = regs[0]; g_esi = regs[1]; g_edi = regs[2];
    }
    g_ecx = h;
    XItemHandler_Player_DoScriptCmdEvent_00077AF0_orig();
    eax = g_eax;
    as_p0_leave(&st);
    g_eax = eax;
}

/* char XItemHandler_Player::MultiUse_CheckForPickup(char *) -- wrapped. The
 * action button by an item: the game finds the pickup in reach (+0x588, its
 * data +0x758) and plays the pickup animation (mode 0x900008C/8D, or others
 * for heavy things and weapons), whose script event takes it (PickupItem).
 * The multiplayer characters (sheet rows 6-23, Change Character) have no
 * pickup animations: the check found nothing to play and nothing happened.
 * For them the item is taken at once. */
void XItemHandler_Player_MultiUse_CheckForPickup_000869C0_orig(void);
void XItemHandler_Player_MultiUse_CheckForPickup_000869C0(void)
{
    void XItemHandler_Player_PickupItem_00086660(void);
    void XItemCharacterHandler_CheckForAnimModeInSet_000AED40(void);
    uint32_t h = g_ecx, item = MEM32(h + 4), row = item ? MEM32(item + 0x16C) : 0, pk, data, esp0;
    uint32_t regs[3] = { g_ebx, g_esi, g_edi };
    uint8_t mode0 = MEM8(h + 0x749);
    int anim;
    if ((row <= 5 && !getenv("BUFFY_PICKUP_LOG")) || in_multiplayer()) {
        XItemHandler_Player_MultiUse_CheckForPickup_000869C0_orig();
        return;
    }
    MEM32(h + 0x588) = 0;
    XItemHandler_Player_MultiUse_CheckForPickup_000869C0_orig();
    pk = MEM32(h + 0x588);
    data = pk ? MEM32(pk + 0x154) : 0;
    if (getenv("BUFFY_PICKUP_LOG"))
        fprintf(stderr, "[MODS] pickup check, character %u: ret %u pickup %08X data %08X +758 %08X mode %02X->%02X\n",
                row, g_eax & 0xFF, pk, data, MEM32(h + 0x758), mode0, MEM8(h + 0x749));
    if (row <= 5 || !(g_eax & 0xFF) || !pk || !MEM32(h + 0x758) || MEM8(h + 0x749) != mode0 || (data && (MEM32(data + 0x60) & 0x80)))
        return;                                            /* nothing in reach, an animation, or taken at once */
    esp0 = g_esp;
    g_esp -= 4; MEM32(g_esp) = 0x9000002u;
    g_esp -= 4; MEM32(g_esp) = 0x900008Cu;
    g_esp -= 4; MEM32(g_esp) = 0;
    g_ecx = h;
    XItemCharacterHandler_CheckForAnimModeInSet_000AED40();
    anim = (g_eax & 0xFF) != 0;
    g_esp = esp0;
    if (getenv("BUFFY_MODS_LOG"))
        fprintf(stderr, "[MODS] pickup by character %u: pickup %08X, pickup animation %s\n", row, pk,
                anim ? "yes" : "no -- taken at once");
    if (!anim)
        call_this0(XItemHandler_Player_PickupItem_00086660, h);
    g_ebx = regs[0]; g_esi = regs[1]; g_edi = regs[2];
    g_eax = 1;
}

/* XTrigger_InventoryUse (a key for a lock, the book cabinet's): the game
 * checks player 1 -- the item equipped in inventory 0, player 1 in range.
 * When player 1 does not set it off, it is checked again as player 2 (their
 * update's player-0 swap), so the one holding the key can use it. */
void XTrigger_InventoryUse_Active_DoUpdate_0009BD00_orig(void);
void XTrigger_InventoryUse_Active_DoUpdate_0009BD00(void)
{
    uint32_t trig = g_ecx, f44, f48, eax, regs[3];
    AsP0 st;
    f44 = MEM32(trig + 0x44);
    f48 = MEM32(trig + 0x48);
    XTrigger_InventoryUse_Active_DoUpdate_0009BD00_orig();
    eax = g_eax;
    if (!s_story_coop || !MEM32(0x26DC58) || in_multiplayer()
            || MEM32(trig + 0x44) != f44 || MEM32(trig + 0x48) != f48 || eax == 2)
        return;                                          /* player 1 set it off, or is in range */
    regs[0] = g_ebx; regs[1] = g_esi; regs[2] = g_edi;
    as_p0_enter(&st);
    g_ecx = trig;
    g_esp -= 4; MEM32(g_esp) = 0;                        /* return address */
    XTrigger_InventoryUse_Active_DoUpdate_0009BD00_orig();
    {
        uint32_t e2 = g_eax;
        as_p0_leave(&st);
        g_ebx = regs[0]; g_esi = regs[1]; g_edi = regs[2];
        if (MEM32(trig + 0x44) != f44 || MEM32(trig + 0x48) != f48 || e2 == 2) {
            if (getenv("BUFFY_MODS_LOG"))
                fprintf(stderr, "[MODS] co-op: player 2 used an item on a trigger (%u)\n", e2);
            g_eax = e2;
        } else
            g_eax = eax;
    }
}

/* XTrigger_SwapCharacter::AttemptSwap: the story swaps player 1's character
 * and puts them somewhere else (the Magic Box's changes of place). Player 2
 * comes along -- left where they were, their part of the level could be
 * unloaded under them. */
void XTrigger_SwapCharacter_AttemptSwap_000979D0_orig(void);
void XTrigger_SwapCharacter_AttemptSwap_000979D0(void)
{
    uint32_t eax;
    XTrigger_SwapCharacter_AttemptSwap_000979D0_orig();
    eax = g_eax;
    /* (tried each frame until the swap goes through: only then) */
    if ((eax & 0xFF) && s_story_coop && MEM32(0x26DC54) && MEM32(0x26DC58) && !in_multiplayer()) {
        coop_follow_at(1);
        fprintf(stderr, "[MODS] co-op: player 2 came along (the story changed place)\n");
    }
    g_eax = eax;
}

/* char EXApp::MainUpdate() -- wrapped: joining, and player 2's frame after
 * player 1's. */
void buffy_export_frame(void);

void EXApp_MainUpdate_000BD240(void)
{
    uint32_t app = g_ecx, eax, regs[4];
    {
        uint32_t r5[5] = { g_ecx, g_ebx, g_esi, g_edi, g_esp };
        buffy_export_frame();                          /* (the launcher's model export run) */
        if (MEM32(0x26DC54)) {
            void buffy_input_level_ready(void);
            static DWORD level_t0;
            const char *pj = getenv("BUFFY_TEST_P2_JOIN");
            static int joined;
            buffy_input_level_ready();                 /* (test scripts' level clock) */
            if (!level_t0)
                level_t0 = GetTickCount();
            /* BUFFY_TEST_P2_JOIN=secs:row (testing): player 2 joins as story
             * character `row`, secs after the level started (as a pick does) */
            if (pj && !joined && strchr(pj, ':') && s_story_coop && s_trig && !MEM32(0x26DC58)
                    && GetTickCount() - level_t0 > (DWORD)(atof(pj) * 1000)) {
                joined = 1;
                s_join_who = 1;
                s_join_change = 0;
                s_join_extra = -1;
                join_finish(atoi(strchr(pj, ':') + 1));
                fprintf(stderr, "[MODS] (test) player 2 joined: %08X\n", MEM32(0x26DC58));
            }
            {
                /* BUFFY_TEST_DELOAD_CHECK=secs:hash (testing): would a deload
                 * trigger keep that model file? (the game's check, as asked) */
                static int asked;
                const char *dc = getenv("BUFFY_TEST_DELOAD_CHECK");
                if (dc && !asked && strchr(dc, ':') && GetTickCount() - level_t0 > (DWORD)(atof(dc) * 1000)) {
                    void XTrigger_Database_Deload_HashCodeFileInUse_00097360(void);
                    uint32_t h = (uint32_t)strtoul(strchr(dc, ':') + 1, NULL, 16), esp0 = g_esp, regs[4] = { g_eax, g_ebx, g_esi, g_edi };
                    asked = 1;
                    g_esp -= 4; MEM32(g_esp) = h;
                    g_esp -= 4; MEM32(g_esp) = 0;
                    XTrigger_Database_Deload_HashCodeFileInUse_00097360();
                    fprintf(stderr, "[MODS] (test) deload check %08X: %s (player 1 row %d, player 2 row %d)\n", h,
                            (g_eax & 0xFF) ? "in use, kept" : "not in use, unloaded", (int)MEM32(MEM32(0x26DC54) + 0x16C),
                            MEM32(0x26DC58) ? (int)MEM32(MEM32(0x26DC58) + 0x16C) : -1);
                    g_esp = esp0;
                    g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
                }
            }
        }
        {
            /* BUFFY_TEST_LEVEL=hash (testing): a new game starts in that level
             * (0x01000024 the cemetery): the level LoadGame's single-player
             * start takes (0x1B7FAC), set until a player is made */
            static int level_set, logged;
            const char *lv = getenv("BUFFY_TEST_LEVEL");
            if (lv && !level_set) {
                uint32_t want = (uint32_t)strtoul(lv, NULL, 16), k;
                if (!logged) {
                    logged = 1;
                    fprintf(stderr, "[MODS] (test) chapters:");
                    for (k = 0; k < 24; k++)
                        fprintf(stderr, " %u:%08X/%08X", k, MEM32(0x19CA00 + k * 4), MEM32(0x19CA60 + k * 4));
                    fprintf(stderr, "\n");
                }
                if (MEM32(0x26DC54))
                    level_set = 1;
                else
                    for (k = 0; k < 24; k++)
                        if (MEM32(0x19CA00 + k * 4) == want || MEM32(0x19CA60 + k * 4) == want)
                            MEM8(0x26E918) = (uint8_t)k;       /* (the chapter the single-player start takes) */
            }
        }
        g_ecx = r5[0]; g_ebx = r5[1]; g_esi = r5[2]; g_edi = r5[3]; g_esp = r5[4];
        app = g_ecx;
    }
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
    {
        /* each player's inventory window only in their own window */
        uint32_t w = MEM32(g_esp + 4), w2 = hud_inv2();
        if (w2 && (w == w2 ? !s_in_pass2 : s_in_pass2 && w == hud_inv1())) {
            g_eax = 1;
            g_esp += 4;
            return;
        }
    }
    EXBaseDisplay_RedrawWindow_000D7290_orig();
    {
        void buffy_menu_ivory_restore(void);
        buffy_menu_ivory_restore();
    }
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
static void coop_follow(void);
void XItemCharacterHandler_SetFadeIn_000AA790(void);
void XItemHandler_Player_InitializeSpecificPlayer_0007A5D0(void);

void XItemHandler_Player_HandlePlayerDeath_00082BA0(void)
{
    uint32_t h = g_ecx, item = MEM32(h + 4), t;
    /* (player 2's update runs as player 0: player 2 is coop_p2_item, not
     * slot 0x26DC58 -- by the slot, their fall into the acid ended the game) */
    if (!s_story_coop || in_multiplayer() || !item || item != coop_p2_item()) {
        XItemHandler_Player_HandlePlayerDeath_00082BA0_orig();
        return;
    }
    g_esp += 4;                                     /* ret */
    t = MEM32(h + 0x70C);
    if (t < P2_RESPAWN_FRAMES) {
        MEM32(h + 0x70C) = t + 1;
        return;
    }
    s_p2_respawn_due = 1;                           /* brought back after their update (p2_respawn_now) */
}

/* Player 2 back beside player 1 (their death's wait over), with the slots
 * as they normally are. */
static void p2_respawn_now(void)
{
    uint32_t p2 = MEM32(0x26DC58), h = p2 ? MEM32(p2 + 0x14C) : 0, regs[4];
    s_p2_respawn_due = 0;
    if (!h || !MEM32(0x26DC54))
        return;
    regs[0] = g_ebx; regs[1] = g_esi; regs[2] = g_edi; regs[3] = g_eax;
    MEM32(h + 0x70C) = 0;
    coop_follow();
    call_this0(XItemCharacterHandler_SetFadeIn_000AA790, h);
    call_this1(XItemHandler_Player_InitializeSpecificPlayer_0007A5D0, h, 1);
    g_ebx = regs[0]; g_esi = regs[1]; g_edi = regs[2]; g_eax = regs[3];
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

/* ── window 2's inventory window ──────────────────────────────────────────
 *
 * The HUD's inventory window (XHudWnd +0x50, an XHudScriptWnd on script
 * 0x04000188) shows the selected item's name and its turning 3D model.
 * Updated once for each player a frame, it swapped its model every frame and
 * each screen showed the other player's item behind its own. So player 2 has
 * a window of their own, made the game's way (XHudWnd::ShowInventory, with
 * +0x50 empty for the call): updated reading player 2's inventory and
 * controller, drawn only in window 2 -- and player 1's only in window 1. Its
 * pointer lives in a word of Xbox memory, which its +0x1B4 points at, so the
 * game clears it when the window goes. */
static uint32_t s_inv2_slot;               /* player 2's inventory window */

static uint32_t hud_inv1(void)
{
    uint32_t app = MEM32(0x26D868), hud = app ? MEM32(app + 0x230) : 0;
    return hud ? MEM32(hud + 0x50) : 0;
}

static uint32_t hud_inv2(void)
{
    return s_inv2_slot ? MEM32(s_inv2_slot) : 0;
}

/* Before player 2's pass: player 2's inventory window made if missing. */
static void hud_for_p2(int on)
{
    uint32_t app = MEM32(0x26D868), hud = app ? MEM32(app + 0x230) : 0, w1 = hud ? MEM32(hud + 0x50) : 0, w2;
    uint32_t regs[4] = { g_eax, g_ebx, g_esi, g_edi };
    if (!on || !w1 || !MEM32(0x26DC58) || hud_inv2())
        return;
    if (!s_inv2_slot) {
        s_inv2_slot = xbox_HeapAlloc(4, 4);
        if (!s_inv2_slot)
            return;
        MEM32(s_inv2_slot) = 0;
    }
    MEM32(hud + 0x50) = 0;
    call_this0(XHudWnd_ShowInventory_000551B0, hud);
    w2 = MEM32(hud + 0x50);
    MEM32(hud + 0x50) = w1;
    if (w2) {
        MEM32(s_inv2_slot) = w2;
        MEM32(w2 + 0x1B4) = s_inv2_slot;
    }
    g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
}

/* char XHudScriptWnd::Update() -- wrapped: player 2's inventory window
 * reads player 2's inventory ("player 1's", XGetInventory(0), made 1's) and
 * controller (slot 0's through the pad map: the d-pad cycles the selection).
 * It goes with player 2, or with player 1's. */
void XHudScriptWnd_Update_00052DD0_orig(void);
void XHudScriptWnd_Update_00052DD0(void)
{
    uint32_t w = g_ecx, gp, m0, m1;
    if (!w || w != hud_inv2()) {
        XHudScriptWnd_Update_00052DD0_orig();
        return;
    }
    if (!MEM32(0x26DC58) || !hud_inv1() || in_multiplayer()) {
        if (!(MEM32(w + 0x1A8) & 0x8000000u)) {
            call_this0(XHudScriptWnd_KillNextFrame_00050280, w);
            g_ecx = w;
        }
        XHudScriptWnd_Update_00052DD0_orig();
        return;
    }
    gp = MEM32(0x26EBB8);
    m0 = gp ? MEM32(gp + 0xCA8) : 0;
    m1 = gp ? MEM32(gp + 0xCAC) : 0;
    if (gp) { MEM32(gp + 0xCA8) = m1; MEM32(gp + 0xCAC) = m0; }
    s_hud_p2 = 1;
    XHudScriptWnd_Update_00052DD0_orig();
    s_hud_p2 = 0;
    if (gp) { MEM32(gp + 0xCA8) = m0; MEM32(gp + 0xCAC) = m1; }
}

/* buffy_input.c: while player 2 is in a story level, controller 2's Back is
 * ours (it brings player 2 to player 1) and the game does not see it. */
int buffy_coop_owns_back(void)
{
    return s_story_coop && s_teleport_on_back && MEM32(0x26DC58) && !in_multiplayer();
}

/* The in-level Character Select keeps X from the game (its own X picks a
 * random character): X there changes the outfit (cs_outfit_input). */
int buffy_coop_owns_x(int port)
{
    return s_join_wnd && port == (s_join_who & 1);
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
    /* the old model file is unloaded only if player 1's character is not in
     * it: Buffy, Willow, Faith (and Tara) share one */
    int unload = p1 && model_of_row((int)MEM32(p1 + 0x16C)) != model_of_row((int)old);
    if (!h || old == (uint32_t)row)
        return;
    if (row > 5 || old > 5) {
        p2_respawn_as(row);                  /* the swap takes story characters only */
        return;
    }
    if (p1 && MEM32(p1 + 0x16C) != old)
        stop_char_sounds(extra_sound_row(1, (int)old));   /* (player 1 is someone else) */
    {
        uint32_t inv_old = player_inventory(p2);
        g_esp -= 4; MEM32(g_esp) = (uint32_t)unload;
        g_esp -= 4; MEM32(g_esp) = (uint32_t)(row + 1);
        g_esp -= 4; MEM32(g_esp) = 0;
        g_ecx = h;
        XItemHandler_Player_SwapCharacter_0007B200_orig();
        g_esp = esp0;
        inv_carry(inv_old, player_inventory(p2));
    }
    load_char_sounds(row);
    set_char_voice(p2, row);
    s_inv_1 = s_inv_2 = 0;
    fprintf(stderr, "[MODS] co-op: player 2 is now character %d\n", (int)MEM32(p2 + 0x16C));
}

static void join_finish(int row)
{
    uint32_t regs[4] = { g_eax, g_ebx, g_esi, g_edi };
    if (s_join_who == 0) {
        p1_swap_to(row);                                     /* player 1's Change Character */
        extra_set(0, s_join_extra);
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
    if (MEM32(0x26DC58))
        extra_set(1, s_join_extra);
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
        for (r = 0; data && r < 24; r++) {
            models[r] = MEM32(data + (uint32_t)r * 0x1C);
            s_sheet_b4[r] = MEM8(data + (uint32_t)r * 0x1C + 4);
            s_sheet_b5[r] = MEM8(data + (uint32_t)r * 0x1C + 5);
            if (getenv("BUFFY_SHEET_LOG")) {
                int k;
                fprintf(stderr, "[SHEET] row %2d:", r);
                for (k = 0; k < 0x1C; k += 4)
                    fprintf(stderr, " %08X", MEM32(data + (uint32_t)r * 0x1C + (uint32_t)k));
                fprintf(stderr, "\n");
            }
        }
        if (getenv("BUFFY_SHEET_LOG"))
            for (r = 0; r < 6; r++)
                fprintf(stderr, "[SHEET] story model %d: %08X\n", r, MEM32(STORY_MODELS + (uint32_t)r * 4));
    }
    return row >= 0 && row < 24 ? models[row] : 0;
}

/* The model file a character-sheet row's character is in. */
static uint32_t model_of_row(int row)
{
    if (row >= 0 && row <= 5)
        return MEM32(STORY_MODELS + (uint32_t)row * 4);
    return sheet_model(row);
}

/* A character's sound set (voice, grunts, footsteps): the game loads it only
 * for the characters a story level is made for, or for each player of a
 * multiplayer match (XTrigger_PlayerStart::Init's table, by character-sheet
 * row). A character changed to mid-level had none: silent. The loader keeps
 * one copy of a bank, so asking again is harmless. */
void EXSoundManager_LoadSoundBank_00110AB0(void);
static const uint8_t k_char_sounds[24] = {
    0x2C, 0x2F, 0x30, 0x31, 0x24, 0x20, 0x2B, 0x29, 0x1C, 0x1A, 0x1C, 0x1A,
    0x2E, 0x25, 0x27, 0x26, 0x2D, 0x1B, 0x28, 0x1D, 0x1A, 0x22, 0x1E, 0x23,
};

/* ...and the character's speech lines (handler +0x71A, from the sheet row's
 * +5; the player start sets it, a swap left the old character's). */
static void set_char_voice(uint32_t item, int row)
{
    uint32_t h = item ? MEM32(item + 0x14C) : 0;
    if (!h || row < 0 || row > 23 || !sheet_model(row))
        return;
    MEM8(h + 0x71A) = s_sheet_b5[row];
}

/* A change of character in the middle of a line: the old character's voice
 * went on over the new one's, and the new sound set loading under it cut it
 * off badly. The old character's sounds that are playing (non-looping, from
 * their sound set's bank) are stopped first, the way the game stops sounds
 * (+0x290, SFXKillNonLooping). Banks: ids at 0x2786F8 (40 slots); a slot's
 * sounds at 0x26F2EC + slot * 0x28 (count at -4, then pairs of id, offset);
 * the playing sounds are a list from 0x278E6C (+0x10 the id, +0x250 the
 * sound's data, whose +0x10 bit 0x40 is looping). */
static void stop_char_sounds(int row)
{
    uint32_t bank, base, n, arr, e, k, i;
    int stopped = 0;
    if (row < 0 || row > 23)
        return;
    bank = k_char_sounds[row];
    for (k = 0; k < 40 && MEM32(0x2786F8 + k * 4) != bank; k++)
        ;
    if (k == 40) {
        if (getenv("BUFFY_MODS_LOG"))
            fprintf(stderr, "[MODS] sound set %02X not loaded\n", bank);
        return;
    }
    base = 0x26F2ECu + k * 0x28u;
    n = MEM32(base - 4);
    arr = MEM32(base);
    for (e = MEM32(0x278E6C); e && arr; e = MEM32(e)) {
        uint32_t id = MEM32(e + 0x10), data = MEM32(e + 0x250);
        if (data && (MEM8(data + 0x10) & 0x40))
            continue;                                      /* looping */
        for (i = 0; i < n && i < 4096; i++)
            if (MEM32(arr + i * 8) == id) {
                MEM32(e + 0x290) = 1;
                stopped++;
                break;
            }
    }
    if (getenv("BUFFY_MODS_LOG"))
        fprintf(stderr, "[MODS] sound set %02X (slot %u, %u sounds): %d playing stopped\n", bank, k, n, stopped);
}

static void load_char_sounds(int row)
{
    uint32_t mgr = MEM32(0x26EB24);
    if (!mgr || row < 0 || row > 23)
        return;
    if (getenv("BUFFY_MODS_LOG")) {
        int i;
        fprintf(stderr, "[MODS] sound bank requests so far:");
        for (i = 0; i < 10; i++)
            fprintf(stderr, " %02X", MEM32(0x27D4A8 + i * 0x1A0) & 0xFFFFFF);
        fprintf(stderr, "\n");
    }
    call_this1(EXSoundManager_LoadSoundBank_00110AB0, mgr, k_char_sounds[row]);
    if (getenv("BUFFY_MODS_LOG"))
        fprintf(stderr, "[MODS] sound set %02X for character %d\n", k_char_sounds[row], row);
}

/* Each story character has an inventory of their own (a multiplayer
 * character uses the first), so a change of character left the items
 * behind. The new inventory takes the old one's items -- all but the ones
 * that belong to a character (fists, own weapon, spells) -- and the
 * selection when it is one of them. */
static void inv_carry(uint32_t from, uint32_t to)
{
    int id;
    uint32_t regs[4] = { g_eax, g_ebx, g_esi, g_edi };
    if (!from || !to || from == to)
        return;
    for (id = 1; id < INV_ITEMS; id++)
        if (inv_shared(id))
            inv_set(to, id, inv_count(from, id));
    {
        uint32_t sel = MEM32(from + 0), eq = MEM32(from + 4);
        if (sel && sel < INV_ITEMS && inv_shared((int)sel) && inv_count(to, (int)sel) >= 0)
            MEM32(to + 0) = sel;
        if (eq && eq < INV_ITEMS && inv_shared((int)eq) && inv_count(to, (int)eq) >= 0)
            MEM32(to + 4) = eq;
    }
    g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
}

/* ── alternate skins ───────────────────────────────────────────────────────
 *
 * A character-sheet row whose +4 byte is not 0xFF wears an alternate skin of
 * its model file -- how the multiplayer characters that share a story
 * character's body get their own look (Tara: Buffy's file p01_buff, skin 5).
 * XTrigger_PlayerStart does it after the player is made; this is the same:
 * the skin table 0x1B1628 (8 bytes an entry, skin 1 first: the skin's id, the
 * section file holding it), the section file loaded (EXGeoHeader::
 * LoadSectionFile on the model's +0x28), the skin looked up
 * (GeoAnimSkinHeader) and set on the character's skinned render object
 * (SetSkinRefObject: +0x4C model, +0x50 skin), and the handler told
 * (+0xABE8 the skin, +0x3D0 skin - 1, the shadow's skin rebuilt). */
void EXGeoHeader_LoadSectionFile_000C67D0(void);
void EXGeoHeader_GeoAnimSkinHeader_0001D200(void);
void XItemCharacterHandler_SetupReducedPolyShadowSkin_000AEBB0(void);

static int apply_skin(uint32_t item, int skin)
{
    uint32_t h = item ? MEM32(item + 0x14C) : 0, r, obj, geo, hdr, esp0, regs[4], id, sect;
    if (!h || skin < 1 || skin > 12 || !(r = MEM32(item + 0x144)))
        return 0;
    obj = r - 4;
    geo = MEM32(obj + 0x20);
    if (!geo || !MEM32(geo + 0x28) || !MEM32(obj + 0xE0))
        return 0;
    id = MEM32(0x1B1628 + (uint32_t)(skin - 1) * 8);
    sect = MEM32(0x1B1628 + (uint32_t)(skin - 1) * 8 + 4);
    regs[0] = g_eax; regs[1] = g_ebx; regs[2] = g_esi; regs[3] = g_edi;
    esp0 = g_esp;
    g_esp -= 4; MEM32(g_esp) = 0;
    g_esp -= 4; MEM32(g_esp) = sect;
    g_esp -= 4; MEM32(g_esp) = 0;
    g_ecx = MEM32(geo + 0x28);
    EXGeoHeader_LoadSectionFile_000C67D0();
    g_esp = esp0;
    g_esp -= 4; MEM32(g_esp) = id;
    g_esp -= 4; MEM32(g_esp) = 0;
    g_ecx = MEM32(geo + 0x28);
    EXGeoHeader_GeoAnimSkinHeader_0001D200();
    hdr = g_eax;
    g_esp = esp0;
    if (hdr) {
        uint32_t ro = MEM32(obj + 0xE0);
        MEM32(ro + 0x4C) = geo;
        MEM32(ro + 0x50) = id;
        MEM32(ro + 0x60) = 0;
        MEM32(ro + 0x64) = 0;
    }
    MEM8(h + 0xABE8) = (uint8_t)skin;
    MEM8(h + 0x3D0) = (uint8_t)(skin - 1);
    call_this0(XItemCharacterHandler_SetupReducedPolyShadowSkin_000AEBB0, h);
    g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
    if (getenv("BUFFY_MODS_LOG"))
        fprintf(stderr, "[MODS] skin %d (%08X from %08X) on item %08X: %s\n", skin, id, sect, item,
                hdr ? "set" : "not found");
    return hdr != 0;
}

/* The page's lit portraits: the story characters and the character mods'. */
static uint32_t cs_visible_bits(void)
{
    uint32_t bits = STORY_ROSTER_BITS;
    int i, r;
    for (i = 0; i < s_n_extra; i++)
        if ((r = extra_roster(i)) >= 0)
            bits |= 1u << r;
    return bits;
}

/* The skin a character mod's look row wears (its sheet +4), when its model
 * file is the base character's: else 0 (the look cannot be worn). */
static int extra_skin(int e, int look)
{
    int base = s_extra[e].base, skin;
    uint32_t m;
    if (look < 0)
        return 0;
    m = sheet_model(look);
    skin = s_sheet_b4[look];
    if (!m || skin == 0xFF || m != MEM32(STORY_MODELS + (uint32_t)base * 4))
        return 0;
    return skin;
}

/* Sound set / voice row of a player: the character mod's look when worn. */
static int extra_sound_row(int who, int row)
{
    int e = s_player_extra[who], skin, look, rigid;
    uint32_t model;
    const wchar_t *file;
    if (e < 0 || s_extra[e].base != row || !s_extra[e].look_sounds)
        return row;
    outfit_of(who, e, &model, &skin, &look, &file, &rigid);
    return look >= 0 ? look : row;
}

static uint32_t s_def_skin[2][4];          /* the base look before a mod's: render +4C/+50, handler +ABE8/+3D0 */
static uint32_t s_def_obj[2];              /* ...of this render object */
static int      s_worn[2];                 /* the look was changed (by skin or model) */
static int      s_remap_due[2];            /* a model's rig table to set once the renderer has the skin */

void EXGeoFile_GetGeoFile_000C6960(void);

/* A model file, loaded if it is not. */
static uint32_t model_geo(uint32_t hash)
{
    /* (GetGeoFile gives the file's record loaded or not: LoadGeoFile loads
     * it when it is not, and gives the same record) */
    uint32_t regs[4] = { g_eax, g_ebx, g_esi, g_edi }, geo;
    geo = call_cdecl2(EXGeoFile_LoadGeoFile_000C69C0, hash, 0);
    g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
    return geo && MEM32(geo + 0x28) ? geo : 0;
}

/* A model's skin, its sections loaded until it is there (the enemy families --
 * vampires, the skeleton -- keep most skins in section files); 0 if none has it. */
static uint32_t model_skin_loaded(uint32_t geo, uint32_t skin_id)
{
    uint32_t hdr = MEM32(geo + 0x28), secs, ns, k, data = skin_data(geo, skin_id);
    for (k = 0, ns = (uint32_t)(int16_t)MEM16(hdr + 0x54), secs = MEM32(hdr + 0x58) ? hdr + 0x58 + MEM32(hdr + 0x58) : 0;
         !data && secs && k < ns && k < 64; k++) {
        uint32_t sect = MEM32(secs + k * 0x14), regs[4] = { g_eax, g_ebx, g_esi, g_edi }, esp0 = g_esp;
        if (!sect)
            continue;
        g_esp -= 4; MEM32(g_esp) = 0;
        g_esp -= 4; MEM32(g_esp) = sect;
        g_esp -= 4; MEM32(g_esp) = 0;
        g_ecx = hdr;
        EXGeoHeader_LoadSectionFile_000C67D0();
        g_esp = esp0;
        g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
        data = skin_data(geo, skin_id);
        if (data && getenv("BUFFY_MODS_LOG"))
            fprintf(stderr, "[MODS] skin %08X is in section %08X\n", skin_id, sect);
    }
    return data;
}

/* Frame hook: the rig tables due. */
static void extra_remaps(void)
{
    int who;
    for (who = 0; who < 2; who++) {
        uint32_t item = MEM32(0x26DC54 + (uint32_t)who * 4), r = item ? MEM32(item + 0x144) : 0, ro = r ? MEM32(r - 4 + 0xE0) : 0, slot;
        if (!s_remap_due[who])
            continue;
        if (!ro) {
            s_remap_due[who] = 0;
            continue;
        }
        slot = MEM32(ro + 0x5C);
        if (getenv("BUFFY_REMAP_LOG")) {
            static int logs;
            if (!(logs++ % 30) && logs < 3000)
                fprintf(stderr, "[MODS] remap wait: slot %u +4C %08X +50 %08X +60 %08X +64 %08X +80 %08X\n", slot, MEM32(ro + 0x4C),
                        MEM32(ro + 0x50), MEM32(ro + 0x60 + slot * 8), MEM32(ro + 0x64 + slot * 8), MEM32(ro + 0x80 + slot * 4));
        }
        if (MEM32(ro + 0x60 + slot * 8) != MEM32(ro + 0x4C))
            continue;                                    /* not drawn with it yet */
        s_rigid_hands = s_worn_rigid[who];
        apply_rig_remap(item);
        s_remap_due[who] = 0;
    }
}

/* Player `who` (0/1) is their character mod if they are its base character. */
static void extra_refresh(int who)
{
    uint32_t item = MEM32(0x26DC54 + (uint32_t)who * 4), h = item ? MEM32(item + 0x14C) : 0, r, ro;
    int e = s_player_extra[who], skin = 0, mskin, look, rigid;
    uint32_t geo = 0, model;
    const wchar_t *file;
    if (!h || e < 0 || in_multiplayer() || MEM32(item + 0x16C) != (uint32_t)s_extra[e].base)
        return;
    if (!(r = MEM32(item + 0x144)) || !(ro = MEM32(r - 4 + 0xE0)))
        return;
    outfit_of(who, e, &model, &mskin, &look, &file, &rigid);
    if (model) {
        if (!(geo = model_geo(model)))
            return;
        if (!model_skin_loaded(geo, 0x8D000000u + (uint32_t)mskin)) {
            static int said;
            if (!said++)
                fprintf(stderr, "[MODS] model %08X has no skin %d\n", model, mskin);
            return;
        }
        if (file)
            buffy_model_import(MEM32(geo + 0x28), skin_data(geo, 0x8D000000u + (uint32_t)mskin), file);
        if (MEM32(ro + 0x4C) == geo && MEM32(ro + 0x50) == 0x8D000000u + (uint32_t)mskin)
            return;                                      /* worn already */
    } else {
        if (!(skin = extra_skin(e, look)))
            return;
        if (MEM8(h + 0xABE8) == skin && MEM32(ro + 0x50) == MEM32(0x1B1628 + (uint32_t)(skin - 1) * 8))
            return;                                      /* worn already */
    }
    if (!s_worn[who] || s_def_obj[who] != ro) {
        s_def_obj[who] = ro;
        s_def_skin[who][0] = MEM32(ro + 0x4C);
        s_def_skin[who][1] = MEM32(ro + 0x50);
        s_def_skin[who][2] = MEM8(h + 0xABE8);
        s_def_skin[who][3] = MEM8(h + 0x3D0);
    }
    if (geo) {
        /* another model's skin on this skeleton; its rig table once drawn */
        MEM32(ro + 0x4C) = geo;
        MEM32(ro + 0x50) = 0x8D000000u + (uint32_t)mskin;
        MEM32(ro + 0x60) = 0;
        MEM32(ro + 0x64) = 0;
        s_remap_due[who] = 1;
        if (getenv("BUFFY_MODS_LOG"))
            fprintf(stderr, "[MODS] player %d wears model %08X skin %d (outfit %d)\n", who + 1, model, mskin, s_outfit[who][e]);
    } else
        apply_skin(item, skin);
    s_worn[who] = 1;
    s_worn_key[who] = e * 64 + s_outfit[who][e];
    s_worn_look[who] = look;
    s_worn_model[who] = model;
    s_worn_rigid[who] = rigid;
    if (s_extra[e].look_sounds && look >= 0) {
        set_char_voice(item, look);
        load_char_sounds(look);
    }
}

/* Player `who` picked character mod `e` (or -1: a plain character). */
static void extra_set(int who, int e)
{
    uint32_t item = MEM32(0x26DC54 + (uint32_t)who * 4), h = item ? MEM32(item + 0x14C) : 0, r, ro;
    int old = s_player_extra[who];
    if (old >= 0 && h && (e != old || (s_worn[who] && s_worn_key[who] != e * 64 + s_outfit[who][e]))) {
        /* the mod's look off: its sounds stopped, the base look back (same
         * render object: the player stayed that character) */
        if (s_extra[old].look_sounds && s_worn_look[who] >= 0)
            stop_char_sounds(s_worn_look[who]);
        r = MEM32(item + 0x144);
        ro = r ? MEM32(r - 4 + 0xE0) : 0;
        if (ro && ro == s_def_obj[who] && s_worn[who] && MEM32(item + 0x16C) == (uint32_t)s_extra[old].base) {
            MEM32(ro + 0x4C) = s_def_skin[who][0];
            MEM32(ro + 0x50) = s_def_skin[who][1];
            MEM32(ro + 0x60) = 0;
            MEM32(ro + 0x64) = 0;
            MEM8(h + 0xABE8) = (uint8_t)s_def_skin[who][2];
            MEM8(h + 0x3D0) = (uint8_t)s_def_skin[who][3];
            call_this0(XItemCharacterHandler_SetupReducedPolyShadowSkin_000AEBB0, h);
        } else if (MEM32(item + 0x16C) != (uint32_t)s_extra[old].base && !s_worn_model[who]) {
            /* the swap made them someone else already, with their own look:
             * only the mark of the mod's skin goes */
            MEM8(h + 0xABE8) = 0xFF;
        }
        s_worn[who] = 0;
        s_worn_key[who] = -1;
        s_worn_look[who] = -1;
        s_worn_model[who] = 0;
        s_remap_due[who] = 0;
        set_char_voice(item, (int)MEM32(item + 0x16C));
        load_char_sounds((int)MEM32(item + 0x16C));
    }
    s_player_extra[who] = e;
    if (e >= 0)
        extra_refresh(who);
    if (getenv("BUFFY_MODS_LOG"))
        fprintf(stderr, "[MODS] player %d plays %s\n", who + 1, e >= 0 ? "a character mod" : "a story character");
}

/* (debugging, BUFFY_MODS_LOG) a player's model, skin and items */
static void dump_player(int who, const char *why)
{
    uint32_t it = MEM32(0x26DC54 + (uint32_t)who * 4), h = it ? MEM32(it + 0x14C) : 0, r, ro = 0, inv;
    int id, n = 0;
    if (!h || !getenv("BUFFY_MODS_LOG"))
        return;
    r = MEM32(it + 0x144);
    if (r)
        ro = MEM32(r - 4 + 0xE0);
    fprintf(stderr, "[MODS] %s: player %d row %u, model %08X anim %08X, skin +3D0 %d +ABE8 %d, render %08X +4C %08X +50 %08X\n",
            why, who + 1, MEM32(it + 0x16C), MEM32(it + 0x20), MEM32(it + 0x24), (int)(int8_t)MEM8(h + 0x3D0),
            (int)MEM8(h + 0xABE8), ro, ro ? MEM32(ro + 0x4C) : 0, ro ? MEM32(ro + 0x50) : 0);
    inv = player_inventory(it);
    fprintf(stderr, "[MODS]   items:");
    for (id = 1; id < INV_ITEMS && inv; id++)
        if (inv_count(inv, id) > 0) {
            fprintf(stderr, " %02X(g%d)x%d", id, MEM8(MEM32(0x1B8010) + id * 0x1F0 + 0x192), inv_count(inv, id));
            n++;
        }
    fprintf(stderr, " (%d)\n", n);
}

/* ── skeletons (for retargeting a foreign model onto the player rig) ──────
 *
 * A model file's skins: header (file +0x28) +0x74 a common array (count word
 * at +0, entries 0x1C bytes from the relative offset at +4); an entry's +0xC
 * is the skin/skeleton data. Its fields hold offsets relative to themselves:
 * +0x1C bone count, +0x20 pivots (16 bytes a bone), +0x28 hierarchy (8 bytes a
 * bone, parent word first), +0x30 the bone ids, (index word, id word) pairs
 * sorted by id, ending with id 0xFFFF. */
void EXGeoCommonArray_FindIndex_000CBFB0(void);

static uint32_t rel32(uint32_t p, uint32_t off)
{
    return MEM32(p + off) ? p + off + MEM32(p + off) : 0;
}

static uint32_t skin_data(uint32_t geo, uint32_t id)
{
    uint32_t hdr = geo ? MEM32(geo + 0x28) : 0, arr, idx, regs[4] = { g_eax, g_ebx, g_esi, g_edi };
    if (!hdr)
        return 0;
    arr = hdr + 0x74;
    idx = call_cdecl3(EXGeoCommonArray_FindIndex_000CBFB0, id, arr, 0x1C);
    g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
    if ((int32_t)idx < 0 || !rel32(arr, 4))
        return 0;
    return MEM32(rel32(arr, 4) + idx * 0x1C + 0xC);
}

static void dump_skeleton(const char *name, uint32_t data)
{
    uint32_t n, ids, hier, piv, k;
    if (!data) {
        fprintf(stderr, "[SKEL] %s: none\n", name);
        return;
    }
    n = MEM32(data + 0x1C);
    ids = rel32(data, 0x30);
    hier = rel32(data, 0x28);
    piv = rel32(data, 0x20);
    fprintf(stderr, "[SKEL] %s: %u bones (data %08X)\n", name, n, data);
    for (k = 0; k < n && k < 128; k++) {
        float x = 0, y = 0, z = 0;
        uint32_t id = 0xFFFF, j;
        for (j = 0; ids && j < 256 && MEM16(ids + j * 4 + 2) != 0xFFFF; j++)
            if (MEM16(ids + j * 4) == k) {
                id = MEM16(ids + j * 4 + 2);
                break;
            }
        if (piv) {
            memcpy(&x, (const void *)XBOX_PTR(piv + k * 16), 4);
            memcpy(&y, (const void *)XBOX_PTR(piv + k * 16 + 4), 4);
            memcpy(&z, (const void *)XBOX_PTR(piv + k * 16 + 8), 4);
        }
        fprintf(stderr, "[SKEL] %s %3u id %04X parent %3d pivot %8.3f %8.3f %8.3f\n", name, k, id,
                hier ? (int)(int16_t)MEM16(hier + k * 8) : -9, x, y, z);
    }
}

/* A foreign skin (another model's, on its own rig) on a player's skeleton:
 * the renderer maps the skin's bones to the skeleton's through a remap table
 * (render object +0x80 + slot * 4, a byte a skin bone), which it only builds
 * when the two rigs' ids differ -- the boss rigs claim the player's, so bone
 * n of the skin was drawn with skeleton bone n: arms and fingers everywhere.
 * The table is built here from the two hierarchies: the root to the root; each
 * bone to the free child of its parent's match whose rest position (pivot) is
 * nearest; a bone with none (a third finger joint, a coat tail) moves with its
 * parent's match. Allocated the renderer's way (EXAlloc), so it frees it. */
void EXAlloc_000BD070(void);

static int build_rig_remap(uint32_t skin, uint32_t skel, uint8_t *map, int max, int16_t *owner)
{
    uint32_t nf = MEM32(skin + 0x1C), np = MEM32(skel + 0x1C);
    uint32_t hf = rel32(skin, 0x28), hp = rel32(skel, 0x28), pf = rel32(skin, 0x20), pp = rel32(skel, 0x20);
    uint8_t taken[256] = { 0 };
    uint32_t k, j;
    if (!hf || !hp || !pf || !pp || nf > (uint32_t)max || np > 255)
        return 0;
    for (k = 0; k < nf; k++) {
        int parent = (int)(int16_t)MEM16(hf + k * 8), best = -1;
        float bd = 1e30f, fx, fy, fz;
        memcpy(&fx, (const void *)XBOX_PTR(pf + k * 16), 4);
        memcpy(&fy, (const void *)XBOX_PTR(pf + k * 16 + 4), 4);
        memcpy(&fz, (const void *)XBOX_PTR(pf + k * 16 + 8), 4);
        for (j = 0; j < np; j++) {
            int pj = (int)(int16_t)MEM16(hp + j * 8);
            float x, y, z, d;
            if (taken[j])
                continue;
            if (parent < 0 ? pj >= 0 : (pj < 0 || (uint32_t)parent >= k || pj != map[parent]))
                continue;
            memcpy(&x, (const void *)XBOX_PTR(pp + j * 16), 4);
            memcpy(&y, (const void *)XBOX_PTR(pp + j * 16 + 4), 4);
            memcpy(&z, (const void *)XBOX_PTR(pp + j * 16 + 8), 4);
            d = (x - fx) * (x - fx) + (y - fy) * (y - fy) + (z - fz) * (z - fz);
            if (d < bd) {
                bd = d;
                best = (int)j;
            }
        }
        if (best >= 0) {
            map[k] = (uint8_t)best;
            taken[best] = 1;
            if (owner)
                owner[best] = (int16_t)k;
        } else
            map[k] = parent >= 0 && (uint32_t)parent < k ? map[parent] : 0;
    }
    if (s_rigid_hands) {
        /* rigid hands: a bone below a hand (a skeleton bone with 4 or more
         * children -- the fingers) moves with the hand; the skeleton's
         * fingers curl about their own pivots, where the foreign hand's
         * fingers are not */
        uint8_t kids[256] = { 0 };
        for (j = 0; j < np; j++) {
            int pj = (int)(int16_t)MEM16(hp + j * 8);
            if (pj >= 0 && pj < 256)
                kids[pj]++;
        }
        for (k = 0; k < nf; k++) {
            int b = map[k], guard = 0;
            while (b >= 0 && guard++ < 64) {
                int pb = (int)(int16_t)MEM16(hp + (uint32_t)b * 8);
                if (pb >= 0 && kids[pb] >= 4) {
                    map[k] = (uint8_t)pb;
                    break;
                }
                b = pb;
            }
        }
    }
    return (int)nf;
}

/* ── retargeting: the foreign model's own proportions ─────────────────────
 *
 * The renderer skins a vertex as W(t) * (v - pivot) with the skeleton's
 * animated matrix W and its rest pivot (rest rotations are identity: the rest
 * pose is only positions). A foreign model on a player skeleton kept the
 * player's joint positions: its longer arms were pulled to the player's
 * shoulders and wrists -- stretched. So for its render object, around
 * ApplySkin: each joint's animated local transform (its rotation, from W of
 * the joint and its parent) is put back together with the foreign model's
 * bone offsets instead of the skeleton's, and the pivot list the renderer
 * reads is the foreign model's for the call. The game's own matrices are
 * restored after the call. Matrices are D3DX row vectors: world = local *
 * parent, the translation in the fourth row. */
#define RT_MAX_BONES 96
typedef struct {
    uint32_t ro, skel, piv_guest;                 /* render object, its skeleton data, our pivot list */
    int n;
    int16_t parent[RT_MAX_BONES];
    float pp[RT_MAX_BONES][3], pr[RT_MAX_BONES][3];   /* the skeleton's pivots, the foreign model's */
} Retarget;
static Retarget s_rt[4];

static void retarget_setup(uint32_t ro, uint32_t skin, uint32_t skel, const int16_t *owner)
{
    Retarget *r = NULL;
    uint32_t hp = rel32(skel, 0x28), pp = rel32(skel, 0x20), pf = rel32(skin, 0x20);
    int i, e, n = (int)MEM32(skel + 0x1C);
    for (i = 0; i < 4 && !r; i++)
        if (s_rt[i].ro == ro)
            r = &s_rt[i];
    for (i = 0; i < 4 && !r; i++)
        if (!s_rt[i].ro)
            r = &s_rt[i];
    if (!r)
        r = &s_rt[0];
    if (!hp || !pp || !pf || n <= 0 || n > RT_MAX_BONES)
        return;
    memset(r, 0, sizeof *r);
    r->ro = ro;
    r->skel = skel;
    r->n = n;
    for (e = 0; e < n; e++) {
        r->parent[e] = (int16_t)MEM16(hp + (uint32_t)e * 8);
        memcpy(r->pp[e], (const void *)XBOX_PTR(pp + (uint32_t)e * 16), 12);
    }
    for (e = 0; e < n; e++) {
        int par = r->parent[e];
        if (owner[e] >= 0)
            memcpy(r->pr[e], (const void *)XBOX_PTR(pf + (uint32_t)owner[e] * 16), 12);
        else if (par >= 0 && par < e) {
            for (i = 0; i < 3; i++)                       /* no counterpart: moves with its parent */
                r->pr[e][i] = r->pp[e][i] + (r->pr[par][i] - r->pp[par][i]);
        } else
            memcpy(r->pr[e], r->pp[e], 12);
    }
    r->piv_guest = xbox_HeapAlloc((uint32_t)n * 16, 16);
    if (r->piv_guest)
        for (e = 0; e < n; e++) {
            memcpy((void *)XBOX_PTR(r->piv_guest + (uint32_t)e * 16), r->pr[e], 12);
            MEM32(r->piv_guest + (uint32_t)e * 16 + 12) = MEM32(pp + (uint32_t)e * 16 + 12);   /* (w as the game has it: 0) */
        }
    if (getenv("BUFFY_MODS_LOG"))
        fprintf(stderr, "[MODS] retarget: render %08X, %d joints, hips %.3f -> %.3f\n", ro, n, r->pp[0][1], r->pr[0][1]);
}

static void m_mul(const float *a, const float *b, float *o)   /* o = a * b (4x4, row-major) */
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            o[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
}

static int m_inv_affine(const float *m, float *o)             /* rows 0-2 a 3x3, row 3 the translation */
{
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g), id;
    if (det > -1e-12f && det < 1e-12f)
        return 0;
    id = 1.0f / det;
    o[0] = (e * i - f * h) * id; o[1] = (c * h - b * i) * id; o[2] = (b * f - c * e) * id; o[3] = 0;
    o[4] = (f * g - d * i) * id; o[5] = (a * i - c * g) * id; o[6] = (c * d - a * f) * id; o[7] = 0;
    o[8] = (d * h - e * g) * id; o[9] = (b * g - a * h) * id; o[10] = (a * e - b * d) * id; o[11] = 0;
    o[12] = -(m[12] * o[0] + m[13] * o[4] + m[14] * o[8]);
    o[13] = -(m[12] * o[1] + m[13] * o[5] + m[14] * o[9]);
    o[14] = -(m[12] * o[2] + m[13] * o[6] + m[14] * o[10]);
    o[15] = 1;
    return 1;
}

/* The retargeted pose is put in place when the object's skin is applied
 * (ApplySkin, inside its DoRender / DoShadowRender) and stays until that
 * render is done: the vertex shader skinning (EXGeoEntity::CalculateVSSkin)
 * reads the matrices and pivots after ApplySkin returns. */
static float    s_rt_saved[RT_MAX_BONES][16];
static uint32_t s_rt_active, s_rt_pivold;
static Retarget *s_rt_cur;

static void retarget_end(void)
{
    int e;
    uint32_t mats;
    if (!s_rt_active || !s_rt_cur)
        return;
    mats = MEM32(s_rt_active + 0x40);
    MEM32(s_rt_cur->skel + 0x20) = s_rt_pivold;
    if (mats)
        for (e = 0; e < s_rt_cur->n; e++)
            memcpy((void *)XBOX_PTR(mats + (uint32_t)e * 64), s_rt_saved[e], 64);
    s_rt_active = 0;
    s_rt_cur = NULL;
}

static void retarget_begin(uint32_t ro)
{
    static float wp[RT_MAX_BONES][16], wr[RT_MAX_BONES][16];
    uint32_t mats, pivfield;
    Retarget *r = NULL;
    int i, e;
    for (i = 0; i < 4 && !r; i++)
        if (s_rt[i].ro && s_rt[i].ro == ro)
            r = &s_rt[i];
    if (!r || !r->piv_guest || s_rt_active || getenv("BUFFY_NO_RETARGET")
            || skin_data(MEM32(ro + 0x30), MEM32(ro + 0x34)) != r->skel || !(mats = MEM32(ro + 0x40)))
        return;
    for (e = 0; e < r->n; e++) {
        memcpy(wp[e], (const void *)XBOX_PTR(mats + (uint32_t)e * 64), 64);
        memcpy(s_rt_saved[e], wp[e], 64);
    }
    for (e = 0; e < r->n; e++) {
        int par = r->parent[e];
        if (par < 0 || par >= e) {
            /* the root: its rest offset moved (taller: the hips higher) */
            float d[3];
            for (i = 0; i < 3; i++)
                d[i] = r->pr[e][i] - r->pp[e][i];
            memcpy(wr[e], wp[e], 64);
            for (i = 0; i < 3; i++)
                wr[e][12 + i] += d[0] * wp[e][i] + d[1] * wp[e][4 + i] + d[2] * wp[e][8 + i];
        } else {
            float inv[16], l[16];
            if (!m_inv_affine(wp[par], inv)) {
                memcpy(wr[e], wp[e], 64);
                continue;
            }
            m_mul(wp[e], inv, l);                        /* the joint's local transform */
            for (i = 0; i < 3; i++)                      /* the foreign bone's offset in place of the skeleton's */
                l[12 + i] += (r->pr[e][i] - r->pr[par][i]) - (r->pp[e][i] - r->pp[par][i]);
            m_mul(l, wr[par], wr[e]);
        }
    }
    if (getenv("BUFFY_RT_LOG")) {
        static int logs;
        if (logs++ == 200) {
            int bones[] = { 0, 1, 5, 10, 12 }, b, q;
            for (b = 0; b < 5; b++) {
                e = bones[b];
                fprintf(stderr, "[RT] joint %d parent %d pp %.3f %.3f %.3f pr %.3f %.3f %.3f\n[RT]   W :", e, r->parent[e], r->pp[e][0], r->pp[e][1],
                        r->pp[e][2], r->pr[e][0], r->pr[e][1], r->pr[e][2]);
                for (q = 0; q < 16; q++)
                    fprintf(stderr, " %.3f", wp[e][q]);
                fprintf(stderr, "\n[RT]   W':");
                for (q = 0; q < 16; q++)
                    fprintf(stderr, " %.3f", wr[e][q]);
                fprintf(stderr, "\n");
            }
        }
    }
    for (e = 0; e < r->n; e++)
        memcpy((void *)XBOX_PTR(mats + (uint32_t)e * 64), wr[e], 64);
    pivfield = r->skel + 0x20;
    s_rt_pivold = MEM32(pivfield);
    MEM32(pivfield) = r->piv_guest - pivfield;           /* (a self-relative offset) */
    s_rt_active = ro;
    s_rt_cur = r;
}

void EXItemRender_SkinAnim_ApplySkin_000F4510_orig(void);
void EXItemRender_SkinAnim_ApplySkin_000F4510(void)
{
    uint32_t ro = g_ecx;
    int who;
    /* a model's rig table due: the renderer has just taken the skin (AssignSkin
     * runs first in the render) -- set before this frame's skinning */
    for (who = 0; who < 2; who++) {
        uint32_t item = MEM32(0x26DC54 + (uint32_t)who * 4), r = item ? MEM32(item + 0x144) : 0;
        if (s_remap_due[who] && r && MEM32(r - 4 + 0xE0) == ro
                && MEM32(ro + 0x60 + MEM32(ro + 0x5C) * 8) == MEM32(ro + 0x4C)) {
            uint32_t regs[4] = { g_eax, g_ebx, g_esi, g_edi };
            s_rigid_hands = s_worn_rigid[who];
            apply_rig_remap(item);
            s_remap_due[who] = 0;
            g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
        }
    }
    retarget_begin(ro);
    g_ecx = ro;
    EXItemRender_SkinAnim_ApplySkin_000F4510_orig();
}

void EXItemRender_SkinAnim_DoRender_000D0400_orig(void);
void EXItemRender_SkinAnim_DoRender_000D0400(void)
{
    uint32_t eax;
    EXItemRender_SkinAnim_DoRender_000D0400_orig();
    eax = g_eax;
    retarget_end();
    g_eax = eax;
}

/* A skinned piece's shader setup. Its morphs (a vampire's game face, the
 * cutscene faces) are looked up by the piece's morph index (its skin record
 * [0x26EB80] +0x10, -1 none) in the SKELETON's skin -- another model's skin
 * on a player's skeleton indexes past the player's morphs (a crash). So such
 * a piece draws without its morphs. */
void EXGeoEntity_CalculateVSSkin_000F2580_orig(void);
void EXGeoEntity_CalculateVSSkin_000F2580(void)
{
    uint32_t ro = MEM32(0x26EB78), ref = MEM32(0x26EB80), keep = 0;
    int foreign = ro && ref && MEM32(ro + 0x4C) && MEM32(ro + 0x30) && MEM32(ro + 0x4C) != MEM32(ro + 0x30)
                  && (int32_t)MEM32(ref + 0x10) >= 0;
    if (foreign) {
        keep = MEM32(ref + 0x10);
        MEM32(ref + 0x10) = 0xFFFFFFFFu;
    }
    EXGeoEntity_CalculateVSSkin_000F2580_orig();
    if (foreign)
        MEM32(ref + 0x10) = keep;
}

void EXItemRender_SkinAnim_DoShadowRender_000D0380_orig(void);
void EXItemRender_SkinAnim_DoShadowRender_000D0380(void)
{
    uint32_t eax;
    EXItemRender_SkinAnim_DoShadowRender_000D0380_orig();
    eax = g_eax;
    retarget_end();
    g_eax = eax;
}

/* The foreign skin on item's render object gets that table (once the
 * renderer has taken the skin). Returns 1 when set. */
static int apply_rig_remap(uint32_t item)
{
    uint32_t r = item ? MEM32(item + 0x144) : 0, ro = r ? MEM32(r - 4 + 0xE0) : 0, slot, skin, skel, tab, regs[4];
    uint8_t map[256];
    int n, k;
    if (!ro)
        return 0;
    slot = MEM32(ro + 0x5C);
    skin = skin_data(MEM32(ro + 0x60 + slot * 8), MEM32(ro + 0x64 + slot * 8));
    skel = skin_data(MEM32(ro + 0x30), MEM32(ro + 0x34));
    if (!skin || !skel || skin == skel) {
        if (getenv("BUFFY_MODS_LOG"))
            fprintf(stderr, "[MODS] rig remap: skin %08X skeleton %08X\n", skin, skel);
        return 0;
    }
    {
        int16_t owner[256];
        for (k = 0; k < 256; k++)
            owner[k] = -1;
        n = build_rig_remap(skin, skel, map, 256, owner);
        if (n > 0)
            retarget_setup(ro, skin, skel, owner);
    }
    if (n <= 0)
        return 0;
    /* the renderer's own table when it made one (its size: the skin's bones,
     * matched by bone id alone), else a new one */
    tab = MEM32(ro + 0x80 + slot * 4);
    if (!tab) {
        regs[0] = g_eax; regs[1] = g_ebx; regs[2] = g_esi; regs[3] = g_edi;
        tab = call_cdecl2(EXAlloc_000BD070, (uint32_t)((n + 3) & ~3), 0);
        g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3];
        if (!tab)
            return 0;
    }
    for (k = 0; k < n; k++)
        MEM8(tab + (uint32_t)k) = map[k];
    MEM32(ro + 0x80 + slot * 4) = tab;
    if (getenv("BUFFY_MODS_LOG")) {
        fprintf(stderr, "[MODS] rig remap (%d bones):", n);
        for (k = 0; k < n; k++)
            fprintf(stderr, " %d>%d", k, map[k]);
        fprintf(stderr, "\n");
    }
    return 1;
}

/* (debugging) BUFFY_MESH_DUMP=secs:hash:skin -- a model's skin: its skinned
 * and rigid mesh pieces (entities), their headers and first vertices */
static void mesh_dump(uint32_t geo, uint32_t skinid)
{
    uint32_t hdr = geo ? MEM32(geo + 0x28) : 0, skin = skin_data(geo, skinid), ents, k, n, list;
    int i;
    if (!hdr || !skin) {
        fprintf(stderr, "[MESH] no skin\n");
        return;
    }
    ents = rel32(hdr + 0x64, 4);
    {
        uint32_t arr = MEM32(hdr + 0x48), blk = arr ? MEM32(arr + 4) : 0, t, q;
        fprintf(stderr, "[MESH] textures %u, array %08X blocks %08X\n", MEM32(hdr + 0x44), arr, blk);
        for (t = 0; blk && t < 8; t++) {
            uint32_t te = MEM32(blk) + t * 0x4C;
            fprintf(stderr, "[MESH]   tex %u:", t);
            for (q = 0; q < 0x4C; q += 4)
                fprintf(stderr, " %08X", MEM32(te + q));
            fprintf(stderr, "\n");
        }
    }
    fprintf(stderr, "[MESH] header %08X entities %d at %08X; skin %08X: +38 %u +40 %u +48 %u\n", hdr,
            (int)(int16_t)MEM16(hdr + 0x64), ents, skin, MEM32(skin + 0x38), MEM32(skin + 0x40), MEM32(skin + 0x48));
    fprintf(stderr, "[MESH] skin data:");
    for (k = 0; k < 0x80; k += 4)
        fprintf(stderr, " %08X", MEM32(skin + k));
    fprintf(stderr, "\n");
    list = rel32(skin, 0x3C);
    n = MEM32(skin + 0x38) + MEM32(skin + 0x40);
    for (k = 0; k < n && k < 40 && list; k++) {
        uint32_t ref = list + k * 0x14, idx = MEM32(ref + 0xC) & 0xFFFFFF, e = ents ? MEM32(ents + idx * 0x20 + 0xC) : 0, vd, j;
        fprintf(stderr, "[MESH] skinned %u: ref", k);
        for (j = 0; j < 0x14; j += 4)
            fprintf(stderr, " %08X", MEM32(ref + j));
        fprintf(stderr, " entity %u obj %08X\n", idx, e);
        if (!e)
            continue;
        if (MEM32(e) == 0x1B3FE8u) {                       /* EXGeoSplitEntity: its pieces */
            uint32_t c, cn = MEM32(e + 0x44);
            for (c = 0; c < cn && c < 4; c++) {
                uint32_t ce = rel32(e + 0x48 + c * 4, 0), cvd;
                fprintf(stderr, "[MESH]   piece %u (%08X, vt %08X):", c, ce, ce ? MEM32(ce) : 0);
                for (j = 0; ce && j < 0xA0; j += 4)
                    fprintf(stderr, " %08X", MEM32(ce + j));
                fprintf(stderr, "\n");
                cvd = ce ? rel32(ce, 0x4C) : 0;
                for (i = 0; i < 4 && cvd; i++) {
                    float f[6];
                    memcpy(f, (const void *)XBOX_PTR(cvd + (uint32_t)i * 24), 24);
                    fprintf(stderr, "[MESH]     v%d: %08X %08X %08X %08X %08X %08X | %.3f %.3f %.3f | %.3f %.3f %.3f\n", i,
                            MEM32(cvd + i * 24), MEM32(cvd + i * 24 + 4), MEM32(cvd + i * 24 + 8), MEM32(cvd + i * 24 + 12),
                            MEM32(cvd + i * 24 + 16), MEM32(cvd + i * 24 + 20), f[0], f[1], f[2], f[3], f[4], f[5]);
                }
            }
            continue;
        }
        fprintf(stderr, "[MESH]   entity:");
        for (j = 0; j < 0xA0; j += 4)
            fprintf(stderr, " %08X", MEM32(e + j));
        fprintf(stderr, "\n");
        vd = rel32(e, 0x4C);
        for (i = 0; i < 3 && vd; i++) {
            float f[6];
            memcpy(f, (const void *)XBOX_PTR(vd + (uint32_t)i * 24), 24);
            fprintf(stderr, "[MESH]   v%d: %08X %08X %08X %08X %08X %08X | %.3f %.3f %.3f\n", i, MEM32(vd + i * 24),
                    MEM32(vd + i * 24 + 4), MEM32(vd + i * 24 + 8), MEM32(vd + i * 24 + 12), MEM32(vd + i * 24 + 16),
                    MEM32(vd + i * 24 + 20), f[0], f[1], f[2]);
        }
    }
    list = rel32(skin, 0x4C);
    for (k = 0; k < MEM32(skin + 0x48) && k < 20 && list; k++) {
        uint32_t ref = list + k * 0x14;
        fprintf(stderr, "[MESH] rigid %u: %08X %08X %08X %08X %08X\n", k, MEM32(ref), MEM32(ref + 4), MEM32(ref + 8),
                MEM32(ref + 12), MEM32(ref + 16));
    }
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
    /* player 2 in the old character's model file (Buffy, Willow, Faith and
     * Tara share one): it stays loaded -- unloading it took player 2's
     * character out from under them */
    int other_in_file = p2 && model_of_row((int)MEM32(p2 + 0x16C)) == model_of_row((int)old);
    uint32_t inv_old;
    if (!h || old == (uint32_t)row || row < 0 || row > 23)
        return;
    inv_old = player_inventory(p1);
    want = row <= 5 ? MEM32(STORY_MODELS + (uint32_t)row * 4) : sheet_model(row);
    if (!want)
        return;
    k = (uint32_t)row;
    if (!plain) {
        k = row <= 5 && (uint32_t)row != cur ? (uint32_t)row : (cur == 1 ? 2u : 1u);
        lent = MEM32(STORY_MODELS + k * 4);
        MEM32(STORY_MODELS + k * 4) = want;
    }
    if (!other_has_old)
        stop_char_sounds(extra_sound_row(0, (int)old));   /* (player 2 is someone else) */
    esp0 = g_esp;
    g_esp -= 4; MEM32(g_esp) = (uint32_t)(plain && !other_in_file);   /* unload: only the plain swap */
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
        if (!other_in_file) {
            uint32_t oldm = old <= 5 ? MEM32(STORY_MODELS + old * 4) : sheet_model((int)old);
            if (oldm && oldm != want)
                call_cdecl2(EXGeoFile_DeLoadGeoFile_000C6B50, oldm, 0);
        }
    }
    inv_carry(inv_old, player_inventory(p1));
    load_char_sounds(row);
    set_char_voice(p1, row);
    if (getenv("BUFFY_MODS_LOG")) {
        int r;
        sheet_model(0);
        fprintf(stderr, "[MODS] sheet +4/+5 by row:");
        for (r = 0; r < 24; r++)
            fprintf(stderr, " %d:%02X/%02X", r, s_sheet_b4[r], s_sheet_b5[r]);
        fprintf(stderr, "\n[MODS] player 1 handler +71A %02X +ABE8 %02X +3D0 %02X\n", MEM8(h + 0x71A), MEM8(h + 0xABE8), MEM8(h + 0x3D0));
    }
    if (getenv("BUFFY_MODS_LOG")) {
        uint32_t inv = player_inventory(p1);
        int id, n = 0;
        for (id = 1; id < INV_ITEMS; id++)
            if (inv && inv_count(inv, id) > 0) {
                fprintf(stderr, "[MODS]   player 1 now holds item %02X x%d\n", id, inv_count(inv, id));
                n++;
            }
        fprintf(stderr, "[MODS]   (%d items; inventory %08X, was %08X)\n", n, inv, inv_old);
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
static int s_page_fresh;                   /* a PC or co-op page is opening: count its lines from 0 */

int buffy_coop_pause_button(uint32_t wnd, uint32_t tmpl, const wchar_t **text)
{
    static uint32_t page;
    static int n;
    if (s_page_fresh) {
        /* (reopened quickly, the new popup can have the old one's address:
         * by the address alone its lines were counted on from the last
         * time, all "extra", all hidden -- a black page) */
        s_page_fresh = 0;
        page = 0;
    }
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
    if (s_pc_page && MEM32(wnd + 0x174) == MAIN_MENU_PAGE)
        ;                                                 /* the front end's main menu page, as the PC settings page */
    else if (MEM32(wnd + 0x174) != OPTIONS_PAGE || (!s_p2_pause && !s_coop_page && !s_pc_page))
        return 0;
    if (s_pc_page) {
        s_pc_wnd = wnd;
        if (!s_pc_built)
            s_pc_built = 1;
    }
    if (page != wnd) {
        page = wnd;
        n = 0;
        s_hidden_n = 0;
    }
    if ((flags & 0x00100000u) && (flags & 0x4u)) {
        /* a selectable text line: the first three are player 2's */
        static const uint32_t types[3] = { TYPE_CONTINUE, TYPE_P2_CHANGE, TYPE_P2_DROP };
        static const wchar_t *labels[3] = { L"Continue", L"Change Character", L"Drop Out" };
        if (n >= (s_pc_page ? PC_LINES : s_p2_pause ? 3 : COOP_LINES)) {
            if (MEM32(tmpl + 0x24) && s_hidden_n < 16) {
                s_hidden_wnd = wnd;
                s_hidden[s_hidden_n++] = MEM32(tmpl + 0x24);
            }
            return 1;
        }
        if (s_pc_page) {
            /* the PC settings page (buffy_menu.c's lines) */
            static wchar_t pc_buf[PC_LINES][64];
            buffy_pc_line_text(n, pc_buf[n], 64);
            MEM32(tmpl + 0x64) = TYPE_PC_LINE + n;
            MEM32(tmpl + 0x68) &= ~0x00100003u;                   /* no jump, no popup, no value display */
            MEM32(tmpl + 0x6C) = 0;
            MEM32(tmpl + 0x70) = 0;
            MEM32(tmpl + 0x9C) = 0;
            MEM32(tmpl + 0x5C) = 0;
            MEM32(tmpl + 0x60) = 0;
            MEM32(tmpl + 0x7C) |= PAD_A_GAME | PAD_LEFT_GAME | PAD_RIGHT_GAME;
            *text = pc_buf[n++];
            if (n == 5 && MEM32(wnd + 0x174) == MAIN_MENU_PAGE)
                s_pc_sixth = 1;                               /* the main menu page has five: the sixth is added */
            return 2;
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
    s_page_fresh = 1;
}

/* buffy_menu.c's PC Settings line: the popup it opens is the PC settings
 * page. The page's lines are added while the popup opens (inside the press);
 * after it the flag goes, so the real Options page is itself again. */
int buffy_pc_page_pending(void)
{
    return s_pc_page;
}

void buffy_pc_page_opening(void)
{
    s_pc_page = 1;
    s_pc_built = 0;
    s_page_fresh = 1;
    if (getenv("BUFFY_MENU_LOG"))
        fprintf(stderr, "[MENU] PC settings page opening\n");
}

/* B closes the PC settings page. It borrows another page's script (in the
 * front end the main menu's, where B does nothing): when B is pressed and
 * the page is still up a few frames later, it goes. Once a frame. */
void buffy_pc_page_back(void)
{
    uint16_t buffy_input_buttons(int port);
    static uint16_t prevb;
    static int wait;
    uint16_t b = buffy_input_buttons(0);
    int open = s_pc_wnd && s_pc_parent && MEM32(s_pc_parent + 0x1B8) == s_pc_wnd;
    if (!open)
        wait = 0;
    else if ((b & 0x2000u) && !(prevb & 0x2000u))
        wait = 1;
    else if (wait && ++wait > 4) {
        wait = 0;
        call_this0(XHudScriptWnd_KillNextFrame_00050280, s_pc_wnd);
        if (getenv("BUFFY_MENU_LOG"))
            fprintf(stderr, "[MENU] B: PC settings page closed\n");
    }
    prevb = b;
}

void buffy_pc_page_opened(void)
{
    /* (the popup's lines come a frame later: the flag stays until then) */
    if (getenv("BUFFY_MENU_LOG"))
        fprintf(stderr, "[MENU] PC settings page opened\n");
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
    if (s_pc_wnd && MEM32(wnd + 0x1B8) == s_pc_wnd
            && (MEM32(wnd + 0x174) == OPTIONS_PAGE || MEM32(wnd + 0x174) == OPTIONS_PAGE_FE)) {
        /* the Options page under its PC settings popup (parent +0x1B8 is its
         * popup): not drawn, or its lines and values show through */
        s_pc_parent = wnd;
        g_eax = 1;
        g_esp += 4;                                       /* ret */
        return;
    }
    if (s_pc_parent && MEM32(wnd + 0x1B8) == s_pc_parent && MEM32(s_pc_parent + 0x1B8) == s_pc_wnd) {
        /* ...and the menu the Options page is a popup of (the pause menu):
         * with the Options page not drawn it would show through too */
        g_eax = 1;
        g_esp += 4;                                       /* ret */
        return;
    }
    if (wnd == s_hidden_wnd && (s_p2_pause || s_coop_page || wnd == s_pc_wnd) && s_hidden_n) {
        uint32_t start = s_hidden[0] + 4;
        int dir;
        for (dir = 0; dir < 2; dir++) {
            uint32_t node = dir ? MEM32(start + 4) : start;
            int guard = 0;
            while (node && guard++ < 256 && n < 8) {
                uint32_t an = node - 4;
                if (getenv("BUFFY_MENU_LOG") && wnd == s_pc_wnd) {
                    static int shown;
                    if (shown++ < 60)
                        fprintf(stderr, "[MENU] pc page object %08X model %08X flags %02X\n", an, MEM32(an + 0x24), MEM8(an + 0x10));
                }
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
/* ── two players, one set of files ────────────────────────────────────────
 *
 * The characters share model files (Buffy, Willow, Faith and Tara are all
 * p01_buff) and a model file's sections -- the story characters' skins,
 * Willow's spell moves, the weapons' animation sets -- are loaded into the
 * file, not the player. The game unloads a section when its one player is
 * done with it (a skin changed, a weapon put away), which with two players
 * took it from the other: a character's look or moves gone. While player 2
 * is in a story level no section of a file either player's character is in
 * is unloaded (EXGeoHeader::DeLoadSectionFile, thiscall on the file's +0x28,
 * the section's hash; ret 4). */
void EXGeoHeader_DeLoadSectionFile_000C6270_orig(void);
void EXGeoHeader_DeLoadSectionFile_000C6270(void)
{
    int k;
    if (s_story_coop && MEM32(0x26DC58) && !in_multiplayer()) {
        for (k = 0; k < 2; k++) {
            uint32_t it = MEM32(0x26DC54 + (uint32_t)k * 4), geo = it ? MEM32(it + 0x20) : 0;
            if (geo && MEM32(geo + 0x28) == g_ecx) {
                if (getenv("BUFFY_MODS_LOG"))
                    fprintf(stderr, "[MODS] co-op: section %08X kept (player %d's model file)\n", MEM32(g_esp + 4), k + 1);
                g_esp += 4 + 4;
                g_eax = 1;
                return;
            }
        }
    }
    EXGeoHeader_DeLoadSectionFile_000C6270_orig();
}

/* void XWeapon_Stake::DoUpdate() -- wrapped. The stake's wear is read from
 * the holder's inventory (weapon +0x60, the player index) but for the item
 * player 1 has equipped (slot 0's inventory): player 2's stake was measured
 * by player 1's item -- when player 1 was someone else (Willow), gone, and
 * player 2 (Faith, Buffy) could not stake. For player 2's stake, slot 0 is
 * player 2 for the call. */
void XWeapon_Stake_DoUpdate_000A6590_orig(void);
void XWeapon_Stake_DoUpdate_000A6590(void)
{
    uint32_t w = g_ecx, p1 = MEM32(0x26DC54), p2 = MEM32(0x26DC58);
    if (!s_story_coop || !p2 || MEM8(w + 0x60) != 1 || in_multiplayer()) {
        XWeapon_Stake_DoUpdate_000A6590_orig();
        return;
    }
    MEM32(0x26DC54) = p2;
    XWeapon_Stake_DoUpdate_000A6590_orig();
    MEM32(0x26DC54) = p1;
}

int buffy_coop_hide_line(uint32_t btn)
{
    /* the Character Select in a level: the slots after the story characters
     * are left empty (for character mods to fill) */
    if (s_join_wnd && s_cs_laid && MEM32(btn + 0x1C) == s_join_wnd && MEM32(btn + 0x64) == BTN_PORTRAIT
            && MEM32(btn + 0x5C) < 24 && !(cs_visible_bits() & (1u << MEM32(btn + 0x5C))))
        return 1;
    return 0;
}

void coop_set_screens_test(int v)
{
    fprintf(stderr, "[MODS] co-op: (test) Screens -> %d\n", v);
    coop_set_screens(v);
}

/* For bug reports (buffy_debug.c): the ticked mods and the co-op state. */
void buffy_mods_debug_report(FILE *f)
{
    int i;
    fprintf(f, "mods ticked: %d\n", s_count);
    for (i = 0; i < s_count; i++)
        fwprintf(f, L"  %s\n", s_dirs[i]);
    fprintf(f, "story co-op %s, player 1 change character %s, multiplayer %s\n",
            s_story_coop ? "on" : "off", s_p1_change ? "on" : "off", in_multiplayer() ? "yes" : "no");
    if (s_story_coop)
        fprintf(f, "co-op: screens %d (1 one, 2 two windows, 3 split), player 2 %s (row %d), "
                   "friendly fire %d, shared inventory %d, back brings player 2 %d, respawn %ds\n",
                s_screens, MEM32(0x26DC58) ? "in" : "not in", s_p2_row, s_friendly_fire, s_share_inventory,
                s_teleport_on_back, s_respawn_secs);
    fprintf(f, "paused: %s; join page open: %s\n",
            s_xapp && MEM32(s_xapp + 0x84) ? "yes" : "no", s_join_wnd ? (s_join_who ? "player 2's" : "player 1's") : "no");
}

/* ── co-op: the level around BOTH players ────────────────────────────────
 *
 * The level streams its sections by where the view is: each frame
 * EXSubMapInfoTable::UpdateSubMapLoaded finds the BSP node the view position
 * (0x29EE38) is in and loads that node's needed sections (a 64-bit mask at
 * node +0x2C, nodes 0x58 bytes from map +0x64), unloading the rest -- so
 * where player 2 was, far from player 1's view, the rooms went (and with them
 * the enemies there, and player 2's own surroundings), spawns and triggers
 * there never ran, and walking back in stalled on the loads. Here player 2's
 * node's sections are added to the view's while it decides.
 *
 * The same masks stand for "live" rooms: EXItemEnv::IsSubMapVisible (a room's
 * portal visibility, player 1's view) gates monsters, spawns and the
 * press-a-button triggers (computers, the book cabinet) -- a room in player
 * 2's node's mask counts as live too. */
void EXGeoBspTree_WhichNode_000EB040(void);
void EXSubMapInfoTable_UpdateSubMapLoaded_000EDD80_orig(void);
void EXItemEnv_IsSubMapVisible_000C3160_orig(void);
void XTrigger_CheckRange_00096C00_orig(void);
void XTrigger_Database_Deload_HashCodeFileInUse_00097360_orig(void);
void XApp_StartGameMode_0002E2C0_orig(void);

static uint32_t s_p2_sect[2];              /* player 2's node's sections (this frame's view update) */
static int      s_p2_sect_ok;

/* The BSP node `pos` (guest float4) is in, in `map` (UpdateSubMapLoaded's), or -1. */
static int map_node_at(uint32_t map, uint32_t pos)
{
    uint32_t bsp, esp0 = g_esp, regs[5] = { g_eax, g_ebx, g_esi, g_edi, g_edx }, r;
    if (!map || !MEM32(map + 4))
        return -1;
    bsp = map + 4 + MEM32(map + 4);
    g_esp -= 4; MEM32(g_esp) = pos;
    g_esp -= 4; MEM32(g_esp) = 0;
    g_ecx = bsp;
    EXGeoBspTree_WhichNode_000EB040();
    r = g_eax;
    g_esp = esp0;
    g_eax = regs[0]; g_ebx = regs[1]; g_esi = regs[2]; g_edi = regs[3]; g_edx = regs[4];
    return (int32_t)r < 0 || r > 4096 ? -1 : (int)r;
}

static int coop_world_on(void)
{
    return s_story_coop && MEM32(0x26DC54) && coop_p2_item() && !in_multiplayer() && !getenv("BUFFY_COOP_ONE_VIEW");
}

/* Rooms live in the other window's view: each window's pass recomputes the
 * rooms' visibility for its own camera (the submap table, [env +0x4C] - 4,
 * +0x100: 0x8C a room, +0x63 its visibility), so the game's logic, run before
 * drawing, saw only the last pass's (player 2's) -- the other player's rooms
 * looked dead: no spawns, doors put away. Each view update first keeps the
 * visibility the previous pass (the other window) left. */
#define MAX_ROOMS 256
static uint8_t s_vis_other[MAX_ROOMS];

static void rooms_keep_other_view(void)
{
    uint32_t m = MEM32(0x26DC68 + 0x4C), obj, tab, i;
    if (!m || !(obj = m - 4) || !(tab = MEM32(obj + 0x100)))
        return;
    for (i = 0; i < MAX_ROOMS; i++)
        s_vis_other[i] = MEM8(tab + i * 0x8C + 0x63);
    if (getenv("BUFFY_COOP_WORLD_LOG")) {
        /* (the rooms each view sees, when they change: two views alternate) */
        static char last[2][96];
        static int which;
        char now[96];
        int n = 0;
        for (i = 0; i < 64 && n < 90; i++)
            if ((int8_t)s_vis_other[i] > 0)
                n += sprintf_s(now + n, sizeof now - (size_t)n, "%u ", i);
        now[n] = 0;
        which ^= 1;
        if (strcmp(now, last[which])) {
            strcpy_s(last[which], sizeof last[which], now);
            fprintf(stderr, "[COOP] world: view %d sees rooms %s\n", which, now);
        }
    }
}

void EXSubMapInfoTable_UpdateSubMapLoaded_000EDD80(void)
{
    uint32_t map = MEM32(g_esp + 8), self = g_ecx, e1 = 0, keep[2] = { 0, 0 };
    int n1;
    s_p2_sect_ok = 0;
    if (coop_world_on() && map && (n1 = map_node_at(map, 0x29EE38)) >= 0) {
        /* the sections this view needs, and both players' */
        static uint32_t pos;
        uint32_t add[2] = { 0, 0 }, pl[2], k;
        pl[0] = MEM32(0x26DC54);
        pl[1] = coop_p2_item();
        if (pl[0] == pl[1])
            pl[0] = s_asp0_p2 ? MEM32(0x26DC58) : pl[0];
        if (!pos)
            pos = xbox_HeapAlloc(16, 16);
        rooms_keep_other_view();
        for (k = 0; pos && k < 2; k++) {
            int n;
            if (!pl[k])
                continue;
            MEM32(pos) = MEM32(pl[k] + 0xAC);
            MEM32(pos + 4) = MEM32(pl[k] + 0xB0);
            MEM32(pos + 8) = MEM32(pl[k] + 0xB4);
            MEM32(pos + 12) = 0;
            if ((n = map_node_at(map, pos)) >= 0) {
                uint32_t e = map + 0x64 + (uint32_t)n * 0x58;
                add[0] |= MEM32(e + 0x2C);
                add[1] |= MEM32(e + 0x30);
            }
        }
        e1 = map + 0x64 + (uint32_t)n1 * 0x58;
        keep[0] = MEM32(e1 + 0x2C);
        keep[1] = MEM32(e1 + 0x30);
        MEM32(e1 + 0x2C) = keep[0] | add[0];
        MEM32(e1 + 0x30) = keep[1] | add[1];
        if (getenv("BUFFY_COOP_WORLD_LOG") && ((keep[0] | add[0]) != keep[0] || (keep[1] | add[1]) != keep[1])) {
            static uint32_t last[2];
            if (last[0] != add[0] || last[1] != add[1])
                fprintf(stderr, "[COOP] world: view node %d needs %08X %08X, the players' add %08X %08X\n", n1, keep[0], keep[1],
                        add[0], add[1]);
            last[0] = add[0];
            last[1] = add[1];
        }
    }
    g_ecx = self;
    EXSubMapInfoTable_UpdateSubMapLoaded_000EDD80_orig();
    if (e1) {
        MEM32(e1 + 0x2C) = keep[0];
        MEM32(e1 + 0x30) = keep[1];
    }
}

void EXItemEnv_IsSubMapVisible_000C3160(void)
{
    uint32_t idx = MEM32(g_esp + 4);
    EXItemEnv_IsSubMapVisible_000C3160_orig();
    if (!(g_eax & 0xFF) && idx < MAX_ROOMS && (int8_t)s_vis_other[idx] > 0 && coop_world_on())
        g_eax = (g_eax & ~0xFFu) | 1;                    /* seen in the other window */
}

/* A portal (a door's opening, a room's way into the next) is visible when a
 * room on either side is -- in either window's view. Suspend triggers put
 * doors and the like away when it is not. */
void EXItemEnv_IsPortalVisible_000C31C0_orig(void);
void EXItemEnv_IsPortalVisible_000C31C0(void)
{
    uint32_t pt = MEM32(g_esp + 4), a = pt ? MEM16(pt) : 0xFFFF, b = pt ? MEM16(pt + 2) : 0xFFFF;
    EXItemEnv_IsPortalVisible_000C31C0_orig();
    if (!(g_eax & 0xFF) && coop_world_on()
            && ((a < MAX_ROOMS && (int8_t)s_vis_other[a] > 0) || (b < MAX_ROOMS && (int8_t)s_vis_other[b] > 0)))
        g_eax = (g_eax & ~0xFFu) | 1;
}

/* A monster spawn waits for the player to come near (distance from slot 0's
 * player); player 2 coming near sets it off too. */
void XTrigger_MonsterSpawn_Active_DoUpdate_00097C30_orig(void);
void XTrigger_MonsterSpawn_Active_DoUpdate_00097C30(void)
{
    uint32_t trig = g_ecx, ret = MEM32(g_esp), esp0 = g_esp, p1 = MEM32(0x26DC54), p2 = coop_p2_item(), eax;
    XTrigger_MonsterSpawn_Active_DoUpdate_00097C30_orig();
    if (!coop_world_on() || !p2 || p2 == p1 || !(MEM32(trig + 0x48) & 0x400))
        return;                                          /* (already set off) */
    eax = g_eax;
    g_esp = esp0;
    MEM32(g_esp) = ret;
    g_ecx = trig;
    MEM32(0x26DC54) = p2;
    XTrigger_MonsterSpawn_Active_DoUpdate_00097C30_orig();
    MEM32(0x26DC54) = p1;
    if (!(MEM32(trig + 0x48) & 0x400) && getenv("BUFFY_COOP_WORLD_LOG"))
        fprintf(stderr, "[COOP] world: player 2 set off a monster spawn (%08X)\n", trig);
    g_eax = eax;
}

/* XTrigger::CheckRange: is player 1 in the trigger's range -- or player 2. */
void XTrigger_CheckRange_00096C00(void)
{
    uint32_t ret = MEM32(g_esp), p1 = MEM32(0x26DC54), p2 = coop_p2_item(), esp0 = g_esp;
    XTrigger_CheckRange_00096C00_orig();
    if ((g_eax & 0xFF) || !coop_world_on() || !p2 || p2 == p1)
        return;
    {
        uint32_t eax;
        g_esp = esp0;                                    /* (a plain ret: the arguments are still there) */
        MEM32(g_esp) = ret;
        MEM32(0x26DC54) = p2;
        XTrigger_CheckRange_00096C00_orig();
        eax = g_eax;
        MEM32(0x26DC54) = p1;
        g_eax = eax;
    }
}

/* Deload triggers keep a model file the players use: the game only asks about
 * player 1's character; player 2's, and a character mod's model, count too. */
void XTrigger_Database_Deload_HashCodeFileInUse_00097360(void)
{
    uint32_t hash = MEM32(g_esp + 4), p2 = coop_p2_item();
    int who, used = 0;
    if (hash != 0x1000000 && !in_multiplayer()) {
        if (p2 && MEM32(p2 + 0x16C) <= 23 && model_of_row((int)MEM32(p2 + 0x16C)) == hash)
            used = 1;
        for (who = 0; who < 2; who++)
            if (s_worn[who] && s_worn_model[who] == hash)
                used = 1;
    }
    if (used) {
        if (getenv("BUFFY_MODS_LOG"))
            fprintf(stderr, "[MODS] co-op: model file %08X kept (a player uses it)\n", hash);
        g_eax = (g_eax & ~0xFFu) | 1;
        g_esp += 4;                                      /* ret (cdecl) */
        return;
    }
    XTrigger_Database_Deload_HashCodeFileInUse_00097360_orig();
}

/* BUFFY_TEST_LEVEL=hash (testing): the first level started is that one
 * (0x01000024 the cemetery, 0x0100003A the Magic Box). */
void XApp_StartGameMode_0002E2C0(void)
{
    static int done;
    const char *lv = getenv("BUFFY_TEST_LEVEL");
    if (lv)
        fprintf(stderr, "[MODS] (test) StartGameMode %08X %08X %08X %08X\n", MEM32(g_esp + 4), MEM32(g_esp + 8),
                MEM32(g_esp + 12), MEM32(g_esp + 16));
    if (lv && !done && MEM32(g_esp + 8) != MEM32(0x1B7F68) && MEM32(g_esp + 8) >> 24 == 1) {
        uint32_t want = (uint32_t)strtoul(lv, NULL, 16);
        done = 1;
        fprintf(stderr, "[MODS] (test) level %08X instead of %08X\n", want, MEM32(g_esp + 8));
        MEM32(g_esp + 8) = want;
    }
    XApp_StartGameMode_0002E2C0_orig();
}
