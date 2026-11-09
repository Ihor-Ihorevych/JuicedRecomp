package io.github.juiced;

import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.view.ViewGroup;
import android.widget.Button;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.view.DisplayCutout;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

/**
 * The game's controls on the screen, one button per action it binds (its input/input.XBO), each drawn as an icon from
 * SVG path data: Jump (A), Fight (B: attack, drop) and Use (X: use, move an object, the oars), HUD (Y), Sprint (right trigger), Sniff
 * (left trigger), Select and Start, and a camera reset (right stick click). There are no sticks on the screen until a
 * finger lands: a touch on the left half moves (left stick), a touch on the right half turns the camera (right stick),
 * each stick centred where the finger came down. A button at the top left opens the settings (the resolution), and
 * under it four arrows are the controller's D-pad, which the mini-games and menus use (hidden unless the settings show
 * them). The settings can also move and resize every button (startEditing); the places are saved as screen fractions.
 * Every finger is tracked on its own, and one that slides from a button to another presses the one under it.
 * The state goes to the game's input code through nativeSetPad. While a physical gamepad is connected only the settings
 * button stays, unless the settings force the controls (some phones report a built-in sensor, such as uinput-goodix,
 * as a gamepad).
 */
public class TouchControls extends View implements InputManager.InputDeviceListener {
    static native void nativeSetPad(int buttons, int a, int b, int x, int y, int black, int white, int lt, int rt,
                                    int lx, int ly, int rx, int ry);

    // the console controller's button bits, as input::Pad::buttons
    private static final int DPAD_UP = 1, DPAD_DOWN = 2, DPAD_LEFT = 4, DPAD_RIGHT = 8, START = 16, BACK = 32, R3 = 128;
    // input::Pad::analog indices
    private static final int A = 0, B = 1, X = 2, Y = 3, BLACK = 4, WHITE = 5, LT = 6, RT = 7;

    // ---- icons: SVG path data in a 24 x 24 box ------------------------------------------------------------------

    /** An icon: a filled path and/or a stroked one. */
    private static final class Icon {
        final Path fill, stroke;
        final boolean evenOdd;
        final float strokeWidth;
        Icon(String fillData, String strokeData, float strokeWidth, boolean evenOdd) {
            this.fill = fillData == null ? null : SvgPath.parse(fillData);
            this.stroke = strokeData == null ? null : SvgPath.parse(strokeData);
            this.strokeWidth = strokeWidth;
            this.evenOdd = evenOdd;
            if (fill != null && evenOdd) fill.setFillType(Path.FillType.EVEN_ODD);
        }
    }

    private static Icon filled(String d) { return new Icon(d, null, 0, false); }

    // an up arrow
    private static final String JUMP = "M12 3 L20 12 H15 V20 H9 V12 H4 Z";
    // a paw: the pad and four toes
    private static final String ACTION =
        "M12 12.5 C8.5 12.5 6 15.2 6 17.6 C6 19.6 7.6 20.5 9.4 20.5 C10.4 20.5 11.2 20.1 12 20.1 "
        + "C12.8 20.1 13.6 20.5 14.6 20.5 C16.4 20.5 18 19.6 18 17.6 C18 15.2 15.5 12.5 12 12.5 Z "
        + SvgPath.circle(5.5f, 10.5f, 2.0f) + SvgPath.circle(9.0f, 6.5f, 2.1f) + SvgPath.circle(15.0f, 6.5f, 2.1f)
        + SvgPath.circle(18.5f, 10.5f, 2.0f);
    // a hand: grab, use, move
    private static final String HAND =
        "M6 11 V6.5 H8.5 V11 Z M9 11 V4 H11.5 V11 Z M12 11 V4.5 H14.5 V11 Z M15 11 V6.5 H17.5 V11 Z "
        + "M5 10.5 H18.5 V15.5 Q18.5 21 12 21 Q5 21 5 15.5 Z";
    // a screen with lines of text in it
    private static final String HUD =
        "M3 5 H21 V19 H3 Z M5.5 7.5 V16.5 H18.5 V7.5 Z M7.5 10 H13 V11.8 H7.5 Z M7.5 13.2 H16 V15 H7.5 Z";
    // two chevrons: fast forward
    private static final String SPRINT =
        "M4 5 L11 12 L4 19 L7 19 L14 12 L7 5 Z M11 5 L18 12 L11 19 L14 19 L21 12 L14 5 Z";
    // three waves: a scent
    private static final String SNIFF =
        wave(7) + wave(12) + wave(17);
    // a list: the mission briefing
    private static final String SELECT =
        "M4 6 H6.5 V8.5 H4 Z M9 6 H20 V8.5 H9 Z M4 10.75 H6.5 V13.25 H4 Z M9 10.75 H20 V13.25 H9 Z "
        + "M4 15.5 H6.5 V18 H4 Z M9 15.5 H20 V18 H9 Z";
    // a play triangle: start
    private static final String START_ICON = "M7 4.5 V19.5 L19.5 12 Z";
    // crosshair: centre the camera
    private static final String CAMERA_STROKE =
        "M12 2.5 V6.5 M12 17.5 V21.5 M2.5 12 H6.5 M17.5 12 H21.5 " + SvgPath.circle(12, 12, 5.2f);
    private static final String CAMERA_FILL = SvgPath.circle(12, 12, 1.6f);
    // the D-pad's four arrows (the mini-games and menus ask for them)
    private static final String ARROW_UP = "M12 5 L19.5 16 H4.5 Z";
    private static final String ARROW_DOWN = "M12 19 L4.5 8 H19.5 Z";
    private static final String ARROW_LEFT = "M5 12 L16 4.5 V19.5 Z";
    private static final String ARROW_RIGHT = "M19 12 L8 4.5 V19.5 Z";
    // sliders: the settings
    private static final String SETTINGS_STROKE = "M3.5 7 H20.5 M3.5 12 H20.5 M3.5 17 H20.5";
    private static final String SETTINGS_FILL =
        SvgPath.circle(9, 7, 2.4f) + SvgPath.circle(15, 12, 2.4f) + SvgPath.circle(8, 17, 2.4f);

