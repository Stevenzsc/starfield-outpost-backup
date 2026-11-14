// xref - small static-analysis helper for Starfield.exe (read-only, works on the file on disk).
//   xref refs  <exe> <rvaLo> <rvaHi>   instructions whose RIP-relative operand points into [lo, hi]
//   xref calls <exe> <rva>             call/jmp instructions that target rva
//   xref func  <exe> <rva>             disassemble the whole function containing rva (.pdata bounds)
//   xref dis   <exe> <rva> <count>     disassemble count instructions from rva
// Numbers are hex (0x optional).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Iced.Intel;

class Pe
{
    public byte[] Data; public ulong ImageBase;
    public List<(string name, uint va, uint vsize, uint raw, uint rawSize)> Sections = new();
    public List<(uint begin, uint end)> Funcs = new();

    public Pe(string path)
    {
        Data = File.ReadAllBytes(path);
        int pe = BitConverter.ToInt32(Data, 0x3C);
        int nsec = BitConverter.ToUInt16(Data, pe + 6);
        int optSize = BitConverter.ToUInt16(Data, pe + 20);
        int opt = pe + 24;
        ImageBase = BitConverter.ToUInt64(Data, opt + 24);
        uint excRva = BitConverter.ToUInt32(Data, opt + 112 + 3 * 8), excSize = BitConverter.ToUInt32(Data, opt + 112 + 3 * 8 + 4);
        int sh = opt + optSize;
        for (int i = 0; i < nsec; i++)
        {
            int s = sh + i * 40;
            string name = System.Text.Encoding.ASCII.GetString(Data, s, 8).TrimEnd('\0');
            Sections.Add((name, BitConverter.ToUInt32(Data, s + 12), BitConverter.ToUInt32(Data, s + 8), BitConverter.ToUInt32(Data, s + 20), BitConverter.ToUInt32(Data, s + 16)));
        }
        int eo = (int)ToFile(excRva);
        for (uint o = 0; o + 12 <= excSize; o += 12)
            Funcs.Add((BitConverter.ToUInt32(Data, eo + (int)o), BitConverter.ToUInt32(Data, eo + (int)o + 4)));
        Funcs.Sort();
    }

    public long ToFile(uint rva)
    {
        foreach (var s in Sections) if (rva >= s.va && rva < s.va + Math.Max(s.vsize, s.rawSize)) return s.raw + (rva - s.va);
        return -1;
    }

    public (uint begin, uint end) FuncOf(uint rva)
    {
        int lo = 0, hi = Funcs.Count - 1;
        while (lo <= hi) { int m = (lo + hi) / 2; if (Funcs[m].begin <= rva) lo = m + 1; else hi = m - 1; }
        return hi >= 0 && rva < Funcs[hi].end ? Funcs[hi] : (0, 0);
    }

    public IEnumerable<Instruction> Decode(uint rvaStart, uint rvaEnd)
    {
        long f = ToFile(rvaStart); if (f < 0) yield break;
        var reader = new ByteArrayCodeReader(Data, (int)f, (int)(rvaEnd - rvaStart));
        var dec = Decoder.Create(64, reader, ImageBase + rvaStart);
        while (dec.IP < ImageBase + rvaEnd) { dec.Decode(out var ins); yield return ins; }
    }
}

static class Program
{
    static uint H(string s) => Convert.ToUInt32(s.StartsWith("0x") ? s[2..] : s, 16);
    static readonly NasmFormatter Fmt = new();

    static string Show(Pe pe, Instruction ins)
    {
        var o = new StringOutput(); Fmt.Format(ins, o);
        return $"{ins.IP - pe.ImageBase:X8}  {o.ToStringAndReset()}";
    }

