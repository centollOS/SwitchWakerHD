package org.wwhdrecomp.wwhd;

import android.content.Context;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import java.io.File;
import java.io.IOException;
import org.json.JSONObject;

/** Read-only key access, with durable recovery hints and safe legacy-job handling. */
public final class SetupKeyAccess {
    private SetupKeyAccess() { }
    public static final class Missing extends IOException {
        Missing(String key) { super((key.equals("disc") ? "Disc" : "Common") +
            " key: choose the key again. Access to the selected file was lost."); }
    }
    static File legacyFile(File job, JSONObject input, String key) throws Exception {
        String path = input.optString(key + "_key");
        if (path.isEmpty()) return null;
        if (!path.equals("input/" + key + ".key")) throw new IOException("Invalid private key destination");
        File file = new File(job, path);
        if (!file.getCanonicalPath().equals(new File(job.getCanonicalFile(), path).getPath()))
            throw new IOException("Invalid private key destination");
        return file.isFile() ? file : null;
    }
    public static ParcelFileDescriptor open(Context context, File job, JSONObject configuration, String key) throws Exception {
        JSONObject input = configuration.getJSONObject("input");
        File legacy = legacyFile(job, input, key);
        if (legacy != null) {
            // Interrupted old releases must not depend on a grant that may since have expired.
            return ParcelFileDescriptor.open(legacy, ParcelFileDescriptor.MODE_READ_ONLY);
        }
        File sourceFile = new File(job, "source.json");
        JSONObject source = sourceFile.isFile() ? SetupStore.read(sourceFile) : new JSONObject();
        String uri = source.optString(key + "_uri");
        try {
            if (uri.isEmpty()) throw new IOException("No key selection");
            ParcelFileDescriptor descriptor = context.getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
            if (descriptor == null) throw new IOException("No key descriptor");
            clearFailure(job, key);
            return descriptor;
        } catch (SecurityException | IOException | IllegalArgumentException lost) {
            File record = new File(job, "key-access.json");
            JSONObject value = record.isFile() ? SetupStore.read(record) : new JSONObject().put("schema", 1);
            for (String lostKey : SetupKeyRecoveryPolicy.lostKeys(key, uri,
                    source.optString(SetupKeyRecoveryPolicy.other(key) + "_uri"))) {
                if (legacyFile(job, input, lostKey) == null) value.put(lostKey, true);
            }
            SetupStore.write(record, value);
            // Provider diagnostics may include private URIs; expose only the role and recovery action.
            throw new Missing(key);
        }
    }
    public static boolean failed(File job, String key) throws Exception {
        File record = new File(job, "key-access.json");
        return record.isFile() && SetupStore.read(record).optBoolean(key);
    }
    public static void clearFailure(File job, String key) throws Exception {
        clearFailures(job, new String[] {key});
    }
    public static void clearFailures(File job, String[] keys) throws Exception {
        File record = new File(job, "key-access.json");
        if (record.isFile()) {
            JSONObject value = SetupStore.read(record);
            for (String key : keys) value.remove(key);
            SetupStore.write(record, value);
        }
    }
    public static void cleanupCompleted(File job) throws Exception {
        File host = new File(job, "host.json");
        if (!host.isFile() || !SetupStore.read(host).optString("state").equals("complete")) return;
        File configuration = new File(job, "job.json");
        JSONObject value = SetupStore.read(configuration);
        JSONObject input = value.getJSONObject("input");
        for (String key : new String[] {"disc", "common"}) input.remove(key + "_key");
        SetupStore.write(configuration, value);
        for (String key : new String[] {"disc", "common"}) {
            SetupStore.remove(new File(job, "input/" + key + ".key"));
            SetupStore.remove(new File(job, "import-partial/" + key));
        }
    }
}
