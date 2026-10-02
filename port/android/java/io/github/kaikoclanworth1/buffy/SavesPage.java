// Buffy the Vampire Slayer: Chaos Bleeds - the Saves tab, as the PC
// launcher's: each save is one file in the game folder's SaveData; back them
// up into SaveBackups\<date>, restore a backup (the current saves are backed
// up first), or send a backup elsewhere (Android's share sheet, as a zip).

package io.github.kaikoclanworth1.buffy;

import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

final class SavesPage {
    private final LauncherActivity a_;
    private final LinearLayout view_, list_;
    private final TextView summary_;

    SavesPage(LauncherActivity a) {
        a_ = a;
        view_ = a.column();
        LinearLayout info = a.card(view_, "Saves");
        summary_ = a.para(info, "");
        list_ = new LinearLayout(a);
        list_.setOrientation(LinearLayout.VERTICAL);
        info.addView(list_);

        LinearLayout act = a.card(view_, "Backups");
        a.para(act, "A backup is a copy of every save, in SaveBackups in the game folder (a PC can reach it over USB).");
        Button backup = a.button("Back up saves", LauncherActivity.kAccent);
        backup.setOnClickListener(v -> backup());
        Button restore = a.button("Restore a backup…", LauncherActivity.kBand);
        restore.setOnClickListener(v -> restore());
        act.addView(a.pair(backup, restore), a.fullWidth(4));
        Button share = a.button("Send a backup…", LauncherActivity.kBand);
        share.setOnClickListener(v -> share());
        act.addView(share, a.fullWidth(10));
        act.addView(new View(a), new LinearLayout.LayoutParams(1, a.dp(10)));
    }

    View view() { return view_; }

    static File saves() { return new File(InstallActivity.gameFolder(), "SaveData"); }
    static File backups() { return new File(InstallActivity.gameFolder(), "SaveBackups"); }

    private static File[] saveFiles(File dir) {
        File[] f = dir.listFiles(x -> x.isFile());
        if (f == null) return new File[0];
        Arrays.sort(f, (x, y) -> x.getName().compareToIgnoreCase(y.getName()));
        return f;
    }

    void refresh() {
        File[] f = saveFiles(saves());
        list_.removeAllViews();
        SimpleDateFormat when = new SimpleDateFormat("d MMM yyyy, HH:mm", Locale.getDefault());
        for (File s : f) {
            String name = s.getName().replaceAll("\\.sav$", "");
            a_.row(list_, name, when.format(new Date(s.lastModified())) + " · " + (s.length() / 1024) + " KB", null);
        }
        summary_.setText(f.length == 0 ? "No saves yet." : f.length + " save" + (f.length == 1 ? "" : "s")
            + ", in SaveData in the game folder.");
    }

    private static void copy(File from, File to) throws IOException {
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) {
            byte[] b = new byte[1 << 16];
            int n;
            while ((n = in.read(b)) > 0) out.write(b, 0, n);
        }
    }

    // Every save into SaveBackups\<date><suffix>; the folder, or null.
    private static File backupTo(String suffix) {
        File dst = new File(backups(), new SimpleDateFormat("yyyy-MM-dd HH.mm.ss", Locale.US).format(new Date()) + suffix);
        if (!dst.mkdirs()) return null;
        try {
            for (File s : saveFiles(saves())) copy(s, new File(dst, s.getName()));
            return dst;
        } catch (IOException e) {
            return null;
        }
    }

    private void backup() {
        if (!a_.gameClosed()) return;
        if (saveFiles(saves()).length == 0) {
            a_.status("No saves to back up yet.");
            return;
        }
        File made = backupTo("");
        a_.status(made != null ? "Backed up to SaveBackups/" + made.getName() + "." : "The backup could not be written.");
    }

    private File[] backupFolders() {
        File[] b = backups().listFiles(File::isDirectory);
        if (b == null) return new File[0];
        Arrays.sort(b, (x, y) -> y.getName().compareTo(x.getName()));      // newest first
        return b;
    }

    private void pick(String title, java.util.function.Consumer<File> chosen) {
        File[] b = backupFolders();
        if (b.length == 0) {
            a_.status("There are no backups yet.");
            return;
        }
        String[] names = new String[b.length];
        for (int i = 0; i < b.length; i++) names[i] = b[i].getName() + "  (" + saveFiles(b[i]).length + " saves)";
        new AlertDialog.Builder(a_, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle(title)
            .setItems(names, (d, which) -> chosen.accept(b[which]))
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void restore() {
        if (!a_.gameClosed()) return;
        pick("Restore which backup?", b -> a_.confirm("Restore " + b.getName() + "? Your current saves are backed up "
            + "first.", "Restore", () -> {
                if (saveFiles(saves()).length > 0 && backupTo(" (before restore)") == null) {
                    a_.status("The current saves could not be backed up, so nothing was restored.");
                    return;
                }
                saves().mkdirs();
                try {
                    for (File s : saveFiles(saves())) s.delete();
                    for (File s : saveFiles(b)) copy(s, new File(saves(), s.getName()));
                    a_.status("Restored " + b.getName() + ".");
                } catch (IOException e) {
                    a_.status("Restoring failed: " + e.getMessage());
                }
                refresh();
            }));
    }

    // A backup as a zip, to wherever Android's share sheet sends it.
    private void share() {
        pick("Send which backup?", b -> {
            File zip = new File(a_.getCacheDir(), "Buffy saves " + b.getName() + ".zip");
            try (ZipOutputStream z = new ZipOutputStream(new FileOutputStream(zip))) {
                for (File s : saveFiles(b)) {
                    z.putNextEntry(new ZipEntry(s.getName()));
                    try (InputStream in = new FileInputStream(s)) {
                        byte[] buf = new byte[1 << 16];
                        int n;
                        while ((n = in.read(buf)) > 0) z.write(buf, 0, n);
                    }
                    z.closeEntry();
                }
            } catch (IOException e) {
                a_.status("The zip could not be made: " + e.getMessage());
                return;
            }
            Uri uri = Uri.parse("content://" + a_.getPackageName() + ".files/cache/" + Uri.encode(zip.getName()));
            Intent send = new Intent(Intent.ACTION_SEND);
            send.setType("application/zip");
            send.putExtra(Intent.EXTRA_STREAM, uri);
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            a_.startActivity(Intent.createChooser(send, "Send saves"));
        });
    }
}
