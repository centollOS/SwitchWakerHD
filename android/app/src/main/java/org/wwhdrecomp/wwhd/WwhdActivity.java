package org.wwhdrecomp.wwhd;

import android.os.Bundle;
import android.util.Log;
import android.view.Display;
import android.view.Window;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.File;

// The game: SDL loads libmain.so and runs its SDL_main (runtime/src/main.cpp).
public class WwhdActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // The game files go to Android/data/org.wwhdrecomp.wwhd/files/game (code, content, meta):
        // create the folders so they show up for copying over USB.
        File files = getExternalFilesDir(null);
        if (files != null) {
            new File(files, "game").mkdirs();
            new File(files, "save").mkdirs();
            Log.i("wwhd", "game folder: " + new File(files, "game"));
        }
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    // Frame interpolation at 120/240 fps (runtime/src/platform/display_rate.cpp, called from native
    // code on any thread). Phones with 90/120 Hz screens often run apps at 60 Hz unless the app asks
    // for more, so the game asks for a display mode of the same resolution with at least the
    // frame rate it draws, and reads back the rate the display actually runs at.

    // the display's current refresh rate (Hz; 0: no display)
    public static float displayRefreshRate() {
        Display d = mSingleton != null ? mSingleton.getDisplay() : null;
        return d != null ? d.getRefreshRate() : 0;
    }

    // the highest refresh rate the display offers at its current resolution
    public static float maxRefreshRate() {
        Display d = mSingleton != null ? mSingleton.getDisplay() : null;
        if (d == null) return 0;
        Display.Mode cur = d.getMode();
        float best = d.getRefreshRate();
        for (Display.Mode m : d.getSupportedModes())
            if (m.getPhysicalWidth() == cur.getPhysicalWidth() && m.getPhysicalHeight() == cur.getPhysicalHeight())
                best = Math.max(best, m.getRefreshRate());
        return best;
    }

    // ask for the display mode (same resolution) with the lowest refresh rate of at least hz, or the
    // highest one if none reaches it; hz <= 0 leaves the choice to the system again
    public static void requestRefreshRate(final float hz) {
        final SDLActivity a = mSingleton;
        if (a == null) return;
        a.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Window w = a.getWindow();
                Display d = a.getDisplay();
                if (w == null || d == null) return;
                int id = 0;
                if (hz > 0) {
                    Display.Mode cur = d.getMode(), pick = null, highest = null;
                    for (Display.Mode m : d.getSupportedModes()) {
                        if (m.getPhysicalWidth() != cur.getPhysicalWidth() || m.getPhysicalHeight() != cur.getPhysicalHeight()) continue;
                        if (highest == null || m.getRefreshRate() > highest.getRefreshRate()) highest = m;
                        if (m.getRefreshRate() >= hz - 1 && (pick == null || m.getRefreshRate() < pick.getRefreshRate())) pick = m;
                    }
                    if (pick == null) pick = highest;
                    if (pick != null) id = pick.getModeId();
                }
                WindowManager.LayoutParams lp = w.getAttributes();
                if (lp.preferredDisplayModeId == id) return;
                lp.preferredDisplayModeId = id;
                w.setAttributes(lp);
                Log.i("wwhd", "display mode " + id + " requested for " + hz + " fps");
            }
        });
    }
}
