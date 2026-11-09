package io.github.juiced;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.BufferedInputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FilterInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * The launcher. The game is files/game in the app's own storage, where the native side looks. Three ways it gets there:
 * - a selfpack APK carries it in assets/game (listed in assets/game.list, "size TAB path" per line): unpacked on the first start;
 * - the light APK asks for juiced-game.zip (unpacked once) or the folder the game's files are in (default.xbe and its
 *   folders, e.g. from a disc image, copied once), with the system pickers;
 * - the files are already there (adb push): nothing to do.
 * A game that carries a dub (game/lang/ru) asks for the voices the first time it starts (Languages).
 */
public class SetupActivity extends Activity {
    private static final int PICK_FOLDER = 1, PICK_ZIP = 2;
    private static final String XBE = "default.xbe";

    private File game;
    private LinearLayout box;
    private TextView status;
    private ProgressBar bar;
    private Button choose, chooseZip;
    private long lastUi;

    /** One file to copy: where it goes (relative), what it is called in the picked tree, its size. */
    private static final class Entry {
        final String path, docId;
        final long size;
        Entry(String path, String docId, long size) {
            this.path = path;
            this.docId = docId;
            this.size = size;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        game = new File(getExternalFilesDir(null), "game");
        buildUi();

        final AssetManager assets = getAssets();
        final List<String> names = new ArrayList<>();
        long total = 0;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(assets.open("game.list"), "UTF-8"))) {
            for (String line; (line = r.readLine()) != null; ) {
                int tab = line.indexOf('\t');
                if (tab <= 0) continue;
                total += Long.parseLong(line.substring(0, tab));
                names.add(line.substring(tab + 1));
            }
        } catch (IOException | NumberFormatException e) {
            names.clear();  // no list: the light APK
        }

