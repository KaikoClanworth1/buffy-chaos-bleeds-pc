// Buffy the Vampire Slayer: Chaos Bleeds - the launcher on the phone: the
// PC launcher's tabs (launcher/buffy_launcher.c) for a touch screen. Play
// starts the game; the other tabs read and write the game folder's
// buffy_settings.ini, mods, saves and texture packs as the PC launcher does,
// each change saved at once.

package io.github.kaikoclanworth1.buffy;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public class LauncherActivity extends Activity {
    // The PC launcher's title band and text, and a blood red.
    static final int kAccent = 0xFFB3122B, kBackground = 0xFF120D17, kBand = 0xFF1C1224, kCard = 0xFF1F1628,
        kText = 0xFFF0E8D6, kDim = 0xFFB0A0BA, kLine = 0xFF33263D;

    IniFile ini_;
    private Drivers drivers_;
    private android.widget.ImageView logo_;
    private View bar_;

    void showLogo() {
        android.graphics.Bitmap b = GameIcon.load();
        logo_.setImageBitmap(b);
        logo_.setVisibility(b != null ? View.VISIBLE : View.GONE);
        bar_.setVisibility(b != null ? View.GONE : View.VISIBLE);
        GameIcon.taskCard(this);
    }
    private final List<Button> tabs_ = new ArrayList<>();
    private final List<View> pages_ = new ArrayList<>();
    private final List<Runnable> onShow_ = new ArrayList<>();
    private final List<String> names_ = new ArrayList<>();
    private final List<Runnable> refreshers_ = new ArrayList<>();
    TextView status_;
    private boolean loading_;

    int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    static File settingsFile() { return new File(InstallActivity.gameFolder(), "buffy_settings.ini"); }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setStatusBarColor(kBand);
        getWindow().setNavigationBarColor(kBackground);
        InstallActivity.gameFolder().mkdirs();
        ini_ = new IniFile(settingsFile());

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(kBackground);

        // The title band, as the PC launcher's.
        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setBackgroundColor(kBand);
        header.setPadding(dp(16), dp(12), dp(16), dp(10));
        // the game's own logo (read from the installed game: GameIcon), else the red bar
        logo_ = new android.widget.ImageView(this);
        header.addView(logo_, new LinearLayout.LayoutParams(dp(48), dp(48)));
        bar_ = new View(this);
        bar_.setBackgroundColor(kAccent);
        header.addView(bar_, new LinearLayout.LayoutParams(dp(6), ViewGroup.LayoutParams.MATCH_PARENT));
        header.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout titles = new LinearLayout(this);
        titles.setOrientation(LinearLayout.VERTICAL);
        titles.setPadding(dp(12), 0, 0, 0);
        TextView title = text("Buffy the Vampire Slayer: Chaos Bleeds", 21, kText);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        titles.addView(title);
        titles.addView(text("Android launcher · version " + versionName(), 13, kDim));
        header.addView(titles);
        root.addView(header);

        HorizontalScrollView tabScroll = new HorizontalScrollView(this);
        tabScroll.setHorizontalScrollBarEnabled(false);
        tabScroll.setBackgroundColor(kBand);
        LinearLayout tabRow = new LinearLayout(this);
        tabRow.setPadding(dp(8), 0, dp(8), 0);
        tabScroll.addView(tabRow);
        root.addView(tabScroll);
        View line = new View(this);
        line.setBackgroundColor(kLine);
        root.addView(line, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1)));

        FrameLayout content = new FrameLayout(this);
        root.addView(content, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        status_ = text("", 13, kDim);
        status_.setPadding(dp(16), dp(6), dp(16), dp(10));
        root.addView(status_);

        addPage(tabRow, content, "Play", playPage(), null);
        addPage(tabRow, content, "Settings", settingsPage(), null);
        addPage(tabRow, content, "Controls", controlsPage(), null);
        ModsPage mods = new ModsPage(this);
        addPage(tabRow, content, "Mods", mods.view(), mods::refresh);
        TexturesPage textures = new TexturesPage(this);
        addPage(tabRow, content, "Textures", textures.view(), textures::refresh);
        SavesPage saves = new SavesPage(this);
        addPage(tabRow, content, "Saves", saves.view(), saves::refresh);
        InstallPage install = new InstallPage(this);
        addPage(tabRow, content, "Install", install.view(), install::refresh);
        String tab = getIntent().getStringExtra("tab");
        select(Math.max(0, tab == null ? 0 : names_.indexOf(tab)));

        // Edge to edge: clear of the status and navigation bars and the cut-out.
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            android.graphics.Insets b = insets.getInsets(android.view.WindowInsets.Type.systemBars()
                | android.view.WindowInsets.Type.displayCutout());
            v.setPadding(b.left, b.top, b.right, b.bottom);
            return android.view.WindowInsets.CONSUMED;
        });
        setContentView(root);
    }

    @Override
    protected void onResume() {
        super.onResume();
        ini_.load();           // (the game's own PC Settings page may have changed some)
        refresh();
        if (playStatus_ != null) updatePlayStatus();
        showLogo();
        if (drivers_ != null) {
            drivers_.refresh();
            drivers_.checkCrash();
        }
    }

    String versionName() {
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            return info.versionName;
        } catch (Exception e) {
            return "?";
        }
    }

    // ── shared by the pages ──────────────────────────────────────────────

    void status(String s) { status_.setText(s); }

    // Is the game running (its own process, ":game")?
    boolean gameRunning() {
        android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
        java.util.List<android.app.ActivityManager.RunningAppProcessInfo> ps = am.getRunningAppProcesses();
        if (ps != null)
            for (android.app.ActivityManager.RunningAppProcessInfo p : ps)
                if (p.processName != null && p.processName.endsWith(":game")) return true;
        return false;
    }

    boolean gameClosed() {
        if (!gameRunning()) return true;
        status("Close the game first: it keeps these files open while it runs.");
        return false;
    }

    void confirm(String message, String action, Runnable onYes) {
        new AlertDialog.Builder(this, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setMessage(message)
            .setPositiveButton(action, (d, w) -> onYes.run())
            .setNegativeButton("Cancel", null)
            .show();
    }

    interface Result { void run(int resultCode, Intent data); }
    private final Map<Integer, Result> results_ = new HashMap<>();
    private int nextRequest_ = 1000;

    void startForResult(Intent intent, Result onResult) {
        int code = nextRequest_++;
        results_.put(code, onResult);
        try {
            startActivityForResult(intent, code);
        } catch (Exception e) {
            results_.remove(code);
            status("Could not open the file picker: " + e.getMessage());
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        Result r = results_.remove(requestCode);
        if (r != null) r.run(resultCode, data);
        else super.onActivityResult(requestCode, resultCode, data);
    }

    void background(Runnable work, Runnable done) {
        new Thread(() -> {
            work.run();
            if (done != null) runOnUiThread(done);
        }, "launcher").start();
    }

    LinearLayout.LayoutParams fullWidth(int topMargin) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(topMargin);
        return lp;
    }

    LinearLayout pair(Button a, Button b) {
        LinearLayout r = new LinearLayout(this);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
        lp.rightMargin = dp(6);
        r.addView(a, lp);
        LinearLayout.LayoutParams lp2 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
        lp2.leftMargin = dp(6);
        r.addView(b, lp2);
        return r;
    }

    TextView text(String s, float sp, int color) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        return t;
    }

    TextView para(LinearLayout card, String s) {
        TextView t = text(s, 14, kDim);
        t.setPadding(0, dp(10), 0, dp(10));
        card.addView(t);
        return t;
    }

    GradientDrawable rounded(int color, float radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(radius));
        return d;
    }

    Button button(String label, int color) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(16);
        b.setBackground(rounded(color, 10));
        b.setMinHeight(dp(52));
        return b;
    }

    LinearLayout column() {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(16), dp(12), dp(16), dp(16));
        return c;
    }

    LinearLayout card(LinearLayout column, String heading) {
        TextView h = text(heading.toUpperCase(), 12, kDim);
        h.setTypeface(Typeface.DEFAULT_BOLD);
        h.setLetterSpacing(0.08f);
        h.setPadding(dp(4), dp(12), 0, dp(6));
        column.addView(h);
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setBackground(rounded(kCard, 12));
        c.setPadding(dp(14), dp(4), dp(14), dp(4));
        column.addView(c);
        return c;
    }

    void row(LinearLayout card, String label, String hint, View control) {
        LinearLayout r = new LinearLayout(this);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setMinimumHeight(dp(56));
        r.setPadding(0, dp(6), 0, dp(6));
        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.addView(text(label, 16, kText));
        if (hint != null) texts.addView(text(hint, 12, kDim));
        r.addView(texts, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        if (control != null) r.addView(control);
        if (card.getChildCount() > 0) {
            View sep = new View(this);
            sep.setBackgroundColor(kLine);
            card.addView(sep, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 1));
        }
        card.addView(r);
    }

    Switch makeSwitch() {
        Switch s = new Switch(this);
        int[][] states = {{android.R.attr.state_checked}, {}};
        s.setThumbTintList(new ColorStateList(states, new int[] {kAccent, 0xFFB0A8B8}));
        s.setTrackTintList(new ColorStateList(states, new int[] {0x88B3122B, 0xFF4A3F54}));
        return s;
    }

    private void addPage(LinearLayout tabRow, FrameLayout content, String name, View page, Runnable onShow) {
        names_.add(name);
        onShow_.add(onShow);
        final int index = tabs_.size();
        Button tab = new Button(this);
        tab.setText(name);
        tab.setAllCaps(false);
        tab.setTextSize(15);
        tab.setBackgroundColor(Color.TRANSPARENT);
        tab.setMinHeight(dp(48));
        tab.setPadding(dp(16), 0, dp(16), 0);
        tab.setOnClickListener(v -> select(index));
        tabRow.addView(tab);
        tabs_.add(tab);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(page);
        content.addView(scroll);
        pages_.add(scroll);
    }

    void select(int index) {
        for (int i = 0; i < tabs_.size(); i++) {
            boolean on = i == index;
            tabs_.get(i).setTextColor(on ? Color.WHITE : kDim);
            tabs_.get(i).setTypeface(on ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
            tabs_.get(i).setBackground(on ? underline() : null);
            pages_.get(i).setVisibility(on ? View.VISIBLE : View.GONE);
        }
        Button tab = tabs_.get(index);
        HorizontalScrollView bar = (HorizontalScrollView) tab.getParent().getParent();
        bar.post(() -> bar.smoothScrollTo(tab.getLeft() + tab.getWidth() / 2 - bar.getWidth() / 2, 0));
        if (onShow_.get(index) != null) onShow_.get(index).run();
    }

    void select(String name) {
        int i = names_.indexOf(name);
        if (i >= 0) select(i);
    }

    private Drawable underline() {
        LayerDrawable l = new LayerDrawable(new Drawable[] {new ColorDrawable(Color.TRANSPARENT), new ColorDrawable(kAccent)});
        l.setLayerInset(1, dp(12), dp(44), dp(12), 0);
        return l;
    }

    void saved(String what) {
        if (loading_) return;
        status_.setText(ini_.save() ? what + " saved." : "Could not save the settings (is the game folder there?).");
    }

    void refresh() {
        loading_ = true;
        for (Runnable r : refreshers_) r.run();
        loading_ = false;
    }

    boolean loading() { return loading_; }

    void onRefresh(Runnable r) { refreshers_.add(r); }

    // An on/off setting in buffy_settings.ini.
    void toggle(LinearLayout card, String label, String hint, String sec, String key, boolean fallback) {
        Switch s = makeSwitch();
        s.setOnCheckedChangeListener((b, on) -> {
            if (loading_ || on == ini_.getBool(sec, key, fallback)) return;
            ini_.setBool(sec, key, on);
            saved(label);
        });
        refreshers_.add(() -> s.setChecked(ini_.getBool(sec, key, fallback)));
        row(card, label, hint, s);
    }

    // A choice between labelled values; setter null: written as sec/key.
    interface Setter { void set(int index); }
    interface Getter { int get(); }

    void choice(LinearLayout card, String label, String hint, String[] labels, Getter get, Setter set) {
        Spinner sp = new Spinner(this, Spinner.MODE_DROPDOWN);
        ArrayAdapter<String> a = new ArrayAdapter<String>(this, android.R.layout.simple_spinner_item, labels) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                TextView t = (TextView) super.getView(position, convertView, parent);
                t.setTextColor(kText);
                t.setTextSize(15);
                return t;
            }
        };
        a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(a);
        final boolean[] touched = {false};
        sp.setOnTouchListener((v, e) -> {
            touched[0] = true;
            return false;
        });
        sp.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (loading_ || !touched[0] || position == get.get()) return;
                set.set(position);
                saved(label);
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {}
        });
        refreshers_.add(() -> sp.setSelection(Math.max(0, Math.min(labels.length - 1, get.get())), false));
        row(card, label, hint, sp);
    }

    // A choice stored as one of a list of integers.
    void intChoice(LinearLayout card, String label, String hint, String sec, String key, String[] labels,
                   int[] values, int fallback) {
        choice(card, label, hint, labels, () -> {
            int v = ini_.getInt(sec, key, fallback);
            for (int i = 0; i < values.length; i++) if (values[i] == v) return i;
            return 0;
        }, i -> ini_.setInt(sec, key, values[i]));
    }

    // ── Play ─────────────────────────────────────────────────────────────

    private TextView playStatus_;
    private Button play_;

    private View playPage() {
        LinearLayout c = column();
        LinearLayout about = card(c, "Game");
        TextView desc = text("The PC port of the 2003 Xbox game, running the original game code — on your phone.",
            15, kText);
        desc.setPadding(0, dp(10), 0, dp(6));
        about.addView(desc);
        TextView folder = text("Game folder: games/" + InstallActivity.kFolderName, 13, kDim);
        folder.setPadding(0, 0, 0, dp(10));
        about.addView(folder);

        playStatus_ = text("", 14, kDim);
        playStatus_.setPadding(dp(4), dp(14), dp(4), 0);
        c.addView(playStatus_);

        play_ = button("Play", kAccent);
        play_.setTextSize(22);
        play_.setTypeface(Typeface.DEFAULT_BOLD);
        play_.setMinHeight(dp(72));
        play_.setOnClickListener(v -> play());
        c.addView(play_, fullWidth(12));

        TextView tips = text("Play with a controller (Bluetooth or USB) or the on-screen touch controls "
            + "(Controls tab). Android's Back gesture pauses the game. In the game, Options → PC Settings has "
            + "the resolution, FPS limit and language too.", 13, kDim);
        tips.setPadding(dp(4), dp(14), dp(4), 0);
        c.addView(tips);

        LinearLayout home = card(c, "Home screen");
        para(home, "A shortcut with the game's own logo, read from your installed game (the app's icon can't show it: "
            + "nothing of the game is in the app).");
        Button pin = button("Add the logo icon to the home screen", kBand);
        pin.setOnClickListener(v -> status(GameIcon.pinShortcut(this)));
        home.addView(pin, fullWidth(0));
        home.addView(new View(this), new LinearLayout.LayoutParams(1, dp(10)));
        updatePlayStatus();
        return c;
    }

    private void updatePlayStatus() {
        boolean ok = InstallActivity.installed();
        playStatus_.setText(ok ? "Ready to play." : "The game isn't installed yet: Install tab.");
        play_.setText(ok ? "Play" : "Install the game");
    }

    void play() {
        if (!InstallActivity.installed()) {
            select("Install");
            return;
        }
        ini_.save();
        startActivity(new Intent(this, GameActivity.class));
    }

    // ── Settings ─────────────────────────────────────────────────────────

    // The game's render sizes (src/buffy_settings.c, k_res).
    static final int[][] kRes = { {640, 480}, {1280, 960}, {1920, 1440}, {2560, 1920},
                                  {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160} };

    private View settingsPage() {
        LinearLayout c = column();
        LinearLayout display = card(c, "Display");
        String[] resLabels = new String[kRes.length];
        for (int i = 0; i < kRes.length; i++)
            resLabels[i] = kRes[i][0] + " × " + kRes[i][1] + (kRes[i][0] * 3 == kRes[i][1] * 4 ? " (4:3)" : " (16:9)");
        choice(display, "Resolution", "The size the game is drawn at. 16:9 sizes play in the game's own widescreen "
            + "mode; menus and movies stay 4:3.", resLabels, () -> {
                int w = ini_.getInt("Display", "Width", 1920), h = ini_.getInt("Display", "Height", 1080);
                for (int i = 0; i < kRes.length; i++) if (kRes[i][0] == w && kRes[i][1] == h) return i;
                return 5;
            }, i -> {
                ini_.setInt("Display", "Width", kRes[i][0]);
                ini_.setInt("Display", "Height", kRes[i][1]);
            });
        toggle(display, "VSync", "No tearing; waits for the screen's refresh", "Display", "VSync", true);
        choice(display, "Widescreen view", "At 16:9 sizes", new String[] {"The full 16:9 view", "The original 4:3 zoomed"},
            () -> ini_.getBool("Display", "WidescreenWide", true) ? 0 : 1,
            i -> ini_.setBool("Display", "WidescreenWide", i == 0));

        LinearLayout perf = card(c, "Performance");
        intChoice(perf, "FPS limit", "The game keeps its own speed at any frame rate", "Display", "FpsLimit",
            new String[] {"30", "60", "90", "120"}, new int[] {30, 60, 90, 120}, 60);
        toggle(perf, "Frame interpolation", "At a 60 FPS limit (experimental)", "Display", "FrameInterpolation", false);
        toggle(perf, "Preload game data", "Reads the game's archives at start-up: no hitches later", "Game", "PreloadData", true);
        toggle(perf, "Show FPS counter", null, "Display", "ShowFps", false);

        LinearLayout game = card(c, "Game");
        toggle(game, "Skip the intro movies", "At start-up", "Game", "SkipIntroMovies", false);
        choice(game, "Language", "The European release's languages; used from the next start",
            new String[] {"English", "Français", "Deutsch", "Español"}, () -> {
                String v = ini_.get("Game", "Language", "English");
                String[] names = {"English", "French", "German", "Spanish"};
                for (int i = 0; i < names.length; i++) if (names[i].equalsIgnoreCase(v)) return i;
                return 0;
            }, i -> ini_.set("Game", "Language", new String[] {"English", "French", "German", "Spanish"}[i]));
        toggle(game, "Invert camera left / right", "The right stick", "Controls", "InvertCameraX", false);

        LinearLayout coop = card(c, "Story co-op");
        para(coop, "A second player in the story mode: press Start on the second controller to join. "
            + "(Switch the mod on in the Mods tab.)");
        intChoice(coop, "Screens", null, "Coop", "Screens", new String[] {"One screen", "Split screen"},
            new int[] {1, 3}, 1);
        toggle(coop, "Friendly fire", null, "Coop", "FriendlyFire", false);
        toggle(coop, "Shared inventory", null, "Coop", "SharedInventory", true);
        toggle(coop, "Back teleports to the other player", null, "Coop", "BackTeleport", true);
        intChoice(coop, "Respawn after", null, "Coop", "RespawnSeconds", new String[] {"1 s", "3 s", "5 s", "10 s"},
            new int[] {1, 3, 5, 10}, 3);

        drivers_ = new Drivers(this);
        drivers_.build(c);

        Button defaults = button("Restore defaults", kCard);
        defaults.setOnClickListener(v -> confirm("Put every setting back to how it started?", "Restore", () -> {
            ini_.setInt("Display", "Width", 1920);
            ini_.setInt("Display", "Height", 1080);
            ini_.setBool("Display", "VSync", true);
            ini_.setBool("Display", "WidescreenWide", true);
            ini_.setInt("Display", "FpsLimit", 60);
            ini_.setBool("Display", "FrameInterpolation", false);
            ini_.setBool("Game", "PreloadData", true);
            ini_.setBool("Display", "ShowFps", false);
            ini_.setBool("Game", "SkipIntroMovies", false);
            ini_.set("Game", "Language", "English");
            ini_.setBool("Controls", "InvertCameraX", false);
            saved("Defaults");
            refresh();
        }));
        c.addView(defaults, fullWidth(18));
        return c;
    }

    // ── Controls ─────────────────────────────────────────────────────────

    private View controlsPage() {
        LinearLayout c = column();
        LinearLayout touch = card(c, "Touch controls");
        para(touch, "The on-screen pad: a move stick wherever your left thumb lands, the d-pad above it, A / B / X / Y "
            + "and Black / White on the right, the triggers in the top corners, Back and Start at the top. Drag on the "
            + "right half to turn the camera. Using a controller hides them; touch the screen to bring them back.");
        toggle(touch, "Touch controls", "Show the on-screen pad", "Android", "TouchControls", true);
        intChoice(touch, "Size", null, "Android", "TouchScale", new String[] {"Small", "Normal", "Large"},
            new int[] {80, 100, 120}, 100);
        intChoice(touch, "Opacity", null, "Android", "TouchOpacity", new String[] {"Faint", "Normal", "Strong"},
            new int[] {60, 100, 140}, 100);
        intChoice(touch, "Camera speed", "Dragging on the right half", "Android", "TouchCameraSpeed",
            new String[] {"Slow", "Normal", "Fast"}, new int[] {70, 100, 140}, 100);
        toggle(touch, "Vibrate on press", null, "Android", "TouchVibrate", true);
        Button edit = button("Edit the layout", kAccent);
        edit.setOnClickListener(v -> {
            if (!InstallActivity.installed()) {
                select("Install");
                return;
            }
            ini_.save();
            // the game, straight into the pad's edit mode (or EDIT on the pad while playing)
            startActivity(new Intent(this, GameActivity.class).putExtra("BUFFY_TOUCH_EDIT", "1"));
        });
        Button reset = button("Reset the layout", kBand);
        reset.setOnClickListener(v -> confirm("Put every touch control back where it started?", "Reset", () -> {
            ini_.set("Android", "TouchLayout", "");
            saved("Touch layout");
        }));
        touch.addView(pair(edit, reset), fullWidth(6));
        touch.addView(new View(this), new LinearLayout.LayoutParams(1, dp(10)));

        LinearLayout pads = card(c, "Controllers");
        para(pads, "Bluetooth and USB controllers work as Xbox controllers: the first is player 1, the next player 2 "
            + "(multiplayer and story co-op). The left and right bumpers are the Xbox's Black and White buttons.");
        toggle(pads, "Invert camera left / right", "The right stick", "Controls", "InvertCameraX", false);
        return c;
    }
}
