// Buffy the Vampire Slayer: Chaos Bleeds - the app's entry. The game folder
// is in shared storage (games/Buffy Chaos Bleeds), laid out like the PC
// build's: reachable from a PC over USB and from file managers, for mods,
// saves and texture packs. So the first step is all-files access; then the
// launcher (on its Install tab while the game is not installed).

package io.github.kaikoclanworth1.buffy;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;

public class InstallActivity extends Activity {
    static final String kFolderName = "Buffy Chaos Bleeds";

    // <shared storage>/games/Buffy Chaos Bleeds
    static File gameFolder() {
        return new File(new File(Environment.getExternalStorageDirectory(), "games"), kFolderName);
    }

    static boolean installed() {
        return new File(gameFolder(), "default.xbe").isFile() && new File(gameFolder(), "Buffy").isDirectory();
    }

    private TextView text_;
    private Button button_;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setBackgroundColor(LauncherActivity.kBackground);
        layout.setPadding(64, 64, 64, 64);
        TextView title = new TextView(this);
        title.setText("Buffy the Vampire Slayer: Chaos Bleeds");
        title.setTextColor(LauncherActivity.kText);
        title.setTextSize(24);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.CENTER);
        text_ = new TextView(this);
        text_.setTextColor(LauncherActivity.kDim);
        text_.setTextSize(17);
        text_.setGravity(Gravity.CENTER);
        text_.setPadding(0, 32, 0, 32);
        button_ = new Button(this);
        button_.setAllCaps(false);
        button_.setTextColor(0xFFFFFFFF);
        button_.setTextSize(18);
        android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable();
        bg.setColor(LauncherActivity.kAccent);
        bg.setCornerRadius(24);
        button_.setBackground(bg);
        button_.setPadding(48, 24, 48, 24);
        layout.addView(title);
        layout.addView(text_);
        layout.addView(button_);
        setContentView(layout);
    }

    @Override
    protected void onResume() {
        super.onResume();
        next();
    }

    private void next() {
        if (!Environment.isExternalStorageManager()) {
            text_.setText("The game is kept in the phone's games folder:\n"
                + "games/" + kFolderName + "\n\n(as the PC version's game folder: your saves, mods and texture packs "
                + "go there too, and a PC can reach it over USB)\n\nAllow access to all files on the next screen.");
            button_.setText("Allow access");
            button_.setOnClickListener(v -> startActivity(new Intent(
                Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName()))));
            return;
        }
        gameFolder().mkdirs();
        // Automated tests over USB debugging (am start ... --es BUFFY_TEST_LEVEL ...): straight to the game.
        android.os.Bundle extras = getIntent().getExtras();
        if (installed() && extras != null) {
            for (String k : extras.keySet())
                if (k.startsWith("BUFFY_") || k.startsWith("RECOMP_")) {
                    startActivity(new Intent(this, GameActivity.class).putExtras(extras));
                    finish();
                    return;
                }
        }
        Intent launcher = new Intent(this, LauncherActivity.class);
        if (!installed()) launcher.putExtra("tab", "Install");
        startActivity(launcher);
        finish();
    }
}
