// memscan - find where Starfield keeps the player's planetary latitude/longitude.
// Uses the CPS overlay (Nexus mod 17020) as the "known value" oracle: scan for a lat value that has
// the matching lon value stored close by, then narrow the candidates down after moving.
//
// Build (no Visual Studio needed):
//   C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /optimize /platform:x64 /out:memscan.exe memscan.cs
// Usage (game running, player on a planet surface):
//   memscan first <latDeg> <lonDeg>   full scan, writes candidates.txt
//   memscan next  <latDeg> <lonDeg>   keep candidates that now hold the new values
//   memscan show                      print current values of the candidates
// Read-only: the tool never writes to game memory.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;

static class MemScan
{
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);
    [DllImport("kernel32.dll")] static extern IntPtr VirtualQueryEx(IntPtr h, IntPtr addr, out MBI info, IntPtr len);
    [StructLayout(LayoutKind.Sequential)]
    struct MBI { public IntPtr BaseAddress; public IntPtr AllocationBase; public uint AllocationProtect; public uint pad0; public IntPtr RegionSize; public uint State; public uint Protect; public uint Type; public uint pad1; }

    const uint PROCESS_QUERY_INFORMATION = 0x0400, PROCESS_VM_READ = 0x0010;
    const uint MEM_COMMIT = 0x1000, PAGE_GUARD = 0x100, PAGE_NOACCESS = 0x01;
    const uint WRITABLE = 0x04 | 0x08 | 0x40 | 0x80; // RW, WRITECOPY, EXEC_RW, EXEC_WRITECOPY
    static int NEAR = 64;                               // lon must sit within +-NEAR bytes of lat (box mode: 256)
    const double TOL_DEG = 0.0002;                      // CPS shows 4 decimals
    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
    static string CandFile = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "candidates.txt");

    // A candidate: lat at Addr, lon at Addr+LonOff, element kind 'd' (double) or 'f' (float), unit 'r' or 'g' (degrees)
    class Cand { public long Addr; public int LonOff; public char Kind; public char Unit; }

    static int Main(string[] args)
    {
        if (args.Length < 1) { Console.WriteLine("usage: memscan first|next <latDeg> <lonDeg> | show"); return 1; }
        Process p = FindGame();
        if (p == null) { Console.WriteLine("Starfield.exe is not running."); return 2; }
        IntPtr h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, p.Id);
        if (h == IntPtr.Zero) { Console.WriteLine("OpenProcess failed: " + Marshal.GetLastWin32Error()); return 3; }
        string cmd = args[0].ToLowerInvariant();
        if (cmd == "show") { Show(h, p); return 0; }
        // box mode: no known value, only a lat/lon window (degrees). Radian doubles only.
        if (cmd == "boxfirst" || cmd == "boxnext")
        {
            if (args.Length < 5) { Console.WriteLine("need <latMin> <latMax> <lonMin> <lonMax>"); return 1; }
            boxMode = true;
            NEAR = 256;
            box = new[] { double.Parse(args[1], Inv), double.Parse(args[2], Inv), double.Parse(args[3], Inv), double.Parse(args[4], Inv) };
            if (cmd == "boxfirst") First(h, 1, 0); else Next(h, 1, 0); // 1 = lat window, 0 = lon window
            return 0;
        }
        if (args.Length < 3) { Console.WriteLine("need <latDeg> <lonDeg>"); return 1; }
        double lat = double.Parse(args[1], Inv), lon = double.Parse(args[2], Inv);
        if (cmd == "first") First(h, lat, lon);
        else if (cmd == "next") Next(h, lat, lon);
        else { Console.WriteLine("unknown command " + cmd); return 1; }
        return 0;
    }

    static Process FindGame()
    {
        Process[] ps = Process.GetProcessesByName("Starfield");
        return ps.Length > 0 ? ps[0] : null;
    }

    static bool boxMode = false;
    static double[] box; // latMin, latMax, lonMin, lonMax (degrees)
    static bool InBox(double radians, bool isLat)
    {
        if (double.IsNaN(radians) || double.IsInfinity(radians)) return false;
        double d = radians * 180.0 / Math.PI;
        return isLat ? (d >= box[0] && d <= box[1]) : (d >= box[2] && d <= box[3]);
    }

    static bool Near(double v, double target, char unit)
    {
        if (boxMode) return InBox(unit == 'r' ? v : v * Math.PI / 180.0, target == 1);
        if (double.IsNaN(v) || double.IsInfinity(v)) return false;
        double t = unit == 'r' ? target * Math.PI / 180.0 : target;
        double tol = unit == 'r' ? TOL_DEG * Math.PI / 180.0 : TOL_DEG;
        return Math.Abs(v - t) <= tol;
    }

    static void First(IntPtr h, double lat, double lon)
    {
        var found = new List<Cand>();
        long addr = 0, scanned = 0;
        var sw = Stopwatch.StartNew();
        MBI mbi;
        while (VirtualQueryEx(h, new IntPtr(addr), out mbi, new IntPtr(Marshal.SizeOf(typeof(MBI)))) != IntPtr.Zero)
        {
            long baseA = mbi.BaseAddress.ToInt64(), size = mbi.RegionSize.ToInt64();
            if (mbi.State == MEM_COMMIT && (mbi.Protect & WRITABLE) != 0 && (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0)
            {
                const long CHUNK = 64L * 1024 * 1024;
                for (long off = 0; off < size; off += CHUNK - NEAR * 2)
                {
                    long len = Math.Min(CHUNK, size - off);
                    byte[] buf = new byte[len];
                    IntPtr read;
                    if (!ReadProcessMemory(h, new IntPtr(baseA + off), buf, new IntPtr(len), out read)) continue;
                    int n = (int)read.ToInt64();
                    ScanBuffer(buf, n, baseA + off, lat, lon, found);
                    scanned += n;
                }
            }
            addr = baseA + size;
            if (addr <= 0) break;
        }
        // de-duplicate (chunks overlap)
        var seen = new HashSet<string>(); var uniq = new List<Cand>();
        foreach (Cand c in found) { string k = c.Addr + ":" + c.LonOff + c.Kind + c.Unit; if (seen.Add(k)) uniq.Add(c); }
        Save(uniq);
        Console.WriteLine("scanned {0:N0} MB in {1:N0} s, candidates: {2}", scanned / 1048576, sw.Elapsed.TotalSeconds, uniq.Count);
    }

    static void ScanBuffer(byte[] b, int n, long baseAddr, double lat, double lon, List<Cand> outList)
    {
        foreach (char unit in new[] { 'r', 'g' })
        {
            for (int i = 0; i + 8 <= n; i += 4)
            {
                // double
                if ((i & 7) == 0)
                {
                    double v = BitConverter.ToDouble(b, i);
                    if (Near(v, lat, unit))
                        for (int o = -NEAR; o <= NEAR; o += 8)
                        {
                            int j = i + o; if (o == 0 || j < 0 || j + 8 > n) continue;
                            if (Near(BitConverter.ToDouble(b, j), lon, unit)) outList.Add(new Cand { Addr = baseAddr + i, LonOff = o, Kind = 'd', Unit = unit });
                        }
                }
                // float (not in box mode)
                float f = BitConverter.ToSingle(b, i);
                if (Near(f, lat, unit))
                    for (int o = -NEAR; o <= NEAR; o += 4)
                    {
                        int j = i + o; if (o == 0 || j < 0 || j + 4 > n) continue;
                        if (Near(BitConverter.ToSingle(b, j), lon, unit)) outList.Add(new Cand { Addr = baseAddr + i, LonOff = o, Kind = 'f', Unit = unit });
                    }
            }
        }
    }

    static double ReadVal(IntPtr h, long a, char kind, out bool ok)
    {
        int sz = kind == 'd' ? 8 : 4; byte[] b = new byte[sz]; IntPtr r;
        ok = ReadProcessMemory(h, new IntPtr(a), b, new IntPtr(sz), out r) && r.ToInt64() == sz;
        if (!ok) return double.NaN;
        return kind == 'd' ? BitConverter.ToDouble(b, 0) : BitConverter.ToSingle(b, 0);
    }

    static void Next(IntPtr h, double lat, double lon)
    {
        List<Cand> cs = Load(); var keep = new List<Cand>();
        foreach (Cand c in cs)
        {
            bool ok1, ok2;
            double la = ReadVal(h, c.Addr, c.Kind, out ok1), lo = ReadVal(h, c.Addr + c.LonOff, c.Kind, out ok2);
            if (ok1 && ok2 && Near(la, lat, c.Unit) && Near(lo, lon, c.Unit)) keep.Add(c);
        }
        Save(keep);
        Console.WriteLine("candidates: {0} -> {1}", cs.Count, keep.Count);
        if (keep.Count <= 40) Show(h, null);
    }

    static void Show(IntPtr h, Process p)
    {
        List<Cand> cs = Load();
        long exeBase = 0, exeEnd = 0;
        Process g = p ?? FindGame();
        if (g != null) { exeBase = g.MainModule.BaseAddress.ToInt64(); exeEnd = exeBase + g.MainModule.ModuleMemorySize; }
        int shown = 0;
        foreach (Cand c in cs)
        {
            bool ok1, ok2;
            double la = ReadVal(h, c.Addr, c.Kind, out ok1), lo = ReadVal(h, c.Addr + c.LonOff, c.Kind, out ok2);
            double k = c.Unit == 'r' ? 180.0 / Math.PI : 1.0;
            string where = (c.Addr >= exeBase && c.Addr < exeEnd) ? string.Format("Starfield.exe+0x{0:X}", c.Addr - exeBase) : "heap";
            Console.WriteLine("0x{0:X12} {1}{2} lonOff={3,4}  lat={4,12:F6} lon={5,12:F6}  {6}", c.Addr, c.Kind, c.Unit, c.LonOff, la * k, lo * k, where);
            if (++shown >= 200) { Console.WriteLine("... ({0} total)", cs.Count); break; }
        }
    }

    static void Save(List<Cand> cs)
    {
        using (var w = new StreamWriter(CandFile))
            foreach (Cand c in cs) w.WriteLine("{0:X} {1} {2} {3}", c.Addr, c.LonOff, c.Kind, c.Unit);
    }

    static List<Cand> Load()
    {
        var cs = new List<Cand>();
        if (!File.Exists(CandFile)) return cs;
        foreach (string l in File.ReadAllLines(CandFile))
        {
            string[] t = l.Split(' '); if (t.Length < 4) continue;
            cs.Add(new Cand { Addr = long.Parse(t[0], NumberStyles.HexNumber), LonOff = int.Parse(t[1]), Kind = t[2][0], Unit = t[3][0] });
        }
        return cs;
    }
}
