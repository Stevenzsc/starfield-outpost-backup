// sfgps - read the current landing-zone center (latitude/longitude) straight from Starfield's memory.
// No other mod needed. Read-only.
//
// Found by memory scan on 1.16.244 (see docs). The struct is the static BGSPlanet::Manager object
// (Address Library ID 945580; CommonLibSF singleton ID 937609 points at it):
//   +0x80  uint32 current planet form ID
//   +0x98  double landing-zone center longitude (radians)
//   +0xA0  double landing-zone center latitude  (radians)
//   +0x100 float  planet radius (km)
// The ID is resolved through Data\SFSE\Plugins\versionlib-<game version>.bin (Address Library v5),
// so the tool keeps working after a game update as long as the ID and struct layout stay the same.
//
// Build:  C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /optimize /platform:x64 /out:sfgps.exe sfgps.cs
// Usage:  sfgps            prints "lat lon radiusKm planetFormId" (degrees, km, hex)
//         sfgps -v         also prints how the address was resolved
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;

static class SfGps
{
    const int AddrLibId = 945580;
    const int PlanetOffset = 0x80, LonOffset = 0x98, LatOffset = 0xA0, RadiusOffset = 0x100;

    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);

    static int Main(string[] args)
    {
        bool verbose = args.Length > 0 && args[0] == "-v";
        Process[] ps = Process.GetProcessesByName("Starfield");
        if (ps.Length == 0) { Console.Error.WriteLine("Starfield.exe is not running."); return 2; }
        Process p = ps[0];
        string exe = p.MainModule.FileName;
        FileVersionInfo fv = FileVersionInfo.GetVersionInfo(exe);
        string ver = string.Format("{0}-{1}-{2}-{3}", fv.FileMajorPart, fv.FileMinorPart, fv.FileBuildPart, fv.FilePrivatePart);
        string bin = Path.Combine(Path.GetDirectoryName(exe), "Data", "SFSE", "Plugins", "versionlib-" + ver + ".bin");
        if (!File.Exists(bin)) { Console.Error.WriteLine("Address Library not found: " + bin); return 3; }

        long rva = ResolveId(bin, AddrLibId);
        if (rva <= 0) { Console.Error.WriteLine("ID " + AddrLibId + " not in " + bin); return 4; }

        IntPtr h = OpenProcess(0x0410, false, p.Id); // QUERY_INFORMATION | VM_READ
        if (h == IntPtr.Zero) { Console.Error.WriteLine("OpenProcess failed: " + Marshal.GetLastWin32Error()); return 5; }
        long baseAddr = p.MainModule.BaseAddress.ToInt64();
        byte[] buf = new byte[0x104]; IntPtr read;
        if (!ReadProcessMemory(h, new IntPtr(baseAddr + rva), buf, new IntPtr(buf.Length), out read) || read.ToInt64() != buf.Length)
        { Console.Error.WriteLine("ReadProcessMemory failed"); return 6; }
        uint planet = BitConverter.ToUInt32(buf, PlanetOffset);
        double lon = BitConverter.ToDouble(buf, LonOffset) * 180.0 / Math.PI;
        double lat = BitConverter.ToDouble(buf, LatOffset) * 180.0 / Math.PI;
        float radiusKm = BitConverter.ToSingle(buf, RadiusOffset);
        if (planet == 0 || radiusKm <= 0f) { Console.Error.WriteLine("Not on a planet surface (planet=" + planet + ", radius=" + radiusKm + ")"); return 7; }
        if (verbose) Console.WriteLine("game {0}, id {1} -> Starfield.exe+0x{2:X}, lon at +0x{3:X}, lat at +0x{4:X}", ver, AddrLibId, rva, LonOffset, LatOffset);
        Console.WriteLine(string.Format(CultureInfo.InvariantCulture, "{0:R} {1:R} {2:R} {3:X8}", lat, lon, radiusKm, planet));
        return 0;
    }

    // Address Library v5: 0x00 format(5), 0x04 version[4], 0x14 name[64], 0x54 pointer size,
    // 0x58 reserved, 0x5C count, 0x60 uint32 offsets[count] indexed by ID.
    static long ResolveId(string bin, int id)
    {
        using (var fs = File.OpenRead(bin))
        using (var br = new BinaryReader(fs))
        {
            int format = br.ReadInt32();
            if (format != 5) throw new InvalidDataException("unsupported Address Library format " + format);
            fs.Position = 0x5C;
            int count = br.ReadInt32();
            if (id < 0 || id >= count) return -1;
            fs.Position = 0x60 + (long)id * 4;
            return br.ReadUInt32();
        }
    }
}
