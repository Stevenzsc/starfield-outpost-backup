// Rewrites the menus (MESG records) of OutpostBackup.esp so they need no Creation Kit work.
//   PatchEsp.exe <in.esp> <out.esp>
// - OSB_MenuMsg (0x807) gets the 5-button main menu.
// - Adds OSB_PickMsg and the confirm messages at fixed form IDs (see Messages below; OSB_Slate.psc uses them).
// - Drops the leftover CK duplicate "SY_Trident_LoungeRules_SlateCOPY".
// Records other than MESG are copied byte for byte. Run it again any time: it is idempotent.
// Build: csc /out:PatchEsp.exe PatchEsp.cs  (.NET Framework csc, C# 5)
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

class PatchEsp
{
	class Msg
	{
		public uint Id; public string Edid, Title, Text; public string[] Buttons; public int BackOut;
		public Msg(uint id, string edid, string title, string text, int backOut, params string[] buttons)
		{ Id = id; Edid = edid; Title = title; Text = text; BackOut = backOut; Buttons = buttons; }
	}

	const string NL = "\r\n";

	static readonly Msg[] Messages = {
		new Msg(0x01000807, "OSB_MenuMsg", "Outpost Backup", "What do you want to do?", 4,
			"Back up this outpost", "Land at a saved outpost", "Restore a saved outpost here", "Delete a saved outpost", "Cancel"),
		// button 0 Cancel, 1 More..., 2..9 entries; the plugin rewrites the labels and hides unused ones
		new Msg(0x0100080B, "OSB_PickMsg", "Saved outposts", "Choose an outpost.", 0,
			"Cancel", "More...", "Outpost 1", "Outpost 2", "Outpost 3", "Outpost 4", "Outpost 5", "Outpost 6", "Outpost 7", "Outpost 8"),
		new Msg(0x0100080D, "OSB_FarBeaconMsg", "Outpost Backup",
			"This Outpost Beacon is %.0f m from where the saved beacon stood. The objects go back to their saved spots, which may be outside this outpost." + NL + NL +
			"Restore anyway?", 1,
			"Restore anyway", "Cancel"),
		new Msg(0x0100080E, "OSB_DupMsg", "Outpost Backup",
			"This outpost already has %.0f objects. Restoring adds all saved objects as well, which can create duplicates." + NL + NL +
			"Restore anyway?", 1,
			"Restore anyway", "Cancel"),
		new Msg(0x0100080F, "OSB_DeleteMsg", "Outpost Backup",
			"Delete the selected saved outpost?" + NL + NL +
			"The file is moved to Documents\\My Games\\Starfield\\OutpostBackup\\Deleted.", 1,
			"Delete", "Cancel"),
	};

	static bool Dropped(string edid) { return edid.StartsWith("SY_Trident_LoungeRules_SlateCOPY", StringComparison.Ordinal); }

	class Rec { public byte[] Bytes; public string Type; public uint Id; public string Edid; }

