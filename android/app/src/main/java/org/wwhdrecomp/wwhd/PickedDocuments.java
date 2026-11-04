package org.wwhdrecomp.wwhd;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/** Settings > Mods > More > Install from a file: Android's chooser answers with a content:// URI, which
 *  the game's file code can't open. The document is copied to the app's cache under its own name; the
 *  game installs the copy and removes it (runtime/src/mods/picked_source.h). */
final class PickedDocuments {
    private PickedDocuments() {}

    /** "ok:" + the copy's path, or "error:" + a message for the player. */
    static String copy(Context context, String uriText, long limit) {
        File folder = null;
        try {
            Uri uri = Uri.parse(uriText);
            if (!ContentResolver.SCHEME_CONTENT.equals(uri.getScheme())) return "error:This file can't be opened here";
            ContentResolver resolver = context.getContentResolver();
            String display = null;
            long size = -1;
            try (Cursor row = resolver.query(uri, new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE}, null, null, null)) {
                if (row != null && row.moveToFirst()) {
                    int name = row.getColumnIndex(OpenableColumns.DISPLAY_NAME), bytes = row.getColumnIndex(OpenableColumns.SIZE);
                    if (name >= 0 && !row.isNull(name)) display = row.getString(name);
                    if (bytes >= 0 && !row.isNull(bytes)) size = row.getLong(bytes);
                }
            } catch (RuntimeException ignored) { /* no metadata: the URI's own name, the size as copied */ }
            if (size > limit) return "error:" + tooLarge(limit);
            File picks = new File(context.getCacheDir(), "mod-picks");
            clear(picks);  // copies a closed or crashed game left behind (one install at a time)
            folder = new File(picks, Long.toString(System.nanoTime()));
            if (!folder.mkdirs()) return "error:Cannot make room for the file in the app's storage";
            File out = new File(folder, PickedDocumentNames.fileName(display, uri.getLastPathSegment()));
            long copied = 0;
            try (InputStream in = resolver.openInputStream(uri); OutputStream file = new FileOutputStream(out)) {
                if (in == null) throw new java.io.FileNotFoundException("no data");
                byte[] buffer = new byte[1 << 16];
                for (int n; (n = in.read(buffer)) > 0; ) {
                    copied += n;
                    if (copied > limit) { clear(folder); return "error:" + tooLarge(limit); }
                    file.write(buffer, 0, n);
                }
            }
            return "ok:" + out.getAbsolutePath();
        } catch (Exception failure) {
            Log.e("wwhd-game", "picked document not copied", failure);
            if (folder != null) clear(folder);
            return "error:Cannot read the chosen file: " + failure.getMessage();
        }
    }

    private static String tooLarge(long limit) {
        return "This file is too large for a mod package (over " + (limit >> 20) + " MB)";
    }

    private static void clear(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) clear(child);
        if (!file.delete() && file.exists()) Log.w("wwhd-game", "cannot remove " + file);
    }
}
