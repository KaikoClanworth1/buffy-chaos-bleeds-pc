// Buffy the Vampire Slayer: Chaos Bleeds - the game: Android's NativeActivity
// running libbuffy.so (port/android/src/android_main.c), told where the game
// folder is. The launcher starts it.

package io.github.kaikoclanworth1.buffy;

import android.app.NativeActivity;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.Display;
import android.view.WindowManager;

public class GameActivity extends NativeActivity {
    // While the game runs (the launcher's Saves and Mods tabs wait: it keeps those files open).
    static volatile boolean running;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        running = true;
        try {
            // The game folder: where the PC build's exe folder would be (android_main.c).
            Os.setenv("BUFFY_HOME", InstallActivity.gameFolder().getAbsolutePath(), true);
            // A custom GPU driver (the launcher's Settings: Drivers.java), and where libadrenotools
            // finds its hooks and keeps its files (port/android/src/android_driver.c).
            IniFile ini = new IniFile(LauncherActivity.settingsFile());
            Os.setenv("BUFFY_GPU_DRIVER", ini.get("Android", "GpuDriver", ""), true);
            Os.setenv("BUFFY_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
            Os.setenv("BUFFY_CACHE_DIR", getCacheDir().getAbsolutePath(), true);
            // Automated tests over USB debugging: --es BUFFY_... values become the environment.
            Bundle extras = getIntent().getExtras();
            if (extras != null) {
                for (String key : extras.keySet()) {
                    Object value = extras.get(key);
                    if ((key.startsWith("BUFFY_") || key.startsWith("RECOMP_")) && value != null)
                        Os.setenv(key, value.toString(), true);
                }
                // (testing) BUFFY_TEST_DRIVER_ZIP=<a driver package on the phone>: unpacked
                // into the app's storage and used for this run
                String zip = extras.getString("BUFFY_TEST_DRIVER_ZIP");
                if (zip != null) {
                    String lib = testDriver(new java.io.File(zip));
                    if (lib != null) Os.setenv("BUFFY_GPU_DRIVER", lib, true);
                }
            }
        } catch (ErrnoException e) {
            Log.e("buffy", "setenv failed", e);
        }
        super.onCreate(savedInstanceState);
        preferSixty();
    }

    private String testDriver(java.io.File zipFile) {
        java.io.File dir = new java.io.File(getFilesDir(), "test_driver");
        try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(zipFile)) {
            java.io.File[] old = dir.listFiles();
            if (old != null) for (java.io.File f : old) f.delete();
            dir.mkdirs();
            String library = null, prefix = "";
            for (java.util.Enumeration<? extends java.util.zip.ZipEntry> e = zip.entries(); e.hasMoreElements();) {
                java.util.zip.ZipEntry z = e.nextElement();
                if (z.getName().endsWith("meta.json") && !z.getName().contains("__MACOSX")) {
                    prefix = z.getName().substring(0, z.getName().length() - "meta.json".length());
                    byte[] b = new byte[(int) z.getSize()];
                    try (java.io.DataInputStream in = new java.io.DataInputStream(zip.getInputStream(z))) { in.readFully(b); }
                    library = new org.json.JSONObject(new String(b, "UTF-8")).getString("libraryName");
                }
            }
            if (library == null) return null;
            java.util.zip.ZipEntry z = zip.getEntry(prefix + library);
            String name = library.equals("vulkan.adreno.so") ? "vulkan.custom.so" : library;
            java.io.File out = new java.io.File(dir, name);
            try (java.io.InputStream in = zip.getInputStream(z); java.io.OutputStream os = new java.io.FileOutputStream(out)) {
                byte[] b = new byte[1 << 16];
                int n;
                while ((n = in.read(b)) > 0) os.write(b, 0, n);
            }
            Log.i("buffy", "test driver " + zipFile + ": " + out);
            return out.getAbsolutePath();
        } catch (Exception e) {
            Log.e("buffy", "test driver " + zipFile, e);
            return null;
        }
    }

    @Override
    protected void onDestroy() {
        running = false;
        super.onDestroy();
        // The game is one process's worth of state: a new start is a new process.
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) preferSixty();   // (the Fold: the other screen has its own modes)
    }

    // The game runs at 60 fps: on a 120 Hz screen its frames would land on 2 or 3
    // refreshes in turn (a judder), so it asks for the screen's 60 Hz mode.
    private void preferSixty() {
        try {
            Display display = getWindowManager().getDefaultDisplay();
            Display.Mode current = display.getMode(), best = null;
            for (Display.Mode m : display.getSupportedModes()) {
                if (m.getPhysicalWidth() != current.getPhysicalWidth() || m.getPhysicalHeight() != current.getPhysicalHeight())
                    continue;
                if (Math.abs(m.getRefreshRate() - 60f) < 1f) best = m;
            }
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            if (best != null) lp.preferredDisplayModeId = best.getModeId();
            lp.preferredRefreshRate = 60f;
            getWindow().setAttributes(lp);
        } catch (Exception e) {
            Log.w("buffy", "60 Hz request failed", e);
        }
    }
}