	static int Main(string[] args)
	{
		if (args.Length != 2) { Console.Error.WriteLine("usage: PatchEsp <in.esp> <out.esp>"); return 2; }
		byte[] d = File.ReadAllBytes(args[0]);
		if (Encoding.ASCII.GetString(d, 0, 4) != "TES4") { Console.Error.WriteLine("not a plugin"); return 1; }

		int tes4Len = 24 + BitConverter.ToInt32(d, 4);
		byte[] tes4 = Sub(d, 0, tes4Len);

		// top-level groups: label -> (group header, records)
		var order = new List<string>();
		var headers = new Dictionary<string, byte[]>();
		var groups = new Dictionary<string, List<Rec>>();
		int p = tes4Len;
		while (p < d.Length) {
			if (Encoding.ASCII.GetString(d, p, 4) != "GRUP") throw new Exception("expected GRUP at " + p);
			int gsize = BitConverter.ToInt32(d, p + 4);
			string label = Encoding.ASCII.GetString(d, p + 8, 4);
			order.Add(label);
			headers[label] = Sub(d, p, 24);
			var recs = new List<Rec>();
			int q = p + 24;
			while (q < p + gsize) {
				string type = Encoding.ASCII.GetString(d, q, 4);
				if (type == "GRUP") throw new Exception("nested group in " + label + " not supported");
				int len = 24 + BitConverter.ToInt32(d, q + 4);
				var r = new Rec { Bytes = Sub(d, q, len), Type = type, Id = BitConverter.ToUInt32(d, q + 12) };
				r.Edid = ReadEdid(r.Bytes);
				recs.Add(r);
				q += len;
			}
			groups[label] = recs;
			p += gsize;
		}

		foreach (var label in order)
			groups[label].RemoveAll(r => {
				if (r.Edid != null && Dropped(r.Edid)) { Console.WriteLine("drop {0} {1:X8} {2}", r.Type, r.Id, r.Edid); return true; }
				return false;
			});

		// MESG: keep foreign messages, replace ours
		if (!groups.ContainsKey("MESG")) {
			order.Add("MESG");
			byte[] h = new byte[24];
			Encoding.ASCII.GetBytes("GRUP").CopyTo(h, 0);
			Encoding.ASCII.GetBytes("MESG").CopyTo(h, 8);
			headers["MESG"] = h;
			groups["MESG"] = new List<Rec>();
		}
		var mesg = groups["MESG"];
		byte[] template = null;
		foreach (var r in mesg) if (r.Id == 0x01000807) template = r.Bytes;
		var ours = new HashSet<uint>();
		foreach (var m in Messages) ours.Add(m.Id);
		mesg.RemoveAll(r => ours.Contains(r.Id));
		foreach (var m in Messages) {
			mesg.Add(new Rec { Type = "MESG", Id = m.Id, Edid = m.Edid, Bytes = BuildMesg(m, template) });
			Console.WriteLine("write MESG {0:X8} {1} ({2} buttons)", m.Id, m.Edid, m.Buttons.Length);
		}

		// Star-map marker on the saved outpost's planet: the slate tags the planet's Location with
		// OSB_NavTargetKeyword and starts OSB_NavQuest, which finds that location and creates the same
		// radiant-planet quest target the survey missions use (MB_SurveyPlanet04_LIST).
		Put(order, headers, groups, "KYWD", true, BuildKeyword(NavKeywordId, "OSB_NavTargetKeyword"), template);
		Put(order, headers, groups, "QUST", false, BuildNavQuest(), template);

		// header: record count (records + groups) and next object id
		int count = 0; uint maxId = 0;
		var body = new MemoryStream();
		foreach (var label in order) {
			var recs = groups[label];
			if (recs.Count == 0) continue;
			count++;
			int size = 24;
			foreach (var r in recs) { size += r.Bytes.Length; count++; if ((r.Id & 0xFFFFFF) > maxId) maxId = r.Id & 0xFFFFFF; }
			byte[] h = (byte[])headers[label].Clone();
			BitConverter.GetBytes(size).CopyTo(h, 4);
			body.Write(h, 0, 24);
			foreach (var r in recs) body.Write(r.Bytes, 0, r.Bytes.Length);
		}
		int hedr = Find(tes4, "HEDR", 24);
		if (hedr < 0) throw new Exception("no HEDR");
		BitConverter.GetBytes(count).CopyTo(tes4, hedr + 6 + 4);
		BitConverter.GetBytes(maxId + 1).CopyTo(tes4, hedr + 6 + 8);

		using (var o = File.Create(args[1])) {
			o.Write(tes4, 0, tes4.Length);
			body.WriteTo(o);
		}
		Console.WriteLine("records+groups {0}, next id {1:X}, wrote {2}", count, maxId + 1, args[1]);
		return 0;
	}

	const uint NavKeywordId = 0x01000810;
	const uint NavQuestId = 0x01000811;

	class Built { public string Type; public uint Id; public string Edid; public byte[] Data; }

