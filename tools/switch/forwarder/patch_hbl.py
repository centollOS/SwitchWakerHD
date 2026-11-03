#!/usr/bin/env python3
"""Point nx-hbloader's hbl.json at the forwarder's title ID, as an application.

    patch_hbl.py HBL.json TITLE_ID

Edits the file in place: name wwhd_fwd, the title ID (with or without 0x) as title_id and as its min/max
range, and application_type 1 (application: the application's memory) instead of 2 (applet), and cores 0-3
(highest_cpu_id 3; hbl.json: 0-2).
Shared by build_forwarder.sh and build_forwarder_windows.py. Standard library only.
"""
import json
import re
import sys

if len(sys.argv) != 3:
    sys.exit("usage: patch_hbl.py HBL.json TITLE_ID")
path = sys.argv[1]
tid = "0x" + re.sub(r"^0[xX]", "", sys.argv[2]).lower()
with open(path) as f:
    d = json.load(f)
d.update(name="wwhd_fwd", title_id=tid, title_id_range_min=tid, title_id_range_max=tid)
for cap in d["kernel_capabilities"]:
    if cap["type"] == "application_type":
        cap["value"] = 1  # application (hbl.json: 2, applet)
    if cap["type"] == "kernel_flags":
        cap["value"]["highest_cpu_id"] = 3  # cores 0-3 (hbl.json: 0-2): core 3 for background work (cpu-threading-plan 0.5)
with open(path, "w") as f:
    json.dump(d, f, indent=4)
