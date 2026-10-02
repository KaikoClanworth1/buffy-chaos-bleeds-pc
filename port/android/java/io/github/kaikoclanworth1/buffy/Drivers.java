// Buffy the Vampire Slayer: Chaos Bleeds - custom GPU drivers (the Settings
// tab): driver packages such as Mesa Turnip (a zip with meta.json - name,
// description, author, driverVersion, libraryName - and the driver's .so),
// unpacked into the app's own storage (libadrenotools can't load a driver from
// shared storage). The chosen one is [Android] GpuDriver in buffy_settings.ini;
// GameActivity hands it to the game (port/android/src/android_driver.c loads
// it; it falls back to the phone's own driver when it can't).

package io.github.kaikoclanworth1.buffy;

import android.app.ActivityManager;
import android.app.AlertDialog;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.TextView;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Enumeration;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

final class Drivers {
    static final class Driver {
        File dir, library;
        String name, version, author, description;
    }

    private final LauncherActivity a_;
    private LinearLayout card_;

    Drivers(LauncherActivity a) { a_ = a; }

    static File root(Context c) { return new File(c.getFilesDir(), "drivers"); }

    private String current() { return a_.ini_.get("Android", "GpuDriver", ""); }

    static Driver read(File dir) {
        try {
            JSONObject m = new JSONObject(new String(Files.readAllBytes(new File(dir, "meta.json").toPath()),
                StandardCharsets.UTF_8));
            Driver d = new Driver();
            d.dir = dir;
            d.name = m.optString("name", dir.getName());
            d.version = m.optString("driverVersion", "");
            d.author = m.optString("author", "");
            d.description = m.optString("description", "");
            d.library = new File(dir, m.getString("libraryName"));
            return d.library.isFile() ? d : null;
        } catch (Exception e) {
            return null;
        }
    }

    List<Driver> installed() {
        List<Driver> list = new ArrayList<>();
        File[] dirs = root(a_).listFiles(File::isDirectory);
        if (dirs == null) return list;
        Arrays.sort(dirs, (x, y) -> x.getName().compareToIgnoreCase(y.getName()));
        for (File d : dirs) {
            Driver dr = read(d);
            if (dr != null) list.add(dr);
        }
        return list;
    }

    void build(LinearLayout column) {
        card_ = a_.card(column, "Graphics driver");
        Button add = a_.button("Add a driver (.zip)…", LauncherActivity.kBand);
        add.setOnClickListener(v -> add());
        column.addView(add, a_.fullWidth(10));
        TextView note = a_.text("For Adreno GPUs: driver packages such as Mesa Turnip (a zip with meta.json, as "
            + "other emulators use them). A driver that doesn't work falls back to the phone's own. It applies the next "
            + "time the game starts.", 12, LauncherActivity.kDim);
        note.setPadding(a_.dp(4), a_.dp(8), a_.dp(4), 0);
        column.addView(note);
        refresh();
    }

    void refresh() {
        card_.removeAllViews();
        String cur = current();
        RadioButton phone = radio("The phone's own driver", null, cur.isEmpty() || !new File(cur).isFile());
        phone.setOnClickListener(v -> choose(null));
        card_.addView(phone);
        List<Driver> list = installed();
        for (Driver d : list) {
            String sub = (d.version.isEmpty() ? "" : d.version) + (d.author.isEmpty() ? "" : " · " + d.author);
            RadioButton r = radio(d.name, sub, d.library.getPath().equals(cur));
            r.setOnClickListener(v -> choose(d));
            r.setOnLongClickListener(v -> {
                remove(d);
                return true;
            });
            card_.addView(r);
        }
        if (!list.isEmpty()) {
            TextView hint = a_.text("Hold a driver to remove it.", 12, LauncherActivity.kDim);
            hint.setPadding(0, 0, 0, a_.dp(8));
            card_.addView(hint);
        }
    }

    private RadioButton radio(String title, String sub, boolean checked) {
        RadioButton r = new RadioButton(a_);
        r.setText(sub == null || sub.isEmpty() ? title : title + "\n" + sub);
        r.setTextColor(LauncherActivity.kText);
        r.setTextSize(15);
        r.setChecked(checked);
        r.setButtonTintList(android.content.res.ColorStateList.valueOf(LauncherActivity.kAccent));
        r.setPadding(a_.dp(8), a_.dp(10), 0, a_.dp(10));
        return r;
    }

    // The game stopped with a custom driver chosen, or the driver didn't start:
    // offer the phone's own driver back.
    void checkCrash() {
        String cur = current();
        if (cur.isEmpty() || a_.gameRunning()) return;
        String name = new File(cur).getParentFile().getName();
        for (Driver d : installed())
            if (d.library.getPath().equals(cur)) name = d.name;
        File failed = new File(a_.getCacheDir(), "gpu_driver_failed.txt");
        if (failed.isFile()) {
            String why = "";
            try {
                String[] lines = new String(Files.readAllBytes(failed.toPath()), StandardCharsets.UTF_8).split("\n");
                if (lines.length > 1) why = " (" + lines[1].trim() + ")";
            } catch (IOException ignored) {
            }
            failed.delete();
            ask(name + " didn't work on this phone" + why + ": the game used the phone's own driver. Go back to the "
                + "phone's own driver?", name);
            return;
        }
        SharedPreferences prefs = a_.getSharedPreferences("drivers", Context.MODE_PRIVATE);
        long since = prefs.getLong("chosen_at", 0);
        ActivityManager am = a_.getSystemService(ActivityManager.class);
        ApplicationExitInfo crash = null;
        for (ApplicationExitInfo e : am.getHistoricalProcessExitReasons(null, 0, 16)) {
            if (e.getTimestamp() <= since) break;
            if (e.getProcessName() != null && e.getProcessName().endsWith(":game")
                    && e.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE) {
                crash = e;
                break;
            }
        }
        if (crash == null) return;
        prefs.edit().putLong("chosen_at", crash.getTimestamp()).apply();      // (asked once per crash)
        ask("The game stopped while using the graphics driver " + name + ". Go back to the phone's own driver?", name);
    }

