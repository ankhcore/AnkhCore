# AnkhCore 2.5 — Requirements traceability matrix

**Source of requirements:** `D:\Ankhcore\AnkhCore_2.5_Master_Implementation_Prompt_UPDATED.txt` (the "master", § numbers), plus the owner decisions in `docs/DECISIONS.md`.
**Status as of M-BAK1 (2026-11-11, after `d99f3d3` 2.5 Preview 1)**, taken from the code and the test logs, not from memory. Rows last changed by a milestone name that milestone. **Rows changed later carry their own milestone or date; the Preview 2.1 (FT-*), WT-*, AST-* and closure rows (2026-11-15 to 2026-11-17) were added after it. Deferrals to 2.6+: `docs/DECISIONS.md` D45.**

**Status values:**

| Status | Meaning |
|---|---|
| **Done** | Implemented and verified as stated |
| **Partial** | Some of the requirement is implemented; the rest is open in BACKLOG |
| **2.2** | The 2.2 implementation is present but not yet reworked to the 2.5 requirement |
| **Open** | Not started |
| **Pending** | Waits for an owner decision |

**Verification codes:**

| Code | Means |
|---|---|
| UT | `tools\test_base.cmd` |
| FP | `tools\test_fanpolicy.cmd` |
| ST | `--selftest` |
| SB | setup `--sandbox-test` |
| HOST | real-machine test (needs an approved plan) |
| VIS | screenshot review |

No requirement may move to a lower status, or leave this table, without an owner decision (D19).

## Principles (master §1.2)

| ID | Requirement | Component | Status | Verification | Limitations / notes |
|---|---|---|---|---|---|
| PR-A | Preserve working 2.2 functionality | all | Done so far | ST walks every page and control | Re-check at every milestone |
| PR-C | One application (helpers are internal) | app, AnkhSensors | Done | — | — |
| PR-D | Real functionality only; no fake values or controls | all | Done so far | ST; code review | — |
| PR-E | Safety: validate, back up, log, recover | journal, setup | Partial | UT, SB | Least privilege is open (SEC-1) |
| PR-F | One shared data provider | SensorHub, MonitorStore | Done | ST (single poller, same snapshot) | — |
| PR-I | Preserve user data and Windhawk mods | setup, settings | Done (installer) | SB | — |
| PR-J | No invented CLI switches | all | Done so far | Windhawk CLI verified from `--help` (design doc) | — |

## Navigation (master §3)

| ID | Requirement | Component | Status | Verification | Limitations / notes |
|---|---|---|---|---|---|
| NAV-1 | Accordion sidebar with the target sections | `app.cpp` `kNav`, `NavParent` | **Done in the dev build:** Sidebar (`5` pages), Mini Monitor, Monitoring Preferences, Hardware Information, Network Monitor, Appearance (`5` pages), Apps (`3` pages), Recovery (`2` pages), Optimize & Repair (`Driver Manager`, `Storage Analyzer`), and Activity (`Operation History`, `Error Logs`) implemented with Back, breadcrumbs, and keyboard navigation | ST (`ANKH_NAVONLY=1` 120/120), `docs/test-logs/2026-11-14_M-NAV3.md`, `docs/test-logs/2026-11-14_M-BAK3-5_B04.md`, `docs/test-logs/2026-11-14_M-SYS1.md` | Remaining Windhawk Manager sub-pages gated on P08. |
| NAV-2 | Accordion behaviour: animation, active states, keyboard, memory | `app.cpp` (`NavLayout`, `NavReveal`, `NavClampScroll`), `src/nav_layout.h` | **Done in the dev build (NAV-2):** compact icon-only rail by click and saved; tooltips for every item, the status dot, the expand button and the keyboard-focused item; the rail scrolls to the keyboard selection and never keeps a stale offset | UT (`391/391`), ST; `docs/test-logs/2026-11-14_NAV-2.md` | The rail switch has no keyboard shortcut; tooltips are drawn in the window (no screen-reader text) |
| NAV-3 | New features reachable through the same navigation | `app.cpp` | Done so far | ST | — |

## Visual design (master §4; D12, D13)

