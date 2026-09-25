# Contributors

## KaikoClanworth1: project lead

- Started and directs the port.
- Designs the features: the launcher, the mods, and story co-op.
- Tests every build and plays it on their own PC.

## Claude Code (Anthropic): AI coding assistant

This port was made with [Claude Code](https://claude.com/claude-code). Claude Code wrote most of this repository's code and docs, working under the project lead's direction:

- **Recompiler setup**: regenerating the game's code with xboxrecomp and fixing the translation.
- **Graphics**: the Direct3D 11 renderer, the NV2A emulation fixes, and the resolution and widescreen options.
- **Sound and input**: audio through DirectSound, controllers through XInput, and the keyboard.
- **Movies**: playing the game's movies.
- **Launcher**: installing the game from the player's own disc image.
- **Mods**: the mod overlay system and the bundled mods.
- **Story co-op**: second player, two windows or split screen, the co-op menus.
- **Tools and docs**: the build and deploy scripts, and the README and diagrams.

Commits made with Claude Code end with a `Co-Authored-By: Claude` line.

## sp00nznet: xboxrecomp

[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) is the static recompiler and Xbox runtime this port is built on. It's MIT licensed and vendored in `xboxrecomp/` with changes for this port. See `xboxrecomp/CONTRIBUTORS.md` for its own contributors.
