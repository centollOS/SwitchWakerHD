package org.wwhdrecomp.wwhd;

/**
 * Stateful thresholds avoid repeatedly restarting work near a sensor limit.
 *
 * Heat: pause at Android thermal status SEVERE, at a battery temperature of 45.0 °C, or at
 * 42.0 °C while Android also reports MODERATE or higher; resume at LIGHT or lower and at most
 * 41.0 °C. The user may continue anyway (heat override), which skips that pause for the rest of
 * the setup run. Android thermal status CRITICAL or higher always pauses: the phone throttles hard
 * or shuts down at that level whatever setup does.
 */
public final class SetupPolicy {
    public static final int THERMAL_LIGHT = 1, THERMAL_MODERATE = 2, THERMAL_SEVERE = 3, THERMAL_CRITICAL = 4;
    private boolean hot;
    private boolean low;

    /** Readable PowerManager thermal status (none ... shutdown), "n/a" when unknown. */
    public static String thermalName(int status) {
        String[] names = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
        return status >= 0 && status < names.length ? names[status] : "n/a";
    }

    /** "43.1 °C" from BatteryManager tenths; -1 and Integer.MIN_VALUE mean no reading. */
    public static String batteryTemperature(int tenths) {
        if (tenths == -1 || tenths == Integer.MIN_VALUE) return "n/a";
        return String.format(java.util.Locale.ROOT, "%.1f °C", tenths / 10f);
    }

    public static String readings(int thermal, int temperatureTenths) {
        return "battery " + batteryTemperature(temperatureTenths) + ", thermal status: " + thermalName(thermal);
    }

    public static String description(String reason) {
        if ("manual".equals(reason)) return "Paused by you";
        if ("heat".equals(reason)) return "Waiting for the Android device to cool down";
        if ("critical".equals(reason)) return "Paused because Android reports the device as critically hot";
        if ("battery".equals(reason)) return "Waiting for charging or at least 30% battery";
        if ("battery_unknown".equals(reason)) return "Waiting for battery information";
        return "Paused at a safe checkpoint";
    }

    /** The pause text with the current sensor readings for heat pauses. */
    public static String description(String reason, int thermal, int temperatureTenths) {
        if ("heat".equals(reason))
            return description(reason) + " (" + readings(thermal, temperatureTenths) + ")";
        if ("critical".equals(reason))
            return description(reason) + " (" + readings(thermal, temperatureTenths) + "). " +
                "Setup continues once it cools down. Continue anyway does not apply here: " +
                "at this level the Android device throttles hard or shuts down regardless.";
        return description(reason);
    }

    /** Whether "Continue anyway" can skip this pause reason. */
    public static boolean overridable(String reason) {
        return "heat".equals(reason) || "battery".equals(reason) || "battery_unknown".equals(reason);
    }

    /**
     * Compiler processes to run at once: about two cores each (the game keeps the phone busy
     * otherwise too), about 512 MB of free memory each (a compiler peaked near 260 MB on a test
     * phone) with 1 GB kept free, one on low-memory phones or when the phone is already warm.
     */
    public static int compileJobs(int cores, long availableBytes, boolean lowRam, int thermal) {
        if (lowRam || thermal >= 2) return 1;
        long byMemory = (availableBytes - (1L << 30)) / (512L << 20);
        return (int) Math.max(1, Math.min(4, Math.min(cores / 2, byMemory)));
    }

    /** "Compiling: 30/80 · about 12 min left" from a compile progress event, or null. */
    public static String compileProgress(String state, int compiled, int reused, int total, long etaSeconds) {
        if (total <= 0) return null;
        if ("complete".equals(state)) return "Compiling: done (" + total + "/" + total + ")";
        String text = "Compiling: " + Math.min(total, compiled + reused) + "/" + total;
        if (etaSeconds >= 0) {
            long minutes = (etaSeconds + 59) / 60;
            text += minutes <= 1 ? " · about a minute left" : " · about " + minutes + " min left";
        }
        return text;
    }

    public String update(int thermal, int temperatureTenths, int batteryPercent,
                         boolean charging, boolean manual) {
        return update(thermal, temperatureTenths, batteryPercent, charging, manual, false, false);
    }

    /**
     * The pause reason, or null to run. heatOverride and batteryOverride are the user's
     * "Continue anyway" for this setup run; they never skip a manual pause or CRITICAL status.
     */
    public String update(int thermal, int temperatureTenths, int batteryPercent,
                         boolean charging, boolean manual, boolean heatOverride, boolean batteryOverride) {
        boolean known = temperatureTenths >= 0;
        if (thermal >= THERMAL_SEVERE || (known && temperatureTenths >= 450) ||
            (known && temperatureTenths >= 420 && thermal >= THERMAL_MODERATE)) hot = true;
        else if (thermal <= THERMAL_LIGHT && (!known || temperatureTenths <= 410)) hot = false;
        if (charging || batteryPercent >= 30) low = false;
        else if (batteryPercent >= 0 && batteryPercent <= 20) low = true;
        if (manual) return "manual";
        if (thermal >= THERMAL_CRITICAL) return "critical";
        if (hot && !heatOverride) return "heat";
        if (batteryOverride) return null;
        if (low) return "battery";
        if (!charging && batteryPercent < 0) return "battery_unknown";
        return null;
    }
}
