// sfaim - fine-tune the landing target on the Starfield star map until it matches a latitude/longitude.
// Does not touch game memory. It moves the mouse and clicks, and reads the clicked coordinates from the
// Celestial Positioning System overlay (Nexus mod 17020; game in windowed / borderless mode).
// It only places the landing target. It never presses "Land": you confirm the landing yourself.
//
// Build:  C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /optimize /platform:x64 /out:sfaim.exe sfaim.cs
// Usage:  sfaim <targetLat> <targetLon> [radiusKm]
//         sfaim --backup <path to OSB_Restore.json>      aims at the backed-up beacon spot
// Steps:  1. start sfaim, 2. switch to the game, star map zoomed in on the planet,
//         3. click once near the target. sfaim takes over from there. Press Esc to abort.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

static class SfAim
{
    // ---------- Win32 ----------
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder b, int m);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, StringBuilder l);
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern short GetAsyncKeyState(int vk);
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);

    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public MOUSEINPUT mi; public int pad0, pad1; }
    const uint INPUT_MOUSE = 0, MOVE = 0x0001, LDOWN = 0x0002, LUP = 0x0004;

    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
    static readonly Regex Rx = new Regex(@"LAT:\s*(-?[0-9.]+)[^|]*\|\s*LONG:\s*(-?[0-9.]+)");
    static int gamePid;

    static int Main(string[] args)
    {
        double tLat, tLon, radiusKm = 0;
        if (args.Length >= 2 && args[0] == "--backup") { if (!FromBackup(args[1], out tLat, out tLon, out radiusKm)) return 1; }
        else if (args.Length >= 2)
        {
            tLat = double.Parse(args[0], Inv); tLon = double.Parse(args[1], Inv);
            if (args.Length >= 3) radiusKm = double.Parse(args[2], Inv);
        }
        else { Console.WriteLine("usage: sfaim <lat> <lon> [radiusKm] | sfaim --backup OSB_Restore.json"); return 1; }

        Process[] ps = Process.GetProcessesByName("Starfield");
        if (ps.Length == 0) { Console.WriteLine("Starfield.exe is not running."); return 2; }
        gamePid = ps[0].Id;

        Console.WriteLine("Target {0:F5}, {1:F5}{2}", tLat, tLon, radiusKm > 0 ? string.Format(Inv, " (radius {0} km)", radiusKm) : "");
        Console.WriteLine("Switch to the game, open the star map on the planet, zoom in, and click once near the target.");
        Console.WriteLine("Press Esc at any time to stop. sfaim never presses Land.");

        double[] cur = WaitForReading(null, 300000);
        if (cur == null) { Console.WriteLine("No reading from the CPS overlay."); return 3; }
        Report("start", cur, tLat, tLon, radiusKm);
        WaitOverlayGone(7000);

        // Probe: measure how far the target moves per mouse count in x and y.
        const int S = 25;
        double[] l1 = Step(S, 0, cur); if (l1 == null) return Abort();
        double[] back = Step(-S, S, l1); if (back == null) return Abort();   // now at (0, S)
        double[] l2 = back;
        Step(0, -S, l2);                                                       // back to (0, 0)
        double[,] J = new double[2, 2];
        J[0, 0] = (l1[0] - cur[0]) / S; J[1, 0] = DLon(l1[1], cur[1]) / S;
        J[0, 1] = (l2[0] - cur[0]) / S; J[1, 1] = DLon(l2[1], cur[1]) / S;
        Console.WriteLine("deg per mouse count: dx -> ({0:E2}, {1:E2})  dy -> ({2:E2}, {3:E2})", J[0, 0], J[1, 0], J[0, 1], J[1, 1]);

        double[] best = cur; double bestErr = Err(cur, tLat, tLon);
        for (int it = 1; it <= 20; it++)
        {
            double eLat = tLat - cur[0], eLon = DLon(tLon, cur[1]);
            double det = J[0, 0] * J[1, 1] - J[0, 1] * J[1, 0];
            if (Math.Abs(det) < 1e-18) { Console.WriteLine("Cannot estimate the map scale (no change per count)."); break; }
            double px = ( J[1, 1] * eLat - J[0, 1] * eLon) / det;
            double py = (-J[1, 0] * eLat + J[0, 0] * eLon) / det;
            double mag = Math.Sqrt(px * px + py * py);
            if (mag > 400) { px *= 400 / mag; py *= 400 / mag; }
            int ix = (int)Math.Round(px), iy = (int)Math.Round(py);
            if (ix == 0 && iy == 0) { Console.WriteLine("Within one mouse count of the target; this is as close as the map allows."); break; }

            double[] nxt = Step(ix, iy, cur);
            if (nxt == null) return Abort();
            // Broyden update of the scale estimate
            double dLat = nxt[0] - cur[0], dLon = DLon(nxt[1], cur[1]);
            double pred0 = J[0, 0] * ix + J[0, 1] * iy, pred1 = J[1, 0] * ix + J[1, 1] * iy;
            double nn = (double)ix * ix + (double)iy * iy;
            J[0, 0] += (dLat - pred0) * ix / nn; J[0, 1] += (dLat - pred0) * iy / nn;
            J[1, 0] += (dLon - pred1) * ix / nn; J[1, 1] += (dLon - pred1) * iy / nn;
            cur = nxt;
            Report("step " + it, cur, tLat, tLon, radiusKm);
            double e = Err(cur, tLat, tLon);
            if (e < bestErr) { bestErr = e; best = cur; }
            if (e < 0.0002) { Console.WriteLine("Target reached (CPS shows 4 decimals)."); break; }
            WaitOverlayGone(7000);
        }
        Report("final", cur, tLat, tLon, radiusKm);
        Console.WriteLine("The landing target is placed. Check it and confirm the landing in game.");
        return 0;
    }

    // Remove the current marker (click on it), move by (dx, dy) counts, place a new marker, read it.
    static double[] Step(int dx, int dy, double[] prev)
    {
        if (!GameInFront()) return null;
        Click(); Thread.Sleep(400);
        Move(dx, dy); Thread.Sleep(200);
        if (!GameInFront()) return null;
        Click();
        double[] r = WaitForReading(prev, 6000);
        if (r == null) { Console.WriteLine("No overlay after a click; is the CPS overlay visible?"); return null; }
        WaitOverlayGone(7000);
        return r;
    }

    static double[] WaitForReading(double[] notEqual, int timeoutMs)
    {
        var sw = Stopwatch.StartNew();
        while (sw.ElapsedMilliseconds < timeoutMs)
        {
            if ((GetAsyncKeyState(0x1B) & 0x8000) != 0) return null; // Esc
            string t = OverlayText();
            Match m = Rx.Match(t);
            if (m.Success) return new[] { double.Parse(m.Groups[1].Value, Inv), double.Parse(m.Groups[2].Value, Inv) };
            Thread.Sleep(100);
        }
        return null;
    }

    static void WaitOverlayGone(int timeoutMs)
    {
        var sw = Stopwatch.StartNew();
        while (sw.ElapsedMilliseconds < timeoutMs && OverlayText().Length > 0) Thread.Sleep(100);
    }

    static string OverlayText()
    {
        var sb = new StringBuilder();
        EnumWindows((h, l) =>
        {
            var cls = new StringBuilder(256); GetClassName(h, cls, 256);
            if (cls.ToString() == "CPSStableOverlayClass")
                EnumChildWindows(h, (c, l2) => { sb.Append(GetText(c)).Append(' '); return true; }, IntPtr.Zero);
            return true;
        }, IntPtr.Zero);
        return sb.ToString().Trim();
    }

    static string GetText(IntPtr h)
    {
        int len = (int)SendMessage(h, 0x0E, IntPtr.Zero, IntPtr.Zero);
        if (len <= 0) return "";
        var sb = new StringBuilder(len + 1); SendMessage(h, 0x0D, new IntPtr(len + 1), sb);
        return sb.ToString();
    }

    static bool GameInFront()
    {
        if ((GetAsyncKeyState(0x1B) & 0x8000) != 0) { Console.WriteLine("Esc pressed."); return false; }
        uint pid; GetWindowThreadProcessId(GetForegroundWindow(), out pid);
        if (pid != gamePid) { Console.WriteLine("The game is not the active window; stopping (no clicks sent)."); return false; }
        return true;
    }

    static void Move(int dx, int dy)
    {
        // send in small chunks so menu acceleration stays low
        while (dx != 0 || dy != 0)
        {
            int sx = Math.Max(-10, Math.Min(10, dx)), sy = Math.Max(-10, Math.Min(10, dy));
            Send(new MOUSEINPUT { dx = sx, dy = sy, dwFlags = MOVE });
            dx -= sx; dy -= sy; Thread.Sleep(5);
        }
    }

    static void Click()
    {
        Send(new MOUSEINPUT { dwFlags = LDOWN }); Thread.Sleep(60);
        Send(new MOUSEINPUT { dwFlags = LUP });
    }

    static void Send(MOUSEINPUT mi)
    {
        var inp = new[] { new INPUT { type = INPUT_MOUSE, mi = mi } };
        SendInput(1, inp, Marshal.SizeOf(typeof(INPUT)));
    }

    static double DLon(double a, double b) { double d = a - b; if (d > 180) d -= 360; else if (d < -180) d += 360; return d; }
    static double Err(double[] c, double lat, double lon) { return Math.Sqrt(Math.Pow(lat - c[0], 2) + Math.Pow(DLon(lon, c[1]) * Math.Cos(lat * Math.PI / 180), 2)); }

    static void Report(string tag, double[] c, double lat, double lon, double radiusKm)
    {
        double e = Err(c, lat, lon);
        string km = radiusKm > 0 ? string.Format(Inv, "  ~{0:F2} km off", e * Math.PI / 180 * radiusKm) : "";
        Console.WriteLine("{0,-8} lat {1:F4} lon {2:F4}  error {3:F4} deg{4}", tag, c[0], c[1], e, km);
    }

    static int Abort() { Console.WriteLine("Stopped."); return 4; }

    // Beacon spot = old landing center + beacon local offset (+X east, +Y north, meters).
    static bool FromBackup(string path, out double lat, out double lon, out double radiusKm)
    {
        lat = lon = radiusKm = 0;
        if (!File.Exists(path)) { Console.WriteLine("Not found: " + path); return false; }
        string j = File.ReadAllText(path);
        Func<string, double> num = k =>
        {
            Match m = Regex.Match(j, "\"" + k + "\"\\s*:\\s*(-?[0-9.eE+-]+)");
            if (!m.Success) throw new InvalidDataException("missing " + k + " in " + path);
            return double.Parse(m.Groups[1].Value, Inv);
        };
        radiusKm = num("radiusKm");
        double R = radiusKm * 1000.0, lat0 = num("originLat"), lon0 = num("originLon");
        lat = lat0 + num("beaconY") / R * 180.0 / Math.PI;
        lon = lon0 + num("beaconX") / (R * Math.Cos(lat0 * Math.PI / 180.0)) * 180.0 / Math.PI;
        return true;
    }
}
