package io.github.juiced;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

/** A slider drawn on a canvas: a rounded track filled up to the thumb, in the panel's accent colour. */
final class FancySlider extends View {
    interface Listener {
        /** The value moved (every step of a drag). */
        void onChange(int value);

        /** The finger lifted: the value to keep. */
        void onStop(int value);
    }

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();
    private final float density;
    private final int min, max, step;
    private int value;
    private Listener listener;

    FancySlider(Context context, int min, int max, int step) {
        super(context);
        this.min = min;
        this.max = max;
        this.step = step;
        this.value = max;
        this.density = context.getResources().getDisplayMetrics().density;
    }

    void setListener(Listener l) {
        listener = l;
    }

    int getValue() {
        return value;
    }

    void setValue(int v) {
        value = Math.max(min, Math.min(max, v));
        invalidate();
    }

    @Override
    protected void onMeasure(int widthSpec, int heightSpec) {
        setMeasuredDimension(resolveSize(Math.round(200 * density), widthSpec), Math.round(40 * density));
    }

    private float thumbRadius() {
        return 11 * density;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        final float pad = thumbRadius() + 2 * density;
        final float left = pad, right = getWidth() - pad, cy = getHeight() / 2f, half = 3 * density;
        final float x = left + (right - left) * (value - min) / (float) (max - min);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0xFF3A322A);
        rect.set(left, cy - half, right, cy + half);
        canvas.drawRoundRect(rect, half, half, paint);
        paint.setColor(0xFFE58A2E);
        rect.set(left, cy - half, x, cy + half);
        canvas.drawRoundRect(rect, half, half, paint);
        paint.setColor(0xFFF2EDE6);
        canvas.drawCircle(x, cy, thumbRadius(), paint);
        paint.setColor(0xFFE58A2E);
        canvas.drawCircle(x, cy, thumbRadius() * 0.42f, paint);
    }

    private void moveTo(float touchX) {
        final float pad = thumbRadius() + 2 * density;
        final float fraction = Math.max(0f, Math.min(1f, (touchX - pad) / Math.max(1f, getWidth() - 2 * pad)));
        final int v = min + Math.round(fraction * (max - min) / step) * step;
        if (v != value) {
            value = v;
            invalidate();
            if (listener != null) listener.onChange(value);
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                getParent().requestDisallowInterceptTouchEvent(true);  // a drag here is not a scroll of the panel
                moveTo(e.getX());
                return true;
            case MotionEvent.ACTION_MOVE:
                moveTo(e.getX());
                return true;
            case MotionEvent.ACTION_UP:
                moveTo(e.getX());
                if (listener != null) listener.onStop(value);
                return true;
            case MotionEvent.ACTION_CANCEL:
                if (listener != null) listener.onStop(value);
                return true;
            default:
                return super.onTouchEvent(e);
        }
    }
}