| ID | Requirement | Component | Status | Verification | Limitations / notes |
|---|---|---|---|---|---|
| VIS-1 | Blue Acrylic design system on every surface | `ui.h`, `components.h`, `gfx.h` | **Mostly done** in the dev build: tokens, components, rail, Dashboard, dialogs, toasts, every page | ST 139; gallery capture | Open items: `NEXT_SESSION.md` |
| VIS-2 | Genuine acrylic with fallbacks | `material.h`, `ApplyMaterial`, `WallDraw` | **Done here**: acrylic, frosted, wallpaper glass, solid; D20 default; blur and contrast measured on screen while active | UT; ST probes; `docs/test-logs/2026-11-08_M-RD3-RD6.md` | Windows 10 and the system-setting fallbacks not run on real conditions |
| VIS-3 | Sidebar and Mini Monitor in the Blue Acrylic design | `PushLayered`, `g_widgetSurf` | **Done**: tokens, cards, gradients, area sparklines, optional wallpaper glass | ST | No true blur on click-through widgets (Phase 0) |
| VIS-4 | Motion: glitch identity, transitions, reduced motion | `anim.h`, compositor window animation | **Partial**: Animator, hover, spring, page fade, particles, operation band, Motion setting, Anubis skip | UT 8 | Open items: `NEXT_SESSION.md` |
| VIS-5 | Official logo visible and unobstructed | brand plinth | **Done** in the rail (reserved block; watermark on Dashboard and About) | ST captures | Larger master still needs P03 |
| VIS-6 | Modern icon system with states | Segoe Fluent/MDL2 | **Done**: runtime font choice, hover/active states, glyphs checked against the font | ST | Filled variants not used |
| SB-1 | Advanced Sidebar: per-component detail, sections, modes, graphs | `src/sidebar_layout.h`, `src/sys_probe.h`, `SbRender`; `docs/design/SIDEBAR_ADVANCED.md` | **Done in the dev build:** six sections (CPU, Graphics, Memory, Storage, Network, Fans), Minimal/Standard/Detailed, graphs, full-height fit | UT 14; ST 10; `docs/test-logs/2026-11-10_M-SB1.md` | Graphics mixes the two GPUs (B02). Per-core frequency, memory clock, per-drive temperature and health need a new helper |
| SB-2 | Network adapter selection (Automatic, All, Manual) with per-adapter accounting and persistent history (D27-B) | `src/net_counters.h`, `src/net_probe.h`, `src/engine.h`, `src/ipc.h`; `docs/design/SIDEBAR_ADVANCED.md` | **Done in the dev build:** engine sampling, per-adapter totals (session, today, boot, lifetime), persistence, the three modes, Sidebar/Mini/pages | UT 29; ST 7; harness 1; `docs/test-logs/2026-11-10_M-NET1.md` | Not run on real conditions: Wi-Fi/Ethernet switch, VPN, day rollover, reboot, sleep |
| SB-3 | Sidebar customization: gauges or values, detailed or compact, custom colours, acrylic/blur or solid, opacity, tint, glow, animations, section visibility and order, full-height layout, live preview, persistent settings (D27-C) | `src/sidebar_layout.h`, `SbRender`, `PushLayered`, System Monitor settings | **Done in the dev build:** indicators/gauges/values/graphs per section; Minimal/Standard/Detailed/Custom; accent, temperature, text and tint colours; solid, frosted glass, acrylic blur; opacity, tint, glow, animation; width, height, spacing, text size; live preview; profiles | UT 30; ST 38; `docs/test-logs/2026-11-11_M-SB2.md` | Glass is simulated from the wallpaper (no live blur on a click-through window). Swatches and RGB steps, no free colour picker; profile names are fixed |
| FAN-NBFC | Real fan control through the installed NBFC CLI; NBFC is the actuator only (D21, D22) | `docs/design/FAN_CONTROL_NBFC.md` | **Open: designed.** Register map matched to this PC's firmware | — | Live tests need owner approval (F1-F7) |
| FAN-2 | Manual mode: held speed, slider, per-fan only when the config supports it | Design | Open: designed | — | D23 applies |
| FAN-3 | Automatic mode: AnkhCore curve with smoothing, hysteresis, delays, faster rises than falls | Design | Open: designed | UT planned | Defaults to be tuned in coordinated tests |
| FAN-4 | SensorHub is the only temperature source; NBFC temperature never used | Design | Open: designed | UT planned | D22 |
| FAN-5 | UI: mode selector, temperatures, requested speed, measured RPM, NBFC status, unconfirmed warning | Design | Open: designed | — | — |
| FAN-6 | Safe transitions and failure handling; NBFC Auto/Stop not assumed to restore BIOS | Design | Open: investigation incomplete | Owner observation of one service stop (not a test) | P11, P12, F3-F9, H1-H10. S6 is a release blocker |
| FAN-7 | 40 % minimum in every mode, enforced in control logic and shown in the UI (D23) | Design | Open: designed | UT planned | Physical RPM at 40 % unmeasured (F8) |
| FAN-8 | Manual Thermal Override: trigger, saved target, banner, hysteresis, recovery, critical failure on invalid readings; selected mode vs control authority (D23) | Design | Open: designed | UT planned | Thresholds unvalidated (P12, F9). Does not replace BIOS protection |

## Desktop monitoring (master §5)

