Buffy the Vampire Slayer: Chaos Bleeds - PC port (work in progress)

Start with "Buffy Launcher.exe":

  Install   Choose your Xbox disc image of the game (ISO or XISO) and a folder;
            the launcher copies the game out of it and sets up the PC version
            there (about 4 GB). The disc image is only read. The movies are
            converted for PC playback with FFmpeg; if it is not on the PC, the
            launcher offers to download it (or the game simply skips movies).
  Play      Starts the game.
  Settings  Fullscreen or windowed, resolution (1080p and other widescreen
            sizes, or the original 4:3), VSync, skipping the intro movies,
            how widescreen is framed (by default the original side-to-side
            view with the top and bottom trimmed, which avoids pop-in and
            clipping at the edges; untick for the wider view), and
            inverting the camera left / right.
  Mods      Mods go in the game folder's mods\ folder (mods\README.txt
            explains the layout); tick them here. They replace game files
            without changing them, so unticking a mod removes it.
            "Story co-op" adds a second player to the story: press Start on
            controller 2 during a level, pick a character, and player 2's
            window opens - drag it to another monitor or TV, or go
            fullscreen (Alt+Enter) to fill both screens. Player 1's pause
            menu has a Co-op page (split screen, friendly fire, ...);
            player 2's Start gives Change Character and Drop Out, and
            their Back button brings them to player 1.
  Textures  Texture packs, as in Dolphin and PCSX2, in the game folder's
            textures_replacement\ folder. "Dump textures" saves each
            texture the game shows, once, as a PNG in dump\; "Load custom
            textures" replaces a texture with the image in load\ that has
            the same 16-character code in its name (any size; subfolders
            are fine). textures_replacement\README.txt explains more.

The game itself is buffy_chaos_bleeds.exe; keep the installed folder together
(the exe reads default.xbe and the Buffy\ data folder beside it). Saves go to
SaveData\.

Controls (keyboard, while the game window is focused):
  Enter = Start      Space = A        Backspace = B      E = X     Q = Y
  Esc   = Back       Arrows = D-pad   WASD = left stick  IJKL = right stick
  Z / C = Black / White               Left Shift / F = left / right trigger
Up to four Xbox-style (XInput) controllers work, in any USB/wireless slots:
the first is player 1 (together with the keyboard), the next player 2, and
so on, for multiplayer. Rumble goes to each player's own pad (set BUFFY_NO_RUMBLE=1 to turn rumble off). LB/RB are the Xbox's
Black/White buttons.

Current state: boots to the title screen and main menu with graphics (Direct3D
11) and sound (Windows DirectSound). Movies play with sound from the
Movies folder; press Start to skip one (BUFFY_SKIP_MOVIES=1 skips all). Xbox pixel shaders (register combiners),
all four texture stages, fog and W-buffer depth are emulated; the title
screen and menus render like the original, including the glowing menu
selector, at a steady 60 fps (set BUFFY_UNCAPPED=1 to remove the cap).
New Game works: the first level (Magic Box) loads and is playable, with
cutscenes, combat and saving, at a steady 60 fps.
PC options (in the game's own menus):
  Options page:  Resolution  - Left/Right picks the size the game is drawn
                               at: 1920x1080 (default), 2560x1440, 3840x2160
                               or 1280x720 are widescreen (16:9); 640x480,
                               1280x960, 1920x1440 and 2560x1920 are the
                               original 4:3. In widescreen, gameplay uses the
                               game's own 16:9 mode (wider view, HUD moved to
                               the edges); menus and movies stay 4:3 with
                               black bars at the sides, as on a widescreen TV.
                 VSync       - A or Left/Right toggles it.
  Main menu:     Exit        - quits the game.
  Alt+Enter or F11 switches between a window and borderless fullscreen.
  These are saved in buffy_settings.ini beside the exe.

If the game closes unexpectedly, buffy_log.txt beside the exe records why. Frame rate is shown in the window title.
Closing the window quits the game.
