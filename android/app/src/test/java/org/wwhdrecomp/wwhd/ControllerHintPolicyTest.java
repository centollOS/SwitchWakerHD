package org.wwhdrecomp.wwhd;
public final class ControllerHintPolicyTest {
    public static void main(String[] args) {
        ControllerHintPolicy fresh = new ControllerHintPolicy(false);
        assert !fresh.claim(false);
        assert fresh.claim(true);
        assert !fresh.claim(true);
        assert !fresh.claim(false);
        assert !fresh.claim(true); // reconnect
        ControllerHintPolicy restarted = new ControllerHintPolicy(true);
        assert !restarted.claim(false);
        assert !restarted.claim(true);
        System.out.println("Controller hint: first connection, reconnect, restart PASS");
    }
}
