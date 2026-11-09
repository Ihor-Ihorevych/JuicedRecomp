package io.github.juiced;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.View;

/** Draws one {@link Glyph} tinted, or the rat emblem. Sized by its layout; the icon fills the smaller side. */
final class IconView extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private Glyph glyph;
    private int color = 0xFFF2EDE6;
    private final boolean rat;
    private Path[] ratPaths;

    /** A tinted icon. */
    IconView(Context context, Glyph glyph, int color) {
        super(context);
        this.glyph = glyph;
        this.color = color;
        this.rat = false;
        paint.setStrokeCap(Paint.Cap.ROUND);
        paint.setStrokeJoin(Paint.Join.ROUND);
    }

    /** The rat emblem on its orange tile. */
    IconView(Context context) {
        super(context);
        this.rat = true;
        ratPaths = new Path[Icons.RAT.length];
        for (int i = 0; i < ratPaths.length; i++) ratPaths[i] = SvgPath.parse((String) Icons.RAT[i][0]);
        paint.setStrokeCap(Paint.Cap.ROUND);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        final float side = Math.min(getWidth(), getHeight());
        canvas.save();
        canvas.translate((getWidth() - side) / 2f, (getHeight() - side) / 2f);
        if (rat) {
            final float s = side / 108f;
            canvas.scale(s, s);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0xFFE58A2E);
            canvas.drawRoundRect(new RectF(0, 0, 108, 108), 24, 24, paint);
            paint.setColor(0xFFF2A24A);
            canvas.save();
            canvas.clipRect(0, 0, 108, 108);
            canvas.drawCircle(54, 104, 46, paint);
            canvas.restore();
            for (int i = 0; i < ratPaths.length; i++) {
                float width = (Float) Icons.RAT[i][2];
                paint.setColor((Integer) Icons.RAT[i][1]);
                paint.setStyle(width > 0 ? Paint.Style.STROKE : Paint.Style.FILL);
                paint.setStrokeWidth(width);
                canvas.drawPath(ratPaths[i], paint);
            }
        } else {
            final float s = side / 24f;
            canvas.scale(s, s);
            paint.setColor(color);
            if (glyph.fill != null) {
                paint.setStyle(Paint.Style.FILL);
                canvas.drawPath(glyph.fill, paint);
            }
            if (glyph.stroke != null) {
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(glyph.strokeWidth);
                canvas.drawPath(glyph.stroke, paint);
            }
        }
        canvas.restore();
    }
}