    private static String wave(float y) {
        return "M3 " + y + " Q5.25 " + (y - 3.5f) + " 7.5 " + y + " Q9.75 " + (y + 3.5f) + " 12 " + y + " Q14.25 "
            + (y - 3.5f) + " 16.5 " + y + " Q18.75 " + (y + 3.5f) + " 21 " + y + " ";
    }

    // ---- controls -----------------------------------------------------------------------------------------------

    private static final class Control {
        final int bit;     // button bit, 0 for an analog button
        final int analog;  // analog index, -1 for a digital button
        int analog2 = -1;  // a second analog button pressed with it
        final int color;
        final Icon icon;
        float cx, cy, rx, ry;  // an ellipse (a circle when rx == ry), or a box
        boolean round = true;
        int finger = -1;
        String key;            // its name in the saved layout
        Control[] group;       // buttons that move and resize together (the D-pad), or null
        float drx, dry;        // the default half size, before the player's scale
        float scale = 1;       // the player's size for it
        String xboxLabel;  // the letters this button wears in the Xbox layout
        int xboxColor;
        void xbox(String label, int color) { xboxLabel = label; xboxColor = color; }
        Control(int bit, int analog, int color, Icon icon) {
            this.bit = bit;
            this.analog = analog;
            this.color = color;
            this.icon = icon;
        }
        boolean hit(float x, float y, float slop) {
            if (round) {
                float dx = (x - cx) / (rx * slop), dy = (y - cy) / (ry * slop);
                return dx * dx + dy * dy <= 1;
            }
            return Math.abs(x - cx) <= rx * slop && Math.abs(y - cy) <= ry * slop;
        }
    }

    /** A stick that exists while a finger is down: centred where it landed. */
    private static final class Stick {
        float cx, cy, kx, ky;
        int finger = -1;
    }

