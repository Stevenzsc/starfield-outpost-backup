// hwbp - log which code writes (or reads) an address in Starfield, using a hardware breakpoint.
// Attaches as a debugger, sets DR0 on every thread, logs RIP + likely return addresses for each hit,
// then clears the breakpoint and detaches; the game keeps running. Does not modify game data.
//
// Build:  csc /nologo /optimize /platform:x64 /unsafe /out:hwbp.exe hwbp.cs
// Usage:  hwbp <rva hex> [w|rw] [len 1|2|4|8] [maxHits] [seconds]
//         e.g.  hwbp 6218520 w 8 20 300    (Starfield.exe+0x6218520, writes, 8 bytes)
// Save your game first: a crash while a debugger is attached is possible.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;

static unsafe class HwBp
{
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool DebugActiveProcess(int pid);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool DebugActiveProcessStop(int pid);
    [DllImport("kernel32.dll")] static extern bool DebugSetProcessKillOnExit(bool kill);
    [DllImport("kernel32.dll")] static extern bool WaitForDebugEvent(byte* ev, uint ms);
    [DllImport("kernel32.dll")] static extern bool ContinueDebugEvent(int pid, int tid, uint status);
    [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint access, bool inherit, int tid);
    [DllImport("kernel32.dll")] static extern bool GetThreadContext(IntPtr h, byte* ctx);
    [DllImport("kernel32.dll")] static extern bool SetThreadContext(IntPtr h, byte* ctx);
    [DllImport("kernel32.dll")] static extern uint SuspendThread(IntPtr h);
    [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint a, bool i, int p);
    [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr h, IntPtr a, byte[] b, IntPtr s, out IntPtr r);

    const uint DBG_CONTINUE = 0x00010002, DBG_EXCEPTION_NOT_HANDLED = 0x80010001;
    const uint EXCEPTION_DEBUG_EVENT = 1, CREATE_THREAD_DEBUG_EVENT = 2, CREATE_PROCESS_DEBUG_EVENT = 3,
               EXIT_THREAD_DEBUG_EVENT = 4, EXIT_PROCESS_DEBUG_EVENT = 5, LOAD_DLL_DEBUG_EVENT = 6;
    const uint SINGLE_STEP = 0x80000004, BREAKPOINT = 0x80000003;
    const uint THREAD_ALL = 0x001F03FF;
    const uint CONTEXT_FLAGS = 0x00100010 | 0x00100001; // DEBUG_REGISTERS | CONTROL
    // x64 CONTEXT offsets
    const int OFF_FLAGS = 0x30, OFF_DR0 = 0x48, OFF_DR6 = 0x68, OFF_DR7 = 0x70, OFF_RSP = 0x98, OFF_RIP = 0xF8, CTX_SIZE = 0x4D0;

    static ulong addr, dr7, modBase, modEnd;
    static IntPtr proc;

