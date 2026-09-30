/**
 * The saves' format on the PC (buffy_savefmt.c), shared by the game and the
 * launcher: each save is one plain file, SaveData\<name>.sav beside the game,
 * holding the game's own save data as it wrote it.
 */
#pragma once

#include <windows.h>

/* The file for a save name: "BUFFY A" -> "BUFFY A.sav" (characters Windows
 * does not allow in a name become '_'). */
void savefmt_file_name(const WCHAR *name, WCHAR *out, int n);

/* Converts Xbox-format saves (UDATA\56550005\<id>\ with SaveMeta.xbx) in
 * title_dir into save_dir. A save already there is kept unless overwrite.
 * The number converted, or -1 if one could not be written. */
int savefmt_convert_xbox(const WCHAR *title_dir, const WCHAR *save_dir, int overwrite);

/* The update from the Xbox layout, run by the game and the launcher (both
 * safe to run again; nothing is deleted):
 *   - SaveData\ held the emulated console's files (partition images, caches):
 *     they move to XboxData\;
 *   - the saves in the game folder's UDATA\ are converted into SaveData\;
 *   - with move_originals, the Xbox-format folders (UDATA, TDATA) then move to
 *     SaveBackups\<date> (Xbox saves, before conversion)\.
 * exe_dir holds SaveData and XboxData; game_dir the old UDATA. The number of
 * saves converted, -1 on an error (the originals then stay where they are). */
int savefmt_migrate(const WCHAR *game_dir, const WCHAR *exe_dir, int move_originals);
