package org.wwhdrecomp.wwhd;

/** The file name a picked document's copy gets (PickedDocuments): its display name, made safe as one
 *  file name. The name matters to the mod installer: it tells a ZIP package from a replacement .pack. */
final class PickedDocumentNames {
    private PickedDocumentNames() {}

    static final String FALLBACK = "picked-mod.zip";

    /** displayName: OpenableColumns.DISPLAY_NAME, else null; lastSegment: the URI's last path segment. */
    static String fileName(String displayName, String lastSegment) {
        String name = clean(displayName);
        if (name.isEmpty()) {
            // a document ID such as "primary:Download/My Mod.zip" or "msf:42": its last part
            String segment = lastSegment == null ? "" : lastSegment;
            int cut = Math.max(Math.max(segment.lastIndexOf('/'), segment.lastIndexOf(':')), segment.lastIndexOf('\\'));
            name = clean(segment.substring(cut + 1));
        }
        return name.isEmpty() ? FALLBACK : name;
    }

    private static String clean(String raw) {
        if (raw == null) return "";
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < raw.length(); i++) {
            char c = raw.charAt(i);
            out.append(c == '/' || c == '\\' || c == ':' || c < 0x20 || c == 0x7f ? '_' : c);
        }
        String name = out.toString().trim();
        while (name.startsWith(".")) name = name.substring(1);  // no hidden files, never "." or ".."
        if (name.length() > 120) {  // keep the extension
            int dot = name.lastIndexOf('.');
            String ext = dot > 0 && name.length() - dot <= 16 ? name.substring(dot) : "";
            name = name.substring(0, 120 - ext.length()) + ext;
        }
        return name.trim();
    }
}
