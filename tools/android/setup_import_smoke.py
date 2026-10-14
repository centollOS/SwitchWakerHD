#!/usr/bin/env python3
"""Exercise production SAF import with debug-only, authored pipe documents."""
import argparse
import json
from pathlib import Path
import adb_util
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial", default="emulator-5554")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prefix = [args.adb, "-s", args.serial]
    adb_util.wait_until_ready(prefix)

    def adb(*arguments, check=True):
        return adb_util.run(prefix + list(arguments), check=check, capture_output=True, text=True, timeout=30)

    if adb("shell", "getprop", "ro.kernel.qemu").stdout.strip() != "1":
        raise RuntimeError("This synthetic probe runs only on an emulator")
    package = "org.wwhdrecomp.wwhd"
    def probe(activity, result_path, phase=None):
        adb("shell", "run-as", package, "rm", "-f", result_path)
        command = ["shell", "am", "start", "--activity-clear-top", "-n", package + "/." + activity]
        if phase is not None:
            command += ["--es", "phase", phase]
        adb(*command)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            result = adb("shell", "run-as", package, "cat", result_path, check=False)
            if result.returncode == 0:
                value = json.loads(result.stdout)
                if not value.get("passed"):
                    raise RuntimeError(value.get("error", "Import checks failed"))
                return value
            time.sleep(0.2)
        raise RuntimeError("Timed out waiting for SAF import checks")

    value = probe("SetupImportSmokeActivity", "files/setup-import-smoke.json")
    value["key_recovery"] = []
    for phase in ("prepare", "restart", "revoked", "lost-restart", "reselect", "replacement-restart",
                  "moved", "lost-restart", "reselect", "legacy", "legacy-restart", "legacy-preconfiguration", "legacy-complete",
                  "missing-source", "missing-source-restart", "missing-source-reselect"):
        # Actual process death: no in-memory key or selection can survive this boundary.
        adb("shell", "am", "force-stop", package)
        result = probe("SetupKeyRecoverySmokeActivity", "files/setup-key-recovery-" + phase + ".json", phase)
        value["key_recovery"].append(result)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(value, indent=2) + "\n")
    print("SAF import and key recovery checks passed")


if __name__ == "__main__":
    main()
