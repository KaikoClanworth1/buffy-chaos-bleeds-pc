// Buffy the Vampire Slayer: Chaos Bleeds - the Mods tab, as the PC
// launcher's: each mod is a folder in the game folder's mods (mod.ini, and
// files\ laid out like the game folder). The ticked ones, in order (the lower
// wins when two have the same file), go to mods\.launcher\enabled.txt, which
// the game reads at start-up (src/buffy_mods.c). A mod zip can be added with
// the system's file picker.

package io.github.kaikoclanworth1.buffy;

import android.content.Intent;
import android.net.Uri;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class ModsPage {
    private static final class Mod {
        String folder, name, author, version, desc;
        boolean on;
    }

    private final LauncherActivity a_;
    private final LinearLayout view_, list_;
    private final TextView summary_;
    private final List<Mod> mods_ = new ArrayList<>();

    ModsPage(LauncherActivity a) {
        a_ = a;
        view_ = a.column();
        LinearLayout info = a.card(view_, "Mods");
        summary_ = a.para(info, "");
        list_ = new LinearLayout(a);
        list_.setOrientation(LinearLayout.VERTICAL);
        info.addView(list_);

        LinearLayout add = a.card(view_, "Adding mods");
        a.para(add, "Copy a mod's folder into games/" + InstallActivity.kFolderName + "/mods (each mod: mod.ini and "
            + "files/ laid out like the game folder), or add its zip here. Ticked mods are used from the game's next "
            + "start; the game's own files are never changed.");
        Button addZip = a.button("Add a mod (.zip)…", LauncherActivity.kAccent);
        addZip.setOnClickListener(v -> addZip());
        add.addView(addZip, a.fullWidth(0));
        add.addView(new View(a), new LinearLayout.LayoutParams(1, a.dp(10)));
    }

    View view() { return view_; }

    static File modsDir() { return new File(InstallActivity.gameFolder(), "mods"); }
    static File enabledFile() { return new File(new File(modsDir(), ".launcher"), "enabled.txt"); }

    void refresh() {
        mods_.clear();
        List<String> enabled = new ArrayList<>();
        try {
            if (enabledFile().isFile())
                for (String l : Files.readAllLines(enabledFile().toPath(), StandardCharsets.UTF_8)) {
                    l = l.trim();
                    if (!l.isEmpty() && !l.startsWith("#")) enabled.add(l);
                }
        } catch (IOException ignored) {
        }
        File[] dirs = modsDir().listFiles(f -> f.isDirectory() && !f.getName().startsWith("."));
        List<Mod> rest = new ArrayList<>();
        if (dirs != null) {
            for (File d : dirs) {
                IniFile ini = new IniFile(new File(d, "mod.ini"));
                Mod m = new Mod();
                m.folder = d.getName();
                m.name = ini.get("Mod", "Name", d.getName());
                m.author = ini.get("Mod", "Author", "");
                m.version = ini.get("Mod", "Version", "");
                m.desc = ini.get("Mod", "Description", "");
                rest.add(m);
            }
        }
        Collections.sort(rest, (x, y) -> x.name.compareToIgnoreCase(y.name));
        // the ticked ones first, in their order; then the rest by name
        for (String e : enabled)
            for (Mod m : rest)
                if (m.folder.equalsIgnoreCase(e) && !m.on) {
                    m.on = true;
                    mods_.add(m);
                }
        for (Mod m : rest) if (!m.on) mods_.add(m);
        show();
    }

    private void show() {
        list_.removeAllViews();
        int on = 0;
        for (int i = 0; i < mods_.size(); i++) {
            final int index = i;
            Mod m = mods_.get(i);
            if (m.on) on++;
            LinearLayout ctl = new LinearLayout(a_);
            ctl.setGravity(Gravity.CENTER_VERTICAL);
            Button up = small("▲"), down = small("▼");
            up.setOnClickListener(v -> move(index, -1));
            down.setOnClickListener(v -> move(index, 1));
            up.setEnabled(i > 0);
            down.setEnabled(i < mods_.size() - 1);
            Switch s = a_.makeSwitch();
            s.setChecked(m.on);
            s.setOnCheckedChangeListener((b, c) -> {
                m.on = c;
                save();
            });
            ctl.addView(up);
            ctl.addView(down);
            ctl.addView(s);
            String hint = (m.author.isEmpty() ? "" : "by " + m.author) + (m.version.isEmpty() ? "" : "  v" + m.version)
                + (m.desc.isEmpty() ? "" : (m.author.isEmpty() && m.version.isEmpty() ? "" : "\n") + m.desc);
            a_.row(list_, m.name, hint.isEmpty() ? null : hint, ctl);
        }
        summary_.setText(mods_.isEmpty() ? "No mods yet." : mods_.size() + " mod" + (mods_.size() == 1 ? "" : "s")
            + ", " + on + " on. The lower one wins when two change the same file.");
    }

    private Button small(String label) {
        Button b = new Button(a_);
        b.setText(label);
        b.setTextColor(LauncherActivity.kText);
        b.setBackgroundColor(0);
        b.setMinWidth(a_.dp(44));
        b.setMinimumWidth(a_.dp(44));
        return b;
    }

    private void move(int i, int dir) {
        int j = i + dir;
        if (j < 0 || j >= mods_.size()) return;
        Collections.swap(mods_, i, j);
        save();
        show();
    }

    private void save() {
        if (!a_.gameClosed()) {
            refresh();
            return;
        }
        StringBuilder sb = new StringBuilder("# the ticked mods, in order (Buffy launcher)\r\n");
        for (Mod m : mods_) if (m.on) sb.append(m.folder).append("\r\n");
        enabledFile().getParentFile().mkdirs();
        try (OutputStream os = new FileOutputStream(enabledFile())) {
            os.write(sb.toString().getBytes(StandardCharsets.UTF_8));
            a_.status("Mods saved: they are used from the game's next start.");
        } catch (IOException e) {
            a_.status("Could not save the mods list: " + e.getMessage());
        }
        show();
    }

    private void addZip() {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        a_.startForResult(pick, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null) return;
            Uri uri = data.getData();
            final String[] msg = new String[1];
            a_.status("Adding the mod…");
            a_.background(() -> msg[0] = unzip(uri), () -> {
                a_.status(msg[0]);
                refresh();
            });
        });
    }

    // A mod zip into mods\<name>: the zip's own top folder when it has one
    // with mod.ini in it, else a folder named after the zip.
    private String unzip(Uri uri) {
        String name = uri.getLastPathSegment();
        name = name == null ? "mod" : name.substring(name.lastIndexOf('/') + 1).replaceAll("(?i)\\.zip$", "");
        name = name.replaceAll("[\\\\/:*?\"<>|]", "_");
        List<String> entries = new ArrayList<>();
        try (InputStream in = a_.getContentResolver().openInputStream(uri); ZipInputStream z = new ZipInputStream(in)) {
            for (ZipEntry e; (e = z.getNextEntry()) != null;) entries.add(e.getName());
        } catch (Exception e) {
            return "That is not a zip the launcher can read: " + e.getMessage();
        }
        String top = null;
        for (String e : entries) {
            int slash = e.indexOf('/');
            String first = slash > 0 ? e.substring(0, slash) : null;
            if (first == null) {
                top = null;
                break;
            }
            if (top == null) top = first;
            else if (!top.equals(first)) {
                top = null;
                break;
            }
        }
        File dest = new File(modsDir(), top != null ? top : name);
        String strip = top != null ? top + "/" : "";
        try (InputStream in = a_.getContentResolver().openInputStream(uri); ZipInputStream z = new ZipInputStream(in)) {
            String root = dest.getCanonicalPath() + File.separator;
            byte[] buf = new byte[1 << 16];
            for (ZipEntry e; (e = z.getNextEntry()) != null;) {
                String n = e.getName().startsWith(strip) ? e.getName().substring(strip.length()) : e.getName();
                if (n.isEmpty()) continue;
                File out = new File(dest, n);
                if (!out.getCanonicalPath().startsWith(root)) return "The zip has an unsafe path: " + e.getName();
                if (e.isDirectory()) {
                    out.mkdirs();
                    continue;
                }
                out.getParentFile().mkdirs();
                try (OutputStream os = new FileOutputStream(out)) {
                    int k;
                    while ((k = z.read(buf)) > 0) os.write(buf, 0, k);
                }
            }
        } catch (Exception e) {
            return "Adding the mod failed: " + e.getMessage();
        }
        return "Added " + dest.getName() + ": tick it to use it.";
    }
}