        if (!names.isEmpty()) {
            final String stamp = names.size() + ":" + total;
            if (stamp.equals(read(new File(game, ".unpacked")))) {
                launch();
            } else {
                unpackAssets(assets, names, total, stamp);
            }
            return;
        }
        if (new File(game, XBE).isFile()) {
            launch();
            return;
        }
        askForFolder();
    }

    // ---- ui -----------------------------------------------------------------------------------------------------

    private void buildUi() {
        box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER);
        box.setBackgroundColor(0xFF15110F);
        int pad = Math.round(32 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad, pad, pad);
        status = new TextView(this);
        status.setTextColor(0xFFEEEEEE);
        status.setTextSize(18);
        status.setGravity(Gravity.CENTER);
        box.addView(status, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                          ViewGroup.LayoutParams.WRAP_CONTENT));
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setVisibility(View.GONE);
        box.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                       ViewGroup.LayoutParams.WRAP_CONTENT));
        choose = new Button(this);
        choose.setText("Select the game folder");
        choose.setVisibility(View.GONE);
        choose.setOnClickListener(v -> openPicker());
        box.addView(choose, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                                                          ViewGroup.LayoutParams.WRAP_CONTENT));
        chooseZip = new Button(this);
        chooseZip.setText("Select the game zip");
        chooseZip.setVisibility(View.GONE);
        chooseZip.setOnClickListener(v -> openZipPicker());
        box.addView(chooseZip, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                                                             ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(box);
    }

    private void askForFolder() {
        bar.setVisibility(View.GONE);
        choose.setVisibility(View.VISIBLE);
        chooseZip.setVisibility(View.GONE);
        status.setText("Select the folder with the game's files (the one that holds " + XBE
            + ", from your extracted disc image).\nThey are copied into the app once.");
    }

    private void openPicker() {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER);
    }

    private void showProgress(final String text, final long done, final long total, boolean force) {
        long now = System.nanoTime();
        if (!force && now - lastUi < 100_000_000L) return;
        lastUi = now;
        runOnUiThread(() -> {
            bar.setVisibility(View.VISIBLE);
            bar.setProgress((int) (done * 1000 / Math.max(total, 1)));
            status.setText(text + "\n" + (done >> 20) + " / " + (total >> 20) + " MB");
        });
    }

    private void fail(final String message) {
        runOnUiThread(() -> {
            status.setText(message);
            bar.setVisibility(View.GONE);
            choose.setVisibility(View.VISIBLE);
            chooseZip.setVisibility(View.GONE);
        });
    }

    // ---- the selfpack's assets ----------------------------------------------------------------------------------

    private void unpackAssets(final AssetManager assets, final List<String> names, final long total, final String stamp) {
        status.setText("Unpacking the game (first start only)");
        new Thread(() -> {
            try {
                requireSpace(total);
                new File(game, ".unpacked").delete();
                byte[] buf = new byte[1 << 20];
                long done = 0;
                for (String name : names) {
                    File out = new File(game, name);
                    out.getParentFile().mkdirs();
                    try (InputStream in = assets.open("game/" + name, AssetManager.ACCESS_STREAMING);
                         FileOutputStream os = new FileOutputStream(out)) {
                        for (int n; (n = in.read(buf)) > 0; ) {
                            os.write(buf, 0, n);
                            done += n;
                            showProgress("Unpacking the game (first start only)", done, total, done == total);
                        }
                    }
                }
                try (FileOutputStream m = new FileOutputStream(new File(game, ".unpacked"))) {
                    m.write(stamp.getBytes("UTF-8"));
                }
                runOnUiThread(this::launch);
            } catch (IOException e) {
                fail("Could not unpack the game: " + e.getMessage());
            }
        }, "unpack").start();
    }

    // ---- the picked folder --------------------------------------------------------------------------------------

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_FOLDER && requestCode != PICK_ZIP) return;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            askForFolder();
            return;
        }
        if (requestCode == PICK_ZIP) {
            final Uri zip = data.getData();
            choose.setVisibility(View.GONE);
            chooseZip.setVisibility(View.GONE);
            status.setText("Opening the zip...");
            new Thread(() -> unzip(zip), "unzip").start();
            return;
        }
        final Uri tree = data.getData();
        choose.setVisibility(View.GONE);
        status.setText("Looking at the folder...");
        new Thread(() -> copyTree(tree), "copy").start();
    }

    private void copyTree(Uri tree) {
        try {
            ContentResolver cr = getContentResolver();
            List<Entry> all = new ArrayList<>();
            list(cr, tree, DocumentsContract.getTreeDocumentId(tree), "", all);
            // the game's root is the shallowest folder holding default.xbe: the picked one, or one inside it
            String prefix = null;
            for (Entry e : all) {
                String lower = e.path.toLowerCase();
                if (!lower.equals(XBE) && !lower.endsWith("/" + XBE)) continue;
                String dir = e.path.substring(0, e.path.length() - XBE.length());
                if (prefix == null || dir.length() < prefix.length()) prefix = dir;
            }
            if (prefix == null) {
                fail("There is no " + XBE + " in that folder (or the ones inside it).\nSelect the folder with the game's files.");
                return;
            }
            List<Entry> files = new ArrayList<>();
            long total = 0;
            for (Entry e : all) {
                if (!e.path.startsWith(prefix)) continue;
                files.add(new Entry(e.path.substring(prefix.length()), e.docId, e.size));
                total += e.size;
            }
            requireSpace(total);
            byte[] buf = new byte[1 << 20];
            long done = 0;
            for (Entry e : files) {
                File out = new File(game, e.path);
                out.getParentFile().mkdirs();
                try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, e.docId));
                     FileOutputStream os = new FileOutputStream(out)) {
                    if (in == null) throw new IOException("cannot read " + e.path);
                    for (int n; (n = in.read(buf)) > 0; ) {
                        os.write(buf, 0, n);
                        done += n;
                        showProgress("Copying the game (first start only)", done, total, done == total);
                    }
                }
            }
            runOnUiThread(this::launch);
        } catch (IOException | RuntimeException e) {
            fail("Could not copy the game: " + e.getMessage());
        }
    }

    /** Every file under a document, with its path below the picked folder. */
    private void list(ContentResolver cr, Uri tree, String docId, String path, List<Entry> out) throws IOException {
        if (out.size() > 50000) throw new IOException("that folder has too many files, pick the game's own folder");
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        final String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                                  DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                                  DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE};
        List<String[]> dirs = new ArrayList<>();
        try (Cursor c = cr.query(children, columns, null, null, null)) {
            if (c == null) throw new IOException("cannot read the folder");
            while (c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    dirs.add(new String[] {id, path + name + "/"});
                } else {
                    out.add(new Entry(path + name, id, c.isNull(3) ? 0 : c.getLong(3)));
                }
            }
        }
        for (String[] d : dirs) list(cr, tree, d[0], d[1], out);
    }

    // ---- shared -------------------------------------------------------------------------------------------------

    private void requireSpace(long total) throws IOException {
        game.mkdirs();  // the free space of a folder that is not there yet reads as zero
        if (game.getUsableSpace() < total + (64L << 20))
            throw new IOException("not enough free space: the game needs " + (total >> 20) + " MB");
    }

    private static String read(File f) {
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), "UTF-8"))) {
            return r.readLine();
        } catch (IOException e) {
            return null;
        }
    }

    /** To the game; the first time a game with a dub starts, the voices are asked for first. */
    private void launch() {
        if (Languages.chosen(this) == null && Languages.available(this, Languages.RUSSIAN)) {
            askForLanguage();
            return;
        }
        startActivity(new Intent(this, GameActivity.class));
        finish();
    }

    private void askForLanguage() {
        bar.setVisibility(View.GONE);
        choose.setVisibility(View.GONE);
        chooseZip.setVisibility(View.GONE);
        status.setText("Voices and cutscenes\n\nThe menus and subtitles stay English either way.\nYou can change this later in the settings.");
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        Button en = new Button(this);
        en.setText("English");
        Button ru = new Button(this);
        ru.setText("Русский (Russian)");
        en.setOnClickListener(v -> { Languages.choose(this, Languages.ENGLISH); launch(); });
        ru.setOnClickListener(v -> { Languages.choose(this, Languages.RUSSIAN); launch(); });
        row.addView(en);
        row.addView(ru);
        box.addView(row, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                                                       ViewGroup.LayoutParams.WRAP_CONTENT));
    }

    // ---- the game zip (juiced-game.zip: game/... with game/lang/<code>/... for the dubs) --------------------

    private void openZipPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed",
                                                         "application/octet-stream"});
        startActivityForResult(i, PICK_ZIP);
    }

    private void unzip(Uri uri) {
        try {
            ContentResolver cr = getContentResolver();
            long total = 0;
            try (Cursor c = cr.query(uri, new String[] {OpenableColumns.SIZE}, null, null, null)) {
                if (c != null && c.moveToFirst() && !c.isNull(0)) total = c.getLong(0);
            }
            requireSpace(total + (total >> 3));  // it is stored compressed, a little more lands on the disk
            final long size = total;
            final long[] read = {0};
            InputStream raw = cr.openInputStream(uri);
            if (raw == null) throw new IOException("cannot read the zip");
            // counts what is read from the zip itself, for the progress
            InputStream counted = new FilterInputStream(new BufferedInputStream(raw, 1 << 20)) {
                @Override public int read() throws IOException {
                    int b = super.read();
                    if (b >= 0) read[0]++;
                    return b;
                }
                @Override public int read(byte[] b, int off, int len) throws IOException {
                    int n = super.read(b, off, len);
                    if (n > 0) read[0] += n;
                    return n;
                }
            };
            boolean sawXbe = false;
            byte[] buf = new byte[1 << 20];
            try (ZipInputStream zin = new ZipInputStream(counted)) {
                for (ZipEntry e; (e = zin.getNextEntry()) != null; ) {
                    String name = e.getName().replace('\\', '/');
                    if (name.startsWith("game/")) name = name.substring(5);  // the zip's own top folder
                    if (name.isEmpty() || name.contains("..")) continue;
                    File out = new File(game, name);
                    if (e.isDirectory()) {
                        out.mkdirs();
                        continue;
                    }
                    if (name.equalsIgnoreCase(XBE)) sawXbe = true;
                    out.getParentFile().mkdirs();
                    try (FileOutputStream os = new FileOutputStream(out)) {
                        for (int n; (n = zin.read(buf)) > 0; ) {
                            os.write(buf, 0, n);
                            showProgress("Unpacking the game zip (first start only)", read[0], size, false);
                        }
                    }
                }
            }
            if (!sawXbe) {
                fail("That zip has no " + XBE + ". Select juiced-game.zip, or the game's folder.");
                return;
            }
            showProgress("Unpacking the game zip (first start only)", size, size, true);
            runOnUiThread(this::launch);
        } catch (IOException | RuntimeException e) {
            fail("Could not unpack the zip: " + e.getMessage());
        }
    }
}
