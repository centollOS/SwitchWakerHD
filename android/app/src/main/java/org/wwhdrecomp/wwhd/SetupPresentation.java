package org.wwhdrecomp.wwhd;

/** Shared wording for the setup screen and its foreground notification. */
public final class SetupPresentation {
    private SetupPresentation() {}
    public static final String READING_SHADERS = "Reading shaders from your game files…";
    public static final String UPDATING = "Updating the game for the new version…";
    public static String stageLabel(String stage) {
        return "shaders".equals(stage) ? READING_SHADERS : stage;
    }
    public static boolean showUpdate(boolean updating, String state) {
        return updating && !"complete".equals(state) && !"failed".equals(state);
    }
}
