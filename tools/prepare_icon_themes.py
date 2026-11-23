"""Development-time preparation and validation pipeline for AnkhCore 2.5 Icon Themes
on Resource Redirect 1.3 (D41 / APP-8).

Reads original icon packages from D:\\Ankhcore\\Icon pack read-only (never modifies
originals), extracts .7z archives in a temporary work directory using 7-Zip, parses
all .res files, links each .res into a resource-only PE (.dll/.exe/.cpl) via MSVC
link.exe (/DLL /NOENTRY /MACHINE:X64), validates every generated PE module via
LoadLibraryExW + resource enumeration + PrivateExtractIconsW, generates theme.ini
and theme.json in D:\\Ankhcore\\release-inputs\\icon-themes\\IconThemes\\<id>, and
writes the full compatibility report.
"""
import configparser
import ctypes
import ctypes.wintypes as wt
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys

ICON_PACK_ROOT = r"D:\Ankhcore\Icon pack"
SEVEN_ZIP = r"D:\Ankhcore\Tools\7-Zip\7z.exe"
OUT_ROOT = r"D:\Ankhcore\release-inputs\icon-themes"
THEMES_OUT = os.path.join(OUT_ROOT, "IconThemes")
WORK_DIR = os.path.join(OUT_ROOT, "_work")
VCVARS = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