    private void ask(String message, String name) {
        new AlertDialog.Builder(a_, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setMessage(message)
            .setPositiveButton("Use the phone's driver", (dlg, w) -> choose(null))
            .setNegativeButton("Keep " + name, null)
            .show();
    }

    private void choose(Driver d) {
        a_.getSharedPreferences("drivers", Context.MODE_PRIVATE).edit().putLong("chosen_at", System.currentTimeMillis()).apply();
        a_.ini_.set("Android", "GpuDriver", d == null ? "" : d.library.getPath());
        a_.status(a_.ini_.save() ? (d == null ? "The phone's own driver." : d.name + " chosen.")
                                       + " It applies the next time the game starts."
                                 : "Could not save the settings.");
        refresh();
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }

    private void remove(Driver d) {
        a_.confirm("Remove the driver " + d.name + "?", "Remove", () -> {
            if (d.library.getPath().equals(current())) {
                a_.ini_.set("Android", "GpuDriver", "");
                a_.ini_.save();
            }
            deleteTree(d.dir);
            a_.status("Removed " + d.name + ".");
            refresh();
        });
    }

    private void add() {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("application/zip");
        a_.startForResult(pick, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null) return;
            Uri uri = data.getData();
            final String[] result = new String[1];
            a_.background(() -> {
                try {
                    result[0] = "Added " + unpack(uri).name + ". Choose it above to use it.";
                } catch (Exception e) {
                    result[0] = "That driver can't be added: " + e.getMessage();
                }
            }, () -> {
                a_.status(result[0]);
                refresh();
            });
        });
    }

    private String zipName(Uri uri) {
        try (android.database.Cursor c = a_.getContentResolver().query(uri,
                 new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                String n = c.getString(0);
                if (n != null) return n.toLowerCase().endsWith(".zip") ? n.substring(0, n.length() - 4) : n;
            }
        } catch (Exception ignored) {
        }
        return "driver";
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] b = new byte[1 << 16];
        int n;
        while ((n = in.read(b)) > 0) out.write(b, 0, n);
    }

    private Driver unpack(Uri uri) throws IOException {
        File tmp = new File(a_.getCacheDir(), "driver.zip");
        try (InputStream in = a_.getContentResolver().openInputStream(uri); OutputStream os = new FileOutputStream(tmp)) {
            if (in == null) throw new IOException("can't read it");
            copy(in, os);
        }
        try (ZipFile zip = new ZipFile(tmp)) {
            ZipEntry metaEntry = null;
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) {
                ZipEntry z = e.nextElement();
                String n = z.getName();
                if (n.startsWith("__MACOSX/") || !(n.equals("meta.json") || n.endsWith("/meta.json"))) continue;
                if (metaEntry == null || n.length() < metaEntry.getName().length()) metaEntry = z;
            }
            if (metaEntry == null) throw new IOException("it has no meta.json (not a driver package)");
            String prefix = metaEntry.getName().substring(0, metaEntry.getName().length() - "meta.json".length());
            String metaText;
            try (InputStream in = zip.getInputStream(metaEntry)) {
                ByteArrayOutputStream b = new ByteArrayOutputStream();
                copy(in, b);
                metaText = b.toString("UTF-8");
            }
            String name, library;
            try {
                JSONObject m = new JSONObject(metaText);
                name = m.optString("name", "");
                if (name.isEmpty()) name = zipName(uri);
                library = m.getString("libraryName");
            } catch (JSONException e) {
                throw new IOException("its meta.json can't be read");
            }
            if (zip.getEntry(prefix + library) == null) throw new IOException("it doesn't contain " + library);
            StringBuilder folder = new StringBuilder();
            for (char c : name.toCharArray()) folder.append(Character.isLetterOrDigit(c) || c == '-' || c == '.' ? c : '_');
            File dir = new File(root(a_), folder.length() == 0 ? "driver" : folder.toString());
            deleteTree(dir);
            dir.mkdirs();
            String destPath = dir.getCanonicalPath() + File.separator;
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) {
                ZipEntry z = e.nextElement();
                if (z.isDirectory() || !z.getName().startsWith(prefix) || z.getName().contains("__MACOSX/")) continue;
                File out = new File(dir, z.getName().substring(prefix.length()));
                if (!out.getCanonicalPath().startsWith(destPath)) throw new IOException("bad path in the package");
                out.getParentFile().mkdirs();
                try (InputStream in = zip.getInputStream(z); OutputStream os = new FileOutputStream(out)) {
                    copy(in, os);
                }
            }
            if (library.equals("vulkan.adreno.so")) {
                // Qualcomm's own drivers keep the phone driver's file name: loading it
                // again by that name just gives the phone's back.
                File renamed = new File(dir, "vulkan.custom.so");
                if (!new File(dir, library).renameTo(renamed)) throw new IOException("can't set the driver up");
                try {
                    JSONObject m = new JSONObject(metaText);
                    m.put("libraryName", renamed.getName());
                    Files.write(new File(dir, "meta.json").toPath(), m.toString(2).getBytes(StandardCharsets.UTF_8));
                } catch (JSONException e) {
                    throw new IOException("its meta.json can't be read");
                }
            }
            Driver d = read(dir);
            if (d == null) throw new IOException("its meta.json can't be read");
            return d;
        } finally {
            tmp.delete();
        }
    }
}