    private final Control jump = new Control(0, A, 0xFF4CAF50, filled(JUMP));
    // B: attack, drop and swing (a thrown spoon). X is the separate "use" button below
    private final Control attack = new Control(0, B, 0xFFE53935, filled(ACTION));
    // X alone: use, move an object, grab, and the oars in the boat. Sent apart from B, or the game takes B (attack) first
    private final Control use = new Control(0, X, 0xFF2196F3, filled(HAND));
    private final Control hud = new Control(0, Y, 0xFFFDD835, new Icon(HUD, null, 0, true));
    // the game's R and L are the triggers or the Black and White buttons: each button sends both, the other pair is unused
    private final Control sprint = new Control(0, RT, 0xFFFF9800, filled(SPRINT));
    private final Control sniff = new Control(0, LT, 0xFFAB47BC, new Icon(null, SNIFF, 2.0f, false));
    private final Control black = new Control(0, BLACK, 0xFF616161, filled(START_ICON));
    private final Control white = new Control(0, WHITE, 0xFF9E9E9E, filled(START_ICON));
    private final Control select = new Control(BACK, -1, 0xFFBDBDBD, filled(SELECT));
    private final Control start = new Control(START, -1, 0xFFBDBDBD, filled(START_ICON));
    private final Control camera = new Control(R3, -1, 0xFFBDBDBD, new Icon(CAMERA_FILL, CAMERA_STROKE, 1.8f, false));
    // opens the settings panel, never sent to the game
    private final Control settings = new Control(0, -1, 0xFF90A4AE, new Icon(SETTINGS_FILL, SETTINGS_STROKE, 1.8f, false));
    private final Control up = new Control(DPAD_UP, -1, 0xFF78909C, filled(ARROW_UP));
    private final Control down = new Control(DPAD_DOWN, -1, 0xFF78909C, filled(ARROW_DOWN));
    private final Control leftArrow = new Control(DPAD_LEFT, -1, 0xFF78909C, filled(ARROW_LEFT));
    private final Control rightArrow = new Control(DPAD_RIGHT, -1, 0xFF78909C, filled(ARROW_RIGHT));
    private final Control[] controls = {jump, attack, use, hud, sprint, sniff, black, white, select, start, camera, settings,
                                        up, down, leftArrow, rightArrow};
    // the saved layout's names, in the order of controls
    private static final String[] KEYS = {"jump", "attack", "use", "hud", "sprint", "sniff", "black", "white", "select", "start", "camera",
                                          "settings", "up", "down", "left", "right"};
    private boolean xboxLayout;  // letters (A B X Y, LT RT) on the buttons instead of the icons

