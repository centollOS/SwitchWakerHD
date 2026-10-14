package org.wwhdrecomp.wwhd;

// One claim per install; the caller persists the claim in SharedPreferences.
final class ControllerHintPolicy {
    private boolean shown;
    ControllerHintPolicy(boolean remembered) { shown = remembered; }
    boolean claim(boolean controllerConnected) {
        if (!controllerConnected || shown) return false;
        shown = true;
        return true;
    }
}
