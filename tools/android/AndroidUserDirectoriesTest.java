package org.wwhdrecomp.wwhd;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

/** Existing settings/controls remain in place; no Android/device dependency. */
public final class AndroidUserDirectoriesTest {
    public static void main(String[] args) throws Exception {
        Path external = Files.createTempDirectory("wwhd-external-user-");
        try {
            Path oldConfig = external.resolve("config/wwhd");
            Files.createDirectories(oldConfig);
            Path settings = oldConfig.resolve("settings.ini");
            Path controls = oldConfig.resolve("controls.json");
            Files.write(settings, "synthetic settings\n".getBytes(StandardCharsets.UTF_8));
            Files.write(controls, "synthetic controls\n".getBytes(StandardCharsets.UTF_8));
            File selected = AndroidUserDirectories.config(external.toFile());
            if (!selected.toPath().equals(oldConfig)) throw new AssertionError("config dir changed");
            if (!AndroidUserDirectories.shaderPrebuild(external.toFile()).toPath()
                    .equals(oldConfig.resolve("shader-prebuild"))) throw new AssertionError("shader root diverged");
            if (!new String(Files.readAllBytes(selected.toPath().resolve("settings.ini")), StandardCharsets.UTF_8)
                    .equals("synthetic settings\n")) throw new AssertionError("settings lost");
            if (!new String(Files.readAllBytes(selected.toPath().resolve("controls.json")), StandardCharsets.UTF_8)
                    .equals("synthetic controls\n")) throw new AssertionError("controls lost");
            if (Files.exists(external.resolve("user"))) throw new AssertionError("created replacement user dir");
        } finally {
            try (java.util.stream.Stream<Path> paths = Files.walk(external)) {
                paths.sorted(java.util.Comparator.reverseOrder()).forEach(path -> {
                    try { Files.delete(path); } catch (Exception e) { throw new RuntimeException(e); }
                });
            }
        }
    }
}
