package io.github.juiced;

import android.content.Context;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;

/**
 * The voices the game plays. The game's own files are English; a dub sits beside them in game/lang/<code>, at the game's
 * own paths (MUSIC/DIALOG/01.SXB, VIDEOS/NTSC/*.BIK), and the native side reads those in their place. The choice is
 * files/language.txt, which the native side reads when the game starts.
 */
final class Languages {
    static final String ENGLISH = "en", RUSSIAN = "ru";

    private Languages() {}

    private static File file(Context ctx) {
        return new File(ctx.getExternalFilesDir(null), "language.txt");
    }

    static File gameDir(Context ctx) {
        return new File(ctx.getExternalFilesDir(null), "game");
    }

    /** Whether the game folder carries this dub. */
    static boolean available(Context ctx, String code) {
        return ENGLISH.equals(code) || new File(gameDir(ctx), "lang/" + code).isDirectory();
    }

    /** The chosen code, or null when nothing was chosen yet. */
    static String chosen(Context ctx) {
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(file(ctx)), "UTF-8"))) {
            String s = r.readLine();
            return s == null || s.trim().isEmpty() ? null : s.trim();
        } catch (IOException e) {
            return null;
        }
    }

    /** The chosen code, English when nothing was chosen. */
    static String current(Context ctx) {
        String c = chosen(ctx);
        return c == null ? ENGLISH : c;
    }

    static void choose(Context ctx, String code) {
        try (FileOutputStream os = new FileOutputStream(file(ctx))) {
            os.write(code.getBytes("UTF-8"));
        } catch (IOException e) {
            // the next start asks again
        }
    }
}