PACK_DEFS = [
    {
        "id": "bigsur",
        "group": "bigsur",
        "name": "Big Sur 2",
        "variant": "Regular",
        "author": "niivu",
        "desc": "macOS Big Sur style folders and drives.",
        "preview_rc": "PV_BIGSUR",
        "folder": "7tsp Big Sur 2",
        "archive": None,
        "preview_rel": r"Extra\Preview.png",
    },
    {
        "id": "bigsur-dark",
        "group": "bigsur",
        "name": "Big Sur 2",
        "variant": "Dark",
        "author": "niivu",
        "desc": "macOS Big Sur style folders and drives (Dark mode).",
        "preview_rc": "PV_BIGSUR_DARK",
        "folder": "7tsp Big Sur 2 DarkMode",
        "archive": None,
        "preview_rel": r"Extra\Preview.png",
    },
    {
        "id": "bigsur-light",
        "group": "bigsur",
        "name": "Big Sur 2",
        "variant": "Light",
        "author": "niivu",
        "desc": "macOS Big Sur style folders and drives (Light mode).",
        "preview_rc": "PV_BIGSUR_LIGHT",
        "folder": "7tsp Big Sur 2 LightMode",
        "archive": None,
        "preview_rel": r"Extra\Preview.png",
    },
    {
        "id": "sweet",
        "group": "sweet",
        "name": "Sweet Paranoia",
        "variant": "",
        "author": "niivu",
        "desc": "Neon candy colours on dark: purple, pink and orange.",
        "preview_rc": "PV_SWEET",
        "folder": "sweet-paranoia_9a9fc25612_VSTHEMES-ORG",
        "archive": r"Sweet Paranoia Icon Theme\7TSP\7TSP candy.7z.remove",
        "preview_rel": r"Sweet Paranoia Icon Theme\Sweet Paranoia Preview.png",
    },
    {
        "id": "candy",
        "group": "candy",
        "name": "7TSP Candy",
        "variant": "",
        "author": "niivu",
        "desc": "Neon gradient folder and system icons (loose 7TSP package, identical resources to Sweet Paranoia).",
        "preview_rc": "PV_SWEET",
        "folder": "7TSP candy",
        "archive": None,
        "preview_rel": None,
        # The owner's supplied picture, copied unmodified as the theme's preview (2026-11-15).
        "preview_abs": os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "recovered_res", "candy-icons.png"),
    },
    {
        "id": "tech",
        "group": "tech",
        "name": "Tech Icons",
        "variant": "",
        "author": "Dark Knight",
        "desc": "Dark tech hardware with blue accents. Made for Windows 10 22H2 and newer.",
        "preview_rc": "PV_TECH",
        "folder": "7tsptech_icon_pack_windows10_22h2__by_darkknight2264_dgf8ng2",
        "archive": None,
        "preview_rel": r"Extra\preview.bmp",
    },
    {
        "id": "magnum",
        "group": "magnum",
        "name": "Magnum Dark",
        "variant": "",
        "author": "Razorsedge & Moonnique",
        "desc": "Glossy black with blue reflections; changes system icon modules.",
        "preview_rc": "PV_MAGNUM",
        "folder": "magnum_d682839d63_VSTHEMES-ORG",
        "archive": None,
        "preview_rel": r"Extra\Preview.bmp",
    },
    {
        "id": "hud-machine-blue",
        "group": "hud",
        "name": "HUD Machine Blue",
        "variant": "",
        "author": "Mr GRiM & Moonnique",
        "desc": "Black and electric-blue HUD icons for Windows 11 21H2+.",
        "preview_rc": "PV_HUD",
        "folder": "HUD_Machine_Blue_IconPack_7TSP",
        "archive": None,
        "preview_rel": r"Extra\Preview.bmp",
    },
    {
        "id": "alienware-eclipse",
        "group": "alienware-eclipse",
        "name": "Alienware Eclipse",
        "variant": "",
        "author": "Mr Blade",
        "desc": "Dark metallic Alienware Eclipse icon theme.",
        "preview_rc": "",
        "folder": "7tsp_Alienware_Eclipse_Pack",
        "archive": None,
        "preview_rel": r"Extra\Preview.bmp",
    },
    {
        "id": "alienware-invader",
        "group": "alienware-invader",
        "name": "Alienware Invader Blue",
        "variant": "",
        "author": "ADMIN",
        "desc": "Blue sci-fi Alienware Invader folder and drive icons.",
        "preview_rc": "",
        "folder": "alienware-invader_a3573a58c6_VSTHEMES-ORG",
        "archive": None,
        "preview_rel": r"Extra\Preview.bmp",
    },
    {
        "id": "pear-blue-dark",
        "group": "pear-blue-dark",
        "name": "PearBlueDark",
        "variant": "",
        "author": "niivu",
        "desc": "Dark blue macOS-inspired folders and system icons.",
        "preview_rc": "",
        "folder": "pearbluedark-icons_c1d98ebfe2_VSTHEMES-ORG",
        "archive": r"PearBlueDark Icons Pack\7TSP PearBlueDark\7TSP PearBlueDark.7z remove",
        "preview_rel": r"PearBlueDark Icons Pack\Preview.PNG",
    },
    # 2026-11-15: seven further 7TSP packs (HUD Machine Blue above was already registered, not duplicated).
    {"id": "3d-purple", "group": "3d-purple", "name": "3D Purple", "variant": "", "author": "WinX Customization",
     "desc": "Purple 3D folders and system icons.", "preview_rc": "", "folder": "3D_Purple_IconPack_7TSP", "archive": None,
     "preview_rel": r"Extra\preview.png"},
    {"id": "amy-dark", "group": "amy-dark", "name": "Amy Dark", "variant": "", "author": "Digifeye",
     "desc": "Dark folders and system icons.", "preview_rc": "", "folder": "Amy_Dark_IconPack_7TSP", "archive": None,
     "preview_rel": r"Extra\preview.png"},
    {"id": "coloration", "group": "coloration", "name": "Coloration", "variant": "", "author": "WinX Customization",
     "desc": "Colourful folders and system icons.", "preview_rc": "", "folder": "Coloration_IconPack_7TSP", "archive": None,
     "preview_rel": r"Extra\Preview.png"},
    {"id": "hal-green", "group": "hal-green", "name": "Hal Green System", "variant": "", "author": "7TSP",
     "desc": "Green system folders and icons.", "preview_rc": "", "folder": "Hal_Green_System_v1_IconPack_7TSP",
     "archive": "Hal_Green_System_v1_IconPack_7TSP.7z remove", "preview_rel": r"Extra\preview.bmp", "preview_in_archive": True},
    {"id": "macos-elegance", "group": "macos-elegance", "name": "macOS Elegance", "variant": "", "author": "WinX Customization",
     "desc": "macOS-style elegant folders and system icons.", "preview_rc": "", "folder": "macOS_Elegance_IconPack_7TSP", "archive": None,
     "preview_rel": r"Extra\Preview.png"},
    {"id": "tela-black", "group": "tela-black", "name": "Tela Black", "variant": "", "author": "Digifeye",
     "desc": "Tela black folders and system icons.", "preview_rc": "", "folder": "Tela_Black_IconPack_7TSP", "archive": None,
     "preview_rel": r"Extra\preview.png"},
    {"id": "windows-23", "group": "windows-23", "name": "Windows 23", "variant": "", "author": "Link Vegas",
     "desc": "Windows 23 style folders and system icons.", "preview_rc": "", "folder": "Windows_23_IconPack_7TSP", "archive": None,
     "preview_rel": r"Extra\preview.png"},
]