    static int Main(string[] a)
    {
        if (a.Length < 1) { Console.WriteLine("usage: hwbp <rva hex> [w|rw] [len] [maxHits] [seconds]"); return 1; }
        Process[] ps = Process.GetProcessesByName("Starfield");
        if (ps.Length == 0) { Console.WriteLine("Starfield.exe is not running."); return 2; }
        Process p = ps[0];
        modBase = (ulong)p.MainModule.BaseAddress.ToInt64(); modEnd = modBase + (ulong)p.MainModule.ModuleMemorySize;
        ulong rva = Convert.ToUInt64(a[0], 16);
        addr = modBase + rva;
        string mode = a.Length > 1 ? a[1] : "w";
        int len = a.Length > 2 ? int.Parse(a[2]) : 8;
        int maxHits = a.Length > 3 ? int.Parse(a[3]) : 20;
        int seconds = a.Length > 4 ? int.Parse(a[4]) : 300;
        ulong rw = mode == "rw" ? 3UL : 1UL;
        ulong ln = len == 1 ? 0UL : len == 2 ? 1UL : len == 8 ? 2UL : 3UL;
        dr7 = 1UL | (rw << 16) | (ln << 18);
        proc = OpenProcess(0x0410, false, p.Id);

        if (!DebugActiveProcess(p.Id)) { Console.WriteLine("DebugActiveProcess failed: " + Marshal.GetLastWin32Error()); return 3; }
        DebugSetProcessKillOnExit(false);
        Console.WriteLine("Attached. Breakpoint on Starfield.exe+0x{0:X} ({1}, {2} bytes). Waiting up to {3} s for {4} hits...", rva, mode, len, seconds, maxHits);

        var threads = new Dictionary<int, IntPtr>();
        var seen = new Dictionary<ulong, int>();
        int hits = 0; var sw = Stopwatch.StartNew();
        byte* ev = stackalloc byte[0xB0];
        byte[] ctxBuf = new byte[CTX_SIZE + 16];
        fixed (byte* raw = ctxBuf)
        {
            byte* ctx = (byte*)(((long)raw + 15) & ~15L);
            bool stopping = false;
            while (true)
            {
                if (!stopping && (hits >= maxHits || sw.Elapsed.TotalSeconds > seconds)) stopping = true;
                if (stopping) break;
                if (!WaitForDebugEvent(ev, 500)) continue;
                uint code = *(uint*)ev; int pid = *(int*)(ev + 4), tid = *(int*)(ev + 8);
                uint cont = DBG_CONTINUE;
                switch (code)
                {
                    case CREATE_PROCESS_DEBUG_EVENT:
                    case CREATE_THREAD_DEBUG_EVENT:
                    {
                        IntPtr th = OpenThread(THREAD_ALL, false, tid);
                        if (th != IntPtr.Zero) { threads[tid] = th; SetBp(th, ctx, true); }
                        break;
                    }
                    case EXIT_THREAD_DEBUG_EVENT:
                    {
                        IntPtr th; if (threads.TryGetValue(tid, out th)) { CloseHandle(th); threads.Remove(tid); }
                        break;
                    }
                    case EXIT_PROCESS_DEBUG_EVENT: Console.WriteLine("Game exited."); return 4;
                    case EXCEPTION_DEBUG_EVENT:
                    {
                        uint exc = *(uint*)(ev + 16);
                        bool first = *(uint*)(ev + 16 + 0x98) != 0;
                        if (exc == SINGLE_STEP)
                        {
                            IntPtr th; if (!threads.TryGetValue(tid, out th)) { th = OpenThread(THREAD_ALL, false, tid); threads[tid] = th; }
                            *(uint*)(ctx + OFF_FLAGS) = CONTEXT_FLAGS;
                            if (GetThreadContext(th, ctx) && (*(ulong*)(ctx + OFF_DR6) & 1) != 0)
                            {
                                ulong rip = *(ulong*)(ctx + OFF_RIP), rsp = *(ulong*)(ctx + OFF_RSP);
                                int n; seen.TryGetValue(rip, out n); seen[rip] = n + 1;
                                if (n < 3)
                                {
                                    hits++;
                                    double val = ReadDouble(addr);
                                    Console.WriteLine("hit {0}: rip=Starfield.exe+0x{1:X}  value now {2:G10} ({3:F6} deg)  thread {4}", hits, rip - modBase, val, val * 180 / Math.PI, tid);
                                    Console.WriteLine("   stack: " + StackReturns(rsp));
                                }
                                *(ulong*)(ctx + OFF_DR6) = 0; SetThreadContext(th, ctx);
                            }
                        }
                        else if (exc != BREAKPOINT) cont = first ? DBG_EXCEPTION_NOT_HANDLED : DBG_EXCEPTION_NOT_HANDLED;
                        break;
                    }
                    case LOAD_DLL_DEBUG_EVENT:
                    {
                        IntPtr hf = *(IntPtr*)(ev + 16); if (hf != IntPtr.Zero) CloseHandle(hf);
                        break;
                    }
                }
                ContinueDebugEvent(pid, tid, cont);
            }
            // clear breakpoints on all threads, then detach
            foreach (var kv in threads) { SuspendThread(kv.Value); SetBp(kv.Value, ctx, false); ResumeThread(kv.Value); CloseHandle(kv.Value); }
        }
        DebugActiveProcessStop(p.Id);
        Console.WriteLine("Detached after {0} hits. Unique writers:", hits);
        foreach (var kv in seen) Console.WriteLine("   Starfield.exe+0x{0:X}  x{1}", kv.Key - modBase, kv.Value);
        return 0;
    }

    static void SetBp(IntPtr th, byte* ctx, bool on)
    {
        *(uint*)(ctx + OFF_FLAGS) = CONTEXT_FLAGS;
        if (!GetThreadContext(th, ctx)) return;
        *(ulong*)(ctx + OFF_DR0) = on ? addr : 0;
        ulong d7 = *(ulong*)(ctx + OFF_DR7) & ~(3UL | (0xFUL << 16));
        if (on) d7 |= dr7;
        *(ulong*)(ctx + OFF_DR7) = d7;
        SetThreadContext(th, ctx);
    }

    static double ReadDouble(ulong a)
    {
        byte[] b = new byte[8]; IntPtr r;
        return ReadProcessMemory(proc, new IntPtr((long)a), b, new IntPtr(8), out r) ? BitConverter.ToDouble(b, 0) : double.NaN;
    }

    // return addresses into Starfield.exe found in the first 0x400 bytes of the stack (heuristic)
    static string StackReturns(ulong rsp)
    {
        byte[] b = new byte[0x400]; IntPtr r;
        if (!ReadProcessMemory(proc, new IntPtr((long)rsp), b, new IntPtr(b.Length), out r)) return "?";
        var l = new List<string>();
        for (int i = 0; i + 8 <= b.Length && l.Count < 12; i += 8)
        {
            ulong v = BitConverter.ToUInt64(b, i);
            if (v > modBase + 0x1000 && v < modEnd) l.Add("+" + (v - modBase).ToString("X"));
        }
        return string.Join(" ", l.ToArray());
    }
}
