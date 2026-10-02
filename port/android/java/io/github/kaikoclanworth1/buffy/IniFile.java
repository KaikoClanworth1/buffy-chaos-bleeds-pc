// Buffy the Vampire Slayer: Chaos Bleeds - an .ini file as Windows'
// GetPrivateProfileString / WritePrivateProfileString keep it: [Section] and
// Key=Value lines, names matched ignoring case, other lines (comments, other
// sections) left as they are. buffy_settings.ini and the mods' mod.ini.

package io.github.kaikoclanworth1.buffy;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

final class IniFile {
    private final File file_;
    private final List<String> lines_ = new ArrayList<>();

    IniFile(File file) {
        file_ = file;
        load();
    }

    File file() { return file_; }

    void load() {
        lines_.clear();
        try {
            if (file_.isFile()) lines_.addAll(Files.readAllLines(file_.toPath(), StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
    }

    boolean save() {
        StringBuilder sb = new StringBuilder();
        for (String l : lines_) sb.append(l).append("\r\n");
        File tmp = new File(file_.getPath() + ".tmp");
        try (FileOutputStream os = new FileOutputStream(tmp)) {
            os.write(sb.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            return false;
        }
        return tmp.renameTo(file_) || (file_.delete() && tmp.renameTo(file_));
    }

    private static String section(String line) {
        String t = line.trim();
        return t.startsWith("[") && t.indexOf(']') > 0 ? t.substring(1, t.indexOf(']')).trim() : null;
    }

    // The line index of the key in the section, or -1.
    private int find(String sec, String key) {
        String cur = null;
        for (int i = 0; i < lines_.size(); i++) {
            String l = lines_.get(i), s = section(l);
            if (s != null) {
                cur = s;
                continue;
            }
            int eq = l.indexOf('=');
            if (cur != null && cur.equalsIgnoreCase(sec) && eq > 0 && !l.trim().startsWith(";")
                    && l.substring(0, eq).trim().equalsIgnoreCase(key))
                return i;
        }
        return -1;
    }

    String get(String sec, String key, String fallback) {
        int i = find(sec, key);
        if (i < 0) return fallback;
        String l = lines_.get(i);
        return l.substring(l.indexOf('=') + 1).trim();
    }

    int getInt(String sec, String key, int fallback) {
        try {
            String v = get(sec, key, null);
            return v == null || v.isEmpty() ? fallback : Integer.parseInt(v.split("[ ;]")[0]);
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    boolean getBool(String sec, String key, boolean fallback) {
        return getInt(sec, key, fallback ? 1 : 0) != 0;
    }

    void set(String sec, String key, String value) {
        int i = find(sec, key);
        String line = key + "=" + value;
        if (i >= 0) {
            lines_.set(i, line);
            return;
        }
        // the end of the section, or a new section at the end
        String cur = null;
        int last = -1;
        for (int k = 0; k < lines_.size(); k++) {
            String s = section(lines_.get(k));
            if (s != null) cur = s;
            if (cur != null && cur.equalsIgnoreCase(sec) && !lines_.get(k).trim().isEmpty()) last = k;
        }
        if (last >= 0) {
            lines_.add(last + 1, line);
        } else {
            lines_.add("[" + sec + "]");
            lines_.add(line);
        }
    }

    void setInt(String sec, String key, int v) { set(sec, key, Integer.toString(v)); }

    void setBool(String sec, String key, boolean v) { set(sec, key, v ? "1" : "0"); }
}
