// Unit tests for AnkhSensors.FanPolicy (pure logic; no hardware, no driver, no files).
// Build + run: tools\test_fanpolicy.cmd
using System;
using AnkhSensors;

static class FanPolicyTests
{
    static int fails = 0, passes = 0;
    static void Check(bool ok, string name) { if (ok) passes++; else { fails++; Console.WriteLine("FAIL  " + name); } }

    static int Main()
    {
        DateTime t0 = new DateTime(2026, 11, 7, 12, 0, 0, DateTimeKind.Utc);
        Func<int, DateTime> at = s => t0.AddSeconds(s);

        // F2: a command written before the helper started is ignored -> stays automatic
        var p = new FanPolicy(t0);
        var d = p.Tick(at(1), "manual 80", at(-5), true, 50, true);
        Check(d.Mode == FanMode.Auto, "stale pre-start command ignored");

        // manual accepted, clamped to minimum, ramped
        p = new FanPolicy(t0);
        d = p.Tick(at(1), "manual 10", at(1), true, 50, true);
        Check(d.Mode == FanMode.Manual && d.Percent == FanPolicy.MinPercent && d.Percent == 40f, "manual 10% raised to the 40% minimum (D23/F4)");
        d = p.Tick(at(2), "manual 90", at(2), true, 50, true);
        Check(d.Percent == 50, "ramp +10 per tick (40 -> 50)");
        for (int i = 3; i < 12; i++) d = p.Tick(at(i), "manual 90", at(i), true, 50, true);
        Check(d.Percent == 90, "ramp reaches target 90");
        d = p.Tick(at(12), "manual 150", at(12), true, 50, true);
        d = p.Tick(at(13), "manual 150", at(13), true, 50, true);
        Check(d.Percent == 100, "manual 150% clamped to 100%");

        // F3/F1: lease expiry -> automatic
        p = new FanPolicy(t0);
        p.Tick(at(1), "manual 50", at(1), true, 50, true);
        d = p.Tick(at(1 + (int)FanPolicy.LeaseSeconds + 1), "manual 50", at(1), true, 50, true);
        Check(d.Mode == FanMode.Auto && d.Reason.Contains("lease"), "lease expired -> automatic");

        // lease renewal keeps manual (same content, newer timestamp)
        p = new FanPolicy(t0);
        p.Tick(at(1), "manual 50", at(1), true, 50, true);
        d = p.Tick(at(9), "manual 50", at(8), true, 50, true);
        d = p.Tick(at(17), "manual 50", at(16), true, 50, true);
        Check(d.Mode == FanMode.Manual, "renewed lease keeps manual");

        // command file deleted while manual -> automatic
        d = p.Tick(at(18), null, DateTime.MinValue, true, 50, true);
        Check(d.Mode == FanMode.Auto, "missing command file -> automatic");

        // thermal override: brief Turbo spikes (100 C for a few seconds) keep manual; sustained heat does not
        p = new FanPolicy(t0);
        p.Tick(at(1), "manual 40", at(1), true, 50, true);
        d = p.Tick(at(2), "manual 40", at(2), true, 100, true);
        d = p.Tick(at(4), "manual 40", at(4), true, 100, true);
        Check(d.Mode == FanMode.Manual, "3 s Turbo spike at 100 C does not drop manual");
        d = p.Tick(at(5), "manual 40", at(5), true, 65, true);
        d = p.Tick(at(6), "manual 40", at(6), true, 99, true);
        d = p.Tick(at(12), "manual 40", at(12), true, 99, true);
        Check(d.Mode == FanMode.Manual, "spike timer resets when the temperature falls");
        d = p.Tick(at(14), "manual 40", at(14), true, 95, true);
        Check(d.Mode == FanMode.Auto && d.Reason.Contains("thermal"), "sustained heat (8 s >= 92 C) -> automatic");

        // no controllable fan: manual refused (never a false success)
        p = new FanPolicy(t0);
        d = p.Tick(at(1), "manual 60", at(1), true, 50, false);
        Check(d.Mode == FanMode.Auto && d.Reason.Contains("refused"), "manual refused when nothing is controllable");

        // invalid percentage refused
        p = new FanPolicy(t0);
        d = p.Tick(at(1), "manual abc", at(1), true, 50, true);
        Check(d.Mode == FanMode.Auto && d.Reason.Contains("invalid"), "invalid percentage refused");

        // explicit auto and quit
        p = new FanPolicy(t0);
        p.Tick(at(1), "manual 60", at(1), true, 50, true);
        d = p.Tick(at(2), "auto", at(2), true, 50, true);
        Check(d.Mode == FanMode.Auto, "auto command -> automatic");
        d = p.Tick(at(3), "quit", at(3), true, 50, true);
        Check(d.Mode == FanMode.Auto && d.Quit, "quit -> automatic and exit");

        // F1/F6: AnkhCore gone -> automatic and exit
        p = new FanPolicy(t0);
        p.Tick(at(1), "manual 60", at(1), true, 50, true);
        d = p.Tick(at(5), "manual 60", at(5), false, 50, true);
        Check(d.Mode == FanMode.Manual && !d.Quit, "short owner gap tolerated");
        d = p.Tick(at(1 + (int)FanPolicy.OrphanSeconds + 2), "manual 60", at(15), false, 50, true);
        Check(d.Mode == FanMode.Auto && d.Quit, "owner gone -> automatic and exit");

        // hardware rejection: automatic, and the same command is not retried
        p = new FanPolicy(t0);
        p.Tick(at(1), "manual 60", at(1), true, 50, true);
        d = p.Reject("hardware rejected the command: test");
        Check(d.Mode == FanMode.Auto, "reject -> automatic");
        d = p.Tick(at(2), "manual 60", at(1), true, 50, true);
        Check(d.Mode == FanMode.Auto, "rejected command not retried");

        // vendor reading liveness: frozen values are never reported as live
        var lv = new FanLiveness();
        lv.Observe("HP BIOS:CPU Fan", 4819); lv.Observe("HP BIOS:CPU Fan", 4819); lv.Observe("HP BIOS:CPU Fan", 4819.4f);
        Check(!lv.IsLive("HP BIOS:CPU Fan"), "constant vendor reading not live");
        lv.Observe("HP BIOS:CPU Fan", 3100);
        Check(lv.IsLive("HP BIOS:CPU Fan"), "changed vendor reading becomes live");
        Check(!lv.IsLive("HP BIOS:GPU Fan"), "unseen fan not live");

        // FCD-01: per-fan capability assessment
        var capNone = FanCapabilityDetector.Assess(false, -1f, -1f, "", false);
        Check(!capNone.Controllable && capNone.Kind == FanCapabilityKind.MonitorOnly && capNone.WireName == "monitor_only", "no IControl -> monitor_only");
        var capVendor = FanCapabilityDetector.Assess(false, -1f, -1f, "", true);
        Check(!capVendor.Controllable && capVendor.Kind == FanCapabilityKind.VendorReadOnly && capVendor.WireName == "vendor_readonly", "vendor WMI -> vendor_readonly");
        var capGood = FanCapabilityDetector.Assess(true, 0f, 100f, "software", false);
        Check(capGood.Controllable && capGood.Kind == FanCapabilityKind.Controllable && capGood.WireName == "controllable" && capGood.MinPercent == 0f && capGood.MaxPercent == 100f, "valid IControl -> controllable");
        var capBadRange = FanCapabilityDetector.Assess(true, 0f, 35f, "default", false);
        Check(!capBadRange.Controllable && capBadRange.Kind == FanCapabilityKind.MonitorOnly, "max < 40% floor -> not controllable");
        var capZeroRange = FanCapabilityDetector.Assess(true, 50f, 50f, "default", false);
        Check(!capZeroRange.Controllable && capZeroRange.Kind == FanCapabilityKind.MonitorOnly, "zero-width range -> not controllable");

        // Per-fan manual & auto commands
        p = new FanPolicy(t0);
        d = p.Tick(at(1), "manual 1 65", at(1), true, 50, true);
        Check(d.Mode == FanMode.Manual && d.TargetFanIndex == 1 && d.Percent == 65f, "per-fan manual 1 65 sets TargetFanIndex=1 and Percent=65");
        p = new FanPolicy(t0);
        d = p.Tick(at(1), "manual 0 15", at(1), true, 50, true);
        Check(d.Mode == FanMode.Manual && d.TargetFanIndex == 0 && d.Percent == 40f, "per-fan manual 0 15 clamped to 40% minimum");
        d = p.Tick(at(2), "auto 1", at(2), true, 50, true);
        Check(d.Mode == FanMode.Auto && d.TargetFanIndex == 1, "per-fan auto 1 targets fan 1");

        Console.WriteLine((fails == 0 ? "ALL PASSED" : fails + " FAILED") + " (" + passes + " passed)");
        return fails == 0 ? 0 : 1;
    }
}
