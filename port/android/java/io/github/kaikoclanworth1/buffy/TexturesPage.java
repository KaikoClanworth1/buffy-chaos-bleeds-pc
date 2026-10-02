// Buffy the Vampire Slayer: Chaos Bleeds - the Textures tab, as the PC
// launcher's: texture packs (textures_replacement\load in the game folder)
// and dumping the game's textures to make one ([Textures] in
// buffy_settings.ini, read by the renderer: nv2a_texpack.inc).

package io.github.kaikoclanworth1.buffy;

import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;

final class TexturesPage {
    private final LauncherActivity a_;
    private final LinearLayout view_;
    private final TextView counts_;

    TexturesPage(LauncherActivity a) {
        a_ = a;
        view_ = a.column();
        LinearLayout about = a.card(view_, "Texture packs");
        a.para(about, "Swap the game's textures for your own, like Dolphin and PCSX2 texture packs. Put a pack's PNG or DDS files "
            + "in textures_replacement/load in the game folder (a PC install's pack copies straight over).");
        counts_ = a.para(about, "");

        LinearLayout load = a.card(view_, "Custom textures");
        a.toggle(load, "Load custom textures", "From textures_replacement/load", "Textures", "Load", false);
        a.toggle(load, "Load them all at start-up", "No stutter the first time each one is used; more memory",
            "Textures", "Prefetch", false);

        LinearLayout dump = a.card(view_, "Dumping");
        a.toggle(dump, "Dump textures while playing", "Each one saved once, as a PNG, to textures_replacement/dump",
            "Textures", "Dump", false);
        a.para(dump, "Making a pack: 1. switch Dump on and play the parts you want to change. 2. Edit the PNGs (keep "
            + "their names) and move them to load. 3. Switch Dump off and Load on.");
    }

    View view() { return view_; }

    private static int pngs(File dir) {
        File[] f = dir.listFiles();
        int n = 0;
        if (f == null) return 0;
        for (File x : f) {
            if (x.isDirectory()) n += pngs(x);
            else if (x.getName().toLowerCase().endsWith(".png") || x.getName().toLowerCase().endsWith(".dds")) n++;
        }
        return n;
    }

    void refresh() {
        File root = new File(InstallActivity.gameFolder(), "textures_replacement");
        int load = pngs(new File(root, "load")), dump = pngs(new File(root, "dump"));
        counts_.setText(load + " texture" + (load == 1 ? "" : "s") + " in load, " + dump + " in dump.");
    }
}
