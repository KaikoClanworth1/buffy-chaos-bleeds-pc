// Buffy the Vampire Slayer: Chaos Bleeds - the game's own logo, from the
// player's installed game (the PC launcher's icon: launcher/buffy_launcher.c,
// icon_pixels): Buffy\Binary\_bin_xb\buffytitle.xbx, a 128x128 DXT1 image in
// an XPR0. Nothing of the game ships in the APK: the logo is read from the
// player's own copy - for the launcher's header, the recent-apps card and a
// home-screen shortcut.

package io.github.kaikoclanworth1.buffy;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Bitmap;
import android.graphics.drawable.Icon;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;

final class GameIcon {
    private static Bitmap cached_;

    private static void dxt1(byte[] b, int at, int[] out) {
        int c0 = (b[at] & 0xFF) | (b[at + 1] & 0xFF) << 8, c1 = (b[at + 2] & 0xFF) | (b[at + 3] & 0xFF) << 8;
        long bits = (b[at + 4] & 0xFFL) | (b[at + 5] & 0xFFL) << 8 | (b[at + 6] & 0xFFL) << 16 | (b[at + 7] & 0xFFL) << 24;
        int r0 = (c0 >> 11) * 255 / 31, g0 = ((c0 >> 5) & 63) * 255 / 63, b0 = (c0 & 31) * 255 / 31;
        int r1 = (c1 >> 11) * 255 / 31, g1 = ((c1 >> 5) & 63) * 255 / 63, b1 = (c1 & 31) * 255 / 31;
        int[] pal = new int[4];
        pal[0] = 0xFF000000 | r0 << 16 | g0 << 8 | b0;
        pal[1] = 0xFF000000 | r1 << 16 | g1 << 8 | b1;
        if (c0 > c1) {
            pal[2] = 0xFF000000 | ((2 * r0 + r1) / 3) << 16 | ((2 * g0 + g1) / 3) << 8 | (2 * b0 + b1) / 3;
            pal[3] = 0xFF000000 | ((r0 + 2 * r1) / 3) << 16 | ((g0 + 2 * g1) / 3) << 8 | (b0 + 2 * b1) / 3;
        } else {
            pal[2] = 0xFF000000 | ((r0 + r1) / 2) << 16 | ((g0 + g1) / 2) << 8 | (b0 + b1) / 2;
            pal[3] = 0;
        }
        for (int i = 0; i < 16; i++) out[i] = pal[(int) ((bits >> (i * 2)) & 3)];
    }

    // The 128x128 logo, or null (the game isn't installed, or the file isn't there).
    static synchronized Bitmap load() {
        if (cached_ != null) return cached_;
        File f = new File(InstallActivity.gameFolder(), "Buffy/Binary/_bin_xb/buffytitle.xbx");
        byte[] buf = new byte[0x2800];
        try (InputStream in = new FileInputStream(f)) {
            int got = 0, n;
            while (got < buf.length && (n = in.read(buf, got, buf.length - got)) > 0) got += n;
            if (got < buf.length || buf[0] != 'X' || buf[1] != 'P' || buf[2] != 'R' || buf[3] != '0' || buf[0x19] != 0x0C)
                return null;
        } catch (IOException e) {
            return null;
        }
        int[] px = new int[128 * 128], blk = new int[16];
        for (int by = 0; by < 32; by++)
            for (int bx = 0; bx < 32; bx++) {
                dxt1(buf, 0x800 + (by * 32 + bx) * 8, blk);
                for (int i = 0; i < 16; i++) px[(by * 4 + i / 4) * 128 + bx * 4 + i % 4] = blk[i];
            }
        cached_ = Bitmap.createBitmap(px, 128, 128, Bitmap.Config.ARGB_8888);
        return cached_;
    }

    // The recent-apps card: the logo, on the launcher's colour.
    static void taskCard(Activity a) {
        Bitmap b = load();
        if (b != null)
            a.setTaskDescription(new ActivityManager.TaskDescription("Buffy: Chaos Bleeds", b, LauncherActivity.kBand));
    }

    // A home-screen shortcut with the logo (Android asks the player first).
    static String pinShortcut(Activity a) {
        Bitmap b = load();
        if (b == null) return "The logo is read from the installed game: install it first.";
        ShortcutManager sm = a.getSystemService(ShortcutManager.class);
        if (sm == null || !sm.isRequestPinShortcutSupported()) return "This home screen can't take shortcuts.";
        Intent open = new Intent(a, InstallActivity.class).setAction(Intent.ACTION_MAIN);
        ShortcutInfo info = new ShortcutInfo.Builder(a, "buffy-logo")
            .setShortLabel("Buffy")
            .setLongLabel("Buffy the Vampire Slayer: Chaos Bleeds")
            .setIcon(Icon.createWithAdaptiveBitmap(adaptive(b)))
            .setIntent(open)
            .build();
        return sm.requestPinShortcut(info, null) ? "Android asks where to put the shortcut."
                                                 : "The shortcut couldn't be added.";
    }

    // An adaptive icon's 108dp square: the logo in the middle 72, on its own edge colour.
    private static Bitmap adaptive(Bitmap logo) {
        int n = 432, inner = 288;
        Bitmap out = Bitmap.createBitmap(n, n, Bitmap.Config.ARGB_8888);
        android.graphics.Canvas c = new android.graphics.Canvas(out);
        c.drawColor(logo.getPixel(2, 2) | 0xFF000000);
        android.graphics.Paint p = new android.graphics.Paint(android.graphics.Paint.FILTER_BITMAP_FLAG);
        int o = (n - inner) / 2;
        c.drawBitmap(logo, null, new android.graphics.Rect(o, o, o + inner, o + inner), p);
        return out;
    }
}