EMPTY_RES_HEADER = bytes.fromhex("0000000020000000ffff0000ffff000000000000000000000000000000000000")

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def parse_res_file(path):
    data = open(path, "rb").read()
    if len(data) < 32 or data[:32] != EMPTY_RES_HEADER:
        return None, 0, None, "Missing 32-byte RES header"
    entries = []
    raw_chunks = []
    o = 0
    n = len(data)
    while o + 8 <= n:
        dsz, hsz = struct.unpack_from("<II", data, o)
        if hsz < 32 or o + hsz + dsz > n:
            return None, 0, None, f"Truncated RES entry at offset {o}"
        next_o = (o + hsz + dsz + 3) & ~3
        q = o + 8
        def read_id(pos):
            w = struct.unpack_from("<H", data, pos)[0]
            if w == 0xFFFF:
                return True, struct.unpack_from("<H", data, pos + 2)[0], "", pos + 4
            chars = []
            while pos + 2 <= n:
                c = struct.unpack_from("<H", data, pos)[0]
                pos += 2
                if c == 0:
                    break
                chars.append(chr(c))
            return False, 0, "".join(chars), pos
        t_is_id, t_id, t_str, q = read_id(q)
        n_is_id, n_id, n_str, q = read_id(q)
        q = (q + 3) & ~3
        lang = struct.unpack_from("<H", data, q + 6)[0]
        raw = data[o + hsz : o + hsz + dsz]
        if not (t_is_id and t_id == 0 and n_is_id and n_id == 0 and dsz == 0):
            key = (t_is_id, t_id, t_str.upper(), n_is_id, n_id, n_str.upper(), lang)
            entries.append({
                "key": key,
                "type_is_id": t_is_id,
                "type_id": t_id,
                "type_str": t_str,
                "name_is_id": n_is_id,
                "name_id": n_id,
                "name_str": n_str,
                "lang": lang,
                "size": dsz,
                "raw": raw if (t_is_id and t_id == 14) else b"",
                "chunk": data[o:next_o],
            })
        o = next_o

    # Deduplicate (type, name, lang) keeping the last entry (matching UpdateResourceW semantics)
    last_idx = {}
    for i, e in enumerate(entries):
        last_idx[e["key"]] = i
    dup_count = len(entries) - len(last_idx)
    if dup_count > 0:
        dedup_entries = [entries[i] for i in sorted(last_idx.values())]
        dedup_bytes = bytearray(EMPTY_RES_HEADER)
        for e in dedup_entries:
            dedup_bytes.extend(e["chunk"])
        return dedup_entries, dup_count, bytes(dedup_bytes), None
    return entries, 0, None, None

def check_icon_groups(entries):
    icon_ids = {e["name_id"] for e in entries if e["type_is_id"] and e["type_id"] == 3 and e["name_is_id"]}
    missing_members = 0
    groups = 0
    for e in entries:
        if e["type_is_id"] and e["type_id"] == 14:
            groups += 1
            raw = e["raw"]
            if len(raw) >= 6:
                _, _, count = struct.unpack_from("<HHH", raw, 0)
                for i in range(count):
                    off = 6 + i * 14
                    if off + 14 <= len(raw):
                        nid = struct.unpack_from("<H", raw, off + 12)[0]
                        if nid not in icon_ids:
                            missing_members += 1
    return groups, len(icon_ids), missing_members

