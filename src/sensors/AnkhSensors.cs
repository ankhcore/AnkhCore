// AnkhSensors — AnkhCore hardware monitoring + fan helper (2.5).
// Built on LibreHardwareMonitor (MPL-2.0) and PawnIO (signed driver). No WinRing0.
// Compiled with the in-box C# compiler (%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe),
// so it is kept to C# 5 syntax. Runs elevated in the background, writes the current readings to
// a flat key=value file every second, and reads a command file for fan control.
//
// Fan safety (2.5): every decision is made by FanPolicy (pure logic, unit-tested in
// tests\FanPolicyTests.cs). Automatic (BIOS/firmware) control is the default and is restored on:
// start-up, "auto", "quit", an expired UI lease, a thermal limit, AnkhCore exiting, an error, and
// helper exit. Manual speed is clamped and ramped, and every request is read back and reported.
//
// Developer: As3aD.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Runtime.InteropServices;
using System.Threading;
using LibreHardwareMonitor.Hardware;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

namespace AnkhSensors
{
    // ---------------------------------------------------------------- fan policy (no hardware)
    public enum FanMode { Auto, Manual }

    public class FanDecision
    {
        public FanMode Mode;
        public float Percent;      // target sent to the hardware this tick (Manual only)
        public int TargetFanIndex = -1; // -1 = all controllable fans; >= 0 = specific control index
        public string Reason;      // why the mode is what it is
        public bool Changed;       // mode or percent changed this tick
        public bool Quit;          // helper should restore auto and exit
    }

    public class FanPolicy
    {
        public const float MinPercent = 40f, MaxPercent = 100f, RampPerTick = 10f;
        public const double LeaseSeconds = 10, OrphanSeconds = 15;
        // Thermal override needs SUSTAINED heat: Turbo Boost legitimately spikes some CPUs to 97-100 C for
        // a few seconds (observed on the ZBook 15 G5); a single hot reading must not drop manual control.
        public const float ThermalLimitC = 92f;
        public const double ThermalHoldSeconds = 8;
        DateTime hotSinceUtc = DateTime.MinValue;

        readonly DateTime startUtc;
        DateTime lastCommandStamp = DateTime.MinValue;
        string lastCommand = null;
        DateTime ownerLastSeenUtc;
        FanMode mode = FanMode.Auto;
        float target = 0, current = 0;
        int targetFanIndex = -1;
        string reason = "start-up: automatic";

        public FanPolicy(DateTime startUtc) { this.startUtc = startUtc; ownerLastSeenUtc = startUtc; }
        public FanMode Mode { get { return mode; } }
        public float Current { get { return current; } }