    static int Main(string[] a)
    {
        if (a.Length < 3) { Console.WriteLine("usage: xref refs|calls|func|dis <exe> ..."); return 1; }
        var pe = new Pe(a[1]);
        Fmt.Options.RipRelativeAddresses = true;
        var text = pe.Sections.First(s => s.name == ".text");
        switch (a[0])
        {
            case "refs":
            {
                uint lo = H(a[2]), hi = H(a[3]);
                foreach (var ins in pe.Decode(text.va, text.va + text.vsize))
                {
                    if (!ins.IsIPRelativeMemoryOperand) continue;
                    ulong t = ins.IPRelativeMemoryAddress - pe.ImageBase;
                    if (t >= lo && t <= hi)
                    {
                        var f = pe.FuncOf((uint)(ins.IP - pe.ImageBase));
                        Console.WriteLine($"{Show(pe, ins)}    ; target +{t:X}  func {f.begin:X8}-{f.end:X8}");
                    }
                }
                break;
            }
            case "fieldwrites":
            {
                // functions that reference any of the given globals, then memory writes there with the given displacements
                var globals = a[2].Split(',').Select(H).ToHashSet();
                var disps = a[3].Split(',').Select(s => (long)H(s)).ToHashSet();
                var funcs = new SortedSet<(uint, uint)>();
                foreach (var ins in pe.Decode(text.va, text.va + text.vsize))
                    if (ins.IsIPRelativeMemoryOperand && globals.Contains((uint)(ins.IPRelativeMemoryAddress - pe.ImageBase)))
                    { var f = pe.FuncOf((uint)(ins.IP - pe.ImageBase)); if (f.end != 0) funcs.Add(f); }
                Console.WriteLine($"; {funcs.Count} functions reference the globals");
                foreach (var f in funcs)
                    foreach (var ins in pe.Decode(f.Item1, f.Item2))
                    {
                        if (ins.OpCount < 1 || ins.Op0Kind != OpKind.Memory || ins.IsIPRelativeMemoryOperand) continue;
                        if (!disps.Contains((long)ins.MemoryDisplacement64)) continue;
                        Console.WriteLine($"{Show(pe, ins)}    ; func {f.Item1:X8}-{f.Item2:X8}");
                    }
                break;
            }
            case "multistore":
            {
                // functions that store (non-stack base) to at least `need` of the given displacements with the same base register
                var disps = a[2].Split(',').Select(s => (long)H(s)).ToHashSet();
                int need = a.Length > 3 ? int.Parse(a[3]) : 2;
                foreach (var f in pe.Funcs)
                {
                    if (f.end <= f.begin || f.end - f.begin > 0x20000) continue;
                    var byReg = new Dictionary<Register, SortedSet<long>>();
                    var lines = new List<string>();
                    foreach (var ins in pe.Decode(f.begin, f.end))
                    {
                        if (ins.OpCount < 2 || ins.Op0Kind != OpKind.Memory || ins.IsIPRelativeMemoryOperand) continue;
                        var b = ins.MemoryBase;
                        if (b == Register.RSP || b == Register.RBP || b == Register.None || ins.MemoryIndex != Register.None) continue;
                        long d = (long)ins.MemoryDisplacement64;
                        if (!disps.Contains(d)) continue;
                        if (ins.MemorySize != MemorySize.Float64 && ins.MemorySize != MemorySize.Packed128_Float64 && ins.MemorySize != MemorySize.Packed128_Float32 && ins.MemorySize != MemorySize.UInt128 && ins.MemorySize != MemorySize.UInt64 && ins.MemorySize != MemorySize.Int64) continue;
                        if (!byReg.TryGetValue(b, out var set)) byReg[b] = set = new SortedSet<long>();
                        set.Add(d); lines.Add(Show(pe, ins));
                    }
                    if (byReg.Values.Any(s => s.Count >= need))
                    {
                        Console.WriteLine($"; func {f.begin:X8}-{f.end:X8}");
                        foreach (var l in lines) Console.WriteLine("   " + l);
                    }
                }
                break;
            }
            case "calls":
            {
                ulong target = pe.ImageBase + H(a[2]);
                foreach (var ins in pe.Decode(text.va, text.va + text.vsize))
                    if ((ins.FlowControl == FlowControl.Call || ins.FlowControl == FlowControl.UnconditionalBranch) && ins.Op0Kind == OpKind.NearBranch64 && ins.NearBranch64 == target)
                    {
                        var f = pe.FuncOf((uint)(ins.IP - pe.ImageBase));
                        Console.WriteLine($"{Show(pe, ins)}    ; in func {f.begin:X8}-{f.end:X8}");
                    }
                break;
            }
            case "func":
            {
                var f = pe.FuncOf(H(a[2]));
                if (f.end == 0) { Console.WriteLine("no .pdata entry"); return 2; }
                Console.WriteLine($"; func {f.begin:X8}-{f.end:X8}");
                foreach (var ins in pe.Decode(f.begin, f.end)) Console.WriteLine(Show(pe, ins));
                break;
            }
            case "dis":
            {
                int n = int.Parse(a[3]); uint start = H(a[2]);
                foreach (var ins in pe.Decode(start, start + (uint)n * 16)) { Console.WriteLine(Show(pe, ins)); if (--n <= 0) break; }
                break;
            }
        }
        return 0;
    }
}