def resolve_module_targets(res_filename):
    n = res_filename
    if n.lower().endswith(".res"):
        n = n[:-4].rstrip()   # Amy Dark ships "fontext.dll .res" (a space before the extension)
    is_mun = False
    is_mui = False
    if n.lower().endswith(".mun"):
        n = n[:-4]
        is_mun = True
    elif n.lower().endswith(".mui"):
        n = n[:-4]
        is_mui = True
    win_dir = os.environ.get("SystemRoot", r"C:\Windows")
    sys32 = os.path.join(win_dir, "System32")
    syswow = os.path.join(win_dir, "SysWOW64")
    sysres = os.path.join(win_dir, "SystemResources")

    ini_keys = []
    exists_on_host = False
    if os.path.isfile(os.path.join(win_dir, n)):
        ini_keys.append(rf"%SystemRoot%\{n}")
        exists_on_host = True
    if os.path.isfile(os.path.join(sys32, n)) or os.path.isfile(os.path.join(sysres, n + ".mun")):
        ini_keys.append(rf"%SystemRoot%\System32\{n}")
        exists_on_host = True
    if os.path.isfile(os.path.join(syswow, n)):
        ini_keys.append(rf"%SystemRoot%\SysWOW64\{n}")
        exists_on_host = True

    if not ini_keys:
        if n.lower() in ("explorer.exe", "regedit.exe", "bfsvc.exe", "hh.exe", "winhlp32.exe"):
            ini_keys.append(rf"%SystemRoot%\{n}")
        else:
            ini_keys.append(rf"%SystemRoot%\System32\{n}")
    return n, ini_keys, exists_on_host, is_mun, is_mui

k32 = ctypes.windll.kernel32
u32 = ctypes.windll.user32
k32.LoadLibraryExW.restype = ctypes.c_void_p
k32.LoadLibraryExW.argtypes = [wt.LPCWSTR, wt.HANDLE, wt.DWORD]
k32.FreeLibrary.restype = wt.BOOL
k32.FreeLibrary.argtypes = [ctypes.c_void_p]
k32.GetPrivateProfileSectionW.restype = wt.DWORD
k32.GetPrivateProfileSectionW.argtypes = [wt.LPCWSTR, wt.LPWSTR, wt.DWORD, wt.LPCWSTR]

def verify_built_pe(pe_path, expected_groups, expected_icons):
    data = open(pe_path, "rb").read()
    if len(data) < 0x40 or data[:2] != b"MZ":
        return False, "Not an MZ PE file"
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_off + 0x18 > len(data) or data[pe_off : pe_off + 4] != b"PE\x00\x00":
        return False, "Missing PE signature"
    h = k32.LoadLibraryExW(pe_path, None, 0x00000002 | 0x00000020)
    if not h:
        return False, f"LoadLibraryExW failed (err={ctypes.GetLastError()})"
    k32.FreeLibrary(h)
    if expected_groups > 0:
        hicon = wt.HICON()
        iconid = wt.UINT()
        extracted = u32.PrivateExtractIconsW(pe_path, 0, 32, 32, ctypes.byref(hicon), ctypes.byref(iconid), 1, 0)
        if hicon.value:
            u32.DestroyIcon(hicon)
        if extracted == 0 or extracted == 0xFFFFFFFF:
            return False, f"PrivateExtractIconsW(0) failed (ret={extracted})"
    return True, "OK"