| ID | Requirement | Component | Status | Verification | Limitations / notes |
|---|---|---|---|---|---|
| MON-1 | Native integration under Monitor > Desktop Monitoring | `PageDesktop` | Partial | ST | See MON-11 |
| MON-2 | One data backend, no duplicate polling | SensorHub | Done | ST | — |
| MON-3 | Close hides to tray; monitoring continues; Exit is clean | `WndProc`, tray | Done | ST; HOST pending | Real sign-out is HOST (plan) |
| MON-4 | Tray menu: Open, Sidebar, Mini, **Pause/Resume, Settings**, Exit | `TrayMenu` (`src/app.cpp`) | **Done in the dev build (M-MON2):** `Pause monitoring` / `Resume monitoring` (`106`) and `Monitoring settings` (`107`) added to `TrayMenu` | ST (`docs/test-logs/2026-11-14_M-MON2.md`) | — |
| MON-5 | Sidebar: passive desktop-background layer (D40, changeset §4): no reserved space, covered by every window, no input or focus; transparency, edge docking, monitors, DPI, persistence. Overlay layers (top, normal, below windows) remain selectable | `SbFindDesktopHost`, `SbApplyStyle`, `SbBackdrop`, `PushLayered`; `src/sidebar_layout.h` | **Done in the dev build (M-DSK1):** child of the desktop beneath the icon view, blended over the wallpaper; one-time migration with restore | UT 9; ST 13; `docs/test-logs/2026-11-13_M-DSK1.md` | Not tested: full-screen games, multiple monitors, mixed DPI, a real Explorer restart, real icon dragging. Tile/Span wallpapers approximated |
| MON-6 | Sidebar: **height, custom position, auto-hide, lock position, smooth show/hide** | `src/sidebar_layout.h`, `SbPlace`, `SidebarProc` | **Done in the dev build:** height, position along the edge, drag to another screen or side when unlocked, lock, auto-hide, sliding on and off | UT 8; ST 16; `docs/test-logs/2026-11-11_M-WID1.md` | A real mouse drag and a second monitor are not run |
| MON-7 | Mini: enable, drag, top, click-through, transparency, compact/expanded, metrics, graphs, persistence, independence | Mini | Done | ST, UT | — |
| MON-8 | Mini: adjustable width and height, and full customization (D27-G) | `src/mini_layout.h`, `MiniRender`, `MiniPlace`, `PageMiniSet` | **Done in the dev build (M-WID1, M-MON2):** width, line height, overall size, corners, background and tint, accent and text colour, glow, independent `mini.*` settings, **built-in/custom profiles, and per-metric display styles (Value / Bar / Sparkline)** | UT 9 (`374/374`); ST (`PageMiniSet` 1038 clicks); `docs/test-logs/2026-11-14_M-MON2.md` | — |
| MON-9 | Metrics (§5.8): CPU, GPU, RAM, fans, network, uptime, OS | SensorHub, engine probes (`net_probe.h`, `sys_probe.h`, `gpu_probe.h`) | Partial | ST; UT; live GPU test `tools/gpu_verify.ps1` | Done: adapter selection and totals (M-NET1); per-core usage, memory detail, drives (M-SB1); **GPU utilization and VRAM per adapter from Windows, matched by LUID (M-GPU1, fixes the stuck reading)**. Missing: max clock; memory clock; drive type; battery health; per-adapter GPU temperature, clock and power (B02) |
| MON-10 | Graphs and bounded history (§5.9) | `MonitorStore`, `TelemetryEngine`, `PageDesktop` | **Done in the dev build (M-CFG1):** bounded in-memory ring buffers for CPU/GPU temp & load, RAM, Fan, Network Up/Down, and Disk Read/Write (`SDiskIo`) | UT (`380/380`), ST; `docs/test-logs/2026-11-14_M-CFG1.md` | — |
| MON-11 | Settings pages inside Desktop Monitoring (§5.11); hierarchical sub-pages Layout, Appearance, Widgets, Behavior, Profiles (D36) | `PageDesktop`, `PageMonPrefs`; plan `docs/design/UX_DESKTOP_PRIVACY_PLAN.md` §4 | **Done in the dev build (M-NAV1..3, M-CFG1):** Sidebar Layout / Appearance / Widgets / Behavior / Profiles, Mini Monitor, Monitoring Preferences, and `°C`/`°F` temperature unit switching (`ConvertTempUnit`) | UT (`380/380`), ST; `docs/test-logs/2026-11-14_M-CFG1.md` | — |
| MON-12 | Alerts (§5.10): CPU/GPU temperature, RAM, disk, sustained load, network | `AlertEngine` (`src/monitor_store.h`), `TelemetryEngine` (`src/engine.h`), `PageMonPrefs` | **Done in the dev build (M-MON2):** `cpu.warn`, `cpu.crit`, `gpu.warn`, `gpu.crit`, `fan.stop`, `ram.high`, `disk.low`, `cpu.load`, and `net.high` with hold, hysteresis, cooldown, and Windows-counter evaluation even without SensorHub | UT (`374/374`), ST, harness; `docs/test-logs/2026-11-14_M-MON2.md` | — |
| MON-13 | Dashboard Desktop Monitoring card with quick actions (§5.12) | `PageDashboard` (`src/app.cpp`) | **Done in the dev build (M-MON2):** `Desktop Monitoring` card with `Open`, `Pause`/`Resume`, `Settings`, live monitoring status, and Sidebar/Mini Monitor toggles & live previews | ST (`PageDashboard` 14 clicks); `docs/test-logs/2026-11-14_M-MON2.md` | — |
| MON-14 | Performance: low idle, no leaks (§5.13) | timer, Hub, engine probes | Done; open questions B05 and B09 are **not confirmed in real use** (D31) | ST idle repaint; owner observation on the installed preview: GPU about 1–2 % | Long-run leak test is HOST. Re-measure CPU on a quiet machine before any optimization |
| MON-15 | Readings freshness: fresh / stale / unavailable | SensorHub | Done | UT, ST | — |

## Appearance Center (master §6, §19)

