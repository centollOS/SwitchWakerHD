import org.wwhdrecomp.wwhd.SetupPresentation;
public final class SetupPresentationTest {
    public static void main(String[] args) {
        if (!SetupPresentation.stageLabel("shaders").equals("Reading shaders from your game files…"))
            throw new AssertionError("Setup shader reading must differ from skippable preparation");
        if (!SetupPresentation.stageLabel("compile").equals("compile")) throw new AssertionError();
        for (String state : new String[]{"preparing", "running", "paused", "activating", "warming shaders", "complete", "failed"}) {
            if (SetupPresentation.showUpdate(false, state)) throw new AssertionError("First install");
            if (SetupPresentation.showUpdate(true, state) != (!state.equals("complete") && !state.equals("failed")))
                throw new AssertionError(state);
        }
    }
}
