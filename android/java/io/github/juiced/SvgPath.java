package io.github.juiced;

import android.graphics.Path;

/**
 * SVG path data ("M12 3 L20 12 H4 Z") as an android.graphics.Path, so the controls' icons are written as SVG paths.
 * Supports M L H V C Q Z and their lower case (relative) forms; arcs are left out, the icons do not use them.
 */
final class SvgPath {
    private final String d;
    private int i;

    private SvgPath(String d) {
        this.d = d;
    }

    static Path parse(String data) {
        return new SvgPath(data).run();
    }

    /** A circle as path data: four cubic curves. */
    static String circle(float cx, float cy, float r) {
        float k = 0.5523f * r;
        return "M" + (cx - r) + " " + cy
            + " C" + (cx - r) + " " + (cy - k) + " " + (cx - k) + " " + (cy - r) + " " + cx + " " + (cy - r)
            + " C" + (cx + k) + " " + (cy - r) + " " + (cx + r) + " " + (cy - k) + " " + (cx + r) + " " + cy
            + " C" + (cx + r) + " " + (cy + k) + " " + (cx + k) + " " + (cy + r) + " " + cx + " " + (cy + r)
            + " C" + (cx - k) + " " + (cy + r) + " " + (cx - r) + " " + (cy + k) + " " + (cx - r) + " " + cy + " Z ";
    }

    private Path run() {
        Path p = new Path();
        float x = 0, y = 0, startX = 0, startY = 0;
        char cmd = 0;
        float[] a = new float[6];
        while (true) {
            skipSeparators();
            if (i >= d.length()) break;
            char ch = d.charAt(i);
            if (Character.isLetter(ch)) {
                cmd = ch;
                i++;
                if (cmd == 'Z' || cmd == 'z') {
                    p.close();
                    x = startX;
                    y = startY;
                    continue;
                }
            }
            boolean rel = Character.isLowerCase(cmd);
            switch (Character.toUpperCase(cmd)) {
                case 'M':
                    read(a, 2);
                    x = rel ? x + a[0] : a[0];
                    y = rel ? y + a[1] : a[1];
                    p.moveTo(x, y);
                    startX = x;
                    startY = y;
                    cmd = rel ? 'l' : 'L';  // more pairs after a move are lines
                    break;
                case 'L':
                    read(a, 2);
                    x = rel ? x + a[0] : a[0];
                    y = rel ? y + a[1] : a[1];
                    p.lineTo(x, y);
                    break;
                case 'H':
                    read(a, 1);
                    x = rel ? x + a[0] : a[0];
                    p.lineTo(x, y);
                    break;
                case 'V':
                    read(a, 1);
                    y = rel ? y + a[0] : a[0];
                    p.lineTo(x, y);
                    break;
                case 'C':
                    read(a, 6);
                    p.cubicTo(rel ? x + a[0] : a[0], rel ? y + a[1] : a[1], rel ? x + a[2] : a[2], rel ? y + a[3] : a[3],
                              rel ? x + a[4] : a[4], rel ? y + a[5] : a[5]);
                    x = rel ? x + a[4] : a[4];
                    y = rel ? y + a[5] : a[5];
                    break;
                case 'Q':
                    read(a, 4);
                    p.quadTo(rel ? x + a[0] : a[0], rel ? y + a[1] : a[1], rel ? x + a[2] : a[2], rel ? y + a[3] : a[3]);
                    x = rel ? x + a[2] : a[2];
                    y = rel ? y + a[3] : a[3];
                    break;
                default:
                    throw new IllegalArgumentException("unsupported path command " + cmd + " in " + d);
            }
        }
        return p;
    }

    private void skipSeparators() {
        while (i < d.length() && (d.charAt(i) == ' ' || d.charAt(i) == ',' || d.charAt(i) == '\n' || d.charAt(i) == '\t')) i++;
    }

    private void read(float[] out, int count) {
        for (int k = 0; k < count; k++) {
            skipSeparators();
            int start = i;
            if (i < d.length() && (d.charAt(i) == '-' || d.charAt(i) == '+')) i++;
            boolean dot = false;
            while (i < d.length()) {
                char c = d.charAt(i);
                if (c >= '0' && c <= '9') {
                    i++;
                } else if (c == '.' && !dot) {
                    dot = true;
                    i++;
                } else {
                    break;
                }
            }
            if (start == i) throw new IllegalArgumentException("a number was expected at " + start + " in " + d);
            out[k] = Float.parseFloat(d.substring(start, i));
        }
    }
}
