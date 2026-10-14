# Scripted comparison runs

Two small tools to check that two builds (or two settings) play the same game, e.g. the decomp build
against the normal build ([docs/decomp-build.md](../../docs/decomp-build.md)).

```sh
# one headless, muted run each (a copy of your save; the game boots, enters the game from the title
# screen, and plays the scenario: here, walking for 10 s of game time)
tools/gametest/run_scenario.sh build/normal/wwhd game save runs/normal 20 WWHD_TEST_STICK=2-12:0:1
tools/gametest/run_scenario.sh build/decomp/wwhd game save runs/decomp 20 WWHD_TEST_STICK=2-12:0:1
# step by step: Link, camera, random numbers, sound starts, every game object (memory hash), save data
ALIGN=link python3 tools/gametest/compare.py runs/normal runs/decomp 20
```

- `run_scenario.sh EXE GAME SAVE OUT END [VAR=value ...]`: the game runs with hidden windows, no
  audio and no host input, and is stopped when the script ends. Extra `VAR=value` arguments are passed
  to the game (scripted input `WWHD_TEST_STICK`, `WWHD_TEST_PRESS`, or the decomp build's debug
  switches). `OUT/perf.txt` has the process CPU time and wall time.
- `compare.py A B [END]`: aligned at the scenario origin (`ALIGN=link`: at Link's first update, which
  is robust against a different boot length). Exit status 0 when everything is identical.
