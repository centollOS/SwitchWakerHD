import org.wwhdrecomp.wwhd.SetupPolicy;

public final class SetupPolicyTest {
    private static void expect(String expected, String actual) {
        if (!java.util.Objects.equals(expected, actual))
            throw new AssertionError("Expected " + expected + ", got " + actual);
    }
    public static void main(String[] args) {
        SetupPolicy policy = new SetupPolicy();
        expect(null, policy.update(0, 300, 80, false, false));
        // Android thermal status SEVERE pauses; hysteresis holds until LIGHT and <= 41.0 °C
        expect("heat", policy.update(3, 300, 80, true, false));
        expect("heat", policy.update(2, 300, 80, true, false));
        expect("heat", policy.update(1, 415, 80, true, false));
        expect(null, policy.update(1, 410, 80, true, false));
        // battery temperature alone: 42.0-44.9 °C runs while Android reports none/light (issue #142)
        expect(null, policy.update(0, 431, 80, true, false));
        expect(null, policy.update(1, 449, 80, true, false));
        expect("heat", policy.update(0, 450, 80, true, false));
        expect("heat", policy.update(0, 420, 80, true, false));
        expect("heat", policy.update(2, 411, 80, true, false));
        expect(null, policy.update(0, 400, 80, true, false));
        // 42.0 °C together with MODERATE pauses; 41.9 °C with MODERATE does not
        expect(null, policy.update(2, 419, 80, true, false));
        expect("heat", policy.update(2, 420, 80, true, false));
        expect(null, policy.update(0, -1, 80, true, false));
        // battery level
        expect("battery", policy.update(0, 390, 20, false, false));
        expect("battery", policy.update(0, 300, 29, false, false));
        expect(null, policy.update(0, 300, 30, false, false));
        expect("manual", policy.update(3, 450, 10, false, true));
        expect("heat", policy.update(3, 450, 10, true, false));
        expect(null, policy.update(0, 300, 10, true, false));
        expect("battery_unknown", policy.update(0, -1, -1, false, false));
        expect(null, policy.update(0, -1, -1, true, false));

        // Continue anyway: heat override skips heat pauses, never CRITICAL or a manual pause
        SetupPolicy anyway = new SetupPolicy();
        expect("heat", anyway.update(3, 431, 80, true, false, false, false));
        expect(null, anyway.update(3, 431, 80, true, false, true, false));
        expect(null, anyway.update(0, 470, 80, true, false, true, false));
        expect("critical", anyway.update(4, 431, 80, true, false, true, false));
        expect("critical", anyway.update(6, 431, 80, true, false, true, true));
        expect("critical", anyway.update(4, 300, 80, true, false, false, false));
        expect("manual", anyway.update(4, 300, 80, true, true, true, true));
        expect(null, anyway.update(3, 431, 80, true, false, true, false));
        // the heat override does not skip battery pauses; the battery override does
        expect("battery", anyway.update(3, 431, 10, false, false, true, false));
        expect(null, anyway.update(3, 431, 10, false, false, true, true));
        expect("heat", anyway.update(3, 431, 10, false, false, false, true));
        expect(null, anyway.update(0, 300, -1, false, false, false, true));
        expect("battery_unknown", new SetupPolicy().update(0, 300, -1, false, false, false, false));
        // the hysteresis state keeps tracking while overridden
        expect(null, anyway.update(1, 300, 80, true, false, false, false));

        // texts
        expect("none", SetupPolicy.thermalName(0));
        expect("moderate", SetupPolicy.thermalName(2));
        expect("critical", SetupPolicy.thermalName(4));
        expect("shutdown", SetupPolicy.thermalName(6));
        expect("n/a", SetupPolicy.thermalName(-1));
        expect("n/a", SetupPolicy.thermalName(7));
        expect("43.1 °C", SetupPolicy.batteryTemperature(431));
        expect("n/a", SetupPolicy.batteryTemperature(-1));
        expect("n/a", SetupPolicy.batteryTemperature(Integer.MIN_VALUE));
        expect("Waiting for the Android device to cool down (battery 43.1 °C, thermal status: none)",
            SetupPolicy.description("heat", 0, 431));
        expect("Waiting for the Android device to cool down (battery n/a, thermal status: severe)",
            SetupPolicy.description("heat", 3, -1));
        String critical = SetupPolicy.description("critical", 4, 470);
        if (!critical.startsWith("Paused because Android reports the device as critically hot (battery 47.0 °C, thermal status: critical).") ||
            !critical.contains("Continue anyway does not apply"))
            throw new AssertionError(critical);
        expect("Waiting for charging or at least 30% battery", SetupPolicy.description("battery", 0, 300));
        expect("Paused by you", SetupPolicy.description("manual", 4, 470));
        if (!SetupPolicy.overridable("heat") || !SetupPolicy.overridable("battery") ||
            !SetupPolicy.overridable("battery_unknown") || SetupPolicy.overridable("critical") ||
            SetupPolicy.overridable("manual") || SetupPolicy.overridable(null))
            throw new AssertionError("overridable reasons");
        // compile workers: 8 cores and 10 GB free -> 4; memory and heat limit it
        if (SetupPolicy.compileJobs(8, 10L << 30, false, 0) != 4) throw new AssertionError("8 cores, 10 GB");
        if (SetupPolicy.compileJobs(8, (1L << 30) + (1100L << 20), false, 0) != 2) throw new AssertionError("2.1 GB free");
        if (SetupPolicy.compileJobs(8, 512L << 20, false, 0) != 1) throw new AssertionError("little memory");
        if (SetupPolicy.compileJobs(4, 10L << 30, false, 0) != 2) throw new AssertionError("4 cores");
        if (SetupPolicy.compileJobs(8, 10L << 30, true, 0) != 1) throw new AssertionError("low-RAM phone");
        if (SetupPolicy.compileJobs(8, 10L << 30, false, 2) != 1) throw new AssertionError("warm phone");
        expect("Compiling: 30/80 · about 12 min left", SetupPolicy.compileProgress("compiled", 22, 8, 80, 700));
        expect("Compiling: 3/80", SetupPolicy.compileProgress("compiled", 3, 0, 80, -1));
        expect("Compiling: 79/80 · about a minute left", SetupPolicy.compileProgress("compiled", 79, 0, 80, 20));
        expect("Compiling: done (80/80)", SetupPolicy.compileProgress("complete", 80, 0, 80, -1));
        expect(null, SetupPolicy.compileProgress("compiled", 1, 0, 0, -1));
        System.out.println("Setup policy checks passed");
    }
}
