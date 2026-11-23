#!/usr/bin/env python3
"""AnkhCore privacy gate: no location access.

AnkhCore is a hardware monitor. It has no feature that needs to know where the PC is, so nothing it
ships may call an interface Windows treats as location access. Since Windows 11 24H2 that includes the
Wi-Fi calls that return a network's BSSID: each call is recorded under Settings > Privacy & security >
Location > Recent activity and lights the location icon. (2.5 Preview 1 did this every 5 seconds through
WlanQueryInterface(wlan_intf_opcode_current_connection); see docs/test-logs/2026-11-12_M-PRIV1.md.)

Checks, on the sources (always) and on built programs (when present, or given as arguments):
  1. no forbidden interface name appears;
  2. every WlanQueryInterface call passes kWlanOpcodeRssi and nothing else.

Usage:  python tools/check_privacy.py [binary ...]
Exit code 0 = clean, 1 = a finding, 2 = the check itself could not run.
"""
import os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

# Interfaces Windows gates behind location consent (Microsoft: "Changes to API behavior for Wi-Fi access
# and location"), plus the location interfaces themselves.
FORBIDDEN = [
    "WlanGetAvailableNetworkList", "WlanGetNetworkBssList", "WlanScan", "WlanRegisterNotification",
    "WLAN_CONNECTION_ATTRIBUTES", "wlan_intf_opcode_current_connection", "wlanapi.h",
    "Windows.Devices.Geolocation", "Geolocator", "Windows.Devices.WiFi", "WiFiAdapter", "GetLanIdentifiers",
    "ILocation", "LocationApi", "locationapi", "GeoCoordinateWatcher", "System.Device.Location",
    "GetGeoInfo", "GetUserGeoID", "wiFiControl",
]
# Names that may not appear in a built program at all (import tables and GetProcAddress strings).
FORBIDDEN_BINARY = [
    "WlanGetAvailableNetworkList", "WlanGetNetworkBssList", "WlanScan", "WlanRegisterNotification",
    "Windows.Devices.Geolocation", "Windows.Devices.WiFi", "GetLanIdentifiers", "LocationApi.dll", "locationapi.dll",
    "System.Device.Location",
]
SOURCE_DIRS = ["src", os.path.join("src", "sensors")]
SOURCE_EXT = (".h", ".cpp", ".cs", ".rc", ".manifest")
DEFAULT_BINARIES = [
    os.path.join("build", "out", "AnkhCore_test.exe"), os.path.join("build-test", "out", "AnkhCore_TEST.exe"),
    os.path.join("build-setup", "out", "AnkhCore_2.5_Setup.exe"), os.path.join("build-setup", "out", "AnkhSetup_TEST.exe"),
]

def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return "\n".join(re.sub(r"//.*$", "", line) for line in text.split("\n"))

def check_sources():
    findings, files = [], 0
    for d in SOURCE_DIRS:
        full = os.path.join(ROOT, d)
        if not os.path.isdir(full):
            continue
        for name in sorted(os.listdir(full)):
            if not name.lower().endswith(SOURCE_EXT):
                continue
            path = os.path.join(full, name); files += 1
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                code = strip_comments(f.read())
            rel = os.path.relpath(path, ROOT)
            for i, line in enumerate(code.split("\n"), 1):
                for word in FORBIDDEN:
                    if word in line:
                        findings.append("%s:%d: location-gated interface '%s'" % (rel, i, word))
                # the query function may be named (GetProcAddress) but each call must pass the RSSI opcode
                for m in re.finditer(r"\bwlanQuery_\s*\(([^;]*)\)", line):
                    args = m.group(1)
                    if "wlanH_" in args and "kWlanOpcodeRssi" not in args:
                        findings.append("%s:%d: WlanQueryInterface is called with something other than kWlanOpcodeRssi" % (rel, i))
            if "kWlanOpcodeRssi =" in code and not re.search(r"kWlanOpcodeRssi\s*=\s*0x10000102\b", code):
                findings.append("%s: kWlanOpcodeRssi is no longer wlan_intf_opcode_rssi (0x10000102)" % rel)
    return findings, files

def check_binary(path):
    with open(path, "rb") as f:
        data = f.read()
    out = []
    for word in FORBIDDEN_BINARY:
        if word.encode("ascii") in data or word.encode("utf-16-le") in data:
            out.append("%s: contains '%s'" % (os.path.relpath(path, ROOT) if path.startswith(ROOT) else path, word))
    return out

def main():
    findings, files = check_sources()
    if files == 0:
        print("check_privacy: no source files found under", ROOT); return 2
    binaries = sys.argv[1:] or [os.path.join(ROOT, b) for b in DEFAULT_BINARIES if os.path.isfile(os.path.join(ROOT, b))]
    for b in binaries:
        if not os.path.isfile(b):
            print("check_privacy: not found:", b); return 2
        findings += check_binary(b)
    for line in findings:
        print("PRIVACY  " + line)
    print("check_privacy: %d source files, %d programs, %s" % (files, len(binaries), "%d finding(s)" % len(findings) if findings else "no location access"))
    return 1 if findings else 0

if __name__ == "__main__":
    sys.exit(main())