        // command: file content (null if no file); stamp: file write time (UTC).
        public FanDecision Tick(DateTime nowUtc, string command, DateTime stampUtc, bool ownerAlive, float maxTempC, bool controllable)
        {
            FanMode oldMode = mode; float oldCurrent = current;
            bool quit = false;
            if (ownerAlive) ownerLastSeenUtc = nowUtc;
            if (maxTempC >= ThermalLimitC) { if (hotSinceUtc == DateTime.MinValue) hotSinceUtc = nowUtc; }
            else hotSinceUtc = DateTime.MinValue;

            // a command is new only if written after this helper started and different from the last one
            bool fresh = command != null && stampUtc > startUtc && (stampUtc != lastCommandStamp || command != lastCommand);
            if (fresh)
            {
                lastCommandStamp = stampUtc; lastCommand = command;
                string c = command.Trim().ToLowerInvariant();
                string[] p = c.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
                if (p.Length == 0) { }
                else if (p[0] == "quit") { quit = true; GoAuto("quit requested"); }
                else if (p[0] == "auto")
                {
                    int fIdx = -1;
                    if (p.Length >= 2) int.TryParse(p[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out fIdx);
                    targetFanIndex = fIdx >= 0 ? fIdx : -1;
                    GoAuto("automatic requested");
                }
                else if (p[0] == "manual")
                {
                    float pct = 0;
                    int fIdx = -1;
                    bool parsed = false;
                    if (p.Length >= 3 && int.TryParse(p[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out fIdx) &&
                        float.TryParse(p[2], NumberStyles.Float, CultureInfo.InvariantCulture, out pct) && !float.IsNaN(pct) && fIdx >= 0)
                        parsed = true;
                    else if (p.Length == 2 && float.TryParse(p[1], NumberStyles.Float, CultureInfo.InvariantCulture, out pct) && !float.IsNaN(pct))
                        parsed = true;
                    if (!controllable) GoAuto("manual refused: no controllable fan");
                    else if (!parsed)
                        GoAuto("manual refused: invalid percentage");
                    else
                    {
                        targetFanIndex = fIdx;
                        target = Math.Max(MinPercent, Math.Min(MaxPercent, pct));
                        if (mode != FanMode.Manual) current = Math.Max(MinPercent, target);   // first step starts from a safe floor
                        mode = FanMode.Manual;
                        reason = pct < MinPercent ? "manual (raised to the " + MinPercent.ToString(CultureInfo.InvariantCulture) + "% minimum)" : "manual";
                    }
                }
                else GoAuto("unknown command ignored; automatic");
            }

            if (mode == FanMode.Manual)
            {
                if (command == null || (nowUtc - stampUtc).TotalSeconds > LeaseSeconds) GoAuto("lease expired (AnkhCore stopped renewing manual control)");
                else if (maxTempC >= ThermalLimitC && hotSinceUtc != DateTime.MinValue && (nowUtc - hotSinceUtc).TotalSeconds >= ThermalHoldSeconds)
                    GoAuto("thermal override: " + maxTempC.ToString("0", CultureInfo.InvariantCulture) + " C for " + ThermalHoldSeconds.ToString(CultureInfo.InvariantCulture) + " s");
                else if (!controllable) GoAuto("control lost");
            }
            if ((nowUtc - ownerLastSeenUtc).TotalSeconds > OrphanSeconds) { quit = true; GoAuto("AnkhCore is not running"); }

            if (mode == FanMode.Manual)
            {
                if (current < target) current = Math.Min(target, current + RampPerTick);
                else if (current > target) current = Math.Max(target, current - RampPerTick);
            }
            FanDecision d = new FanDecision();
            d.Mode = mode; d.Percent = mode == FanMode.Manual ? current : 0; d.TargetFanIndex = targetFanIndex; d.Reason = reason; d.Quit = quit;
            d.Changed = mode != oldMode || Math.Abs(current - oldCurrent) > 0.01f;
            return d;
        }

        // The hardware refused the last command: back to automatic. The command stays consumed,
        // so it is not retried until AnkhCore writes a new one.
        public FanDecision Reject(string why)
        {
            GoAuto(why);
            FanDecision d = new FanDecision(); d.Mode = FanMode.Auto; d.Reason = why; d.Changed = true;
            return d;
        }
        void GoAuto(string why) { mode = FanMode.Auto; target = 0; current = 0; reason = why; }
    }

    // ---------------------------------------------------------------- per-fan capability assessment (FCD-01)
    public enum FanCapabilityKind { MonitorOnly, VendorReadOnly, Controllable, ControlOnly }

    public class FanCapabilityInfo
    {
        public FanCapabilityKind Kind;
        public string WireName;
        public bool Controllable;
        public float MinPercent;
        public float MaxPercent;
        public string ControlMode;
    }

    public static class FanCapabilityDetector
    {
        public static FanCapabilityInfo Assess(bool hasControl, float minVal, float maxVal, string ctrlMode, bool isVendorReadOnly)
        {
            var info = new FanCapabilityInfo();
            info.ControlMode = ctrlMode == null ? "" : ctrlMode;
            if (isVendorReadOnly)
            {
                info.Kind = FanCapabilityKind.VendorReadOnly;
                info.WireName = "vendor_readonly";
                info.Controllable = false;
                info.MinPercent = -1f;
                info.MaxPercent = -1f;
                return info;
            }
            bool validRange = hasControl && !float.IsNaN(minVal) && !float.IsNaN(maxVal) && maxVal > minVal && maxVal >= FanPolicy.MinPercent;
            if (validRange)
            {
                info.Kind = FanCapabilityKind.Controllable;
                info.WireName = "controllable";
                info.Controllable = true;
                info.MinPercent = minVal;
                info.MaxPercent = maxVal;
            }
            else
            {
                info.Kind = FanCapabilityKind.MonitorOnly;
                info.WireName = "monitor_only";
                info.Controllable = false;
                info.MinPercent = hasControl ? minVal : -1f;
                info.MaxPercent = hasControl ? maxVal : -1f;
            }
            return info;
        }
    }

    // ---------------------------------------------------------------- vendor fan readings (read-only)
    // A reading only counts as live once its value has been seen to change: some firmware reports a
    // value that never updates (on the ZBook 15 G5 the HP BIOS temperatures are frozen). Until then the
    // UI marks the source "not yet verified".
    public class FanLiveness
    {
        readonly Dictionary<string, float> first = new Dictionary<string, float>();
        readonly HashSet<string> live = new HashSet<string>();
        public void Observe(string name, float value)
        {
            float f;
            if (!first.TryGetValue(name, out f)) { first[name] = value; return; }
            if (Math.Abs(f - value) >= 1f) live.Add(name);
        }
        public bool IsLive(string name) { return live.Contains(name); }
    }

    public class VendorFan { public string Name; public float Rpm; public string Source; }

    // HP business notebooks: fan speeds from the HP BIOS WMI provider (no driver, no EC access).
    public static class HpWmiFans
    {
        static DateTime next = DateTime.MinValue;
        static List<VendorFan> cache = new List<VendorFan>();
        static bool unavailable = false;
        public static List<VendorFan> Read()
        {
            if (unavailable || DateTime.UtcNow < next) return cache;
            next = DateTime.UtcNow.AddSeconds(3);
            List<VendorFan> v = new List<VendorFan>();
            try
            {
                using (var s = new System.Management.ManagementObjectSearcher(@"root\HP\InstrumentedBIOS",
                           "SELECT Name, CurrentReading, BaseUnits FROM HP_BIOSNumericSensor"))
                foreach (System.Management.ManagementObject mo in s.Get())
                {
                    string name = mo["Name"] == null ? "" : mo["Name"].ToString();
                    object units = mo["BaseUnits"], cur = mo["CurrentReading"];
                    if (units == null || cur == null || Convert.ToInt32(units, CultureInfo.InvariantCulture) != 19) continue;   // 19 = RPM
                    if (name.IndexOf("Fan", StringComparison.OrdinalIgnoreCase) < 0) continue;
                    VendorFan f = new VendorFan(); f.Name = name; f.Rpm = Convert.ToSingle(cur, CultureInfo.InvariantCulture); f.Source = "HP BIOS";
                    v.Add(f);
                }
            }
            catch (System.Management.ManagementException) { unavailable = true; }   // not an HP machine / no provider
            catch { }
            cache = v;
            return cache;
        }
    }

    // Dell notebooks/desktops: fan speeds from Dell Command | Monitor's WMI provider (root\DCIM\SysMgt), read-only.
    // Only present when Dell Command | Monitor is installed; without it Dell exposes no fan RPM through any
    // driverless Windows interface, and the page keeps saying "not exposed" (the diagnostic below shows why).
    public static class DellWmiFans
    {
        static DateTime next = DateTime.MinValue;
        static List<VendorFan> cache = new List<VendorFan>();
        static bool unavailable = false;
        public static List<VendorFan> Read()
        {
            if (unavailable || DateTime.UtcNow < next) return cache;
            next = DateTime.UtcNow.AddSeconds(3);
            List<VendorFan> v = new List<VendorFan>();
            try
            {
                using (var s = new System.Management.ManagementObjectSearcher(@"root\DCIM\SysMgt",
                           "SELECT ElementName, CurrentReading, BaseUnits, UnitModifier FROM DCIM_NumericSensor"))
                foreach (System.Management.ManagementObject mo in s.Get())
                {
                    string name = mo["ElementName"] == null ? "" : mo["ElementName"].ToString();
                    object cur = mo["CurrentReading"], units = mo["BaseUnits"];
                    if (cur == null || name.IndexOf("Fan", StringComparison.OrdinalIgnoreCase) < 0) continue;
                    if (units != null && Convert.ToInt32(units, CultureInfo.InvariantCulture) != 19) continue;   // 19 = RPM
                    float rpm = Convert.ToSingle(cur, CultureInfo.InvariantCulture);
                    object mod = mo["UnitModifier"];
                    if (mod != null) { int m = Convert.ToInt32(mod, CultureInfo.InvariantCulture); if (m != 0 && m > -6 && m < 6) rpm = (float)(rpm * Math.Pow(10, m)); }
                    if (rpm < 0) continue;
                    VendorFan f = new VendorFan(); f.Name = name; f.Rpm = rpm; f.Source = "Dell Command Monitor";
                    v.Add(f);
                }
            }
            catch (System.Management.ManagementException) { unavailable = true; }   // Dell Command | Monitor not installed
            catch { }
            cache = v;
            return cache;
        }
    }


    // ---------------------------------------------------------------- 2.8: PawnIO fan readers (read-only)
    // Notebooks that publish no fan speed through Windows are read through signed PawnIO modules from the official PawnIO.Modules release
    // (LGPL-2.1, https://github.com/namazso/PawnIO.Modules): LpcThinkpadEC (ThinkPad embedded controller) and DellSMM (Dell system management mode).
    // They are loaded into the PawnIO driver AnkhCore already installs. Rules: read-only; only on the maker the module is for; the module file must
    // match the SHA-256 that shipped with AnkhCore; a guard file is written before every access and removed after it, so a machine that ever
    // hangs inside one switches that reader off for good (until the person re-enables it); one file turns both readers off.
    public sealed class PawnModule : IDisposable
    {
        const uint DeviceType = 41394u << 16;
        const uint LoadBinary = DeviceType | (0x821u << 2);
        const uint ExecuteFn = DeviceType | (0x841u << 2);
        SafeFileHandle handle;
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] inBuf, int inSize, byte[] outBuf, int outSize, out int returned, IntPtr overlapped);
        public static PawnModule Load(string path, string expectedSha256, out string why)
        {
            why = "";
            try
            {
                if (!File.Exists(path)) { why = "the module file is missing: " + Path.GetFileName(path); return null; }
                byte[] bin = File.ReadAllBytes(path);
                using (System.Security.Cryptography.SHA256 sha = System.Security.Cryptography.SHA256.Create())
                {
                    string h = BitConverter.ToString(sha.ComputeHash(bin)).Replace("-", "").ToLowerInvariant();
                    if (h != expectedSha256) { why = "the module file differs from the one AnkhCore shipped"; return null; }
                }
                SafeFileHandle hd = CreateFileW("\\\\?\\GLOBALROOT\\Device\\PawnIO", 3, 3, IntPtr.Zero, 3, 0x80, IntPtr.Zero);
                if (hd.IsInvalid) { why = "the PawnIO driver is not available (error " + Marshal.GetLastWin32Error() + ")"; return null; }
                int ret;
                if (!DeviceIoControl(hd, LoadBinary, bin, bin.Length, null, 0, out ret, IntPtr.Zero)) { why = "the driver did not accept the module (error " + Marshal.GetLastWin32Error() + "): not supported on this PC"; hd.Dispose(); return null; }
                PawnModule m = new PawnModule(); m.handle = hd; return m;
            }
            catch (Exception e) { why = e.Message; return null; }
        }
        // null = the call failed
        public long[] Execute(string fn, long[] input, int outLen)
        {
            byte[] total = new byte[32 + input.Length * 8];
            byte[] nm = Encoding.ASCII.GetBytes(fn);
            Buffer.BlockCopy(nm, 0, total, 0, Math.Min(31, nm.Length));
            Buffer.BlockCopy(input, 0, total, 32, input.Length * 8);
            byte[] output = new byte[outLen * 8];
            int read;
            if (!DeviceIoControl(handle, ExecuteFn, total, total.Length, output, output.Length, out read, IntPtr.Zero)) return null;
            long[] r = new long[read / 8];
            Buffer.BlockCopy(output, 0, r, 0, r.Length * 8);
            return r;
        }
        public void Dispose() { if (handle != null) handle.Dispose(); }
    }