| ID | Requirement | Component | Status | Verification | Limitations / notes |
|---|---|---|---|---|---|
| APP-1 | Appearance Center with Icon, Cursor, Theme and Animation pages | `PageAppearance`, `PageAppMaterial`, `PageAppTheme`, `PageAppWallpaper`, `PageAppCursor`, `PageAppIcons` | **Done in the dev build (M-NAV3):** Appearance hub + dedicated AnkhCore Appearance, Windows Theme Manager, Wallpaper Manager, Cursor Manager, and Icon Manager sub-pages | ST; `docs/test-logs/2026-11-14_M-NAV3.md` | Windhawk Anubis settings await WH-5 |
| APP-2 | Icon Manager. **Engine superseded by D41: runtime resource redirection through Resource Redirect 1.3** (see APP-8). The 2.2 7tsp patching stays only as legacy recovery code | `eng_icons.h`, `PageAppearance` | **Done in the dev build (M-ICON1):** switches prebuilt themes via Resource Redirect `themePaths`; legacy 7tsp/backup restore kept under secondary recovery buttons | UT 11; ST; `docs/test-logs/2026-11-14_M-ICON1.md` | Host `imageres.dll.mun` from 2.2 is still patched until owner restores (P04). Previews come from user packs (D10). |
| APP-3 | HUD iPack handler | `eng_icons.h` | Superseded by D41 (`hud-machine-blue` prebuilt Resource Redirect theme) | UT | — |
| APP-4 | Cursor Manager: discovery, role mapping, [SB]/[TB], scheme apply, backup | `src/cursor_scheme.h`, `src/eng_cursors.h`, `PageAppCursor` | **Done in the dev build (APP-4), not yet run against real cursor settings:** packs found in folders and among the schemes saved in Windows; 17 roles; tags and versions understood; preview; apply with the active scheme saved first, read-back and rollback; Restore previous; Restore Windows default; recovery point per `backup.autoBeforeChanges` | UT (`415/415`), ST (sandbox copy of the cursor key); `docs/test-logs/2026-11-14_APP-4.md` | Host apply, sign-out and restart persistence need owner approval. Open: per-role choice of alternates, `button.ani` in Azenis, pack import, `.inf` files, the `cursors` backup category. Packs are user-owned and never bundled (D10) |
| APP-5 | Theme Manager | `eng_theme.h` | 2.2 | — | As3aD/BIB theme from 2.2; assets under review |
| APP-6 | Animation Studio (UI vs Windows vs Windhawk animations) | — | Open | — | Depends on WH-5 for the Anubis settings |
| APP-7 | Customization stays a real user-facing feature (D16 note) | Appearance Center | Required | — | The core installer doesn't carry content, so in-app import/apply is the delivery path |
| APP-8 | Icon Theme Engine on Resource Redirect 1.3 (D41): development-time preparation and validation of each supplied pack, compatibility report, optional installer components, run-time Apply / switch / Restore Default by mod settings only, final mod in `D:\Ankhcore\Mods` | `tools/prepare_icon_themes.py`, `src/eng_icons.h`, `src/pe_rsrc.h`, `src/app.cpp`, `D:\Ankhcore\Mods\icon-resource-redirect.wh.cpp` | **Done in the dev build (M-ICON1):** all 11 packs built & verified (`8 supported`, `3 partial`, `239` PE modules, `0` errors); runtime apply/switch/restore via `windhawk-cli` `themePaths` with readback & rollback; optional installer UI in INS-3 | UT 14 (`329/329` in `test_base.cpp`); sandboxed Windhawk 2.0 compile & CLI test; `docs/test-logs/2026-11-14_M-ICON1.md` | Nothing enabled on the owner's PC without permission |
| VIS-7 | Official logo and Main Window background from `D:\Ankhcore\Ankhcore Desigen` and `D:\Ankhcore\Branding\Backgrounds\AnkhCore_Background_16x9.png` (D44) | `ui.h` `Logo`, main window base layer | **Done in the dev build:** unified transparent `logo.png` (owner, 2026-11-14, `3bc6d81`) in the rail (84 / 40 DIP), About (112) and the setup (240, 40), outer circuitry at reduced opacity (`src/brand_logo.h`); `ankh.ico` below 40 px; 16:9 background (`1672 × 941`) whole, centred and shown edge to edge across the window at 38 % over the tint | ST captures; asset gate; `docs/test-logs/2026-11-14_Branding_M-NAV1-2.md` | — |

## Windhawk Manager (master §7; D09, D18)

| ID | Requirement | Component | Status | Verification | Limitations / notes |
|---|---|---|---|---|---|
| WH-1 | Detect Windhawk, version, engine status; keep the engine running | `eng_system.h` | 2.2 | ST | 2.2 checks the service and reads mods from the registry |
| WH-2 | Installed mods: name, ID, version, author, status, description; search and filter | Windhawk Manager | Partial (2.2 list) | — | Design: `docs/design/WINDHAWK_MANAGER.md` |
| WH-3 | Enable/disable through a supported interface | Windhawk Manager | **Implemented in the dev build (FT-C02), not validated on the installed Windhawk:** enable/disable is staged and applied through `windhawk-cli`, verified by read-back (the 2.2 registry toggle is no longer the path) | UT; SB against an isolated Windhawk root; `docs/test-logs/2026-11-15_FT-3.md` | No live Apply (needs approval) |
| WH-4 | Install, update, uninstall mods | Windhawk Manager | Open | — | — |
| WH-5 | Dynamic settings editor: bool, number, text, enum, colour, arrays, nested, advanced; unknown keys preserved | Windhawk Manager | **Partial (FT-C01):** generated from the mod's declared schema: bool, enum, int, real, text, colour, nested groups as sections, unknown keys kept; **lists are shown read-only** (edit them in Windhawk) | UT 31; SB (real CLI, isolated root) | Array/list editing not implemented; not run on the installed Windhawk |
| WH-6 | Custom-storage mods identified, with a compatible strategy | Windhawk Manager | Open | — | Design §4 |
| WH-7 | Status and diagnostics: version, engine, counts, last result, reload needed | Windhawk Manager | Partial | — | — |
| WH-8 | Install Windhawk (supplied tested build) when absent; hide its tray icon through a supported setting; keep a launch/repair path | Windhawk Manager | Open | — | **Defect:** the current page says "Install AnkhCore again to get Windhawk" but the core installer no longer installs it (BACKLOG B01) |
| WH-9 | Backup of the Windhawk configuration | Backup Center + `windhawk-cli data export` | Open | — | — |

## Backup & Migration (master §8)

