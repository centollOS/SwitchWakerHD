package org.wwhdrecomp.wwhd;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Host-testable decisions shared by key access and picker recovery. */
public final class SetupKeyRecoveryPolicy {
    private SetupKeyRecoveryPolicy() { }
    private static final String[] KEYS = {"disc", "common"};
    private static final Pattern ENTRY = Pattern.compile("(?m)^\\s*[0-9a-fA-F]{32}(?=$|\\s|#|;|//)");
    private static final int LIMIT = 1024 * 1024;
    public static String other(String key) { return key.equals("disc") ? "common" : "disc"; }

    public static String[] lostKeys(String key, String uri, String otherUri) {
        return uri != null && !uri.isEmpty() && uri.equals(otherUri) ?
            new String[] {key, other(key)} : new String[] {key};
    }

    public static String[] replacementKeys(String key, boolean multiKey, String oldUri, String otherUri,
                                           boolean selectedLost, boolean otherLost) {
        boolean sameLost = selectedLost && oldUri != null && !oldUri.isEmpty() && oldUri.equals(otherUri);
        return multiKey && (otherUri == null || otherUri.isEmpty() || sameLost || otherLost) ?
            new String[] {key, other(key)} : new String[] {key};
    }

    public static boolean isMultiKeyDocument(InputStream input) throws IOException {
        ByteArrayOutputStream data = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        int count;
        while ((count = input.read(buffer, 0, Math.min(buffer.length, LIMIT + 1 - data.size()))) != -1) {
            data.write(buffer, 0, count);
            if (data.size() > LIMIT) return false;
        }
        String text = new String(data.toByteArray(), StandardCharsets.UTF_8);
        if (text.startsWith("\uFEFF")) text = text.substring(1);
        Matcher entries = ENTRY.matcher(text);
        return entries.find() && entries.find();
    }

    public interface Opener<T> { T open(String key) throws Exception; }

    /** Check both roles even if one is inaccessible; close successful descriptors on any failure. */
    public static <T extends AutoCloseable> Map<String, T> openBoth(Opener<T> opener) throws Exception {
        Map<String, T> opened = new LinkedHashMap<>();
        IOException first = null;
        boolean complete = false;
        try {
            for (String key : KEYS) {
                try { opened.put(key, opener.open(key)); }
                catch (IOException failure) { if (first == null) first = failure; }
            }
            if (first != null) throw first;
            complete = true;
            return opened;
        } finally {
            if (!complete) for (T descriptor : opened.values()) {
                try { descriptor.close(); } catch (Exception ignored) { }
            }
        }
    }
}