    public static class PawnGuard
    {
        static string Dir { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "AnkhCore\\Sensors"); } }
        public static bool UserDisabled() { try { return File.Exists(Path.Combine(Dir, "disable-pawnio-fans.txt")); } catch { return false; } }
        public static bool Unfinished(string what) { try { return File.Exists(Path.Combine(Dir, "fan-guard." + what)); } catch { return false; } }
        public static void Begin(string what) { try { Directory.CreateDirectory(Dir); File.WriteAllText(Path.Combine(Dir, "fan-guard." + what), DateTime.UtcNow.ToString("o")); } catch { } }
        public static void End(string what) { try { File.Delete(Path.Combine(Dir, "fan-guard." + what)); } catch { } }
        public static string Bios(string value)
        {
            try { object v = Registry.GetValue(@"HKEY_LOCAL_MACHINE\HARDWARE\DESCRIPTION\System\BIOS", value, ""); return v == null ? "" : v.ToString().Trim(); }
            catch { return ""; }
        }
        public static bool IsThinkPad()
        {
            if (!string.Equals(Bios("SystemManufacturer"), "LENOVO", StringComparison.OrdinalIgnoreCase)) return false;
            return Bios("SystemFamily").StartsWith("ThinkPad", StringComparison.OrdinalIgnoreCase) || Bios("SystemVersion").StartsWith("ThinkPad", StringComparison.OrdinalIgnoreCase);
        }
        public static bool IsDell() { return Bios("SystemManufacturer").IndexOf("Dell", StringComparison.OrdinalIgnoreCase) >= 0; }
        public static string BaseDir() { return AppDomain.CurrentDomain.BaseDirectory; }
    }

    // ThinkPad: the embedded controller's fan speed (EC registers 0x84 low byte, 0x85 high byte, as the Linux thinkpad_acpi driver reads them),
    // through the ACPI EC protocol on the 0x1600 window the module allows, under the system's own Access_EC mutex. Fan 1 only.
    public static class ThinkpadEcFans
    {
        const string ModuleSha256 = "54bbf7f10d3511abfff167597485926ddf1587045f6ae849671cfaa85f97a1db";   // LpcThinkpadEC.bin, PawnIO.Modules 0.2.12
        static bool init, off; static PawnModule mod; static DateTime next = DateTime.MinValue; static int errors;
        static List<VendorFan> cache = new List<VendorFan>();
        public static string Status = "not checked";
        static int In(PawnModule m, int port) { long[] r = m.Execute("ioctl_pio_read", new long[] { port }, 1); return (r == null || r.Length < 1) ? -1 : (int)(r[0] & 0xFF); }
        static bool Out(PawnModule m, int port, int val) { return m.Execute("ioctl_pio_write", new long[] { port, val }, 0) != null; }
        static bool Wait(PawnModule m, int bit, bool set)   // status register 0x1604: bit0 = output buffer full, bit1 = input buffer full
        {
            Stopwatch w = Stopwatch.StartNew();
            while (w.ElapsedMilliseconds < 60) { int st = In(m, 0x1604); if (st < 0) return false; if (((st & bit) != 0) == set) return true; Thread.Sleep(1); }
            return false;
        }
        static int ReadEc(PawnModule m, int addr)
        {
            if (!Wait(m, 2, false) || !Out(m, 0x1604, 0x80) || !Wait(m, 2, false) || !Out(m, 0x1600, addr) || !Wait(m, 1, true)) return -1;
            return In(m, 0x1600);
        }
        public static List<VendorFan> Read()
        {
            if (off || DateTime.UtcNow < next) return cache;
            next = DateTime.UtcNow.AddSeconds(3);
            try
            {
                if (!init)
                {
                    init = true;
                    if (PawnGuard.UserDisabled()) { off = true; Status = "turned off (disable-pawnio-fans.txt)"; return cache; }
                    if (!PawnGuard.IsThinkPad()) { off = true; Status = "not a ThinkPad"; return cache; }
                    if (PawnGuard.Unfinished("tpec")) { off = true; Status = "switched off: an earlier ThinkPad EC read did not finish on this PC"; return cache; }
                    string why;
                    mod = PawnModule.Load(Path.Combine(PawnGuard.BaseDir(), "PawnIO\\LpcThinkpadEC.bin"), ModuleSha256, out why);
                    if (mod == null) { off = true; Status = why; return cache; }
                    Status = "module loaded";
                }
                Mutex mx = null; bool got = false;
                try
                {
                    mx = new Mutex(false, "Global\\Access_EC");
                    got = mx.WaitOne(300);
                    if (!got) { Status = "the system's EC lock was busy"; return cache; }
                    PawnGuard.Begin("tpec");
                    int lo = ReadEc(mod, 0x84), hi = lo < 0 ? -1 : ReadEc(mod, 0x85);
                    PawnGuard.End("tpec");
                    if (lo < 0 || hi < 0) { if (++errors >= 3) { off = true; Status = "switched off: the embedded controller did not answer"; } return cache; }
                    errors = 0;
                    int rpm = lo | (hi << 8);
                    List<VendorFan> v = new List<VendorFan>();
                    if (rpm >= 0 && rpm < 20000) { VendorFan f = new VendorFan(); f.Name = "ThinkPad fan 1"; f.Rpm = rpm; f.Source = "ThinkPad EC"; v.Add(f); Status = "reading fan 1"; }
                    cache = v;
                }
                finally { if (got && mx != null) { try { mx.ReleaseMutex(); } catch { } } if (mx != null) mx.Close(); }
            }
            catch (Exception e) { Status = "error: " + e.Message; }
            return cache;
        }
    }

    // Dell: the system management mode interface the Linux dell-smm-hwmon driver uses. Only two read calls are ever issued: fan status (0x00a3) and
    // fan speed (0x02a3). The module itself loads only when the firmware answers Dell's signature. Skipped when Dell Command | Monitor already reports fans.
    public static class DellSmmFans
    {
        const string ModuleSha256 = "15c32962fdef19258546ae4c232088d80957eeaa87996e9c2c1e6f85c04ca155";   // DellSMM.bin, PawnIO.Modules 0.2.12
        static bool init, off; static PawnModule mod; static DateTime next = DateTime.MinValue; static int errors;
        static List<int> fans = new List<int>();
        static List<VendorFan> cache = new List<VendorFan>();
        public static string Status = "not checked";
        static long[] Smm(PawnModule m, long eax, long ebx)
        {
            long[] r = m.Execute("ioctl_query_smm", new long[] { eax, 0, 0, ebx, 0, 0 }, 6);   // order: eax, ecx, edx, ebx, esi, edi
            if (r == null || r.Length < 1) return null;
            if ((r[0] & 0xFFFF) == 0xFFFF || (r[0] & 0xFFFFFFFFL) == 0xFFFFFFFFL) return null;   // the firmware refused the request
            return r;
        }
        public static List<VendorFan> Read()
        {
            if (off || DateTime.UtcNow < next) return cache;
            next = DateTime.UtcNow.AddSeconds(3);
            try
            {
                if (!init)
                {
                    init = true;
                    if (PawnGuard.UserDisabled()) { off = true; Status = "turned off (disable-pawnio-fans.txt)"; return cache; }
                    if (!PawnGuard.IsDell()) { off = true; Status = "not a Dell"; return cache; }
                    if (PawnGuard.Unfinished("dellsmm")) { off = true; Status = "switched off: an earlier Dell SMM read did not finish on this PC"; return cache; }
                }
                if (DellWmiFans.Read().Count > 0) { Status = "not needed: Dell Command | Monitor reports the fans"; cache = new List<VendorFan>(); return cache; }
                if (mod == null)
                {
                    string why;
                    mod = PawnModule.Load(Path.Combine(PawnGuard.BaseDir(), "PawnIO\\DellSMM.bin"), ModuleSha256, out why);
                    if (mod == null) { off = true; Status = why; return cache; }
                    Status = "module loaded";
                }
                PawnGuard.Begin("dellsmm");
                List<VendorFan> v = new List<VendorFan>();
                if (fans.Count == 0) for (int fan = 0; fan < 3; ++fan) if (Smm(mod, 0x00a3, fan) != null) fans.Add(fan);   // which fans exist
                foreach (int fan in fans)
                {
                    long[] r = Smm(mod, 0x02a3, fan);
                    if (r == null) continue;
                    int rpm = (int)(r[0] & 0xFFFF);
                    if (rpm >= 0 && rpm < 20000) { VendorFan f = new VendorFan(); f.Name = "Dell fan " + (fan + 1); f.Rpm = rpm; f.Source = "Dell SMM"; v.Add(f); }
                }
                PawnGuard.End("dellsmm");
                if (v.Count == 0) { if (++errors >= 3) { off = true; Status = "switched off: the firmware reports no fan speed"; } }
                else { errors = 0; Status = "reading " + v.Count + " fan(s)"; }
                cache = v;
            }
            catch (Exception e) { Status = "error: " + e.Message; }
            return cache;
        }
    }


    // ---------------------------------------------------------------- 2.8: AnkhCore's own fan-control engine (no NBFC, no WinRing0)
    // The fans of notebooks are set through the embedded controller (EC). AnkhCore does it itself: the model's register map is read from the
    // configuration the person chose and confirmed (FanConfigs\<model>.xml, the community notebook configurations), and the EC is reached through the
    // signed PawnIO module LpcACPIEC (PawnIO.Modules, LGPL-2.1) with the standard ACPI EC protocol. SAFETY: this code only ever sends the EC commands
    // 0x80 (read) and 0x81 (write) to ports 0x62/0x66, under the system's Access_EC mutex; nothing is written until the person confirmed a
    // configuration (ec-fan.txt); every request still goes through FanPolicy (40% minimum, ramp, lease, thermal override); the BIOS gets control back
    // on auto, on exit, on error; a guard file switches the engine off for good on a PC that ever hangs inside an EC access.
    public interface IFanActuator { void SetPercent(float p); void SetAuto(); bool IsManual { get; } float Percent { get; } void Tick(); }

    public sealed class LhmActuator : IFanActuator
    {
        public readonly IControl Inner;
        public LhmActuator(IControl c) { Inner = c; }
        public void SetPercent(float p) { Inner.SetSoftware(p); }
        public void SetAuto() { Inner.SetDefault(); }
        public bool IsManual { get { return Inner.ControlMode == ControlMode.Software; } }
        public float Percent { get { return Inner.SoftwareValue; } }
        public void Tick() { }
    }

    public sealed class AcpiEc : IDisposable
    {
        const string ModuleSha256 = "c38fd116e7aff4d1fdb0a494e296be0a6708e5a22fc72f14587442fb7f8f7906";   // LpcACPIEC.bin, PawnIO.Modules 0.2.12 (identical to LibreHardwareMonitor's copy)
        PawnModule m; static Mutex lockMx;
        public static AcpiEc Open(out string why)
        {
            PawnModule mod = PawnModule.Load(Path.Combine(PawnGuard.BaseDir(), "PawnIO\\LpcACPIEC.bin"), ModuleSha256, out why);
            if (mod == null) return null;
            AcpiEc e = new AcpiEc(); e.m = mod; return e;
        }
        int In(int port) { if (port != 0x62 && port != 0x66) return -1; long[] r = m.Execute("ioctl_pio_read", new long[] { port }, 1); return (r == null || r.Length < 1) ? -1 : (int)(r[0] & 0xFF); }
        bool Out(int port, int v) { if (port != 0x62 && port != 0x66) return false; return m.Execute("ioctl_pio_write", new long[] { port, v & 0xFF }, 0) != null; }
        bool Wait(int bit, bool set)   // status 0x66: bit0 output buffer full, bit1 input buffer full
        {
            Stopwatch w = Stopwatch.StartNew();
            while (w.ElapsedMilliseconds < 80) { int st = In(0x66); if (st < 0) return false; if (((st & bit) != 0) == set) return true; Thread.Sleep(1); }
            return false;
        }
        static bool Lock() { if (lockMx == null) lockMx = new Mutex(false, "Global\\Access_EC"); try { return lockMx.WaitOne(500); } catch (AbandonedMutexException) { return true; } }
        static void Unlock() { try { lockMx.ReleaseMutex(); } catch { } }
        public bool ReadByte(int reg, out int val)
        {
            val = 0; if (reg < 0 || reg > 255 || !Lock()) return false;
            try
            {
                if (!Wait(2, false) || !Out(0x66, 0x80) || !Wait(2, false) || !Out(0x62, reg) || !Wait(2, false) || !Wait(1, true)) return false;
                int v = In(0x62); if (v < 0) return false; val = v; return true;
            }
            finally { Unlock(); }
        }
        public bool WriteByte(int reg, int val)
        {
            if (reg < 0 || reg > 255 || val < 0 || val > 255 || !Lock()) return false;
            try { return Wait(2, false) && Out(0x66, 0x81) && Wait(2, false) && Out(0x62, reg) && Wait(2, false) && Out(0x62, val) && Wait(2, false); }
            finally { Unlock(); }
        }
        public bool Read(int reg, bool words, out int val)
        {
            val = 0; int lo, hi = 0;
            for (int t = 0; t < 2; ++t)
            {
                if (ReadByte(reg, out lo) && (!words || ReadByte(reg + 1, out hi))) { val = words ? (lo | (hi << 8)) : lo; return true; }
            }
            return false;
        }
        public bool Write(int reg, bool words, int val)
        {
            for (int t = 0; t < 2; ++t)
            {
                if (WriteByte(reg, val & 0xFF) && (!words || WriteByte(reg + 1, (val >> 8) & 0xFF))) return true;
            }
            return false;
        }
        public void Dispose() { if (m != null) m.Dispose(); }
    }

    public class EcFanCfg
    {
        public string Name; public int ReadReg, WriteReg, Min, Max, MinRead, MaxRead, ResetValue;
        public bool Independent, ResetRequired;
        public Dictionary<int, int> WriteOverrides = new Dictionary<int, int>();   // percentage -> value
        public Dictionary<int, int> ReadOverrides = new Dictionary<int, int>();    // value -> percentage
    }
    public class EcWriteCfg { public int Reg, Value, ResetValue; public string Mode = "Set", ResetMode = "Set", Occasion = "OnWriteFanSpeed"; public bool ResetRequired; }
    public class EcConfig
    {
        public string Model = ""; public bool Words; public int PollMs = 1000; public List<EcFanCfg> Fans = new List<EcFanCfg>(); public List<EcWriteCfg> Writes = new List<EcWriteCfg>();
        static int I(System.Xml.XmlNode n, string name, int def) { System.Xml.XmlNode c = n.SelectSingleNode(name); int v; return (c != null && int.TryParse(c.InnerText.Trim(), out v)) ? v : def; }
        static bool B(System.Xml.XmlNode n, string name) { System.Xml.XmlNode c = n.SelectSingleNode(name); return c != null && c.InnerText.Trim().Equals("true", StringComparison.OrdinalIgnoreCase); }
        static string T(System.Xml.XmlNode n, string name, string def) { System.Xml.XmlNode c = n.SelectSingleNode(name); return c == null ? def : c.InnerText.Trim(); }
        public static EcConfig Parse(string path, out string why)
        {
            why = "";
            try
            {
                System.Xml.XmlDocument d = new System.Xml.XmlDocument(); d.XmlResolver = null; d.Load(path);
                System.Xml.XmlNode root = d.DocumentElement;
                if (root == null || root.Name != "FanControlConfigV2") { why = "not a notebook fan configuration"; return null; }
                EcConfig c = new EcConfig(); c.Model = T(root, "NotebookModel", ""); c.Words = B(root, "ReadWriteWords"); c.PollMs = Math.Max(500, Math.Min(3000, I(root, "EcPollInterval", 1000)));
                foreach (System.Xml.XmlNode fn in root.SelectNodes("FanConfigurations/FanConfiguration"))
                {
                    EcFanCfg f = new EcFanCfg();
                    f.Name = T(fn, "FanDisplayName", "Fan " + (c.Fans.Count + 1)); f.ReadReg = I(fn, "ReadRegister", -1); f.WriteReg = I(fn, "WriteRegister", -1);
                    f.Min = I(fn, "MinSpeedValue", -1); f.Max = I(fn, "MaxSpeedValue", -1); f.Independent = B(fn, "IndependentReadMinMaxValues");
                    f.MinRead = f.Independent ? I(fn, "MinSpeedValueRead", f.Min) : f.Min; f.MaxRead = f.Independent ? I(fn, "MaxSpeedValueRead", f.Max) : f.Max;
                    f.ResetRequired = B(fn, "ResetRequired"); f.ResetValue = I(fn, "FanSpeedResetValue", 0);
                    foreach (System.Xml.XmlNode on in fn.SelectNodes("FanSpeedPercentageOverrides/FanSpeedPercentageOverride"))
                    {
                        int pct = I(on, "FanSpeedPercentage", -1), val = I(on, "FanSpeedValue", -1); string op = T(on, "TargetOperation", "ReadWrite");
                        if (op.IndexOf("Write", StringComparison.OrdinalIgnoreCase) >= 0 && !f.WriteOverrides.ContainsKey(pct)) f.WriteOverrides[pct] = val;
                        if (op.IndexOf("Read", StringComparison.OrdinalIgnoreCase) >= 0 && !f.ReadOverrides.ContainsKey(val)) f.ReadOverrides[val] = pct;
                    }
                    c.Fans.Add(f);
                }
                foreach (System.Xml.XmlNode wn in root.SelectNodes("RegisterWriteConfigurations/RegisterWriteConfiguration"))
                {
                    EcWriteCfg w = new EcWriteCfg(); w.Reg = I(wn, "Register", -1); w.Value = I(wn, "Value", 0); w.Mode = T(wn, "WriteMode", "Set"); w.Occasion = T(wn, "WriteOccasion", "OnWriteFanSpeed");
                    w.ResetRequired = B(wn, "ResetRequired"); w.ResetValue = I(wn, "ResetValue", 0); w.ResetMode = T(wn, "ResetWriteMode", "Set");
                    c.Writes.Add(w);
                }
                if (c.Fans.Count == 0 || c.Fans.Count > 4) { why = "the configuration lists " + c.Fans.Count + " fans"; return null; }
                foreach (EcFanCfg f in c.Fans)
                    if (f.ReadReg < 0 || f.ReadReg > 255 || f.WriteReg < 0 || f.WriteReg > 255 || f.Min < 0 || f.Max < 0 || f.Min == f.Max || (!c.Words && (f.Min > 255 || f.Max > 255))) { why = "a fan entry of the configuration is not usable"; return null; }
                foreach (EcWriteCfg w in c.Writes) if (w.Reg < 0 || w.Reg > 255 || w.Value < 0 || w.Value > 255 || w.ResetValue < 0 || w.ResetValue > 255) { why = "a register entry of the configuration is not usable"; return null; }
                return c;
            }
            catch (Exception e) { why = "the configuration could not be read: " + e.Message; return null; }
        }
        // The documented mapping of the notebook configurations: write value = min + (max - min) * percent / 100, rounded; an override replaces it.
        public static int ToValue(EcFanCfg f, float pct)
        {
            int ip = (int)Math.Round(pct); int ov;
            if (Math.Abs(pct - ip) < 0.001f && f.WriteOverrides.TryGetValue(ip, out ov)) return ov;
            return (int)Math.Round(f.Min + ((f.Max - f.Min) * (double)pct) / 100.0);
        }
        public static float ToPercent(EcFanCfg f, int value)
        {
            int pv; if (f.ReadOverrides.TryGetValue(value, out pv)) return pv;
            if (f.MinRead == f.MaxRead) return 0;
            float p = ((float)(value - f.MinRead) / (f.MaxRead - f.MinRead)) * 100f;
            return p < 0 ? 0 : (p > 100 ? 100 : p);
        }
    }

    public sealed class EcFan : IFanActuator
    {
        readonly EcEngine eng; readonly int idx; bool manual; float pct; DateTime last = DateTime.MinValue; float readback = -1;
        public EcFan(EcEngine e, int i) { eng = e; idx = i; }
        public string Name { get { return eng.Cfg.Fans[idx].Name; } }
        public void SetPercent(float p)
        {
            if (!eng.WriteFan(idx, p)) throw new InvalidOperationException("the embedded controller did not accept the fan speed");
            manual = true; pct = p; last = DateTime.UtcNow; readback = eng.ReadPercent(idx);
        }
        public void SetAuto() { eng.ResetFan(idx); manual = false; eng.AfterAuto(); }
        public bool IsManual { get { return manual; } }
        public float Percent { get { return readback >= 0 ? readback : pct; } }
        public float ReadPercentNow() { float r = eng.ReadPercent(idx); if (r >= 0) readback = r; return Percent; }
        public void Tick()   // the EC takes the fan back after a few seconds: the chosen speed is written again, as the polling interval of the configuration says
        {
            if (!manual || (DateTime.UtcNow - last).TotalMilliseconds < eng.Cfg.PollMs) return;
            if (eng.WriteFan(idx, pct)) { last = DateTime.UtcNow; readback = eng.ReadPercent(idx); }
        }
    }

    public sealed class EcEngine : IDisposable
    {
        public readonly EcConfig Cfg; readonly AcpiEc ec; bool initDone; public readonly List<EcFan> Fans = new List<EcFan>();
        public EcEngine(EcConfig c, AcpiEc e) { Cfg = c; ec = e; for (int i = 0; i < c.Fans.Count; ++i) Fans.Add(new EcFan(this, i)); }
        bool ApplyReg(int reg, int value, string mode)
        {
            int v = value;
            if (mode.Equals("And", StringComparison.OrdinalIgnoreCase) || mode.Equals("Or", StringComparison.OrdinalIgnoreCase))
            {
                int cur; if (!ec.ReadByte(reg, out cur)) return false;
                v = mode.Equals("And", StringComparison.OrdinalIgnoreCase) ? (value & cur) : (value | cur);
            }
            return ec.WriteByte(reg, v);
        }
        public bool WriteFan(int idx, float pct)
        {
            PawnGuard.Begin("ecfan");
            try
            {
                bool init = !initDone;
                foreach (EcWriteCfg w in Cfg.Writes)
                    if (init || w.Occasion.Equals("OnWriteFanSpeed", StringComparison.OrdinalIgnoreCase)) if (!ApplyReg(w.Reg, w.Value, w.Mode)) return false;
                initDone = true;
                EcFanCfg f = Cfg.Fans[idx];
                return ec.Write(f.WriteReg, Cfg.Words, EcConfig.ToValue(f, pct));
            }
            finally { PawnGuard.End("ecfan"); }
        }
        public void ResetFan(int idx) { EcFanCfg f = Cfg.Fans[idx]; if (f.ResetRequired) { PawnGuard.Begin("ecfan"); try { ec.Write(f.WriteReg, Cfg.Words, f.ResetValue); } finally { PawnGuard.End("ecfan"); } } }
        public void AfterAuto()   // when no fan is on manual any more the register changes of the configuration are undone too
        {
            foreach (EcFan f in Fans) if (f.IsManual) return;
            if (!initDone) return;
            PawnGuard.Begin("ecfan");
            try { foreach (EcWriteCfg w in Cfg.Writes) if (w.ResetRequired) ApplyReg(w.Reg, w.ResetValue, w.ResetMode); }
            finally { PawnGuard.End("ecfan"); }
            initDone = false;
        }
        public float ReadPercent(int idx)
        {
            EcFanCfg f = Cfg.Fans[idx]; int v = 0;
            for (int t = 0; t < 3; ++t)
            {
                if (!ec.Read(f.ReadReg, Cfg.Words, out v)) continue;
                if (v >= Math.Min(f.MinRead, f.MaxRead) && v <= Math.Max(f.MinRead, f.MaxRead)) break;
            }
            return EcConfig.ToPercent(f, v);
        }
        public void Dispose() { ec.Dispose(); }
    }

    // What the person confirmed (written by AnkhCore): ec-fan.txt = config=<file name in FanConfigs>, sha256=<of that file>, confirmed=1.
    public static class EcFanBank
    {
        public static string State = "no configuration chosen";
        public static EcEngine Engine;
        static DateTime stamp = DateTime.MinValue; static bool wasPresent, first = true;
        public static bool Active { get { return Engine != null; } }
        static string Sha(string path) { using (System.Security.Cryptography.SHA256 sha = System.Security.Cryptography.SHA256.Create()) { return BitConverter.ToString(sha.ComputeHash(File.ReadAllBytes(path))).Replace("-", "").ToLowerInvariant(); } }
        public static void Refresh(string dir, string configDir, List<IFanActuator> controls, bool lhmHasFans, bool lhmHasControls)
        {
            string file = Path.Combine(dir, "ec-fan.txt");
            bool present = File.Exists(file);
            DateTime st = present ? File.GetLastWriteTimeUtc(file) : DateTime.MinValue;
            if (!first && present == wasPresent && st == stamp && (Engine != null || !present)) return;   // nothing changed
            first = false;
            wasPresent = present; stamp = st;
            Deactivate(controls);
            if (!present) { State = EcReady() ? "ready: choose a configuration for this model" : State; return; }
            string cfgName = "", sha = ""; bool confirmed = false;
            try { foreach (string line in File.ReadAllLines(file)) { int k = line.IndexOf('='); if (k <= 0) continue; string key = line.Substring(0, k).Trim(), val = line.Substring(k + 1).Trim(); if (key == "config") cfgName = val; else if (key == "sha256") sha = val.ToLowerInvariant(); else if (key == "confirmed") confirmed = val == "1"; } } catch { }
            if (!confirmed) { State = "not active: the choice was not confirmed"; return; }
            if (PawnGuard.UserDisabled()) { State = "not active: turned off (disable-pawnio-fans.txt)"; return; }
            if (PawnGuard.Unfinished("ecfan")) { State = "not active: an earlier embedded-controller access did not finish on this PC; it stays off until it is turned on again"; return; }
            if (lhmHasFans || lhmHasControls) { State = "not active: the sensor helper already lists or controls fans on this PC"; return; }
            if (cfgName.Length == 0 || cfgName.IndexOfAny(new char[] { '\\', '/', ':' }) >= 0 || !cfgName.EndsWith(".xml", StringComparison.OrdinalIgnoreCase)) { State = "not active: the configuration name is not valid"; return; }
            string path = Path.Combine(configDir, cfgName);
            if (!File.Exists(path)) { State = "not active: the configuration file is missing: " + cfgName; return; }
            if (Sha(path) != sha) { State = "not active: the configuration file changed after it was confirmed"; return; }
            string why; EcConfig c = EcConfig.Parse(path, out why);
            if (c == null) { State = "not active: " + why; return; }
            AcpiEc ec = AcpiEc.Open(out why);
            if (ec == null) { State = "not active: " + why; return; }
            EcEngine e = new EcEngine(c, ec);
            Engine = e;
            foreach (EcFan f in e.Fans) controls.Add(f);
            State = "active: " + (c.Model.Length > 0 ? c.Model : cfgName) + ", " + e.Fans.Count + " fan(s)";
        }
        static bool ecProbed, ecOk;
        static bool EcReady()
        {
            if (!ecProbed) { ecProbed = true; string why; AcpiEc t = AcpiEc.Open(out why); ecOk = t != null; if (t != null) t.Dispose(); else State = "unavailable: " + why; }
            return ecOk;
        }
        static void Deactivate(List<IFanActuator> controls)
        {
            if (Engine == null) return;
            foreach (EcFan f in Engine.Fans) { try { f.SetAuto(); } catch { } controls.Remove(f); }
            Engine.Dispose(); Engine = null;
        }
        public static void Release(List<IFanActuator> controls) { Deactivate(controls); }
    }

    public static class VendorFans
    {
        public static List<VendorFan> Read()
        {
            List<VendorFan> v = new List<VendorFan>(HpWmiFans.Read());
            v.AddRange(DellWmiFans.Read());
            v.AddRange(ThinkpadEcFans.Read());
            v.AddRange(DellSmmFans.Read());
            return v;
        }
        // Read-only diagnostic text: which fan sources this PC really has (for "not exposed" reports).
        public static string Diagnose()
        {
            StringBuilder sb = new StringBuilder();
            try
            {
                using (var s = new System.Management.ManagementObjectSearcher("SELECT Manufacturer, Model FROM Win32_ComputerSystem"))
                foreach (System.Management.ManagementObject mo in s.Get()) sb.AppendLine("system|" + mo["Manufacturer"] + "|" + mo["Model"]);
            }
            catch (Exception e) { sb.AppendLine("system|error|" + e.Message); }
            foreach (VendorFan f in HpWmiFans.Read()) sb.AppendLine("HP BIOS|" + f.Name + "|" + f.Rpm.ToString("0", CultureInfo.InvariantCulture));
            foreach (VendorFan f in DellWmiFans.Read()) sb.AppendLine("Dell Command Monitor|" + f.Name + "|" + f.Rpm.ToString("0", CultureInfo.InvariantCulture));
            foreach (VendorFan f in ThinkpadEcFans.Read()) sb.AppendLine("ThinkPad EC|" + f.Name + "|" + f.Rpm.ToString("0", CultureInfo.InvariantCulture));
            foreach (VendorFan f in DellSmmFans.Read()) sb.AppendLine("Dell SMM|" + f.Name + "|" + f.Rpm.ToString("0", CultureInfo.InvariantCulture));
            sb.AppendLine("pawnio|ThinkPad EC|" + ThinkpadEcFans.Status);
            sb.AppendLine("pawnio|Dell SMM|" + DellSmmFans.Status);
            string[] namespaces = { @"root\DCIM\SysMgt", @"root\HP\InstrumentedBIOS", @"root\WMI", @"root\CIMV2" };
            foreach (string ns in namespaces)
            {
                try
                {
                    using (var s = new System.Management.ManagementObjectSearcher(ns, "SELECT * FROM meta_class"))
                    {
                        int n = 0;
                        foreach (System.Management.ManagementClass c in s.Get())
                        {
                            string cn = c["__CLASS"] == null ? "" : c["__CLASS"].ToString();
                            if (cn.IndexOf("Fan", StringComparison.OrdinalIgnoreCase) >= 0 || cn.StartsWith("DDV", StringComparison.OrdinalIgnoreCase) ||
                                cn.IndexOf("Dell", StringComparison.OrdinalIgnoreCase) >= 0 || cn.StartsWith("DCIM_Numeric", StringComparison.OrdinalIgnoreCase))
                            { sb.AppendLine("class|" + ns + "|" + cn); n++; }
                        }
                        if (n == 0) sb.AppendLine("class|" + ns + "|(no fan/Dell/DDV class)");
                    }
                }
                catch (Exception e) { sb.AppendLine("class|" + ns + "|unavailable: " + e.Message.Replace("\r", " ").Replace("\n", " ")); }
            }
            return sb.ToString();
        }
    }

    // ---------------------------------------------------------------- hardware visitor
    class Opener : IVisitor
    {
        public void VisitComputer(IComputer c) { c.Traverse(this); }
        public void VisitHardware(IHardware h) { h.Update(); foreach (IHardware sub in h.SubHardware) sub.Accept(this); }
        public void VisitSensor(ISensor s) { }
        public void VisitParameter(IParameter p) { }
    }

    class Program
    {
        static readonly string Dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "AnkhCore\\Sensors");
        static string ReadingsFile { get { return Path.Combine(Dir, "readings.txt"); } }
        static string CommandFile { get { return Path.Combine(Dir, "command.txt"); } }
        static string ConfigDir;

        static Computer computer;
        static readonly List<IFanActuator> controls = new List<IFanActuator>();
        static FanPolicy policy;
        static readonly FanLiveness liveness = new FanLiveness();
        static FanDecision lastDecision;
        static string controlError = "";
        static volatile bool restored = false;

        static int Main(string[] args)
        {
            for (int i = 0; i < args.Length; i++) if (args[i] == "--selfcheck") return SelfCheck();
            try { Directory.CreateDirectory(Dir); } catch { }
            ConfigDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "FanConfigs");
            for (int i = 0; i < args.Length; i++) if (args[i] == "--check") return 0;
            // read-only diagnostic: print vendor fan readings and exit (no hardware access, no mutex)
            for (int i = 0; i < args.Length; i++) if (args[i] == "--dump-fans")
            {
                foreach (VendorFan f in VendorFans.Read()) Console.WriteLine(f.Source + "|" + f.Name + "|" + f.Rpm.ToString("0", CultureInfo.InvariantCulture));
                Console.Write(VendorFans.Diagnose());
                return 0;
            }

            if (args.Length >= 2 && args[0] == "--ec-parse")   // read-only offline check of the notebook configurations: parse every file, no hardware is touched
            {
                int ok = 0, bad = 0, words = 0, regs = 0; List<string> failures = new List<string>();
                foreach (string f in Directory.GetFiles(args[1], "*.xml"))
                {
                    string why; EcConfig c = EcConfig.Parse(f, out why);
                    if (c == null) { ++bad; if (failures.Count < 8) failures.Add(Path.GetFileName(f) + ": " + why); continue; }
                    ++ok; if (c.Words) ++words; if (c.Writes.Count > 0) ++regs;
                    foreach (EcFanCfg fc in c.Fans) { int v100 = EcConfig.ToValue(fc, 100f), v50 = EcConfig.ToValue(fc, 50f); if (Math.Abs(EcConfig.ToPercent(fc, v100) - 100f) > 1.5f && fc.ReadOverrides.Count == 0 && !fc.Independent) Console.WriteLine("roundtrip? " + Path.GetFileName(f) + " " + fc.Name + " " + v100); }
                }
                Console.WriteLine("EC configs: ok=" + ok + " not usable=" + bad + " (word access " + words + ", with register writes " + regs + ")");
                foreach (string fl in failures) Console.WriteLine("  " + fl);
                return 0;
            }
            bool created;
            Mutex mutex = new Mutex(true, "AnkhSensorsService", out created);
            if (!created) return 0;

            policy = new FanPolicy(DateTime.UtcNow);
            AppDomain.CurrentDomain.ProcessExit += delegate { RestoreAuto("helper exit"); };
            AppDomain.CurrentDomain.UnhandledException += delegate { RestoreAuto("helper error"); };

            computer = new Computer();
            computer.IsCpuEnabled = true; computer.IsGpuEnabled = true; computer.IsMemoryEnabled = true;
            computer.IsMotherboardEnabled = true; computer.IsControllerEnabled = true; computer.IsStorageEnabled = true;
            computer.IsNetworkEnabled = true; computer.IsBatteryEnabled = true;
            try { computer.Open(); } catch (Exception e) { WriteLog("open failed: " + e.Message); }
            try { computer.Accept(new Opener()); } catch { }
            CollectControls();
            SetFansAuto();   // never resume an old manual speed after a restart (2.2 defect F2)
            WriteLog("started; " + controls.Count + " controllable fan(s); automatic");

            bool quit = false;
            while (!quit)   // no time cap: the helper lives exactly as long as AnkhCore (2.2 defect F6)
            {
                try
                {
                    Update();
                    EcFanBank.Refresh(Dir, ConfigDir, controls, LhmHasFans(), LhmHasControls());
                    quit = HandleFans();
                    foreach (IFanActuator a in controls) { try { a.Tick(); } catch (Exception e) { controlError = e.Message; } }
                    WriteReadings();
                }
                catch (Exception e) { WriteLog("loop: " + e.Message); SafeAuto("error: " + e.Message); }
                if (!quit) Thread.Sleep(1000);
            }
            RestoreAuto("exit");
            try { EcFanBank.Release(controls); } catch { }
            try { computer.Close(); } catch { }
            GC.KeepAlive(mutex);
            return 0;
        }

        static void Update()
        {
            foreach (IHardware h in computer.Hardware)
            {
                try { h.Update(); foreach (IHardware s in h.SubHardware) s.Update(); } catch { }
            }
        }

        static void CollectControls()
        {
            controls.Clear();
            foreach (IHardware h in computer.Hardware) CollectControlsFrom(h);
        }
        static void CollectControlsFrom(IHardware h)
        {
            foreach (ISensor s in h.Sensors)
                if (s.Control != null && (s.SensorType == SensorType.Control || s.SensorType == SensorType.Fan) &&
                    FanCapabilityDetector.Assess(true, s.Control.MinSoftwareValue, s.Control.MaxSoftwareValue, "", false).Controllable &&
                    !HasLhm(s.Control))
                    controls.Add(new LhmActuator(s.Control));
            foreach (IHardware sub in h.SubHardware) CollectControlsFrom(sub);
        }

        static bool HasLhm(IControl c) { foreach (IFanActuator a in controls) { LhmActuator l = a as LhmActuator; if (l != null && l.Inner == c) return true; } return false; }
        static bool LhmHasFans()
        {
            foreach (IHardware h in computer.Hardware) { foreach (ISensor x in h.Sensors) if (x.SensorType == SensorType.Fan) return true; foreach (IHardware sub in h.SubHardware) foreach (ISensor y in sub.Sensors) if (y.SensorType == SensorType.Fan) return true; }
            return false;
        }
        static bool LhmHasControls() { foreach (IFanActuator a in controls) if (a is LhmActuator) return true; return false; }
        // ---- fan control ----
        static bool OwnerAlive()
        {
            try { return Process.GetProcessesByName("AnkhCore").Length > 0; } catch { return true; }
        }
        static float MaxTemp()
        {
            float max = -1;
            foreach (IHardware h in computer.Hardware)
            {
                if (h.HardwareType != HardwareType.Cpu && h.HardwareType != HardwareType.GpuNvidia &&
                    h.HardwareType != HardwareType.GpuAmd && h.HardwareType != HardwareType.GpuIntel) continue;
                foreach (ISensor s in h.Sensors)
                    if (s.SensorType == SensorType.Temperature && s.Value.HasValue && s.Value.Value > max && s.Value.Value < 150) max = s.Value.Value;
            }
            return max;
        }
        static bool HandleFans()
        {
            string cmd = null; DateTime stamp = DateTime.MinValue;
            try { if (File.Exists(CommandFile)) { cmd = File.ReadAllText(CommandFile); stamp = File.GetLastWriteTimeUtc(CommandFile); } } catch { }
            FanDecision d = policy.Tick(DateTime.UtcNow, cmd, stamp, OwnerAlive(), MaxTemp(), controls.Count > 0);
            if (d.Changed || lastDecision == null || d.Reason != lastDecision.Reason)
            {
                controlError = "";
                if (d.Mode == FanMode.Manual)
                {
                    if (d.TargetFanIndex >= 0 && d.TargetFanIndex < controls.Count)
                    {
                        try { controls[d.TargetFanIndex].SetPercent(d.Percent); }
                        catch (Exception e) { controlError = e.Message; }
                    }
                    else
                    {
                        foreach (IFanActuator c in controls)
                        {
                            try { c.SetPercent(d.Percent); }
                            catch (Exception e) { controlError = e.Message; }
                        }
                    }
                    if (controlError.Length > 0) { SetFansAuto(); d = policy.Reject("hardware rejected the command: " + controlError); }
                }
                else if (d.TargetFanIndex >= 0 && d.TargetFanIndex < controls.Count)
                {
                    try { controls[d.TargetFanIndex].SetAuto(); } catch { }
                }
                else SetFansAuto();
                WriteLog("fans: " + (d.Mode == FanMode.Manual ? "manual " + d.Percent.ToString("0", CultureInfo.InvariantCulture) + "%" : "automatic") + " (" + d.Reason + ")");
            }
            lastDecision = d;
            return d.Quit;
        }
        static void SetFansAuto() { foreach (IFanActuator c in controls) { try { c.SetAuto(); } catch { } } }
        static void SafeAuto(string why) { SetFansAuto(); WriteLog("fans: automatic (" + why + ")"); }
        static void RestoreAuto(string why)
        {
            if (restored) return;
            restored = true;
            SetFansAuto();
            WriteLog("fans: automatic (" + why + ")");
        }

        // ---- readings ----
        static string F(float? v) { return (!v.HasValue || float.IsNaN(v.Value) || float.IsInfinity(v.Value)) ? "" : v.Value.ToString("0.##", CultureInfo.InvariantCulture); }

        // 2.12: on a notebook with two graphics chips the first one listed is often the Intel one, which has no temperature sensor, so the GPU temperature showed
        // "not exposed" although the other chip reports it. The chip that reports a temperature is preferred, a discrete one (NVIDIA / AMD) before an Intel one; if none does, the first.
        static IHardware ChoosePrimaryGpu()
        {
            IHardware best = null; int bestScore = -1;
            foreach (IHardware h in computer.Hardware)
            {
                if (GroupName(h.HardwareType) != "GPU") continue;
                bool temp = false;
                foreach (ISensor s in h.Sensors) if (s.SensorType == SensorType.Temperature && s.Value.HasValue && !float.IsNaN(s.Value.Value) && s.Value.Value > 0) { temp = true; break; }
                int score = (temp ? 2 : 0) + (h.HardwareType == HardwareType.GpuIntel ? 0 : 1);
                if (score > bestScore) { best = h; bestScore = score; }
            }
            return best;
        }

        static void WriteReadings()
        {
            seen.Clear(); maxVal.Clear();
            primaryGpu = ChoosePrimaryGpu(); primaryDisk = null;
            StringBuilder sb = new StringBuilder();
            sb.Append("TIME=").Append(DateTime.Now.ToString("HH:mm:ss")).Append("\n");
            int fanIndex = 0;
            foreach (IHardware h in computer.Hardware)
            {
                string group = GroupName(h.HardwareType);
                if (group == null) continue;
                EmitHardware(sb, h, group, ref fanIndex);
                foreach (IHardware sub in h.SubHardware) EmitHardware(sb, sub, group, ref fanIndex);
            }
            foreach (KeyValuePair<string, float> kv in maxVal) sb.Append(kv.Key).Append("=").Append(kv.Value.ToString("0.##", CultureInfo.InvariantCulture)).Append("\n");
            if (fanIndex == 0 && EcFanBank.Active)   // AnkhCore's own fan control: controllable fans, the RPM (when a vendor interface has one) paired by order
            {
                List<VendorFan> vend = VendorFans.Read();
                for (int i = 0; i < EcFanBank.Engine.Fans.Count; ++i)
                {
                    EcFan ef = EcFanBank.Engine.Fans[i]; VendorFan vf = i < vend.Count ? vend[i] : null;
                    if (vf != null) { liveness.Observe(vf.Source + ":" + vf.Name, vf.Rpm); }
                    sb.Append("FAN.").Append(fanIndex).Append(".NAME=").Append(Clean(ef.Name)).Append("\n");
                    if (vf != null) sb.Append("FAN.").Append(fanIndex).Append(".RPM=").Append(vf.Rpm.ToString("0", CultureInfo.InvariantCulture)).Append("\n");
                    sb.Append("FAN.").Append(fanIndex).Append(".CONTROL=1\n");
                    sb.Append("FAN.").Append(fanIndex).Append(".CAPABILITY=controllable\n");
                    sb.Append("FAN.").Append(fanIndex).Append(".SOURCE=AnkhCore EC").Append(vf != null ? " + " + vf.Source : "").Append("\n");
                    sb.Append("FAN.").Append(fanIndex).Append(".LIVE=").Append(vf != null && liveness.IsLive(vf.Source + ":" + vf.Name) ? "1" : "0").Append("\n");
                    float rp = -1; try { rp = ef.ReadPercentNow(); } catch { }
                    if (rp >= 0) sb.Append("FAN.").Append(fanIndex).Append(".PERCENT=").Append(rp.ToString("0", CultureInfo.InvariantCulture)).Append("\n");
                    sb.Append("FAN.").Append(fanIndex).Append(".MINPCT=0\n");
                    sb.Append("FAN.").Append(fanIndex).Append(".MAXPCT=100\n");
                    sb.Append("FAN.").Append(fanIndex).Append(".CTRLMODE=").Append(ef.IsManual ? "software" : "default").Append("\n");
                    fanIndex++;
                }
            }
            else if (fanIndex == 0)   // LibreHardwareMonitor sees no fan: add read-only vendor readings
                foreach (VendorFan f in VendorFans.Read())
                {
                    liveness.Observe(f.Source + ":" + f.Name, f.Rpm);
                    sb.Append("FAN.").Append(fanIndex).Append(".NAME=").Append(Clean(f.Name)).Append("\n");
                    sb.Append("FAN.").Append(fanIndex).Append(".RPM=").Append(f.Rpm.ToString("0", CultureInfo.InvariantCulture)).Append("\n");
                    sb.Append("FAN.").Append(fanIndex).Append(".CONTROL=0\n");
                    sb.Append("FAN.").Append(fanIndex).Append(".CAPABILITY=vendor_readonly\n");
                    sb.Append("FAN.").Append(fanIndex).Append(".SOURCE=").Append(f.Source).Append("\n");
                    sb.Append("FAN.").Append(fanIndex).Append(".LIVE=").Append(liveness.IsLive(f.Source + ":" + f.Name) ? "1" : "0").Append("\n");
                    fanIndex++;
                }
            sb.Append("ECFAN.STATE=").Append(Clean(EcFanBank.State)).Append("\n");
            sb.Append("FANCOUNT=").Append(fanIndex).Append("\n");
            sb.Append("CONTROLS=").Append(controls.Count).Append("\n");
            // control state as actually read back from the hardware (2.2 defect F5)
            FanDecision d = lastDecision;
            sb.Append("FANCTL.MODE=").Append(d != null && d.Mode == FanMode.Manual ? "manual" : "auto").Append("\n");
            sb.Append("FANCTL.REQUESTED=").Append(d != null && d.Mode == FanMode.Manual ? d.Percent.ToString("0", CultureInfo.InvariantCulture) : "").Append("\n");
            float sum = 0; int n = 0;
            foreach (IFanActuator c in controls) { try { if (c.IsManual) { sum += c.Percent; n++; } } catch { } }
            sb.Append("FANCTL.READBACK=").Append(n > 0 ? (sum / n).ToString("0", CultureInfo.InvariantCulture) : "").Append("\n");
            sb.Append("FANCTL.REASON=").Append(Clean(d != null ? d.Reason : "start-up")).Append("\n");
            sb.Append("FANCTL.ERROR=").Append(Clean(controlError)).Append("\n");
            sb.Append("MODEL=").Append(DetectModel()).Append("\n");
            string match = MatchConfig();
            sb.Append("NBFC_MATCH=").Append(match == null ? "" : match).Append("\n");
            try
            {
                string tmp = ReadingsFile + ".tmp";
                File.WriteAllText(tmp, sb.ToString(), new UTF8Encoding(false));
                if (File.Exists(ReadingsFile)) File.Replace(tmp, ReadingsFile, null);   // atomic: readers never see a missing file
                else File.Move(tmp, ReadingsFile);
            }
            catch { }
        }

        static string GroupName(HardwareType t)
        {
            switch (t)
            {
                case HardwareType.Cpu: return "CPU";
                case HardwareType.GpuNvidia:
                case HardwareType.GpuAmd:
                case HardwareType.GpuIntel: return "GPU";
                case HardwareType.Memory: return "RAM";
                case HardwareType.Storage: return "DISK";
                case HardwareType.Network: return "NET";
                case HardwareType.Motherboard:
                case HardwareType.SuperIO:
                case HardwareType.EmbeddedController:
                case HardwareType.Cooler: return "BOARD";
                case HardwareType.Battery: return "BAT";
                default: return null;
            }
        }

        static void EmitHardware(StringBuilder sb, IHardware h, string group, ref int fanIndex)
        {
            if (group == "GPU") { if (primaryGpu == null) primaryGpu = h; else if (primaryGpu != h) return; }
            if (group == "DISK") { if (primaryDisk == null) primaryDisk = h; else if (primaryDisk != h) return; }
            bool gpuEmitted = false;
            foreach (ISensor s in h.Sensors)
            {
                if (!s.Value.HasValue || float.IsNaN(s.Value.Value) || float.IsInfinity(s.Value.Value)) continue;
                string val = F(s.Value);
                if (val.Length == 0) continue;
                if (group == "GPU" && !gpuEmitted) { EmitFirst(sb, "GPU.NAME", Clean(h.Name)); gpuEmitted = true; }
                switch (s.SensorType)
                {
                    case SensorType.Temperature:
                        EmitFirst(sb, group + ".TEMP", val);
                        if (Clean(s.Name).IndexOf("Hot", StringComparison.OrdinalIgnoreCase) >= 0) EmitFirst(sb, group + ".HOTSPOT", val);
                        break;
                    case SensorType.Load:
                        if (Clean(s.Name).IndexOf("Memory", StringComparison.OrdinalIgnoreCase) >= 0 && group == "GPU") EmitFirst(sb, "GPU.VRAMLOAD", val);
                        else EmitFirst(sb, group + ".LOAD", val);
                        break;
                    case SensorType.Clock: EmitMax(group + ".CLOCK", s.Value.Value); break;
                    case SensorType.Power: EmitFirst(sb, group + ".POWER", val); break;
                    case SensorType.Data:
                        if (group == "GPU" && Clean(s.Name).IndexOf("Used", StringComparison.OrdinalIgnoreCase) >= 0) EmitFirst(sb, "GPU.VRAMUSED", val);
                        break;
                    case SensorType.Throughput:
                        if (group == "NET")
                        {
                            if (Clean(s.Name).IndexOf("Down", StringComparison.OrdinalIgnoreCase) >= 0) AddNet(sb, "NET.DOWN", s.Value.Value);
                            else if (Clean(s.Name).IndexOf("Up", StringComparison.OrdinalIgnoreCase) >= 0) AddNet(sb, "NET.UP", s.Value.Value);
                        }
                        break;
                    case SensorType.Fan:
                        FanCapabilityInfo cap = FanCapabilityDetector.Assess(
                            s.Control != null,
                            s.Control != null ? s.Control.MinSoftwareValue : -1f,
                            s.Control != null ? s.Control.MaxSoftwareValue : -1f,
                            s.Control != null ? s.Control.ControlMode.ToString().ToLowerInvariant() : "",
                            false);
                        sb.Append("FAN.").Append(fanIndex).Append(".NAME=").Append(Clean(s.Name)).Append("\n");
                        sb.Append("FAN.").Append(fanIndex).Append(".RPM=").Append(val).Append("\n");
                        sb.Append("FAN.").Append(fanIndex).Append(".CONTROL=").Append(cap.Controllable ? "1" : "0").Append("\n");
                        sb.Append("FAN.").Append(fanIndex).Append(".CAPABILITY=").Append(cap.WireName).Append("\n");
                        sb.Append("FAN.").Append(fanIndex).Append(".SOURCE=LibreHardwareMonitor\n");
                        sb.Append("FAN.").Append(fanIndex).Append(".LIVE=1\n");
                        if (s.Control != null)
                        {
                            if (cap.Controllable && s.Control.SoftwareValue >= 0) sb.Append("FAN.").Append(fanIndex).Append(".PERCENT=").Append(F(s.Control.SoftwareValue)).Append("\n");
                            sb.Append("FAN.").Append(fanIndex).Append(".MINPCT=").Append(F(s.Control.MinSoftwareValue)).Append("\n");
                            sb.Append("FAN.").Append(fanIndex).Append(".MAXPCT=").Append(F(s.Control.MaxSoftwareValue)).Append("\n");
                            sb.Append("FAN.").Append(fanIndex).Append(".CTRLMODE=").Append(Clean(cap.ControlMode)).Append("\n");
                        }
                        fanIndex++;
                        break;
                }
            }
        }

        static readonly HashSet<string> seen = new HashSet<string>();
        static readonly Dictionary<string, float> maxVal = new Dictionary<string, float>();
        static IHardware primaryGpu, primaryDisk;
        static void EmitFirst(StringBuilder sb, string key, string val)
        {
            if (seen.Contains(key)) return;
            seen.Add(key); sb.Append(key).Append("=").Append(val).Append("\n");
        }
        static void EmitMax(string key, float v)   // 2.2 called this "max" but kept the first value
        {
            float old;
            if (!maxVal.TryGetValue(key, out old) || v > old) maxVal[key] = v;
        }
        static void AddNet(StringBuilder sb, string key, float v)
        {
            if (seen.Contains(key)) return;
            seen.Add(key); sb.Append(key).Append("=").Append(v.ToString("0", CultureInfo.InvariantCulture)).Append("\n");
        }

        static string Clean(string s)
        {
            if (s == null) return "";
            StringBuilder b = new StringBuilder();
            foreach (char c in s) if (c != '\n' && c != '\r' && c != '=') b.Append(c);
            return b.ToString();
        }

        // ---- model + NBFC config name matching (display only; NBFC itself is not used) ----
        static string modelCache;
        static string DetectModel()
        {
            if (modelCache != null) return modelCache;
            try
            {
                using (var searcher = new System.Management.ManagementObjectSearcher("SELECT Manufacturer, Model FROM Win32_ComputerSystem"))
                foreach (System.Management.ManagementObject mo in searcher.Get())
                {
                    object man = mo["Manufacturer"]; object mod = mo["Model"];
                    modelCache = Clean(((man == null ? "" : man.ToString()) + " " + (mod == null ? "" : mod.ToString())).Trim());
                    return modelCache;
                }
            }
            catch { }
            modelCache = "";
            return modelCache;
        }
        // ---- read-only installation check (installer verification) ----
        // Loads LibreHardwareMonitor with every sensor group, reads all hardware once and reports any
        // assembly the runtime could not find. No mutex, no files, no fan commands; works unelevated
        // (fewer sensors without admin rights). Exit code 0 = every dependency resolved.
        static int SelfCheck()
        {
            List<string> unresolved = new List<string>();
            AppDomain.CurrentDomain.AssemblyResolve += delegate(object s, ResolveEventArgs e)
            {
                if (e.Name.IndexOf(".resources", StringComparison.OrdinalIgnoreCase) < 0 && e.Name.IndexOf("XmlSerializers", StringComparison.OrdinalIgnoreCase) < 0) unresolved.Add(e.Name);
                return null;
            };
            int hw = 0, sensors = 0;
            Computer c = new Computer();
            c.IsCpuEnabled = true; c.IsGpuEnabled = true; c.IsMemoryEnabled = true; c.IsMotherboardEnabled = true;
            c.IsControllerEnabled = true; c.IsStorageEnabled = true; c.IsNetworkEnabled = true; c.IsBatteryEnabled = true;
            try { c.Open(); } catch (Exception e) { Console.WriteLine("OPEN_ERROR=" + e.GetType().Name + ": " + e.Message); }
            try
            {
                foreach (IHardware h in c.Hardware)
                {
                    h.Update(); hw++; sensors += h.Sensors.Length;
                    foreach (IHardware sub in h.SubHardware) { sub.Update(); hw++; sensors += sub.Sensors.Length; }
                }
            }
            catch (Exception e) { Console.WriteLine("READ_ERROR=" + e.GetType().Name + ": " + e.Message); }
            try { c.Close(); } catch { }
            Console.WriteLine("HARDWARE=" + hw);
            Console.WriteLine("SENSORS=" + sensors);
            foreach (string u in unresolved) Console.WriteLine("UNRESOLVED=" + u);
            return unresolved.Count == 0 ? 0 : 3;
        }

        static List<string> configNames;
        static string MatchConfig()
        {
            string model = DetectModel();
            if (model.Length == 0) return null;
            if (configNames == null)
            {
                configNames = new List<string>();
                try { if (Directory.Exists(ConfigDir)) foreach (string f in Directory.GetFiles(ConfigDir, "*.xml")) configNames.Add(Path.GetFileNameWithoutExtension(f)); } catch { }
            }
            foreach (string name in configNames)
                if (model.IndexOf(name, StringComparison.OrdinalIgnoreCase) >= 0 || name.IndexOf(model, StringComparison.OrdinalIgnoreCase) >= 0) return name;
            return null;
        }

        static void WriteLog(string msg)
        {
            try { File.AppendAllText(Path.Combine(Dir, "sensors.log"), DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "  " + msg + "\r\n"); } catch { }
        }
    }
}
