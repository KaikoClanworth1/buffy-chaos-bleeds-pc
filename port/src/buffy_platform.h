/**
 * What the game runs on: Windows, or Wine / Proton on Linux (buffy_platform.c).
 */
#pragma once

int         buffy_is_wine(void);          /* Wine or Proton */
int         buffy_on_steam_deck(void);    /* a Steam Deck (SteamDeck=1) */
const char *buffy_platform_text(void);    /* "Windows 10.0.26200", "Wine 9.0 on Linux ... (Steam Deck)" */
