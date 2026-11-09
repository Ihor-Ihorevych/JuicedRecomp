package io.github.juiced;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.TypedValue;
import android.view.MotionEvent;
import android.view.View;

/** A row of options in one rounded pill; the chosen one is filled with the accent colour. */
final class Segmented extends View {
    interface Listener {
        void onSelect(int index);
    }

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();
    private final String[] labels;
    private final float density;
    private int selected;
    private Listener listener;

    Segmented(Context context, String... labels) {
        super(context);
        this.labels = labels;
        this.density = context.getResources().getDisplayMetrics().density;
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setFakeBoldText(true);
        paint.setTextSize(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 14, getResources().getDisplayMetrics()));
    }

    void setListener(Listener l) {
        listener = l;
    }

    int getSelected() {
        return selected;
    }

    void setSelected(int index) {
        selected = Math.max(0, Math.min(labels.length - 1, index));
        invalidate();
    }

    @Override
    protected void onMeasure(int widthSpec, int heightSpec) {
        setMeasuredDimension(resolveSize(Math.round(110 * density * labels.length), widthSpec), Math.round(40 * density));
    }

    @Override
    protected void onDraw(Canvas canvas) {
        final float h = getHeight(), w = getWidth(), seg = w / labels.length, inset = 3 * density;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0xFF2E2822);
        rect.set(0, 0, w, h);
        canvas.drawRoundRect(rect, h / 2, h / 2, paint);
        paint.setColor(0xFFE58A2E);
        rect.set(selected * seg + inset, inset, (selected + 1) * seg - inset, h - inset);
        canvas.drawRoundRect(rect, h / 2, h / 2, paint);
        final float baseline = h / 2 - (paint.descent() + paint.ascent()) / 2;
        for (int i = 0; i < labels.length; i++) {
            paint.setColor(i == selected ? 0xFF1C1814 : 0xFFD8D0C6);
            canvas.drawText(labels[i], seg * (i + 0.5f), baseline, paint);
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (e.getActionMasked() == MotionEvent.ACTION_DOWN) return true;
        if (e.getActionMasked() == MotionEvent.ACTION_UP) {
            int index = Math.max(0, Math.min(labels.length - 1, (int) (e.getX() / (getWidth() / (float) labels.length))));
            if (index != selected) {
                selected = index;
                invalidate();
                if (listener != null) listener.onSelect(index);
            }
            return true;
        }
        return super.onTouchEvent(e);
    }
}
