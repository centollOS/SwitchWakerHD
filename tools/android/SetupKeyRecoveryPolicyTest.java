import org.wwhdrecomp.wwhd.SetupKeyRecoveryPolicy;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

public final class SetupKeyRecoveryPolicyTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    private static void keys(String[] actual, String... expected) {
        check(Arrays.equals(actual, expected), "Wrong affected key roles");
    }
    private static final class Descriptor implements AutoCloseable {
        boolean closed;
        @Override public void close() { closed = true; }
    }
    private static boolean multi(String text) throws IOException {
        return SetupKeyRecoveryPolicy.isMultiKeyDocument(new ByteArrayInputStream(text.getBytes(StandardCharsets.UTF_8)));
    }
    public static void main(String[] args) throws Exception {
        for (String key : new String[] {"disc", "common"}) {
            String other = SetupKeyRecoveryPolicy.other(key);
            keys(SetupKeyRecoveryPolicy.lostKeys(key, "shared-document", "shared-document"), key, other);
            keys(SetupKeyRecoveryPolicy.lostKeys(key, "disc-document", "common-document"), key);
            keys(SetupKeyRecoveryPolicy.lostKeys(key, "", ""), key);
            keys(SetupKeyRecoveryPolicy.lostKeys(key, null, null), key);
            // Shared lost document, even if only the selected role had recorded the failure.
            keys(SetupKeyRecoveryPolicy.replacementKeys(key, true, "lost", "lost", true, false), key, other);
            // Distinct lost documents: a keys.txt selection can supply both roles.
            keys(SetupKeyRecoveryPolicy.replacementKeys(key, true, "lost", "other-lost", true, true), key, other);
            keys(SetupKeyRecoveryPolicy.replacementKeys(key, true, "healthy", "other-lost", false, true), key, other);
            // An explicit healthy other key, or a single-key file, must not be overwritten.
            keys(SetupKeyRecoveryPolicy.replacementKeys(key, true, "lost", "healthy", true, false), key);
            keys(SetupKeyRecoveryPolicy.replacementKeys(key, true, "healthy", "healthy", false, false), key);
            keys(SetupKeyRecoveryPolicy.replacementKeys(key, false, "lost", "lost", true, true), key);
            // An absent other role is not evidence that a single-key file supplies it.
            for (String absent : new String[] {null, ""}) {
                keys(SetupKeyRecoveryPolicy.replacementKeys(key, false, "", absent, true, true), key);
                keys(SetupKeyRecoveryPolicy.replacementKeys(key, false, "first-selection", absent, false, true), key);
                keys(SetupKeyRecoveryPolicy.replacementKeys(key, true, "", absent, true, true), key, other);
            }
        }
        List<String> attempts = new ArrayList<>();
        IOException discFailure = new IOException("authored inaccessible disc document");
        try {
            SetupKeyRecoveryPolicy.<Descriptor>openBoth(key -> {
                attempts.add(key);
                throw key.equals("disc") ? discFailure : new IOException("authored inaccessible common document");
            });
            throw new AssertionError("Access failure was ignored");
        } catch (IOException failure) {
            check(failure == discFailure, "First actionable error was lost");
        }
        check(attempts.equals(Arrays.asList("disc", "common")), "Stopped at the first key failure");
        for (String failed : new String[] {"disc", "common"}) {
            Descriptor successful = new Descriptor();
            attempts.clear();
            try {
                SetupKeyRecoveryPolicy.openBoth(key -> {
                    attempts.add(key);
                    if (key.equals(failed)) throw new IOException("authored unavailable document");
                    return successful;
                });
                throw new AssertionError("Partial success was accepted");
            } catch (IOException expected) { }
            check(attempts.size() == 2 && successful.closed, "Failed key check leaked its other descriptor");
        }
        Map<String, Descriptor> opened = SetupKeyRecoveryPolicy.openBoth(key -> new Descriptor());
        check(opened.size() == 2, "Successful key pair was lost");
        for (Descriptor descriptor : opened.values()) {
            check(!descriptor.closed, "Successful descriptor closed before use");
            descriptor.close();
        }
        String one = "01010101010101010101010101010101", two = "02020202020202020202020202020202";
        check(multi("\uFEFF# Cemu\r\njunk\r\n" + one + "# comment\r\n" + two + " trailing comment\r\n"), "Cemu list not recognized");
        check(multi(one + " // comment\n" + two + "; comment"), "Inline comments not recognized");
        check(!multi(one + " # single key"), "Single-key file replaced both roles");
        for (String key : new String[] {"disc", "common"}) {
            keys(SetupKeyRecoveryPolicy.replacementKeys(key, multi(one), "", "", true, true), key);
            keys(SetupKeyRecoveryPolicy.replacementKeys(key, multi(one + "\n" + two), "", "", true, true),
                key, SetupKeyRecoveryPolicy.other(key));
            keys(SetupKeyRecoveryPolicy.replacementKeys(key,
                SetupKeyRecoveryPolicy.isMultiKeyDocument(new ByteArrayInputStream(new byte[16])),
                "", "", true, true), key);
        }
        check(!multi("junk\n" + one + "0\n" + two), "Malformed entry counted as a key");
        check(!multi(""), "Empty file counted as multi-key");
        check(!SetupKeyRecoveryPolicy.isMultiKeyDocument(new ByteArrayInputStream(new byte[16])), "Raw key counted as multi-key");
        byte[] oversized = new byte[1024 * 1024 + 1];
        check(!SetupKeyRecoveryPolicy.isMultiKeyDocument(new ByteArrayInputStream(oversized)), "Unbounded key file accepted");
        System.out.println("Shared key access, descriptor cleanup and multi-key reselection checks passed");
    }
}
