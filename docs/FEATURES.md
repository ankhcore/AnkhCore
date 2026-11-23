# AnkhCore Control Center

## Complete Feature & Options Reference

> **Purpose:** Product-wide reference to the features, modules,
> settings, and user-facing options documented for AnkhCore Control
> Center. This is not tied to a particular release and is not a
> changelog.
>
> **Scope:** This consolidates the supplied project README. It does not
> claim every option has been independently verified on every Windows
> version or hardware model. Where behavior depends on Windows,
> hardware, policy, or third-party components, that dependency is
> stated.

------------------------------------------------------------------------

# 1. Product Overview

AnkhCore Control Center is a Windows desktop application combining
system monitoring, maintenance, customization, application management,
and recovery tools.

Its documented product areas are:

-   **Monitoring:** CPU, graphics, memory, storage, network, fans,
    temperatures, desktop Sidebar, and floating mini-monitor.
-   **System Care:** read-only scan for supported cleanup candidates,
    broken shortcuts, application updates, and Windows updates.
-   **Windows customization:** themes, wallpapers, cursors, icon packs,
    taskbar and Explorer styling, and window animations through
    Windhawk.
-   **Application management:** installed-app inventory, uninstall
    workflows, and application updates.
-   **Recovery:** recovery points, backups, selective restore, and an
    activity journal with undo support.

The application is documented for 64-bit Windows 10 and Windows 11. Some
individual features have narrower Windows-version requirements.

# 2. Main Navigation

The main navigation contains:

1.  Dashboard
2.  Monitor
3.  Sidebar
4.  Windows Studio
5.  Tweaks
6.  Optimize & Repair
7.  Apps Manager
8.  Recovery
9.  Activity
10. About

The Settings control opens the application's configuration categories.

# 3. Dashboard

The Dashboard provides a summary of system state and quick access to
common actions.

-   **Anubis System Health:** reflects the last scan result, a cleanup
    preview, and the first problem found.
-   Live summary tiles for CPU, graphics, memory, storage, network, and
    fans.
-   Quick actions.
-   Scan actions route to System Care.

# 4. Monitor

## 4.1 System Monitor

-   Live system readings.
-   Graphs.
-   Historical monitoring data.

## 4.2 Hardware Information

-   Hardware inventory and information.
-   Visibility into which sensors AnkhCore can read.
-   Hardware report.
-   Device profile.

## 4.3 Network Monitor

-   Network adapters.
-   Network speed.
-   Transfer totals.
-   Wi-Fi signal information.

## 4.4 Fan Control

AnkhCore includes its own fan-control engine.

Documented options and behavior:

-   Shows which fans are present.
-   Distinguishes between values that can be **read** and fans that can
    be **controlled** on the current PC.
-   Includes 243 built-in notebook configurations.
-   Lets the user choose and confirm a configuration for the computer
    model.
-   Provides a minimum fan-speed control that increases with
    temperature.
-   Provides a thermal override at **92°C for 8 seconds**.
-   Provides **Return to automatic** to return control to the BIOS.
-   Does not independently lower fans below the hardware's own behavior.

Hardware limitations:

-   Actual fan writes depend on the notebook's embedded controller.
-   Some Lenovo Legion, ASUS, MSI, and Acer models may not expose a
    supported driverless reader.
-   Unsupported hardware should be reported as not exposed, with the
    reason.
-   Reading sensor values and writing fan settings are separate
    capabilities.

## 4.5 Startup & Background

-   Start with Windows.
-   Start in the system tray.
-   Background monitoring engine.

## 4.6 Monitoring Preferences

-   Sampling frequency.
-   Amount of history to retain.
-   Alerts and thresholds.
-   Toast notifications.
-   Tray notifications.

# 5. Sidebar and Mini-Monitor

The Sidebar is a desktop widget docked to a screen edge. It is designed
to remain part of the desktop without reserving screen space or taking
mouse input.

## 5.1 Sidebar Sections

Available sections:

-   CPU
-   Graphics
-   Memory
-   Storage
-   Network
-   Fans

Each section supports detail levels:

-   Minimal
-   Standard
-   Detailed

Each section can use a visual style:

-   Indicators
-   Gauges
-   Numeric values
-   Graphs

The Network section supports separate download and upload presentation.

## 5.2 Sidebar Layout and Appearance Options

Documented options:

-   Choose visible sections.
-   Change section order.
-   Width.
-   Height.
-   Screen edge / docking edge.
-   Target screen / display.
-   Opacity.
-   Background material: Solid, Frosted, Acrylic.
-   Tint.
-   Accent colors.
-   Text size.
-   Spacing.
-   Glow.
-   Animation.
-   Display layer: Desktop (default), Below windows, Normal, Always on
    top.
-   Saved appearance profiles.

## 5.3 Mini-Monitor

-   Small floating system readout.
-   Has its own appearance configuration.

# 6. Windows Studio

Windows Studio groups Windows appearance and shell customization
features.

## 6.1 Appearance & Themes

