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
    # Optional trailing args: extra flash parts for THIS chip's webflash entry,
    # each "path@offset" (offset decimal or 0x-hex).  Used by the zbgw mission
    # to append the companion-radio image (radio_h2.bin@0x620000) so ONE
    # manifest drives the two-chip factory webflash AND the in-device OTA.
    extra_parts = []
    for spec in sys.argv[8:]:
        path, off = spec.rsplit("@", 1)
        extra_parts.append({"path": path, "offset": int(off, 0)})
    if os.path.exists(mpath):
        m = json.load(open(mpath))
        if m.get("version") != version:
            # A clash means EITHER a half-updated mission (the case the guard
            # exists for) OR the first chip of a NEW release over last release's
            # manifest.  The operator disambiguates: ALLOW_NEW_MISSION_VERSION=1
            # starts the new version with a FRESH manifest — existing chip
            # entries are DROPPED, not carried, so every chip of the mission
            # must be (re)built in this release sequence and a forgotten chip
            # goes missing loudly instead of shipping under a wrong version.
            if os.environ.get("ALLOW_NEW_MISSION_VERSION") == "1":
                print(f"   manifest: starting NEW mission version {version} "
                      f"(was {m.get('version')}) — dropping previous chip entries")
                m = {"name": name, "version": version,
                     "funding_url": "https://busware.de",
                     "new_install_prompt_erase": True, "builds": [], "ota": {}}
            else:
                sys.stderr.write(
                    f"ABORT: mission manifest {mpath} is version '{m.get('version')}' but this "
                    f"build is '{version}'.\n       All chips of a mission must be released with "
                    f"the SAME version (one tag) — rebuild the whole mission with one tag.\n"
                    f"       First chip of a NEW release: re-run with ALLOW_NEW_MISSION_VERSION=1.\n")
                sys.exit(3)
    else:
        m = {"name": name, "version": version, "funding_url": "https://busware.de",
             "new_install_prompt_erase": True, "builds": [], "ota": {}}
    m["name"] = name
    m["version"] = version
    m.setdefault("builds", [])
    m["builds"] = [b for b in m["builds"] if b.get("chipFamily") != chipfamily]
    m["builds"].append({"chipFamily": chipfamily, "improv": True,
                        "parts": [{"path": factory, "offset": 0}] + extra_parts})
    m["builds"].sort(key=lambda b: b.get("chipFamily", ""))
    m.setdefault("ota", {})
    m["ota"][chipfamily] = {"path": firmware, "md5": fw_md5}
    atomic_write(mpath, json.dumps(m, indent=2) + "\n")
    ep = "".join(f" + {e['path']}@0x{e['offset']:x}" for e in extra_parts)
    print(f"   manifest merged: {chipfamily} → {factory}{ep} (webflash) + {firmware} (OTA)")

main()
