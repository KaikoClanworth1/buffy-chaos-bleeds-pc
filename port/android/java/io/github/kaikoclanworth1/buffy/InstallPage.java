// Buffy the Vampire Slayer: Chaos Bleeds - the Install tab: the game from
// your disc image (an Xbox ISO or XISO), chosen with the system's file picker
// or found in the Download or games folder, into the game folder. As the PC
// launcher's Install tab (launcher/buffy_launcher.c), less the movies (they
// are converted with FFmpeg on a PC; copy a PC install's Movies folder over).

package io.github.kaikoclanworth1.buffy;

import android.content.Intent;
import android.net.Uri;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.os.StatFs;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.nio.channels.FileChannel;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

final class InstallPage {
    private final LauncherActivity a_;
    private final LinearLayout view_;
    private final TextView state_, image_, progressText_;
    private final ProgressBar progress_;
    private final Button choose_, install_, stop_;
    private Uri uri_;            // the chosen image (the picker's), or
    private File file_;          // one found in a folder
    private final AtomicBoolean cancel_ = new AtomicBoolean();
    private boolean busy_;

    InstallPage(LauncherActivity a) {
        a_ = a;
        view_ = a.column();
        LinearLayout st = a.card(view_, "Game");
        state_ = a.para(st, "");

        LinearLayout disc = a.card(view_, "1.  Your disc image");
        a.para(disc, "Buffy the Vampire Slayer: Chaos Bleeds for the Xbox, as an ISO or XISO you made from your own "
            + "disc. This build plays the European (PAL) release.");
        image_ = a.text("No disc image chosen.", 14, LauncherActivity.kText);
        image_.setPadding(0, a.dp(4), 0, a.dp(10));
        disc.addView(image_);
        choose_ = a.button("Choose disc image…", LauncherActivity.kBand);
        choose_.setOnClickListener(v -> choose());
        disc.addView(choose_, a.fullWidth(0));
        TextView found = a.text("", 12, LauncherActivity.kDim);
        disc.addView(found);

        LinearLayout go = a.card(view_, "2.  Install");
        a.para(go, "Into games/" + InstallActivity.kFolderName + " (about 3.5 GB). Your saves, settings and mods "
            + "there stay.");
        progress_ = new ProgressBar(a, null, android.R.attr.progressBarStyleHorizontal);
        progress_.setMax(1000);
        progress_.setProgressTintList(android.content.res.ColorStateList.valueOf(LauncherActivity.kAccent));
        progress_.setVisibility(View.GONE);
        go.addView(progress_);
        progressText_ = a.text("", 12, LauncherActivity.kDim);
        go.addView(progressText_);
        install_ = a.button("Install", LauncherActivity.kAccent);
        install_.setOnClickListener(v -> install());
        stop_ = a.button("Cancel", LauncherActivity.kBand);
        stop_.setVisibility(View.GONE);
        stop_.setOnClickListener(v -> cancel_.set(true));
        go.addView(install_, a.fullWidth(6));
        go.addView(stop_, a.fullWidth(8));
        View spacer = new View(a);
        go.addView(spacer, new LinearLayout.LayoutParams(1, a.dp(10)));

        LinearLayout movies = a.card(view_, "Movies");
        a.para(movies, "The game's movies are converted for playback by the PC launcher (with FFmpeg). Movie playback "
            + "on Android is still to come; until then the game skips them.");
    }

    View view() { return view_; }

    void refresh() {
        state_.setText(InstallActivity.installed()
            ? "Installed in games/" + InstallActivity.kFolderName + ". Installing again replaces the game's files."
            : "Not installed yet.");
        if (uri_ == null && file_ == null) {
            File f = findImage();
            if (f != null) {
                file_ = f;
                image_.setText("Found: " + f.getAbsolutePath());
            }
        }
        install_.setEnabled(!busy_ && (uri_ != null || file_ != null));
        install_.setAlpha(install_.isEnabled() ? 1f : 0.5f);
    }