### Theme Manager

-   Includes documented Windows themes in the BIB and BIB3 families.
-   Theme application itself is documented as Windows 11-only.

### Wallpaper Manager

-   Includes Anubis / AnkhCore wallpapers.
-   Supports wallpaper shuffle.
-   Supports an interval setting.

### Cursor Manager

-   Provides AnkhCore cursor schemes.

### Icon Manager

-   Provides icon themes.
-   Applies icon themes through Resource Redirect.

## 6.2 Windhawk

Windhawk is integrated as a tool managed through AnkhCore.

Documented integration behavior:

-   No automatic Windhawk update.
-   No additional Windhawk shortcuts.
-   Lists available mods in collapsible categories.
-   Provides mod settings and staged application.

The documented mod catalog includes:

1.  Anubis - Windows Animations
2.  Icon Resource Redirect
3.  Translucent Windows
4.  Windows 11 Start Menu Styler
5.  Notification Center Styler
6.  File Explorer Styler
7.  Custom Corner Radius
8.  Dynamic Island for Windows
9.  Taskbar Clock Customization
10. Taskbar Dock Animation
11. Taskbar Notification Icon Spacing
12. Taskbar Start Button Colorizer
13. Taskbar Button Click
14. WinTips OSD
15. Overhauled Alt-Tab
16. Slick Window Arrangement
17. Mouse Trail
18. UxTheme Hook
19. File Operation Styler
20. Explorer Tree Item Tweaker
21. Win11 Power Buttons
22. WinUI Context Menu Animation

### Anubis - Windows Animations

The documented animation system includes:

-   Numbered window animation effects.
-   Reality Effects.
-   A signature library.
-   Random packs.

The exact animation catalog and configuration controls may depend on the
installed Anubis mod build.

## 6.3 PowerToys

-   Setup can install Microsoft PowerToys through an installation
    option.
-   Setup skips installation when PowerToys is already installed.
-   AnkhCore provides controls for 16 PowerToys utilities.
-   The integration changes each utility's `enabled` flag.
-   A backup is retained.

## 6.4 After Dark for Windows 10

-   Setup detects the Windows 10 release.
-   Documented release range: 1607 through 22H2.
-   Installs a matching After Dark pack where one exists.

# 7. Tweaks

The Tweaks area provides 29 documented reversible Windows behavior
tweaks.

-   Each tweak describes what it changes.
-   Changes can be undone.

The individual names and per-tweak values are not enumerated in the
supplied README and should be documented from the current in-app catalog
or source configuration before being added to this reference.

# 8. Optimize & Repair

## 8.1 System Care

System Care provides one read-only Smart scan for supported maintenance
findings.

Documented scan categories:

-   Cleaner items, including caches and temporary files.
-   Eight application caches.
-   Broken shortcuts.
-   Application updates.
-   Windows updates.

### Finding Classification

Each result can include classification, evidence, size, and an Ignore
action.

Documented classes:

-   Cleanable
-   Review required
-   In use
-   Protected

### Scan and Cleanup Behavior

-   Scanning does not delete files.
-   Cleanable items start selected.
-   Review required items do not start selected.
-   Space is counted only after a second measurement.
-   No registry cleaning is performed.
-   Documents, Downloads, photos, passwords, and browser sessions are
    not touched.
-   Broken shortcuts are moved to a recovery folder and can be undone.

### System Care Actions

-   **Clean Selected:** cleans only the items explicitly selected by the
    user.
-   **Update All / Fix All:** performs only the supported safe actions.
-   **Ignore:** excludes a finding from the user's active attention.

## 8.2 Windows Updates

Windows Update support is integrated into System Care.

Documented capabilities:

-   Detect updates offered by Windows Update.
-   Let the user select individual updates.
-   Download only the updates selected by the user.
-   Show download progress.
-   Allow cancellation.
-   Run downloads in the background.

Explicit limitations:

-   AnkhCore does not install Windows updates.
-   AnkhCore does not restart Windows for Windows Updates.
-   AnkhCore does not change Windows Update settings, policies, or
    services.

## 8.3 Process Manager

-   Displays processes.
-   Allows priority changes.
-   Allows CPU-core selection.
-   Supports efficiency mode.
-   Can end an ordinary application.
-   Supports automatic rules.
-   Provides Restore.

Critical and security processes are documented as protected from these
actions.

## 8.4 Drivers

A Drivers area is present under Optimize & Repair. The supplied README
does not enumerate its individual controls or supported driver
operations.

## 8.5 Storage

-   Disk health information.
-   SMART information through the bundled smartctl 7.5 tool.

## 8.6 Windows Tools

Provides access to Windows tools, including:

-   DISM
-   SFC
-   Other Windows utilities

## 8.7 Repair

A Repair area is present. The supplied README does not enumerate all
repair actions or their individual options.

# 9. Apps Manager

## 9.1 Installed Apps

-   Lists installed programs.

## 9.2 Built-in / Store Apps

-   Lists built-in and Store applications.

