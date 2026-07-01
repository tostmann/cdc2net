#!/usr/bin/env python3
# merge_manifest.py — add/update ONE chip's build into a per-MISSION manifest.
#
# A mission dir (webflasher/<mission>/) holds ONE manifest.json serving BOTH:
#   * esp-web-tools webflash — builds[] (one per chipFamily) → esp-web-tools
#     auto-selects the matching factory by the connected chip.
#   * in-device OTA — top-level "version" drives /api/update/check; ota.<chip>.path
#     is that chip's app image (each env's OTA_FIRMWARE_URL points at its own file,
#     so a C3 never pulls the C6 image).
#
# All chips of a mission MUST carry the SAME version (release with one tag) — the
# shared "version" is compared by every chip.  We hard-ABORT on a version clash so
# a half-updated mission dir can never ship inconsistent images.
#
# NFS-atomic write (os.replace over .tmp + dir fsync) per the repo's NFS rule.
import json, os, sys

def atomic_write(path, text):
    d = os.path.dirname(path) or "."
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text); f.flush(); os.fsync(f.fileno())
    os.replace(tmp, path)
    dfd = os.open(d, os.O_DIRECTORY)
    try: os.fsync(dfd)
    finally: os.close(dfd)

def main():
    mpath, name, version, chipfamily, factory, firmware, fw_md5 = sys.argv[1:8]
    if os.path.exists(mpath):
        m = json.load(open(mpath))
        if m.get("version") != version:
            sys.stderr.write(
                f"ABORT: mission manifest {mpath} is version '{m.get('version')}' but this "
                f"build is '{version}'.\n       All chips of a mission must be released with "
                f"the SAME version (one tag) — rebuild the whole mission with one tag.\n")
            sys.exit(3)
    else:
        m = {"name": name, "version": version, "funding_url": "https://busware.de",
             "new_install_prompt_erase": True, "builds": [], "ota": {}}
    m["name"] = name
    m["version"] = version
    m.setdefault("builds", [])
    m["builds"] = [b for b in m["builds"] if b.get("chipFamily") != chipfamily]
    m["builds"].append({"chipFamily": chipfamily, "improv": True,
                        "parts": [{"path": factory, "offset": 0}]})
    m["builds"].sort(key=lambda b: b.get("chipFamily", ""))
    m.setdefault("ota", {})
    m["ota"][chipfamily] = {"path": firmware, "md5": fw_md5}
    atomic_write(mpath, json.dumps(m, indent=2) + "\n")
    print(f"   manifest merged: {chipfamily} → {factory} (webflash) + {firmware} (OTA)")

main()