| ID | Requirement | Component | Status | Verification | Limitations / notes |
|---|---|---|---|---|---|
| BAK-1 | Full, settings-only, custom and portable `.acbackup` with manifest and SHA-256 | `src/acbackup.h`, `src/json_min.h`, `src/eng_backup.h`, `src/app.cpp` (`PageBackup`); `docs/design/BACKUP_MIGRATION.md` | **Done in the dev build (M-BAK1..5):** container, manifest, streamed SHA-256 (`AcbHashFile`), validation (`AcbVerify`), `AcbCreateBackup` (`settings`, `monitoring`), `SerializableBundle` v3 (`AcbBuildUbundle`), `AcbBuildAppInventoryJson`, and `PageBackup` UI | UT 71 (`372/372`), ST (`page_28.png`); `docs/test-logs/2026-11-14_M-BAK3-5_B04.md` | Live `uniget.exe` execution awaits Q2; per-application `appdata` map awaits Q5 |
| BAK-2 | Restore modes, preview, validation, rollback | `src/acbackup.h` (`AcbVerify`, `AcbSelect`), `src/eng_backup.h` (`AcbRestoreBackup`, `AcbMergeSettings`, `AcbPlanAppRestore`), `src/app.cpp` (`PageBackup`, `PageRecHistory`); design §5 | **Done in the dev build (M-BAK1..5):** full validation before write, recovery point (`Recovery\<timestamp>\`), `settings-merge` with range checks & D30 fan-control safety, atomic `file-copy`, reverse-order rollback on failure, `AcbPlanAppRestore`, `restore-report.txt`, and `PageBackup` / `PageRecHistory` UI | UT, ST; `docs/test-logs/2026-11-14_M-BAK3-5_B04.md` | — |
| BAK-3 | Automatic backup before major changes | journal (2.2), `src/eng_backup.h` (`AcbCreatePreChangeRecoveryPoint`, `AcbListRecoveryPoints`), `src/app.cpp` (`PageBackup`, `PageRecHistory`) | **Done in the dev build (M-BAK2, M-BAK5):** 2.2 journal + Undo, `Recovery\<timestamp>` recovery-point creation (`AcbCreatePreChangeRecoveryPoint`), `backup.autoBeforeChanges` policy selector (`Always back up` / `Ask first` / `Never`), and `Recovery History` inspection | UT, ST; `docs/test-logs/2026-11-14_M-BAK3-5_B04.md` | — |
| BAK-4 | Application migration plus UniGetUI bundles | `src/eng_backup.h` (`AcbDetectUniGetUI`, `AcbClassifyUniGetExit`, `AcbParseUbundle`, `AcbBuildUbundle`, `AcbBuildAppInventoryJson`), design §2, §6 | **Done in code & unit tests (M-BAK3):** UniGetUI detection (`uniget.exe` beside `UniGetUI.exe`, `ForceUniGetUIPortable`), exit-code classification (`0..4`), `SerializableBundle` (`export_version: 3`) parser/writer, and `inventory.json` merger | UT (`372/372`); `docs/test-logs/2026-11-14_M-BAK3-5_B04.md` | Live `uniget.exe` binary not run on host until Q2 is approved |
| BAK-5 | Application Updater through UniGetUI (D27-E) | `src/eng_backup.h` (`AcbDetectUniGetUI`, `AcbClassifyUniGetExit`), design §6 | **Partial (M-BAK3):** detection, exit-code handling, and UI status card implemented | UT, ST | Live `uniget.exe show-updates` awaits Q2 approval |
| BAK-6 | Application backup and restore: package-based reinstall, then data restore (D27-E) | `src/eng_backup.h` (`AcbPlanAppRestore`, `AcbBuildUniGetInstallArgs`), design §6 | **Done in code & unit tests for package restore plan (M-BAK4):** classifies `AlreadyInstalled`, `OfferUniGetInstall` (with version-mismatch guard), and `ManualInstallRequired` (`kAcbManualInstallMsg`), and builds safe `uniget.exe install` args without `--skip-hash` | UT (`372/372`); `docs/test-logs/2026-11-14_M-BAK3-5_B04.md` | Per-application `appdata` map awaits Q5 |
| BAK-7 | Registry and application configuration backup where supported (D27-E) | Design §6 | Open: designed (M-BAK0) | — | Mapped HKCU values only; never whole hives or system keys |

## Monitoring and fans (master §9)

| ID | Requirement | Component | Status | Verification | Limitations / notes |
|---|---|---|---|---|---|
| HW-1 | Preserve the hardware monitor; LHM + PawnIO; source and availability shown | AnkhSensors, SensorHub | Done | ST, FP; HOST pending | — |
| HW-2 | Fan control: real control only, read-back, safe fallback | FanPolicy | Done (logic); **switched off in the app (`kFanControlEnabled = false`) until P11 and P12 are decided** | FP; UT | Monitoring only in 2.5 Preview 1. This model: control only through NBFC (D21, D22, amending D05) |
| HW-3 | Helper lifecycle: start, health, quit, no orphans | SensorHub | Done | ST; HOST pending | — |
| HW-4 | Try Fan Compatibility: user-triggered, one candidate at a time, safety gates, RPM proof through SensorHub (D42) | `src/eng_fan.h` (`DiscoverFanCompatCandidates`, `FanCompatTrial`), `src/app.cpp` (`PageFan`) | **Done in code & unit tests (HW-4):** candidate discovery across 243 local XML configs, 40% duty clamp (D23), single-candidate concurrency guard, continuous SensorHub thermal/freshness gates, user Stop, and attributable SensorHub RPM proof; gated by `kFanControlEnabled = false` (D30) | UT 23 (`352/352` in `test_base.cpp`), FP (`22/22`); `docs/test-logs/2026-11-14_HW-4.md` | No hardware run without owner approval (D30) |
| HW-5 | GPU usage of AnkhCore itself measured per state and reduced where wasteful, Full Glass kept (D43) | `gpu_util.h`, `app.cpp` (`ANKH_PERF`) | **Done (M-GPU2):** per-PID GPU, CPU, paint rate and frame time measured across 12 baseline + 6 active-repaint states (`0.00%` idle, `0.07%–0.42%` Main Window materials, `0.28%` Sidebar, `0.19%` Mini, `0.52%–1.08%` full motion; installed Preview 1 `0.25%–0.30%`); no wasteful path found, Full Glass preserved untouched | `docs/test-logs/2026-11-14_M-GPU2.md` | — |

## System management (master §10)

| ID | Requirement | Component | Status | Verification | Limitations / notes |
|---|---|---|---|---|---|
| SYS-1 | Tweaks in categories, with explain, state, confirm, verify and rollback | `src/eng_system.h` (`Tweaks()`), `src/app.cpp` (`PageTweaks`) | **Done in the dev build (M-SYS1):** `Performance`, `Gaming`, `Privacy`, and `Power & Storage` categories, state chips (`Applied` / `Default`), confirm, post-change `isOn()` verification, and journal rollback | UT (`377/377`), ST (`PageTweaks` 89 clicks); `docs/test-logs/2026-11-14_M-SYS1.md` | — |
| SYS-2 | Windows Repair: SFC/DISM/CHKDSK with real output | `PageOptimize` | 2.2 | ST | — |
| SYS-3 | Startup Manager | `PageOptimize` "Startup apps" | 2.2 | ST | — |
| SYS-4 | Driver Manager | `src/eng_system.h` (`EnumSignedDrivers`), `src/app.cpp` (`PageDrivers`) | **Done in the dev build (M-SYS1):** read-only `Win32_PnPSignedDriver` inventory, `All` / `Third-party` / `Unsigned` filters, and report export | UT (`377/377`), ST (`PDrivers` captured); `docs/test-logs/2026-11-14_M-SYS1.md` | — |
| SYS-5 | Storage Analyzer | `src/eng_system.h` (`AnalyzeStorageRoots`), `src/app.cpp` (`PageStorage`) | **Done in the dev build (M-SYS1):** live fixed-drive capacity breakdown and read-only user/temp folder scan | UT (`377/377`), ST (`PStorage` captured); `docs/test-logs/2026-11-14_M-SYS1.md` | — |
| SYS-6 | System Cleanup with preview | `PageOptimize` "Cleanup" | 2.2 | ST | — |
| SYS-7 | Apps: inventory, uninstall, Store apps | `PageApps`, `PageAppsInstalled`, `PageAppsBuiltin`, `PageAppsUninstall` | **Done in the dev build (M-NAV3):** Apps hub + dedicated Installed Applications, Windows Built-in Apps, and Uninstall Manager sub-pages with filters | ST; `docs/test-logs/2026-11-14_M-NAV3.md` | — |

## Configuration, logging, security (master §11–13)

| ID | Requirement | Component | Status | Verification | Limitations / notes |
|---|---|---|---|---|---|
| CFG-1 | Versioned, validated, atomic settings | `settings.h` | Done | UT | — |
| CFG-2 | Profiles (Gaming / Work / Minimal) | `PageMonPrefs` (`ApplySystemProfile`) | **Done in the dev build (M-CFG1):** `Default` / `Gaming` / `Work` / `Minimal` system profiles in Monitoring Preferences | ST; `docs/test-logs/2026-11-14_M-CFG1.md` | — |
| CFG-3 | One source of truth across main window, tray and widgets | snapshot, `Cfg()` | Done | ST | — |
| LOG-1 | Operation history and error logs | journal, log (`LogSyncSharedLocked`) | **Done in the dev build (B04, M-BAK5):** shared `<DataDir>\Logs\ankhcore.log` lines from the engine process appear live on the Activity page, and `Recovery History` displays both recovery points and the undo journal | ST (`B04` check); `docs/test-logs/2026-11-14_M-BAK3-5_B04.md` | — |
| LOG-2 | Honest progress (no fake percentages) | `src/op_progress.h`, `DrawProgress`, `OpLines`, `eng_icons.h`, `setup.cpp` `InstallCore` | **Done in the dev build (LOG-2):** every bar is measured items, counted steps ("Step k of n") or a travelling band for unknown length; no hand-picked numbers; 100 only at the end | UT (`428/428`), sandbox (`53/53`), ST; `docs/test-logs/2026-11-14_LOG-2.md` | No live SFC/DISM/chkdsk run. Separate finding, not changed: several operations report success unconditionally (see the log §4) |
| LOG-3 | Diagnostic export | `AcbExportSupportBundle` (`src/eng_backup.h`), `PageActivity` | **Done in the dev build (M-CFG1):** `Export log` + `Export support bundle (.zip)` packaging system summary, logs, settings, and network counters into a verified ZIP | UT (`380/380`), ST; `docs/test-logs/2026-11-14_M-CFG1.md` | — |
| SEC-1 | Least privilege: non-elevated UI plus elevated broker | — | Open | — | The UI still runs elevated (2.2 design) |
| BG-1 | X button hides to tray; monitoring, Sidebar and fan control continue (D24) | `WM_CLOSE`, engine | Done for monitoring and Sidebar in the dev build | Harness | Fan control does not exist yet |
| BG-2 | Background telemetry, alerts and fan engine independent of the main window | `src/engine.h`, `src/engine_process.h` | Done (telemetry and alerts). **First production run: the installed Preview 1 logs "Background engine started" and runs as two `AnkhCore.exe` plus `AnkhSensors.exe`** | ST; harness; HOST (log, 2026-11-11) | Fan engine pending P11, P12 (D30). Sign-out, sleep and resume not yet observed |
| BG-3 | Tray Exit with a documented lifecycle and no orphans (D26) | Tray 103, engine | Done in the dev build | Harness; sandbox | NBFC handover needs P11 |
| BG-4 | UI ↔ engine IPC with authentication | `src/ipc.h`, `src/engine_process.h` | Done in the dev build (protocol version 4 carries the network adapters, the system detail and the graphics adapters) | UT 17; harness | D23 enforcement arrives with fan control |
| PRIV-1 | No location access: no interface Windows treats as location, no substitute tracking (D34) | `src/net_probe.h` (RSSI query), `tools/check_privacy.py` | **Done in the dev build (M-PRIV1).** Root cause: the engine's Wi-Fi current-connection query every 5 s | UT 3; ST 1; privacy gate in `tools\test_base.cmd`; `docs/test-logs/2026-11-12_M-PRIV1-PERF1.md` | Not yet confirmed in Windows' Location > Recent activity: the installed Preview 1 predates the fix |
| SEC-2 | File, registry and third-party safety | `install.h`, setup, guards | Partial | UT, SB | — |
| SEC-3 | No false success: post-operation verification | `src/op_result.h` (`OpTally`, `UndoCount`), `JournalUndoEx`, `UndoOp`, `eng_system.h`, `eng_theme.h`, `eng_icons.h` | **Done in the dev build (SEC-3) for the operations in the log:** success only when every attempted part succeeded and the read-back matches; partial failure is Failed with counts | UT (`436/436`), ST (undo, registry tweak, startup switch on the test key); `docs/test-logs/2026-11-14_SEC-3.md` | Services, Windhawk, power, wallpaper, icons and the network tools are changed by code review only (not runnable in the test build). Uninstall path not reviewed |

## Installer (D16, D17)

| ID | Requirement | Component | Status | Verification | Limitations / notes |
|---|---|---|---|---|---|
| INS-1 | Core installer: verified payload; upgrade with backup; repair; rollback; downgrade refused; data kept | `setup.cpp`, `install.h` | Done (sandbox). **2.5 Preview 1 (2.5.0.1) installed by the owner on 2026-11-11 (fresh install, finished in 1 s, no errors in the log)** | UT, SB 56 at Preview 2 (incl. uninstall, reinstall, upgrade over 2.5 and over Preview 1 2.5.0.1, `docs/test-logs/2026-11-14_Preview2.md`); HOST: one fresh install by the owner. **2.5 Preview 2 (2.5.0.2) built 2026-11-14, tag `v2.5.0-preview2`, not installed by any session** | Not run on the host: upgrade with backup, rollback, uninstall. No clean-machine run (no Windows Sandbox on this PC) |
| INS-2 | Release asset gate (payload and resources) | `pack_payload.py`, `check_release_assets.py` | Done | build | Licence confirmation pending (P05) |
| INS-3 | Optional installer components: prebuilt icon themes (D41) and the Taskbar Profile (Windows 11 only, unchecked by default, backup, summary line; D44) | `src/install.h`, `src/setup.cpp` | **Done in the dev build (INS-3):** `[ ] Apply AnkhCore Taskbar Profile (Windows 11 only)` unchecked by default, gated by `win11()`, validated `.reg` parser, pre-apply backup & restore, clear summary line; optional prebuilt icon themes copied & validated without conversion or auto-apply | UT 4 (`356/356`), SB 50 (`50/50` in `--sandbox-test`); `docs/test-logs/2026-11-14_INS-3.md` | — |
| INS-3b | The installer never writes a program file in place: the new file is written beside its target, flushed and checked against the package SHA-256, then swapped in with one atomic replace; a cut write, a wrong hash or a refused swap leaves the previous file and the previous version intact | `base.h` `PlaceFile`, `setup.cpp` | **Done in the dev build, uncommitted** | UT 7 (`test_base`), SB 5 (`D2`) | No real install; no process-kill test |
| SEC-3b | The change journal keeps every entry when an undo rewrites it: an entry appended meanwhile (by an undo handler or another writer) is kept; ids stay unique and ascending; each picked entry is undone once | `base.h` `JournalUndoEx` | **Done in the dev build, uncommitted** | UT 3 (threads, foreign append, id continuation) | No second writer exists today; no cross-process lock |
| RUN-1 | Programs run with a timeout are cut off when the timeout passes, also when they print nothing | `base.h` `RunCapture` | **Done in the dev build, uncommitted** | UT 3 | - |
| FT-A04b | The icon status cache serves every refresh request, runs one worker at a time, and stays usable after a failed refresh | `icon_status.h` | **Done in the dev build, uncommitted** | UT 5 (deterministic interleavings) | - |

## Preview 2.1 field findings (FT-*; added 2026-11-15)

Source: the owner's findings file `D:\Ankhcore\AnkhCore_Preview2.1_Field_Testing_Quality_Improvement_Master.txt`. These rows add to the matrix and change no existing row. Status as of 2026-11-17: every row below is implemented in the working tree (uncommitted) and automated-tested; **none is verified on the owner's Windows** (WIN column of the acceptance matrix). Per-level verification: `docs/preview2.1/PREVIEW_2_1_ACCEPTANCE_MATRIX.md`.

| ID | Requirement | Component | Status | Verification | Limitations / notes |
|---|---|---|---|---|---|
| FT-A01 | Icon theme apply never requires a Windows restart; refresh by notification, optional confirmed Explorer restart, at most once | `icon_refresh.h`, `eng_icons.h`, `app.cpp` PageAppIcons | **Done in the dev build (FT-2), awaiting approval** | UT 26 checks (`test_base` 479/479), ST (no restart prompt, 374/374 clean run), SB (Apply / switch / Restore against an isolated Windhawk root); `docs/test-logs/2026-11-15_FT-2.md` | The real apply, the shell notification's effect and Restart Explorer are refused by the test build: owner check on Windows pending. 2.2 file-swap operations still ask for a Windows restart |
| FT-A02 | Apply button on every theme card, states Installed / Active / Pending / Not installed | `app.cpp` PageAppIcons, `icon_status.h` | **Done in the dev build (FT-3), awaiting approval** | UT 10; ST page walk | A real apply not run |
| FT-A03 | Original theme preview images, aspect kept, placeholder if none | `app.cpp`, `icon_status.h`, `thumb_cache.h` | **Done in the dev build, awaiting approval** | UT (thumbs 43) | Candy has no original: neutral text |
| FT-A04 | Icon Manager responsive: async thumbnails, cache, lazy loading, no full-theme reads on the page | `icon_status.h`, `thumb_cache.h`, `app.cpp` | **Done in the dev build (FT-2), awaiting approval** | UT (`test_base`, `tools\test_thumbs.cmd` 42/42), ST, measurement `tools\bench_icon_manager.cmd` | Scrolling frame times on screen not measured. Shows each theme's own preview in the existing cover-crop placement (FT-A03 completes the fit and placeholder) |
| FT-B01 | Wallpaper previews in the source's orientation, EXIF orientation 1-8 | `gfx.h` DibBitmap (committed), `ui.h` DecodeImage | **Done (FT-1 committed; EXIF in the dev build)** | UT: `tools\test_image.cmd` 701/701 | Not verified on Windows. Closes B16 |
| FT-C01 | Windhawk mod settings editable in AnkhCore, generated from the real schema | `wh_schema.h`, `eng_windhawk.h`, `app.cpp` | **Done in the dev build, awaiting approval** | UT 31; SB: real CLI in an isolated root | Overlaps WH-5. Not run on the installed Windhawk |
| FT-C02 | Staged Windhawk changes, explicit Apply, verified result, one restart per Apply | `wh_stage.h`, `eng_windhawk.h`, `app.cpp` | **Done in the dev build, awaiting approval** | UT; SB (isolated root); ST | Overlaps WH-3. No live Apply |
| FT-D01 | Sidebar recovers from loss of its desktop layer without duplicates | `sidebar_layout.h`, `app.cpp` | **Done; accepted provisionally by the owner, not reopened** | UT 12; ST 8 | Cause of the original loss not established |
| FT-E01 | Main Window background softer, identity kept | `brand_logo.h`, `app.cpp` | **Done in the dev build** | UT value check; ST contrast probes | Owner's look pending |
| FT-F01 | Azenis Normal Select from an existing Azenis file | `cursor_scheme.h` | **Done in the dev build** | UT 3 | Owner confirms the file on screen |
| FT-F02 | Mouse Trail lighting controlled in AnkhCore; per-role effects only where the mod supports them (it does not) | Windhawk Manager | **Done in the dev build** | mod source analysis; staged path tests | The mod's behaviour unchanged |
| FT-G01 | Installer lists and installs components from its own package; Taskbar Profile off by default; honest summary; Options page reachable | `setup.cpp` | **Done in the dev build** | SB 81/81; captures | No real install |

## Windows tweaks, storage cleaner, SMART diagnostics and packs (added 2026-11-15)

Status of every row: **implemented in the working tree, not committed**, awaiting the owner. Hardware-dependent rows are not verified on hardware.

| ID | Requirement | Component | Status | Verification | Limitations / notes |
|---|---|---|---|---|---|
| WT-01 | Ten further Windows tweaks (22 in total), each with a state check and undo | `src/eng_system.h` (`Tweaks()`) | Implemented (earlier work) | `tools\test_tweaks.cmd` | Not re-reviewed in this pass |
| WT-02 | Advanced Storage Cleaner that is safe by construction: canonical protected paths, no double counting, `DeleteFiles` with the same guards, no entry through a swapped junction, "Clean selected" limited to the shown category | `src/eng_system.h` (`CleanCanonicalPath`, `IsProtectedCleanupPath`, `SafeCleanDir`, `DeleteFiles`, `CleanSelectedInScope`), `src/app.cpp` (`DrawCleanupSection`) | Implemented; audit findings M1, M2, L1, L2, L3 fixed | `tools\test_cleaner.cmd`; capture `WT02_optimize_cleanup_section.png` | No real cleanup run on the host. A top-level cleanup folder that is itself a junction is still entered (only nested entries are re-checked) |
| WT-03 | SMART disk diagnostics: read-only `smartctl -a -j`, health states Good / Warning / Critical / Unknown / Not supported, advisory findings kept apart from drive-reported ones, Windows-only fallback | `src/eng_smart.h`, `src/base.h` (`SmartctlArgsAreReadOnly`), `src/app.cpp` (`DrawSmartSection`) | Implemented | `tools\test_smart.cmd` (44, fixtures); UI self-test (fixture captures) | **Never run against a real drive or a real smartctl.** Advisory limits 70 °C / 20 % life / 20 % spare are AnkhCore rules of thumb, not hardware standards, not configurable yet, and a result that rests only on them is never Critical |
| WT-03a | smartctl runs only from `<AppDir>\Tools\smartctl.exe` in production (never from the user-writable data folder), only with a closed list of read-only arguments, never a self-test | `src/eng_smart.h` (`SmartctlCandidateDirs`, `SmartExePathOk`), `src/base.h` | Implemented | `tools\test_smart.cmd` | The installed file is checked by the setup (SHA-256) but not re-hashed at run time |
| WT-03b | Optional installer component `Tools\smartctl.exe` + `Tools\COPYING.smartmontools`: both or neither, each in `COMPONENTS.sha256` and unchanged, 64-bit program, GPL text; clean core-only install when absent; removed with the program folder | `src/setup.cpp`, `src/install.h`, `tools/pack_components.py` | Implemented | `--sandbox-test` (19 checks, stand-in files) | **No official binary approved or bundled.** The Options row is shown only when the package carries the component |
| AST-1 | Icon packs: 18 registered, previews from each pack's own `Extra\preview.*` | `src/eng_icons.h`, `tools/prepare_icon_themes.py` | Implemented | `test_base` | Redistribution rights not verified; not applied to Windows |
| AST-2 | Candy preview from the owner-supplied image, unmodified | `recovered_res/candy-icons.png`, `IconThemes\candy\preview.png` | Implemented | `test_base` | In the theme folder, not in `AnkhCore.exe` |
| AST-3 | Azenis: `Arrow.ani` maps directly; `scursor_green` maps from its `install.inf`; new cursor packs are found by folder | `src/cursor_scheme.h` | Implemented | `test_base` | Cursor packs are never packaged; repeated folders listed in `docs/ASSETS.md` |
| AST-4 | Owner-modified `brand_background.png` in the build | `recovered_res/brand_background.png` | Implemented | built exe resource SHA-256 equals the image | Source image in `build\res` was not overwritten |