## 9.3 Uninstall Manager

-   Allows multiple programs to be selected.
-   Runs selected uninstallers one after another using each program's
    own uninstaller.
-   Cleans supported leftovers after uninstall.
-   Uses a recovery folder.
-   Supports Undo for recoverable cleanup actions.

## 9.4 App Updates

Application updates for other programs are handled through a private
UniGetUI session.

Documented options:

-   Update All.
-   Update programs individually.
-   Create a recovery point first.
-   Verify updates afterwards.
-   Cancel between programs.
-   Skip this version.
-   Always skip.
-   Restore skip settings.

This application-update workflow is separate from Windows Update
downloading.

# 10. Recovery

## 10.1 Backup & Migration

-   Creates `.acbackup` backups.
-   Supports selective restore.

## 10.2 Recovery Points

-   Creates and manages recovery points for supported reversible
    changes.

## 10.3 Recovery History

-   Maintains a journal of changes.
-   Supports Undo for supported operations.

# 11. Activity

-   Operation history.
-   Error log.

# 12. Settings

The Settings area has seven documented categories:

1.  General
2.  Appearance
3.  Performance
4.  Notifications
5.  Updates
6.  Privacy
7.  Advanced

## 12.1 Appearance Settings

AnkhCore Appearance controls include:

-   Window material: Acrylic, Frosted, Wallpaper Glass, Solid, Clear
    Glass.
-   Tint.
-   Blur.
-   Glow.
-   Motion.
-   Battery behavior.

### Material Behavior

-   Acrylic or frosted materials are used where supported by Windows 11
    22H2 or later.
-   Simulated wallpaper glass is used on older Windows versions.
-   Solid appearance is used when transparency is disabled, high
    contrast is active, or Battery Saver requires it.
-   Clear Glass makes the window itself see-through without applying
    blur to the entire window; wallpaper blur is applied lightly inside
    cards.

Other Settings categories are present, but the supplied README does not
enumerate every individual setting within General, Performance,
Notifications, Updates, Privacy, and Advanced.

# 13. Installer

The installer is documented as supporting:

-   Fresh installation.
-   Update.
-   Repair.
-   Clean installation.
-   Rollback through `--rollback`.

## 13.1 Installation Flow

Documented pages:

1.  Welcome
2.  Options
3.  Install
4.  Completion

The completion page provides:

-   Restart Later (selected by default).
-   Restart Now.
-   Finish.

The installer must not restart Windows automatically.

## 13.2 Installation Package Integrity

The documented package includes component directories and a
`COMPONENTS.sha256` manifest. Component files are checked against the
manifest before installation.

The README identifies these package components:

-   Windhawk
-   IconThemes
-   Mods
-   WindowsAssets
-   AfterDark
-   PowerToys
-   Tools

## 13.3 Installation Profiles

-   New installations use the developer-selected default appearance
    profile.
-   Existing installations retain their own settings during updates.

## 13.4 Data Preservation

The installer documentation states that backups and personal files are
not deleted by the supported setup modes.

# 14. Safety, Privacy, and Reversibility

The documented product principles include:

-   Read first; change only after the user chooses.
-   Reversible changes have an undo path or recovery point where
    supported.
-   No registry cleaning.
-   No automatic Windows Update installation.
-   No automatic Windows Update restart.
-   No changes to Windows Update policies.
-   No telemetry.
-   No location access.
-   Support bundles and logs remain on the PC.
-   Logs and support bundles may contain folder paths.
-   The sensor helper reads hardware information.
-   Fan writes occur only after the user confirms a configuration.

# 15. System Requirements and Dependencies

Documented requirements:

-   64-bit Windows 10 (1607 or later) or Windows 11.
-   Administrator rights for Setup.
-   Approximately 1.1 GB of package disk space.
-   Internet access is needed for application update operations, Windows
    Update detection and downloads, and optional Windhawk compiler
    functionality.

Some features depend on Windows version, hardware exposure,
embedded-controller support, or third-party components.

# 16. Feature Availability and Verification Notes

This reference describes documented product functionality, not a
guarantee that every feature works identically on every PC.

Examples of hardware- or environment-dependent behavior:

-   Fan reading and fan control depend on hardware support.
-   Windows theme application has a Windows 11 requirement in the
    documented theme manager.
-   Acrylic and frosted materials depend on Windows support and system
    settings.
-   After Dark availability depends on the Windows 10 release and
    whether a matching pack exists.
-   Some Windhawk operations depend on Windhawk components, including
    its compiler where applicable.

Before treating a feature as verified, distinguish between:

-   Source-code implementation.
-   Automated test coverage.
-   Build verification.
-   Sandbox testing.
-   Manual testing on a real PC.
-   Hardware-specific validation.

------------------------------------------------------------------------

**AnkhCore Control Center --- Complete Feature & Options Reference**

This document describes the product as a whole. Release numbers, beta
expiry dates, version-specific changelog entries, signing status, and
release-specific test counts belong in separate release notes rather
than this product-wide reference.