	static void Put(List<string> order, Dictionary<string, byte[]> headers, Dictionary<string, List<Rec>> groups, string label, bool first, Built b, byte[] template)
	{
		if (!groups.ContainsKey(label)) {
			if (first) order.Insert(0, label); else order.Add(label);
			byte[] h = new byte[24];
			Encoding.ASCII.GetBytes("GRUP").CopyTo(h, 0);
			Encoding.ASCII.GetBytes(label).CopyTo(h, 8);
			headers[label] = h;
			groups[label] = new List<Rec>();
		}
		var recs = groups[label];
		recs.RemoveAll(r => r.Id == b.Id);
		byte[] rec = new byte[24 + b.Data.Length];
		Encoding.ASCII.GetBytes(b.Type).CopyTo(rec, 0);
		BitConverter.GetBytes(b.Data.Length).CopyTo(rec, 4);
		BitConverter.GetBytes(b.Id).CopyTo(rec, 12);
		if (template != null) Array.Copy(template, 16, rec, 16, 8);
		else BitConverter.GetBytes((ushort)0x240).CopyTo(rec, 20);
		b.Data.CopyTo(rec, 24);
		recs.Add(new Rec { Type = b.Type, Id = b.Id, Edid = b.Edid, Bytes = rec });
		Console.WriteLine("write {0} {1:X8} {2}", b.Type, b.Id, b.Edid);
	}

	static Built BuildKeyword(uint id, string edid)
	{
		var s = new MemoryStream();
		Sub(s, "EDID", Z(edid));
		Sub(s, "CNAM", new byte[] { 0xFF, 0xFF, 0xFF, 0x00 });
		Sub(s, "TNAM", BitConverter.GetBytes(0u));
		return new Built { Type = "KYWD", Id = id, Edid = edid, Data = s.ToArray() };
	}

	static Built BuildNavQuest()
	{
		var s = new MemoryStream();
		Sub(s, "EDID", Z("OSB_NavQuest"));
		Sub(s, "FULL", Z("Outpost Backup"));
		Sub(s, "DNAM", new byte[12]);
		Sub(s, "NEXT", new byte[0]);
		// objective 10 -> alias 2 (the marker)
		Sub(s, "QOBJ", BitConverter.GetBytes((ushort)10));
		Sub(s, "FNAM", BitConverter.GetBytes(0u));
		Sub(s, "NNAM", Z("Travel to <Alias=Planet> to restore your outpost"));
		Sub(s, "QSTA", Cat(BitConverter.GetBytes(2), new byte[8]));
		Sub(s, "ANAM", BitConverter.GetBytes(3u));
		// alias 0: the planet location (location carrying OSB_NavTargetKeyword)
		Sub(s, "ALLS", BitConverter.GetBytes(0u));
		Sub(s, "ALID", Z("Planet"));
		Sub(s, "FNAM", BitConverter.GetBytes(0x00000100u));
		Sub(s, "ALFG", BitConverter.GetBytes(0u));
		Sub(s, "ALKF", BitConverter.GetBytes(NavKeywordId));
		Sub(s, "ALED", new byte[0]);
		// alias 1: collection created from the radiant-planet target cell at the planet location
		Sub(s, "ALCS", BitConverter.GetBytes(1u));
		Sub(s, "ALMI", new byte[] { 0 });
		Sub(s, "ALST", BitConverter.GetBytes(1u));
		Sub(s, "ALID", Z("PlanetOrbital"));
		Sub(s, "FNAM", BitConverter.GetBytes(0x00080000u));
		Sub(s, "ALFG", BitConverter.GetBytes(0u));
		Sub(s, "ALCO", BitConverter.GetBytes(0x00212179u));   // scRadiantPlanetQT
		Sub(s, "ALCA", BitConverter.GetBytes(0u));           // at alias 0
		Sub(s, "ALCL", BitConverter.GetBytes(0u));
		Sub(s, "VTCK", BitConverter.GetBytes(0u));
		Sub(s, "ALED", new byte[0]);
		// alias 2: the marker in that collection (base RadiantPlanetQTMarker 0x212148)
		Sub(s, "ALST", BitConverter.GetBytes(2u));
		Sub(s, "ALID", Z("PlanetTarget"));
		Sub(s, "FNAM", BitConverter.GetBytes(0u));
		Sub(s, "ALFG", BitConverter.GetBytes(0u));
		Sub(s, "ALCS", BitConverter.GetBytes(1u));
		Sub(s, "CTDA", Hex("00000000 0000803F 48000D41 48212100 00000000 00000000 00000000 FFFFFFFF"));   // GetIsID RadiantPlanetQTMarker == 1
		Sub(s, "VTCK", BitConverter.GetBytes(0u));
		Sub(s, "ALED", new byte[0]);
		return new Built { Type = "QUST", Id = NavQuestId, Edid = "OSB_NavQuest", Data = s.ToArray() };
	}

