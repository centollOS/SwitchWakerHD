package org.wwhdrecomp.wwhd;

import android.app.Activity;
import android.content.ClipData;
import android.content.Intent;
import android.net.Uri;
import android.widget.Toast;
import androidx.core.content.FileProvider;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.ArrayList;

/** Share only the two diagnostic logs, already filtered by the native crash redactor.
 * Never grant access to the game, saves, or the captures directory itself. */
final class LogShare {
    private static final long LIMIT = (64L << 20) + 65536;
    static void share(Activity activity) {
        // Copy off the UI thread; a running game can keep appending after this snapshot.
        new Thread(() -> {
            try {
                Intent share = prepare(activity);
                activity.runOnUiThread(() -> {
                    if (share == null) {
                        Toast.makeText(activity, "No game log yet. Start the game once, then share its log.", Toast.LENGTH_LONG).show();
                        return;
                    }
                    try { activity.startActivity(Intent.createChooser(share, "Share log")); }
                    catch (Exception error) { Toast.makeText(activity, "Cannot open the share sheet", Toast.LENGTH_LONG).show(); }
                });
            } catch (Exception error) {
                activity.runOnUiThread(() -> Toast.makeText(activity, "Cannot share game logs", Toast.LENGTH_LONG).show());
            }
        }, "share-game-log").start();
    }
    // Called on a worker thread. The whitelist is deliberately independent of directory contents.
    static synchronized Intent prepare(android.content.Context activity) throws java.io.IOException {
        File root = activity.getExternalFilesDir(null);
        if (root == null) throw new java.io.IOException("Game storage is unavailable");
        File folder = new File(activity.getCacheDir(), "crash-share");
        if (!folder.isDirectory() && !folder.mkdirs()) throw new java.io.IOException("Cannot create share folder");
        ArrayList<Uri> logs = new ArrayList<>();
        for (String name : new String[] {"wwhd.log", "wwhd-previous.log"}) {
            File source = new File(new File(root, "captures"), name);
            if (!source.isFile() || source.length() == 0) continue;
            File copy = new File(folder, name);
            try (FileInputStream in = new FileInputStream(source);
                 FileOutputStream out = new FileOutputStream(copy)) {
                long remaining = Math.min(source.length(), LIMIT);
                byte[] bytes = new byte[65536];
                while (remaining > 0) {
                    int count = in.read(bytes, 0, (int)Math.min(bytes.length, remaining));
                    if (count < 0) break;
                    out.write(bytes, 0, count);
                    remaining -= count;
                }
            }
            logs.add(FileProvider.getUriForFile(activity, activity.getPackageName() + ".crashlogs", copy));
        }
        if (logs.isEmpty()) return null;
        Intent share = new Intent(Intent.ACTION_SEND_MULTIPLE).setType("text/plain")
            .putParcelableArrayListExtra(Intent.EXTRA_STREAM, logs)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        ClipData clip = ClipData.newRawUri("Game log", logs.get(0));
        for (int i = 1; i < logs.size(); ++i) clip.addItem(new ClipData.Item(logs.get(i)));
        share.setClipData(clip);
        return share;
    }
}
