package io.github.juiced;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.RelativeLayout;
import android.widget.TextView;

import org.libsdl.app.SDLActivity;

/** SDL3's activity: it loads libSDL3.so and libmain.so (the game) and runs SDL_main on its own thread. */
public class GameActivity extends SDLActivity {
    /** The resolution the picture is rendered at, as a percent of the screen's own pixels (100 is native). */
    static native void nativeSetResolution(int percent);

    /** The picture's shape (DISPLAY_FILL, DISPLAY_16_9, DISPLAY_4_3); the game reads it once, when it starts. */
    static native void nativeSetDisplayMode(int mode);

    /** Frames per second at most, 0 for no limit beyond the display's own pace. */
    static native void nativeSetFrameLimit(int fps);

    /** The frames per second the game presents, measured over the last half second. */
    static native float nativeGetFps();

    /** The renderer (RENDERER_AUTO, RENDERER_VULKAN, RENDERER_GLES) and the file of Auto's boot check; read when the game starts. */
    static native void nativeSetRenderer(int renderer, String marker);

    /** The renderer the game started: RENDERER_VULKAN or RENDERER_GLES, RENDERER_AUTO before it has started one. */
    static native int nativeGetActiveRenderer();

    static final int DISPLAY_FILL = 0, DISPLAY_16_9 = 1, DISPLAY_4_3 = 2;
    static final int FPS_MIN = 25, FPS_MAX = 144;
    static final int RENDERER_AUTO = 0, RENDERER_VULKAN = 1, RENDERER_GLES = 2;

    static final String PREFS = "juiced";
    static final String RESOLUTION = "resolution";          // percent of the screen's pixels
    static final String FILL = "fill";                      // boolean, default true: the old setting, read for DISPLAY_MODE
    static final String DISPLAY_MODE = "display_mode";      // int, DISPLAY_*: default from FILL, so fill
    static final String FPS_LIMIT = "fps_limit";            // int, FPS_MIN..FPS_MAX, default FPS_MAX
    static final String FPS_COUNTER = "fps_counter";        // boolean, default false
    static final String CONTROLS_ALPHA = "controls_alpha";  // percent, default 100
    static final String CONTROLS_LAYOUT = "controls_layout"; // int: 0 game icons, 1 Xbox letters
    static final String CONTROLS_SHOWN = "controls_shown";  // boolean, default true
    static final String DPAD_SHOWN = "dpad_shown";          // boolean, default false: the four arrows
    static final String CAMERA_STICK_SHOWN = "camera_stick_shown";  // boolean, default false: draw the right stick
    static final String FORCE_TOUCH = "force_touch";        // boolean, default false: keep the controls with a gamepad
    static final String RENDERER = "renderer";              // int, RENDERER_*, default RENDERER_AUTO
    /** Auto's boot check in the app's own files: a Vulkan start not yet confirmed, or Auto settled on OpenGL ES. */
    static final String RENDERER_MARKER = "renderer_check";

    /** The picture shape this run of the game started with: what the panel compares against to ask for a restart. */
    static int startedDisplayMode = DISPLAY_FILL;
    /** The renderer setting this run started with. */
    static int startedRenderer = RENDERER_AUTO;
    /** The voices this run started with (Languages): the native side read them at boot. */
    static String startedLanguage = Languages.ENGLISH;

    static int displayMode(SharedPreferences prefs) {
        return prefs.getInt(DISPLAY_MODE, prefs.getBoolean(FILL, true) ? DISPLAY_FILL : DISPLAY_16_9);
    }

    static int renderer(SharedPreferences prefs) {
        return Math.max(RENDERER_AUTO, Math.min(RENDERER_GLES, prefs.getInt(RENDERER, RENDERER_AUTO)));
    }

    /** Auto starts over from Vulkan: called when the player changes the renderer. */
    static void clearRendererCheck(Context ctx) {
        new java.io.File(ctx.getFilesDir(), RENDERER_MARKER).delete();
    }

    static int frameLimit(SharedPreferences prefs) {
        return Math.max(FPS_MIN, Math.min(FPS_MAX, prefs.getInt(FPS_LIMIT, FPS_MAX)));
    }

    // the frame rate counter: top centre, over everything, polled twice a second while it is shown
    private TextView fpsView;
    private final Handler fpsHandler = new Handler(Looper.getMainLooper());
    private final Runnable fpsTick = new Runnable() {
        @Override
        public void run() {
            if (fpsView == null || fpsView.getVisibility() != View.VISIBLE) return;
            try {
                fpsView.setText(Math.round(nativeGetFps()) + " FPS");
            } catch (UnsatisfiedLinkError e) {
                // the game's library is not loaded yet
            }
            fpsHandler.postDelayed(this, 500);
        }
    };

    /** Shows or hides the counter as the settings say. */
    void applyFpsCounter() {
        if (fpsView == null) return;
        boolean shown = getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(FPS_COUNTER, false);
        fpsHandler.removeCallbacks(fpsTick);
        fpsView.setVisibility(shown ? View.VISIBLE : View.GONE);
        if (shown) fpsTick.run();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // the picture reaches every edge, under a camera cutout too: the controls keep clear of it (TouchControls)
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode = Build.VERSION.SDK_INT >= 30
                ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
        super.onCreate(savedInstanceState);  // loads libmain.so
        // no game folder yet: back to the picker, and no controls on the screen
        if (!new java.io.File(new java.io.File(getExternalFilesDir(null), "game"), "default.xbe").isFile()) {
            startActivity(new android.content.Intent(this, SetupActivity.class));
            finish();
            return;
        }
        // the saved resolution is in force before the game's first frame
        SharedPreferences prefs = getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        nativeSetResolution(prefs.getInt(RESOLUTION, 100));
        startedDisplayMode = displayMode(prefs);
        startedLanguage = Languages.current(this);
        nativeSetDisplayMode(startedDisplayMode);
        nativeSetFrameLimit(frameLimit(prefs));
        startedRenderer = renderer(prefs);
        nativeSetRenderer(startedRenderer, new java.io.File(getFilesDir(), RENDERER_MARKER).getAbsolutePath());
        // the on-screen controller goes on top of the game's surface
        mLayout.addView(new TouchControls(this),
                new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        // and the frame rate counter over it, which takes no touches
        fpsView = new TextView(this);
        fpsView.setTextColor(0xFFFFFFFF);
        fpsView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        fpsView.getPaint().setFakeBoldText(true);
        fpsView.setShadowLayer(3, 0, 1, 0xFF000000);
        fpsView.setBackgroundColor(0x66000000);
        final int pad = Math.round(4 * getResources().getDisplayMetrics().density);
        fpsView.setPadding(pad * 2, pad, pad * 2, pad);
        fpsView.setClickable(false);
        fpsView.setFocusable(false);
        RelativeLayout.LayoutParams fp = new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                                                                         ViewGroup.LayoutParams.WRAP_CONTENT);
        fp.addRule(RelativeLayout.ALIGN_PARENT_TOP);
        fp.addRule(RelativeLayout.CENTER_HORIZONTAL);
        fp.topMargin = pad;
        mLayout.addView(fpsView, fp);
        applyFpsCounter();
    }

    /** The app is in the background (true) or back in front: the sound stops and starts with it. */
    static native void nativeSetBackground(boolean background);

    @Override
    protected void onPause() {
        try { nativeSetBackground(true); } catch (UnsatisfiedLinkError e) { /* the game's library is not loaded yet */ }
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        try { nativeSetBackground(false); } catch (UnsatisfiedLinkError e) { /* the game's library is not loaded yet */ }
    }
}