    private final Stick left = new Stick(), right = new Stick();
    private float stickRadius;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG),
        iconPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean padPresent;
    private boolean forceTouch;  // the settings keep the controls even with a gamepad

    /** A gamepad plays and the touch controls step aside, all but the settings button. */
    private boolean gamepadMode() {
        return padPresent && !forceTouch;
    }

    public TouchControls(Context context) {
        super(context);
        jump.xbox("A", 0xFF4CAF50);
        attack.xbox("B", 0xFFE53935);
        use.xbox("X", 0xFF2196F3);
        hud.xbox("Y", 0xFFFDD835);
        sprint.xbox("RT", 0xFFFF9800);
        sniff.xbox("LT", 0xFFAB47BC);
        black.xbox("BLK", 0xFF616161);
        white.xbox("WHT", 0xFF9E9E9E);
        for (int i = 0; i < controls.length; i++) controls[i].key = KEYS[i];
        Control[] dpad = {up, down, leftArrow, rightArrow};
        for (Control c : dpad) c.group = dpad;
        stroke.setStyle(Paint.Style.STROKE);
        iconPaint.setStrokeCap(Paint.Cap.ROUND);
        iconPaint.setStrokeJoin(Paint.Join.ROUND);
        setFocusable(false);
        setFocusableInTouchMode(false);
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        InputManager im = (InputManager) getContext().getSystemService(Context.INPUT_SERVICE);
        im.registerInputDeviceListener(this, handler);
        applyPrefs();
        refreshPad();
    }

    @Override
    protected void onDetachedFromWindow() {
        InputManager im = (InputManager) getContext().getSystemService(Context.INPUT_SERVICE);
        im.unregisterInputDeviceListener(this);
        super.onDetachedFromWindow();
    }

    @Override public void onInputDeviceAdded(int id) { refreshPad(); }
    @Override public void onInputDeviceRemoved(int id) { refreshPad(); }
    @Override public void onInputDeviceChanged(int id) { refreshPad(); }

    /** A physical gamepad takes over: only the settings button stays, other touches reach the game's own surface. */
    private void refreshPad() {
        boolean present = false;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d != null && !d.isVirtual() && (d.getSources() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD)
                present = true;
        }
        padPresent = present;
        if (gamepadMode()) release();
        invalidate();
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        layoutControls();
    }

    /** The cutout (a camera in the side of the screen) is found after the view is attached: the controls move again. */
    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        layoutControls();
        invalidate();
        return super.onApplyWindowInsets(insets);
    }

    /** The default places, then the player's saved ones. Left alone while the layout is being edited. */
    private void layoutControls() {
        final int h = getHeight();
        if (h == 0) return;
        final float u = h / 100f;  // a hundredth of the height
        layoutToolbar(u);
        if (editing) return;
        defaultLayout(u);
        for (Control c : controls) {
            c.drx = c.rx;
            c.dry = c.ry;
            c.scale = 1;
        }
        applySavedLayout();
    }

    private void defaultLayout(float u) {
        // controls start inside the cutout, and a little in from the edge
        float inset = 0, insetLeft = 0;
        if (Build.VERSION.SDK_INT >= 28) {
            WindowInsets wi = getRootWindowInsets();
            DisplayCutout dc = wi == null ? null : wi.getDisplayCutout();
            if (dc != null) {
                inset = dc.getSafeInsetRight();
                insetLeft = dc.getSafeInsetLeft();
            }
        }
        final float w = getWidth() - inset;
        stickRadius = 16 * u;
        // top left: the settings
        box(settings, insetLeft + 9 * u, 8 * u, 6 * u, 5 * u);
        // under it, the D-pad: high enough that the lower left stays free for the move stick
        final float dx = insetLeft + 19 * u, dy = 38 * u, step = 10.5f * u, ar = 5.5f * u;
        place(up, dx, dy - step, ar);
        place(down, dx, dy + step, ar);
        place(leftArrow, dx - step, dy, ar);
        place(rightArrow, dx + step, dy, ar);
        // top right: Select and Start
        box(select, w - 28 * u, 8 * u, 6 * u, 5 * u);
        box(start, w - 14 * u, 8 * u, 6 * u, 5 * u);
        // under them: sniff
        box(black, w - 14 * u, 23 * u, 7 * u, 6 * u);
        box(white, w - 30 * u, 23 * u, 7 * u, 6 * u);
        place(sniff, w - 66 * u, 62 * u, 8 * u);
        // bottom right, within reach of the thumb: the four face buttons in a diamond like the controller's
        // (Jump below, Fight/drop right, Use/move left, HUD above), Sprint beside them
        place(jump, w - 24 * u, 85 * u, 8 * u);
        place(attack, w - 10 * u, 70 * u, 8 * u);
        place(use, w - 38 * u, 70 * u, 8 * u);
        place(hud, w - 24 * u, 55 * u, 6.5f * u);
        place(sprint, w - 56 * u, 88 * u, 9.5f * u);
        place(camera, w - 76 * u, 91 * u, 5 * u);
    }

    // ---- the player's layout ------------------------------------------------------------------------------------

    private static final float MIN_SCALE = 0.5f, MAX_SCALE = 2.2f;
    private static final String LAYOUT = "layout_";  // + key + _x, _y (fractions of the view), _s (scale)

    private SharedPreferences prefs() {
        return getContext().getSharedPreferences(GameActivity.PREFS, Context.MODE_PRIVATE);
    }

    private void applySavedLayout() {
        SharedPreferences p = prefs();
        for (Control c : controls) {
            float x = p.getFloat(LAYOUT + c.key + "_x", -1), y = p.getFloat(LAYOUT + c.key + "_y", -1);
            c.scale = clampScale(p.getFloat(LAYOUT + c.key + "_s", 1));
            c.rx = c.drx * c.scale;
            c.ry = c.dry * c.scale;
            if (x >= 0 && y >= 0) {
                c.cx = x * getWidth();
                c.cy = y * getHeight();
            }
        }
    }

    private void saveLayout() {
        SharedPreferences.Editor e = prefs().edit();
        for (Control c : controls) {
            e.putFloat(LAYOUT + c.key + "_x", c.cx / getWidth());
            e.putFloat(LAYOUT + c.key + "_y", c.cy / getHeight());
            e.putFloat(LAYOUT + c.key + "_s", c.scale);
        }
        e.apply();
    }

    /** Back to the default places and sizes, and forgets the saved ones. */
    void resetLayout() {
        SharedPreferences.Editor e = prefs().edit();
        for (Control c : controls)
            for (String s : new String[] {"_x", "_y", "_s"}) e.remove(LAYOUT + c.key + s);
        e.apply();
        final boolean was = editing;
        editing = false;
        layoutControls();
        editing = was;
        invalidate();
    }

    private static float clampScale(float s) {
        return Math.max(MIN_SCALE, Math.min(MAX_SCALE, s));
    }

    // ---- editing the layout: drag a button to move it, pinch (or - and +) to resize it ---------------------------

    private boolean editing;
    private Control selected;
    private int dragFinger = -1, pinchFinger = -1;
    private float lastX, lastY, pinchStartDistance, pinchStartScale;
    private final RectF tbMinus = new RectF(), tbPlus = new RectF(), tbReset = new RectF(), tbCancel = new RectF(),
        tbDone = new RectF();
    private float hintY;

    void startEditing() {
        release();
        editing = true;
        selected = null;
        dragFinger = pinchFinger = -1;
        send();
        invalidate();
    }

    private void finishEditing(boolean keep) {
        editing = false;
        selected = null;
        dragFinger = pinchFinger = -1;
        if (keep) saveLayout();
        layoutControls();  // a cancel goes back to what was saved
        invalidate();
    }

    private void layoutToolbar(float u) {
        final float top = 3 * u, bottom = 12 * u, gap = 2 * u;
        final float[] widths = {10 * u, 10 * u, 18 * u, 18 * u, 18 * u};
        final RectF[] rects = {tbMinus, tbPlus, tbReset, tbCancel, tbDone};
        float total = gap * (widths.length - 1);
        for (float w : widths) total += w;
        float x = (getWidth() - total) / 2;
        for (int i = 0; i < rects.length; i++) {
            rects[i].set(x, top, x + widths[i], bottom);
            x += widths[i] + gap;
        }
        hintY = 17 * u;
    }

    private Control[] members(Control c) {
        return c.group != null ? c.group : new Control[] {c};
    }

    private Control editAt(float x, float y) {
        Control best = null;
        float bestDistance = Float.MAX_VALUE;
        for (Control c : controls) {
            if (!active(c) || !c.hit(x, y, 1.1f)) continue;
            float d = (x - c.cx) * (x - c.cx) + (y - c.cy) * (y - c.cy);
            if (d < bestDistance) {
                bestDistance = d;
                best = c;
            }
        }
        return best;
    }

    /** Moves the selected button (its whole group), kept inside the screen. */
    private void moveSelected(float dx, float dy) {
        Control[] ms = members(selected);
        float loX = -Float.MAX_VALUE, hiX = Float.MAX_VALUE, loY = -Float.MAX_VALUE, hiY = Float.MAX_VALUE;
        for (Control m : ms) {
            loX = Math.max(loX, m.rx - m.cx);
            hiX = Math.min(hiX, getWidth() - m.rx - m.cx);
            loY = Math.max(loY, m.ry - m.cy);
            hiY = Math.min(hiY, getHeight() - m.ry - m.cy);
        }
        if (loX <= hiX) dx = Math.max(loX, Math.min(hiX, dx));
        if (loY <= hiY) dy = Math.max(loY, Math.min(hiY, dy));
        for (Control m : ms) {
            m.cx += dx;
            m.cy += dy;
        }
    }

    /** Resizes the selected button (a group about its centre, the gaps with it). */
    private void scaleSelectedTo(float target) {
        if (selected == null) return;
        target = clampScale(target);
        final float f = target / selected.scale;
        Control[] ms = members(selected);
        float gx = 0, gy = 0;
        for (Control m : ms) {
            gx += m.cx / ms.length;
            gy += m.cy / ms.length;
        }
        for (Control m : ms) {
            m.scale = target;
            m.rx = m.drx * target;
            m.ry = m.dry * target;
            m.cx = gx + (m.cx - gx) * f;
            m.cy = gy + (m.cy - gy) * f;
        }
        moveSelected(0, 0);  // back inside the screen if it grew past the edge
    }

    private float pinchDistance(MotionEvent e) {
        int a = e.findPointerIndex(dragFinger), b = e.findPointerIndex(pinchFinger);
        if (a < 0 || b < 0) return pinchStartDistance;
        float dx = e.getX(a) - e.getX(b), dy = e.getY(a) - e.getY(b);
        return Math.max(1f, (float) Math.sqrt(dx * dx + dy * dy));
    }

    private void editTouch(MotionEvent e) {
        final int action = e.getActionMasked();
        final int index = e.getActionIndex();
        final int id = e.getPointerId(index);
        final float x = e.getX(index), y = e.getY(index);
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                if (dragFinger < 0) {
                    if (tbDone.contains(x, y)) { finishEditing(true); return; }
                    if (tbCancel.contains(x, y)) { finishEditing(false); return; }
                    if (tbReset.contains(x, y)) { resetLayout(); selected = null; break; }
                    if (tbMinus.contains(x, y)) { if (selected != null) scaleSelectedTo(selected.scale / 1.1f); break; }
                    if (tbPlus.contains(x, y)) { if (selected != null) scaleSelectedTo(selected.scale * 1.1f); break; }
                    Control c = editAt(x, y);
                    if (c != null) {
                        selected = c;
                        dragFinger = id;
                        lastX = x;
                        lastY = y;
                    }
                } else if (pinchFinger < 0 && selected != null) {
                    pinchFinger = id;
                    pinchStartDistance = pinchDistance(e);
                    pinchStartScale = selected.scale;
                }
                break;
            case MotionEvent.ACTION_MOVE: {
                int i = e.findPointerIndex(dragFinger);
                if (i < 0 || selected == null) break;
                if (pinchFinger >= 0) {
                    scaleSelectedTo(pinchStartScale * pinchDistance(e) / pinchStartDistance);
                } else {
                    moveSelected(e.getX(i) - lastX, e.getY(i) - lastY);
                }
                lastX = e.getX(i);
                lastY = e.getY(i);
                break;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                if (id == dragFinger) {
                    dragFinger = pinchFinger = -1;
                } else if (id == pinchFinger) {
                    pinchFinger = -1;
                }
                break;
            case MotionEvent.ACTION_CANCEL:
                dragFinger = pinchFinger = -1;
                break;
        }
        invalidate();
    }

    private void drawEditing(Canvas canvas) {
        canvas.drawColor(0x66000000);
        for (Control c : controls) if (active(c)) drawControl(canvas, c);
        if (selected != null) {
            stroke.setColor(0xFFE58A2E);
            stroke.setStrokeWidth(getHeight() * 0.008f);
            for (Control m : members(selected)) {
                if (m.round) {
                    canvas.drawCircle(m.cx, m.cy, m.rx * 1.12f, stroke);
                } else {
                    rect.set(m.cx - m.rx * 1.12f, m.cy - m.ry * 1.25f, m.cx + m.rx * 1.12f, m.cy + m.ry * 1.25f);
                    float r = Math.min(m.rx, m.ry);
                    canvas.drawRoundRect(rect, r, r, stroke);
                }
            }
        }
        final float u = getHeight() / 100f;
        drawToolButton(canvas, tbMinus, "−", selected != null, false);
        drawToolButton(canvas, tbPlus, "+", selected != null, false);
        drawToolButton(canvas, tbReset, "Reset", true, false);
        drawToolButton(canvas, tbCancel, "Cancel", true, false);
        drawToolButton(canvas, tbDone, "Done", true, true);
        iconPaint.setStyle(Paint.Style.FILL);
        iconPaint.setColor(0xFFF2EDE6);
        iconPaint.setTextAlign(Paint.Align.CENTER);
        iconPaint.setFakeBoldText(false);
        iconPaint.setTextSize(3.6f * u);
        String hint = selected == null ? "Touch a button to pick it, then drag it to move it"
                                       : "Drag to move. Pinch with a second finger, or use − and +, to resize";
        canvas.drawText(hint, getWidth() / 2f, hintY + 2 * u, iconPaint);
    }

    private void drawToolButton(Canvas canvas, RectF r, String label, boolean enabled, boolean accent) {
        final float radius = r.height() / 2;
        fill.setColor(accent ? 0xFFE58A2E : 0xE6262019);
        canvas.drawRoundRect(r, radius, radius, fill);
        stroke.setColor(0x88FFFFFF);
        stroke.setStrokeWidth(getHeight() * 0.003f);
        canvas.drawRoundRect(r, radius, radius, stroke);
        iconPaint.setStyle(Paint.Style.FILL);
        iconPaint.setColor(accent ? 0xFF1C1814 : (enabled ? 0xFFF2EDE6 : 0x66F2EDE6));
        iconPaint.setTextAlign(Paint.Align.CENTER);
        iconPaint.setFakeBoldText(true);
        iconPaint.setTextSize(r.height() * 0.48f);
        canvas.drawText(label, r.centerX(), r.centerY() - (iconPaint.descent() + iconPaint.ascent()) / 2, iconPaint);
    }

    private static void place(Control c, float cx, float cy, float r) {
        c.cx = cx; c.cy = cy; c.rx = r; c.ry = r; c.round = true;
    }

    private static void box(Control c, float cx, float cy, float hw, float hh) {
        c.cx = cx; c.cy = cy; c.rx = hw; c.ry = hh; c.round = false;
    }

    // ---- touch --------------------------------------------------------------------------------------------------

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (gamepadMode() && !editing) {
            // only the settings button answers, so the setting that forces the controls can always be reached
            if (e.getActionMasked() == MotionEvent.ACTION_DOWN && controlAt(e.getX(), e.getY()) == settings) {
                handler.post(this::showSettings);
                return true;
            }
            return false;
        }
        if (editing) {
            editTouch(e);
            return true;
        }
        final int action = e.getActionMasked();
        final int index = e.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                down(e.getPointerId(index), e.getX(index), e.getY(index));
                break;
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) move(e.getPointerId(i), e.getX(i), e.getY(i));
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                up(e.getPointerId(index));
                break;
            case MotionEvent.ACTION_CANCEL:
                release();
                break;
        }
        send();
        invalidate();
        return true;
    }

    private void down(int id, float x, float y) {
        Control c = controlAt(x, y);
        if (c == settings) {
            handler.post(this::showSettings);
            return;
        }
        if (!controlsShown) return;
        if (c != null) {
            if (c.finger < 0) c.finger = id;
            return;
        }
        // not on a button: the left half moves, the right half looks
        Stick s = x < getWidth() / 2f ? left : right;
        if (s.finger < 0) {
            s.finger = id;
            s.cx = x;
            s.cy = y;
            s.kx = s.ky = 0;
        }
    }

    private void move(int id, float x, float y) {
        for (Stick s : new Stick[] {left, right}) if (s.finger == id) moveStick(s, x, y);
        // a finger sliding over the buttons presses the one under it
        for (Control c : controls) {
            if (c.finger != id) continue;
            if (!c.hit(x, y, 1.15f)) {
                c.finger = -1;
                Control n = controlAt(x, y);
                if (n != null && n != settings && n.finger < 0) n.finger = id;
            }
        }
    }

    private void up(int id) {
        for (Stick s : new Stick[] {left, right}) {
            if (s.finger == id) {
                s.finger = -1;
                s.kx = s.ky = 0;
            }
        }
        for (Control c : controls) if (c.finger == id) c.finger = -1;
    }

    private void release() {
        for (Stick s : new Stick[] {left, right}) {
            s.finger = -1;
            s.kx = s.ky = 0;
        }
        for (Control c : controls) c.finger = -1;
        send();
        invalidate();
    }

    private Control controlAt(float x, float y) {
        Control best = null;
        float bestDistance = Float.MAX_VALUE;
        for (Control c : controls) {
            if (!active(c) || !c.hit(x, y, 1.15f)) continue;
            float d = (x - c.cx) * (x - c.cx) + (y - c.cy) * (y - c.cy);
            if (d < bestDistance) {
                bestDistance = d;
                best = c;
            }
        }
        return best;
    }

    private void moveStick(Stick s, float x, float y) {
        float dx = x - s.cx, dy = y - s.cy;
        float len = (float) Math.sqrt(dx * dx + dy * dy);
        if (len > stickRadius) {
            dx *= stickRadius / len;
            dy *= stickRadius / len;
        }
        s.kx = dx;
        s.ky = dy;
    }

    // ---- settings -----------------------------------------------------------------------------------------------

    private void showSettings() {
        new SettingsPanel(getContext(), this).show();
    }

    private float controlsAlpha = 1f;
    private boolean controlsShown = true;
    private boolean dpadShown = false;
    private boolean cameraStickShown = false;  // the right (camera) stick drawn under the finger

    /** A hidden arrow is neither drawn nor pressed: a touch there moves the stick instead. */
    private boolean active(Control c) {
        return dpadShown || (c != up && c != down && c != leftArrow && c != rightArrow);
    }

    /** Reads the saved opacity and visibility of the buttons (the panel changes them live). */
    void applyPrefs() {
        SharedPreferences prefs = getContext().getSharedPreferences(GameActivity.PREFS, Context.MODE_PRIVATE);
        controlsAlpha = Math.max(25, Math.min(100, prefs.getInt(GameActivity.CONTROLS_ALPHA, 100))) / 100f;
        controlsShown = prefs.getBoolean(GameActivity.CONTROLS_SHOWN, true);
        xboxLayout = prefs.getInt(GameActivity.CONTROLS_LAYOUT, 1) == 1;
        dpadShown = prefs.getBoolean(GameActivity.DPAD_SHOWN, true);
        cameraStickShown = prefs.getBoolean(GameActivity.CAMERA_STICK_SHOWN, false);
        forceTouch = prefs.getBoolean(GameActivity.FORCE_TOUCH, false);
        if (!controlsShown || !dpadShown || gamepadMode()) release();
        invalidate();
    }

    // ---- state to the game --------------------------------------------------------------------------------------

    private void send() {
        if (editing) {  // nothing reaches the game while the buttons are being moved
            nativeSetPad(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            return;
        }
        int buttons = 0;
        int[] analog = new int[8];
        for (Control c : controls) {
            if (c.finger < 0 || c == settings) continue;
            buttons |= c.bit;
            if (c.analog >= 0) analog[c.analog] = 255;
            if (c.analog2 >= 0) analog[c.analog2] = 255;
        }
        nativeSetPad(buttons, analog[A], analog[B], analog[X], analog[Y], analog[BLACK], analog[WHITE], analog[LT],
                     analog[RT], axis(left.kx), -axis(left.ky), axis(right.kx), -axis(right.ky));
    }

    private int axis(float v) {
        return Math.max(-32768, Math.min(32767, Math.round(v / stickRadius * 32767f)));
    }

    // ---- drawing ------------------------------------------------------------------------------------------------

    @Override
    protected void onDraw(Canvas canvas) {
        if (editing) {
            drawEditing(canvas);
            return;
        }
        if (gamepadMode()) {
            drawControl(canvas, settings);
            return;
        }
        drawStick(canvas, left);
        if (cameraStickShown) drawStick(canvas, right);  // it works either way; the settings choose whether it is drawn
        for (Control c : controls) {
            if ((controlsShown && active(c)) || c == settings) drawControl(canvas, c);
        }
    }

    /** Only while a finger holds it. */
    private void drawStick(Canvas canvas, Stick s) {
        if (s.finger < 0) return;
        fill.setColor(0x22FFFFFF);
        canvas.drawCircle(s.cx, s.cy, stickRadius, fill);
        stroke.setColor(0x88FFFFFF);
        stroke.setStrokeWidth(getHeight() * 0.004f);
        canvas.drawCircle(s.cx, s.cy, stickRadius, stroke);
        fill.setColor(0xAAFFFFFF);
        canvas.drawCircle(s.cx + s.kx, s.cy + s.ky, stickRadius * 0.45f, fill);
    }

    private void drawControl(Canvas canvas, Control c) {
        boolean on = c.finger >= 0;
        // the settings button always stays easy to find, and every button while the layout is edited
        final float scale = c == settings || editing ? 1f : controlsAlpha;
        int alpha = Math.round((on ? 0xCC : 0x55) * scale);
        final boolean letters = xboxLayout && c.xboxLabel != null;
        fill.setColor((alpha << 24) | ((letters ? c.xboxColor : c.color) & 0xFFFFFF));
        stroke.setColor(0xAAFFFFFF);
        stroke.setStrokeWidth(getHeight() * 0.004f);
        if (c.round) {
            canvas.drawCircle(c.cx, c.cy, c.rx, fill);
            canvas.drawCircle(c.cx, c.cy, c.rx, stroke);
        } else {
            rect.set(c.cx - c.rx, c.cy - c.ry, c.cx + c.rx, c.cy + c.ry);
            float r = Math.min(c.rx, c.ry);
            canvas.drawRoundRect(rect, r, r, fill);
            canvas.drawRoundRect(rect, r, r, stroke);
        }
        // the icon: its 24 x 24 box is a bit more than the button's half height across, centred
        final float side = Math.min(c.rx, c.ry) * 1.45f;
        final int color = (on ? 0xFF000000 : 0xF2FFFFFF) & 0xFFFFFF | (Math.round((on ? 0xFF : 0xF2) * scale) << 24);
        if (letters) {
            iconPaint.setStyle(Paint.Style.FILL);
            iconPaint.setColor(color);
            iconPaint.setTextAlign(Paint.Align.CENTER);
            iconPaint.setFakeBoldText(true);
            iconPaint.setTextSize(Math.min(c.rx, c.ry) * (c.xboxLabel.length() > 1 ? 0.95f : 1.2f));
            canvas.drawText(c.xboxLabel, c.cx, c.cy - (iconPaint.descent() + iconPaint.ascent()) / 2, iconPaint);
            return;
        }
        canvas.save();
        canvas.translate(c.cx - side / 2, c.cy - side / 2);
        canvas.scale(side / 24f, side / 24f);
        if (c.icon.fill != null) {
            iconPaint.setStyle(Paint.Style.FILL);
            iconPaint.setColor(color);
            canvas.drawPath(c.icon.fill, iconPaint);
        }
        if (c.icon.stroke != null) {
            iconPaint.setStyle(Paint.Style.STROKE);
            iconPaint.setStrokeWidth(c.icon.strokeWidth);
            iconPaint.setColor(color);
            canvas.drawPath(c.icon.stroke, iconPaint);
        }
        canvas.restore();
    }
}
