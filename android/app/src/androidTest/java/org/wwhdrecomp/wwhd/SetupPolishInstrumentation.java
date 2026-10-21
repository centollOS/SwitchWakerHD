package org.wwhdrecomp.wwhd;

/** Device-side intent check without starting a GPU session or requiring game files. */
public final class SetupPolishInstrumentation extends android.app.Instrumentation {
    @Override public void onCreate(android.os.Bundle arguments) { super.onCreate(arguments); start(); }
    @Override public void onStart() {
        android.os.Bundle result = new android.os.Bundle();
        android.content.Intent intent = SetupService.notificationIntent(getTargetContext());
        if (intent.getComponent() == null ||
                !intent.getComponent().getClassName().equals(SetupActivity.class.getName()) ||
                !intent.getBooleanExtra(SetupActivity.EXTRA_SHOW_SETUP, false)) {
            result.putString("failure", "Notification must bring setup forward, including during shader warming");
            finish(android.app.Activity.RESULT_CANCELED, result);
            return;
        }
        try { checkLogShare(); }
        catch (Throwable error) {
            result.putString("failure", "Log sharing: " + error);
            finish(android.app.Activity.RESULT_CANCELED, result);
            return;
        }
        result.putString("result", "Setup notification and log sharing passed");
        finish(android.app.Activity.RESULT_OK, result);
    }
    private static void require(boolean ok, String message) {
        if (!ok) throw new AssertionError(message);
    }
    private void checkLogShare() throws Exception {
        android.content.Context target = getTargetContext();
        java.io.File root = new java.io.File(target.getCacheDir(), "share-test-source");
        java.io.File captures = new java.io.File(root, "captures");
        captures.mkdirs();
        android.content.Context fixture = new android.content.ContextWrapper(target) {
            @Override public java.io.File getExternalFilesDir(String type) { return root; }
        };
        java.io.File current = new java.io.File(captures, "wwhd.log");
        java.io.File previous = new java.io.File(captures, "wwhd-previous.log");
        current.delete(); previous.delete();
        require(LogShare.prepare(fixture) == null, "Missing logs must not open an empty share sheet");
        java.nio.file.Files.write(previous.toPath(), "previous diagnostic\n".getBytes(java.nio.charset.StandardCharsets.UTF_8));
        android.content.Intent one = LogShare.prepare(fixture);
        require(one.getClipData().getItemCount() == 1, "Previous-only run must be shareable");
        java.nio.file.Files.write(current.toPath(), "current diagnostic ~/game\n".getBytes(java.nio.charset.StandardCharsets.UTF_8));
        java.nio.file.Files.write(new java.io.File(captures, "save.dat").toPath(), "PRIVATE SAVE".getBytes(java.nio.charset.StandardCharsets.UTF_8));
        android.content.Intent both = LogShare.prepare(fixture);
        require(android.content.Intent.ACTION_SEND_MULTIPLE.equals(both.getAction()), "Multi-file share action");
        require("text/plain".equals(both.getType()), "Plain text logs");
        require((both.getFlags() & android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0, "Recipient read grant");
        java.util.ArrayList<android.net.Uri> streams = both.getParcelableArrayListExtra(android.content.Intent.EXTRA_STREAM);
        require(streams != null && streams.size() == 2 && both.getClipData().getItemCount() == 2, "Only the two named logs");
        java.nio.file.Files.write(current.toPath(), "changed after snapshot".getBytes(java.nio.charset.StandardCharsets.UTF_8));
        for (int i = 0; i < streams.size(); ++i) {
            require(streams.get(i).equals(both.getClipData().getItemAt(i).getUri()), "ClipData grant matches attachment");
            try (java.io.InputStream in = target.getContentResolver().openInputStream(streams.get(i))) {
                byte[] bytes = new byte[128];
                int count = in.read(bytes);
                String text = new String(bytes, 0, count, java.nio.charset.StandardCharsets.UTF_8);
                require(text.equals(i == 0 ? "current diagnostic ~/game\n" : "previous diagnostic\n"), "Snapshot survives source change; no save contents");
            }
        }
        for (java.io.File file : captures.listFiles()) file.delete();
        captures.delete(); root.delete();
    }

}
