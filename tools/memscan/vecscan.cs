// vecscan - find a direction vector (3 floats or 3 doubles) whose latitude/longitude falls in a window,
// for any of the 48 axis conventions (which axis is "up", which two form longitude, and their signs).
// Used to find where the star map keeps the selected landing target. Read-only.
//
// Build:  csc /nologo /optimize /platform:x64 /out:vecscan.exe vecscan.cs
// Usage:  vecscan first <latMin> <latMax> <lonMin> <lonMax>
//         vecscan next  <latMin> <latMax> <lonMin> <lonMax>
//         vecscan show
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;

static class VecScan
{
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);
    [DllImport("kernel32.dll")] static extern IntPtr VirtualQueryEx(IntPtr h, IntPtr addr, out MBI info, IntPtr len);
    [StructLayout(LayoutKind.Sequential)]
    struct MBI { public IntPtr BaseAddress; public IntPtr AllocationBase; public uint AllocationProtect; public uint pad0; public IntPtr RegionSize; public uint State; public uint Protect; public uint Type; public uint pad1; }
    const uint MEM_COMMIT = 0x1000, PAGE_GUARD = 0x100, PAGE_NOACCESS = 0x01, WRITABLE = 0x04 | 0x08 | 0x40 | 0x80;
    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
    static readonly string File_ = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "vec-candidates.txt");
    static double latMin, latMax, lonMin, lonMax, sMin, sMax;

    struct Cand { public long Addr; public char Kind; public int Conv; }

    static int Main(string[] a)
    {
        Process[] ps = Process.GetProcessesByName("Starfield");
        if (ps.Length == 0) { Console.WriteLine("Starfield.exe is not running."); return 2; }
        IntPtr h = OpenProcess(0x0410, false, ps[0].Id);
        if (a.Length >= 1 && a[0] == "show") { Show(h); return 0; }
        if (a.Length >= 1 && a[0] == "stable") { Stable(h, a.Length >= 2 ? int.Parse(a[1]) : 3000); return 0; }
        if (a.Length < 5) { Console.WriteLine("usage: vecscan first|next latMin latMax lonMin lonMax | show"); return 1; }
        latMin = double.Parse(a[1], Inv); latMax = double.Parse(a[2], Inv); lonMin = double.Parse(a[3], Inv); lonMax = double.Parse(a[4], Inv);
        sMin = Math.Sin(latMin * Math.PI / 180); sMax = Math.Sin(latMax * Math.PI / 180);
        if (a[0] == "first") First(h); else Next(h);
        return 0;
    }

    // conv 0..47: up axis k (0..2), up sign, lon axis order, two lon signs
    static void Decode(int c, out int k, out double su, out int ia, out int ib, out double sa, out double sb)
    {
        k = c / 16; int r = c % 16;
        su = (r & 8) != 0 ? -1 : 1;
        bool swap = (r & 4) != 0; sa = (r & 2) != 0 ? -1 : 1; sb = (r & 1) != 0 ? -1 : 1;
        int o1 = k == 0 ? 1 : 0, o2 = k == 2 ? 1 : 2;
        ia = swap ? o2 : o1; ib = swap ? o1 : o2;
    }

    static bool LatLon(double x, double y, double z, int c, out double lat, out double lon)
    {
        lat = lon = 0;
        double n = Math.Sqrt(x * x + y * y + z * z);
        if (!(n > 1e-3 && n < 1e9)) return false;
        double[] v = { x / n, y / n, z / n };
        int k, ia, ib; double su, sa, sb; Decode(c, out k, out su, out ia, out ib, out sa, out sb);
        lat = Math.Asin(Math.Max(-1, Math.Min(1, su * v[k]))) * 180 / Math.PI;
        lon = Math.Atan2(sb * v[ib], sa * v[ia]) * 180 / Math.PI;
        return true;
    }

    static bool InWin(double lat, double lon) { return lat >= latMin && lat <= latMax && lon >= lonMin && lon <= lonMax; }

    static void Test(double x, double y, double z, long addr, char kind, List<Cand> outl)
    {
        if (double.IsNaN(x) || double.IsNaN(y) || double.IsNaN(z) || double.IsInfinity(x) || double.IsInfinity(y) || double.IsInfinity(z)) return;
        double n = Math.Sqrt(x * x + y * y + z * z);
        if (!(n > 1e-3 && n < 1e9)) return;
        double ax = Math.Abs(x) / n, ay = Math.Abs(y) / n, az = Math.Abs(z) / n;
        if (ax < 0.01 || ay < 0.01 || az < 0.01) return;          // all three components must matter
        double[] av = { ax, ay, az };
        for (int k = 0; k < 3; k++)
        {
            // quick latitude prefilter on |up component|
            double lo = Math.Min(Math.Abs(sMin), Math.Abs(sMax)), hi = Math.Max(Math.Abs(sMin), Math.Abs(sMax));
            if (sMin < 0 && sMax > 0) lo = 0;
            if (av[k] < lo - 1e-9 || av[k] > hi + 1e-9) continue;
            for (int r = 0; r < 16; r++)
            {
                int c = k * 16 + r; double lat, lon;
                if (LatLon(x, y, z, c, out lat, out lon) && InWin(lat, lon)) outl.Add(new Cand { Addr = addr, Kind = kind, Conv = c });
            }
        }
    }

    static void First(IntPtr h)
    {
        var found = new List<Cand>(); long addr = 0, scanned = 0; var sw = Stopwatch.StartNew(); MBI m;
        while (VirtualQueryEx(h, new IntPtr(addr), out m, new IntPtr(Marshal.SizeOf(typeof(MBI)))) != IntPtr.Zero)
        {
            long b = m.BaseAddress.ToInt64(), size = m.RegionSize.ToInt64();
            if (m.State == MEM_COMMIT && (m.Protect & WRITABLE) != 0 && (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0)
            {
                const long CH = 64L * 1024 * 1024;
                for (long off = 0; off < size; off += CH - 32)
                {
                    long len = Math.Min(CH, size - off); byte[] buf = new byte[len]; IntPtr rd;
                    if (!ReadProcessMemory(h, new IntPtr(b + off), buf, new IntPtr(len), out rd)) continue;
                    int n = (int)rd.ToInt64();
                    for (int i = 0; i + 12 <= n; i += 4)
                    {
                        Test(BitConverter.ToSingle(buf, i), BitConverter.ToSingle(buf, i + 4), BitConverter.ToSingle(buf, i + 8), b + off + i, 'f', found);
                        if ((i & 7) == 0 && i + 24 <= n)
                            Test(BitConverter.ToDouble(buf, i), BitConverter.ToDouble(buf, i + 8), BitConverter.ToDouble(buf, i + 16), b + off + i, 'd', found);
                    }
                    scanned += n;
                }
            }
            addr = b + size; if (addr <= 0) break;
        }
        var seen = new HashSet<string>(); var u = new List<Cand>();
        foreach (var c in found) if (seen.Add(c.Addr + c.Kind.ToString() + c.Conv)) u.Add(c);
        Save(u);
        Console.WriteLine("scanned {0:N0} MB in {1:N0} s, candidates: {2}", scanned / 1048576, sw.Elapsed.TotalSeconds, u.Count);
    }

    static bool Read(IntPtr h, Cand c, out double x, out double y, out double z)
    {
        int sz = c.Kind == 'd' ? 8 : 4; byte[] b = new byte[sz * 3]; IntPtr r; x = y = z = 0;
        if (!ReadProcessMemory(h, new IntPtr(c.Addr), b, new IntPtr(b.Length), out r) || r.ToInt64() != b.Length) return false;
        if (c.Kind == 'd') { x = BitConverter.ToDouble(b, 0); y = BitConverter.ToDouble(b, 8); z = BitConverter.ToDouble(b, 16); }
        else { x = BitConverter.ToSingle(b, 0); y = BitConverter.ToSingle(b, 4); z = BitConverter.ToSingle(b, 8); }
        return true;
    }

    static void Next(IntPtr h)
    {
        var cs = Load(); var keep = new List<Cand>();
        foreach (var c in cs)
        {
            double x, y, z, lat, lon;
            if (Read(h, c, out x, out y, out z) && LatLon(x, y, z, c.Conv, out lat, out lon) && InWin(lat, lon)) keep.Add(c);
        }
        Save(keep);
        Console.WriteLine("candidates: {0} -> {1}", cs.Count, keep.Count);
        if (keep.Count <= 60) Show(h);
    }

    // keep candidates whose values do not change over waitMs (the marker is static while you don't click)
    static void Stable(IntPtr h, int waitMs)
    {
        var cs = Load(); var first = new Dictionary<int, double[]>();
        for (int i = 0; i < cs.Count; i++) { double x, y, z; if (Read(h, cs[i], out x, out y, out z)) first[i] = new[] { x, y, z }; }
        for (int round = 0; round < 3; round++)
        {
            System.Threading.Thread.Sleep(waitMs);
            for (int i = 0; i < cs.Count; i++)
            {
                double[] f; double x, y, z;
                if (!first.TryGetValue(i, out f)) continue;
                if (!Read(h, cs[i], out x, out y, out z) || x != f[0] || y != f[1] || z != f[2]) first.Remove(i);
            }
        }
        var keep = new List<Cand>(); for (int i = 0; i < cs.Count; i++) if (first.ContainsKey(i)) keep.Add(cs[i]);
        Save(keep);
        Console.WriteLine("stable: {0} -> {1}", cs.Count, keep.Count);
        if (keep.Count <= 60) Show(h);
    }

    static void Show(IntPtr h)
    {
        int i = 0;
        foreach (var c in Load())
        {
            double x, y, z, lat, lon;
            if (!Read(h, c, out x, out y, out z)) continue;
            LatLon(x, y, z, c.Conv, out lat, out lon);
            Console.WriteLine("0x{0:X12} {1} conv={2,2}  ({3:G6}, {4:G6}, {5:G6})  lat={6,9:F4} lon={7,10:F4}", c.Addr, c.Kind, c.Conv, x, y, z, lat, lon);
            if (++i >= 200) break;
        }
    }

    static void Save(List<Cand> cs) { using (var w = new StreamWriter(File_)) foreach (var c in cs) w.WriteLine("{0:X} {1} {2}", c.Addr, c.Kind, c.Conv); }
    static List<Cand> Load()
    {
        var l = new List<Cand>(); if (!File.Exists(File_)) return l;
        foreach (var s in File.ReadAllLines(File_)) { var t = s.Split(' '); l.Add(new Cand { Addr = long.Parse(t[0], NumberStyles.HexNumber), Kind = t[1][0], Conv = int.Parse(t[2]) }); }
        return l;
    }
}
