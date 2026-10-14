package org.wwhdrecomp.wwhd;

import java.io.File;

/** Same default as native XDG_CONFIG_HOME=<SDL external files>/config, then /wwhd. */
public final class AndroidUserDirectories {
    private AndroidUserDirectories() { }

    public static File config(File externalFiles) {
        if (externalFiles == null) throw new IllegalStateException("External app files unavailable");
        return new File(externalFiles, "config/wwhd");
    }

    public static File shaderPrebuild(File externalFiles) {
        return new File(config(externalFiles), "shader-prebuild");
    }
}