    // An image where it may have been copied: Download, games, the storage's root.
    static File findImage() {
        File root = Environment.getExternalStorageDirectory();
        File[] places = { new File(root, Environment.DIRECTORY_DOWNLOADS), new File(root, "games"),
                          InstallActivity.gameFolder(), root };
        for (File dir : places) {
            File[] list = dir.listFiles();
            if (list == null) continue;
            for (File f : list) {
                String n = f.getName().toLowerCase();
                if (f.isFile() && (n.endsWith(".iso") || n.endsWith(".xiso")) && f.length() > (500L << 20)) return f;
            }
        }
        return null;
    }

    private void choose() {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        a_.startForResult(pick, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null) return;
            uri_ = data.getData();
            file_ = null;
            image_.setText("Chosen: " + uri_.getLastPathSegment());
            refresh();
        });
    }

    private void install() {
        if (busy_ || !a_.gameClosed()) return;
        File dest = InstallActivity.gameFolder();
        dest.mkdirs();
        long free = new StatFs(dest.getAbsolutePath()).getAvailableBytes();
        busy_ = true;
        cancel_.set(false);
        progress_.setProgress(0);
        progress_.setVisibility(View.VISIBLE);
        stop_.setVisibility(View.VISIBLE);
        choose_.setEnabled(false);
        refresh();
        progressText_.setText("Reading the disc image…");
        final String[] msg = new String[1];
        a_.background(() -> msg[0] = run(dest, free), () -> {
            busy_ = false;
            progress_.setVisibility(View.GONE);
            stop_.setVisibility(View.GONE);
            choose_.setEnabled(true);
            progressText_.setText("");
            a_.status(msg[0]);
            refresh();
        });
    }

    // The install itself (off the UI thread): what to tell the player.
    private String run(File dest, long free) {
        ParcelFileDescriptor pfd = null;
        try {
            FileChannel ch;
            if (uri_ != null) {
                pfd = a_.getContentResolver().openFileDescriptor(uri_, "r");
                if (pfd == null) return "The disc image could not be opened.";
                ch = new FileInputStream(pfd.getFileDescriptor()).getChannel();
            } else {
                ch = new FileInputStream(file_).getChannel();
            }
            Xdvdfs disc = new Xdvdfs(ch);
            String err = disc.open();
            if (err != null) return err;
            switch (disc.release()) {
            case Xdvdfs.RELEASE_PAL:
                break;
            case Xdvdfs.RELEASE_USA:
                return "This is the North American release. The Android build plays the European (PAL) release "
                    + "for now (the PC build plays both).";
            default:
                return "This disc image is not a release the port knows (it needs Buffy the Vampire Slayer: "
                    + "Chaos Bleeds for the Xbox, the European release).";
            }
            if (free < disc.total + (64L << 20))
                return "Not enough free space: the game needs " + (disc.total >> 20) + " MB, there is " + (free >> 20) + " MB.";
            // An old install's default.xbe first: a stopped install is then not taken for a whole one.
            new File(dest, "default.xbe").delete();
            final long[] last = {0};
            disc.extract(dest, (done, total, file) -> {
                if (done - last[0] < (8 << 20) && done != total) return;
                last[0] = done;
                a_.runOnUiThread(() -> {
                    progress_.setProgress((int) (done * 1000 / Math.max(1, total)));
                    progressText_.setText((done >> 20) + " of " + (total >> 20) + " MB — " + file);
                });
            }, cancel_);
            return "Installed. Press Play on the Play tab." + (file_ != null ? " You can delete " + file_.getName()
                + " now to free up space." : "");
        } catch (Exception e) {
            return cancel_.get() ? "Install cancelled: the game is not complete until it is installed again."
                                 : "Installing failed: " + e.getMessage();
        } finally {
            try {
                if (pfd != null) pfd.close();
            } catch (Exception ignored) {
            }
        }
    }
}