def main():
    only = set(sys.argv[2:]) if len(sys.argv) > 2 and sys.argv[1] == "--only" else None   # --only id [id ...]: leave every other theme untouched
    os.makedirs(THEMES_OUT, exist_ok=True)
    if os.path.exists(WORK_DIR):
        shutil.rmtree(WORK_DIR)
    os.makedirs(WORK_DIR, exist_ok=True)

    report = []
    for pdef in PACK_DEFS:
        pid = pdef["id"]
        if only is not None and pid not in only:
            continue
        src_folder = os.path.join(ICON_PACK_ROOT, pdef["folder"])
        theme_out = os.path.join(THEMES_OUT, pid)
        os.makedirs(theme_out, exist_ok=True)

        if pdef["archive"]:
            arch_path = os.path.join(src_folder, pdef["archive"])
            arch_sha = sha256_file(arch_path)
            work_sub = os.path.join(WORK_DIR, pid)
            os.makedirs(work_sub, exist_ok=True)
            subprocess.run([SEVEN_ZIP, "x", arch_path, f"-o{work_sub}", "-y"], check=True, capture_output=True)
            scan_root = work_sub
        else:
            arch_sha = None
            scan_root = src_folder

        res_list = []
        for rroot, _, rfiles in os.walk(scan_root):
            for rf in sorted(rfiles):
                if rf.lower().endswith(".res"):
                    res_list.append(os.path.join(rroot, rf))

        # Check if already built and up to date
        manifest_path = os.path.join(theme_out, "theme.json")
        ini_path = os.path.join(theme_out, "theme.ini")

        modules_info = []
        ini_lines = ["[redirections]"]
        seen_ini_pairs = set()
        used_pe_names = set()
        link_cmds = []

        total_groups = 0
        total_icons = 0
        non_icon_types = set()
        missing_host_modules = []
        build_errors = []

        for res_path in res_list:
            rf = os.path.basename(res_path)
            res_sha = sha256_file(res_path)
            entries, dup_count, dedup_bytes, err = parse_res_file(res_path)
            if err:
                build_errors.append(f"{rf}: {err}")
                continue
            groups, icons, missing_members = check_icon_groups(entries)
            if missing_members > 0:
                build_errors.append(f"{rf}: {missing_members} missing RT_ICON member(s)")
            total_groups += groups
            total_icons += icons

            type_counts = {}
            langs = set()
            for e in entries:
                tk = str(e["type_id"]) if e["type_is_id"] else e["type_str"]
                type_counts[tk] = type_counts.get(tk, 0) + 1
                if tk not in ("3", "14", "2", "16", "24"):
                    non_icon_types.add(tk)
                langs.add(e["lang"])

            mod_name, ini_keys, exists_on_host, is_mun, is_mui = resolve_module_targets(rf)
            if not exists_on_host:
                missing_host_modules.append(mod_name)

            # Determine output PE filename inside theme_out
            out_pe_name = mod_name
            if out_pe_name.lower() in used_pe_names:
                out_pe_name = rf[:-4] + ".dll"
            used_pe_names.add(out_pe_name.lower())
            out_pe_path = os.path.join(theme_out, out_pe_name)
            link_res_path = res_path
            if dup_count > 0:
                dedup_dir = os.path.join(WORK_DIR, f"dedup_{pid}")
                os.makedirs(dedup_dir, exist_ok=True)
                link_res_path = os.path.join(dedup_dir, rf)
                with open(link_res_path, "wb") as df:
                    df.write(dedup_bytes)
            if not os.path.isfile(out_pe_path):
                link_cmds.append((link_res_path, out_pe_path))

            for k in ini_keys:
                pair_key = (k.upper(), out_pe_name.upper())
                if pair_key not in seen_ini_pairs:
                    seen_ini_pairs.add(pair_key)
                    ini_lines.append(f"{k}={out_pe_name}")

            modules_info.append({
                "source_res": rf,
                "source_sha256": res_sha,
                "duplicate_entries_deduped": dup_count,
                "module": out_pe_name,
                "ini_keys": ini_keys,
                "present_on_win11": exists_on_host,
                "icon_groups": groups,
                "icons": icons,
                "missing_icon_members": missing_members,
                "types": type_counts,
                "languages": sorted(langs),
            })

        if link_cmds:
            batch_file = os.path.join(WORK_DIR, f"link_{pid}.cmd")
            with open(batch_file, "w", encoding="utf-8") as bf:
                bf.write('@echo off\n')
                bf.write(f'call "{VCVARS}" >nul || exit /b 90\n')
                for rpath, opath in link_cmds:
                    bf.write(f'link /nologo /DLL /NOENTRY /MACHINE:X64 "{rpath}" /OUT:"{opath}" >nul || exit /b 91\n')
            rc = subprocess.run(["cmd", "/c", batch_file], capture_output=True, text=True)
            if rc.returncode != 0:
                build_errors.append(f"link.exe batch failed (rc={rc.returncode}): {rc.stdout} {rc.stderr}")

        # Verify every built PE
        for m in modules_info:
            pe_path = os.path.join(theme_out, m["module"])
            if not os.path.isfile(pe_path):
                build_errors.append(f"{m['module']}: output file missing")
                continue
            ok, msg = verify_built_pe(pe_path, m["icon_groups"], m["icons"])
            if not ok:
                build_errors.append(f"{m['module']}: {msg}")
            m["module_sha256"] = sha256_file(pe_path)
            m["module_size"] = os.path.getsize(pe_path)
            m["verified"] = ok

        # Write theme.ini (ASCII/ANSI compatible CRLF for GetPrivateProfileSectionW)
        with open(ini_path, "w", encoding="ascii", newline="\r\n") as f:
            f.write("\n".join(ini_lines) + "\n")

        # Verify theme.ini via GetPrivateProfileSectionW
        buf = ctypes.create_unicode_buffer(65536)
        nread = k32.GetPrivateProfileSectionW("redirections", buf, 65536, ini_path)
        if nread == 0 or nread >= 65534:
            build_errors.append(f"GetPrivateProfileSectionW failed on theme.ini (nread={nread})")

        # Copy preview image if available
        preview_file = ""
        if pdef.get("preview_abs") and os.path.isfile(pdef["preview_abs"]):
            ext = os.path.splitext(pdef["preview_abs"])[1].lower()
            preview_file = f"preview{ext}"
            shutil.copyfile(pdef["preview_abs"], os.path.join(theme_out, preview_file))
        elif pdef["preview_rel"]:
            src_prev = os.path.join(scan_root if pdef.get("preview_in_archive") else src_folder, pdef["preview_rel"])
            if os.path.isfile(src_prev):
                ext = os.path.splitext(src_prev)[1].lower()
                preview_file = f"preview{ext}"
                shutil.copyfile(src_prev, os.path.join(theme_out, preview_file))

        status = "supported"
        notes = []
        if build_errors:
            status = "unsupported"
            notes.extend(build_errors)
        else:
            if non_icon_types:
                status = "partial"
                notes.append(f"Contains non-icon resource types ({', '.join(sorted(non_icon_types))}); icons and bitmaps redirect via standard hooks, while raw stream/custom types require allResourceRedirect or are ignored.")
            if missing_host_modules:
                notes.append(f"{len(missing_host_modules)} legacy module target(s) not present on Windows 11 26H2 ({', '.join(missing_host_modules[:6])}{'...' if len(missing_host_modules) > 6 else ''}); active Windows 11 modules redirect normally.")
            if pid == "candy":
                notes.append("Resource-identical to Sweet Paranoia (same 5 .res files and SHA-256 hashes).")

        theme_meta = {
            "id": pid,
            "group": pdef["group"],
            "name": pdef["name"],
            "variant": pdef["variant"],
            "author": pdef["author"],
            "description": pdef["desc"],
            "preview_rc": pdef["preview_rc"],
            "preview_file": preview_file,
            "source_folder": pdef["folder"],
            "source_archive": pdef["archive"] or "",
            "source_archive_sha256": arch_sha or "",
            "status": status,
            "notes": notes,
            "module_count": len(modules_info),
            "present_on_win11_count": sum(1 for m in modules_info if m["present_on_win11"]),
            "total_icon_groups": total_groups,
            "total_icons": total_icons,
            "ini_redirection_count": len(ini_lines) - 1,
            "theme_ini_sha256": sha256_file(ini_path),
            "modules": modules_info,
        }
        with open(manifest_path, "w", encoding="utf-8") as mf:
            json.dump(theme_meta, mf, indent=2)
        report.append(theme_meta)
        print(f"[{status.upper()}] {pid}: {len(modules_info)} modules ({theme_meta['present_on_win11_count']} on Win11), {total_groups} groups, {total_icons} icons, errors={len(build_errors)}")

    if os.path.exists(WORK_DIR):
        shutil.rmtree(WORK_DIR)

    rep_path = os.path.join(OUT_ROOT, "compatibility_report.json")
    if only is not None and os.path.isfile(rep_path):   # keep the entries of the themes that were not rebuilt
        old = json.load(open(rep_path, encoding="utf-8")).get("themes", [])
        report = [t for t in old if t.get("id") not in only] + report
    with open(rep_path, "w", encoding="utf-8") as rf:
        json.dump({"themes": report}, rf, indent=2)
    print(f"Wrote {rep_path} ({len(report)} themes)")

if __name__ == "__main__":
    main()
