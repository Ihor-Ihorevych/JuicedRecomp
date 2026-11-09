package io.github.juiced;

import android.graphics.Path;

/** A 24 x 24 icon from SVG path data: a filled path and/or a stroked one. */
final class Glyph {
    final Path fill, stroke;
    final float strokeWidth;

    Glyph(String fillData, String strokeData, float strokeWidth) {
        this.fill = fillData == null ? null : SvgPath.parse(fillData);
        this.stroke = strokeData == null ? null : SvgPath.parse(strokeData);
        this.strokeWidth = strokeWidth;
    }
}

/** The settings panel's icons, as SVG paths (the same format as android/icon/rat.svg). */
final class Icons {
    private Icons() {}

    static final Glyph CLOSE = new Glyph(null, "M6 6 L18 18 M18 6 L6 18", 2.2f);
    // a monitor
    static final Glyph GRAPHICS = new Glyph(null, "M3 5 H21 V16 H3 Z M9 20 H15 M12 16 V20", 1.8f);
    // four corners pointing outwards: the picture fills the screen
    static final Glyph SCREEN = new Glyph(null, "M4 9 V4 H9 M15 4 H20 V9 M20 15 V20 H15 M9 20 H4 V15", 2.0f);
    // a game pad
    static final Glyph CONTROLS = new Glyph(
        SvgPath.circle(16.2f, 11.2f, 1.1f) + SvgPath.circle(18.6f, 13.2f, 1.1f),
        "M7 8 H17 Q21 8 21 13 V15 Q21 17 19 17 Q17.5 17 16.5 15.5 L15.5 14.5 H8.5 L7.5 15.5 Q6.5 17 5 17 Q3 17 3 15 V13 "
            + "Q3 8 7 8 Z M8 10.5 V13.5 M6.5 12 H9.5", 1.6f);
    // a paper plane
    static final Glyph TELEGRAM = new Glyph("M21.5 3.5 L2.5 10.8 L8.2 12.8 L10.5 19.5 L13.6 15.4 L18.4 19 Z", null, 0);
    // a branch with three nodes: the source code
    static final Glyph CODE = new Glyph(null,
        "M7 7.5 V16.5 M7 12 Q7 9 11 9 H15" + SvgPath.circle(7, 5.5f, 2) + SvgPath.circle(7, 18.5f, 2)
            + SvgPath.circle(17, 9, 2), 1.7f);
    // a check mark
    static final Glyph CHECK = new Glyph(null, "M5 12.5 L10 17.5 L19 7", 2.4f);

    /** The rat emblem's layers on its 108 x 108 tile: path data, colour, stroke width (0 fills). The same shapes as
     * android/res/drawable/ic_launcher_foreground.xml. */
    static final Object[][] RAT = {
        {SvgPath.circle(36, 38, 11) + SvgPath.circle(72, 38, 11), 0xFF8E8A99, 0f},
        {SvgPath.circle(36, 38, 6.5f) + SvgPath.circle(72, 38, 6.5f), 0xFFF4A9B8, 0f},
        {"M54,30 C72,30 80,44 78,58 C76,72 66,80 54,80 C42,80 32,72 30,58 C28,44 36,30 54,30 Z", 0xFFA7A3B3, 0f},
        {"M54,57 C62,57 68,63 66.5,70 C65,76 59,78.5 54,78.5 C49,78.5 43,76 41.5,70 C40,63 46,57 54,57 Z", 0xFFCFCCDA, 0f},
        {SvgPath.circle(54, 64, 3.2f), 0xFFE56F8C, 0f},
        {"M54,67.2 V70.5 M54,70.5 Q51.5,73.5 48.5,71.8 M54,70.5 Q56.5,73.5 59.5,71.8", 0xFF5A4E63, 1.2f},
        {SvgPath.circle(43, 50, 3.4f) + SvgPath.circle(65, 50, 3.4f), 0xFF2B2433, 0f},
        {SvgPath.circle(43.1f, 48.6f, 1.1f) + SvgPath.circle(65.1f, 48.6f, 1.1f), 0xFFFFFFFF, 0f},
        {"M44,68 L25,63.5 M44,71 L25,73.5 M64,68 L83,63.5 M64,71 L83,73.5", 0xFFFFFFFF, 1.1f},
    };
}