	static byte[] Cat(byte[] a, byte[] b) { var r = new byte[a.Length + b.Length]; a.CopyTo(r, 0); b.CopyTo(r, a.Length); return r; }

	static byte[] Hex(string h)
	{
		h = h.Replace(" ", "");
		var r = new byte[h.Length / 2];
		for (int i = 0; i < r.Length; i++) r[i] = Convert.ToByte(h.Substring(i * 2, 2), 16);
		return r;
	}

	static byte[] BuildMesg(Msg m, byte[] template)
	{
		var s = new MemoryStream();
		Sub(s, "EDID", Z(m.Edid));
		Sub(s, "DESC", Z(m.Text));
		Sub(s, "FULL", Z(m.Title));
		Sub(s, "INAM", BitConverter.GetBytes(0u));
		Sub(s, "DNAM", BitConverter.GetBytes(m.BackOut >= 0 ? 5u : 1u));   // 1 message box, 4 allow back out
		if (m.BackOut >= 0) Sub(s, "BNAM", BitConverter.GetBytes((uint)m.BackOut));
		foreach (var b in m.Buttons) Sub(s, "ITXT", Z(b));
		byte[] data = s.ToArray();

		byte[] rec = new byte[24 + data.Length];
		Encoding.ASCII.GetBytes("MESG").CopyTo(rec, 0);
		BitConverter.GetBytes(data.Length).CopyTo(rec, 4);
		BitConverter.GetBytes(m.Id).CopyTo(rec, 12);
		if (template != null) Array.Copy(template, 16, rec, 16, 8);   // version control info + form version
		else { BitConverter.GetBytes((ushort)0x240).CopyTo(rec, 20); }
		data.CopyTo(rec, 24);
		return rec;
	}

	static void Sub(Stream s, string type, byte[] data)
	{
		if (data.Length > 0xFFFF) throw new Exception(type + " too long");
		s.Write(Encoding.ASCII.GetBytes(type), 0, 4);
		s.Write(BitConverter.GetBytes((ushort)data.Length), 0, 2);
		s.Write(data, 0, data.Length);
	}

	static byte[] Z(string text)
	{
		byte[] b = Encoding.UTF8.GetBytes(text);
		byte[] z = new byte[b.Length + 1];
		b.CopyTo(z, 0);
		return z;
	}

	static string ReadEdid(byte[] rec)
	{
		uint flags = BitConverter.ToUInt32(rec, 8);
		if ((flags & 0x40000) != 0) return null;   // compressed
		int i = Find(rec, "EDID", 24);
		if (i != 24) return null;
		int len = BitConverter.ToUInt16(rec, i + 4);
		return Encoding.ASCII.GetString(rec, i + 6, Math.Max(0, len - 1));
	}

	static int Find(byte[] b, string tag, int from)
	{
		byte[] t = Encoding.ASCII.GetBytes(tag);
		for (int i = from; i + 4 <= b.Length; i++)
			if (b[i] == t[0] && b[i + 1] == t[1] && b[i + 2] == t[2] && b[i + 3] == t[3]) return i;
		return -1;
	}

	static byte[] Sub(byte[] b, int at, int len)
	{
		byte[] r = new byte[len];
		Array.Copy(b, at, r, 0, len);
		return r;
	}
}
