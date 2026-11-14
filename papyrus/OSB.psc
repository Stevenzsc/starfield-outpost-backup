Scriptname OSB
{ Outpost Backup prototype. No esp needed; everything runs from the console with cgf.

  Backup:  cgf "OSB.Backup" <beaconRefID as signed decimal>
           -> writes console commands to Documents\My Games\Starfield\Logs\Script\User\OSB_Backup.0.log
  Convert: powershell -ExecutionPolicy Bypass -File tools\make-restore.ps1
           -> writes <Starfield>\OSB_Restore.txt (+ a dated copy in backups\)
  Restore: stand inside the target outpost (beacon already placed), then in the console: bat OSB_Restore

  Objects are linked to the outpost with WorkshopItemKeyword (Starfield.esm 00054BA6),
  which makes them part of the outpost (selectable, movable, scrappable with refund). }

; Game units are meters. Restore looks for a beacon within 300 m of the player.

; ---------- helpers ----------

Keyword Function GetWorkshopItemKeyword() global
	return Game.GetFormFromFile(0x00054BA6, "Starfield.esm") as Keyword
EndFunction

ObjectReference Function GetRef(int aiFormID) global
	ObjectReference ref = Game.GetForm(aiFormID) as ObjectReference
	if ref == None
		Debug.Notification("OSB: refID not found " + aiFormID)
		Debug.Trace("OSB: GetForm failed for " + aiFormID)
	endif
	return ref
EndFunction

; Beacon parts and engine forms that a new beacon already brings with it.
bool Function IsSkippedBase(Form akBase) global
	if akBase == None
		return true
	endif
	; Compare by form identity first: calling GetFormID() on the MapMarker_LongRange base
	; hung the script in game, so beacon parts must be rejected before any member call.
	; OutpostBeaconActivator, OutpostBeacon_Packin, MapMarker_LongRange, NPCClipboard
	if akBase == Game.GetForm(0x002C60E9) || akBase == Game.GetForm(0x0029A8C6) || akBase == Game.GetForm(0x00254244) || akBase == Game.GetForm(0x0025EB0F)
		Debug.Trace("OSB: IsSkippedBase beacon part")
		return true
	endif
	; ActorBase refs are kept: outpost turrets are actors (e.g. 002E181C).
	; Assigned crew are not linked with WorkshopItemKeyword, so they do not show up here.
	; PackIn refs are kept: a built item from another mod was only present as its PackIn ref.
	; GetFormID() also hangs on some bases (e.g. 00000021), so it is never called here.
	; Engine-reserved forms (< 0x800) are filtered by tools\make-restore.ps1 instead.
	return false
EndFunction

ObjectReference Function FindBeacon() global
	Actor player = Game.GetPlayer()
	ObjectReference ws = player.GetWorkshop()
	if ws
		return ws
	endif
	Form beaconBase = Game.GetForm(0x002C60E9)
	if beaconBase
		return Game.FindClosestReferenceOfTypeFromRef(beaconBase, player, 300.0)
	endif
	return None
EndFunction

; ---------- backup ----------

Function Backup(int aiBeaconID) global
	ObjectReference beacon = GetRef(aiBeaconID)
	Keyword kw = GetWorkshopItemKeyword()
	if beacon == None || kw == None
		Debug.Notification("OSB: backup aborted (beacon or keyword missing)")
		return
	endif
	string log = "OSB_Backup"
	Debug.OpenUserLog(log)
	ObjectReference[] refs = beacon.GetRefsLinkedToMe(kw)
	int total = 0
	if refs
		total = refs.Length
	endif
	Debug.TraceUser(log, "OSB-BEGIN beacon=" + beacon + " pos=(" + beacon.GetPositionX() + "," + beacon.GetPositionY() + "," + beacon.GetPositionZ() + ") yaw=" + beacon.GetAngleZ() + " linked=" + total)
	float bx = beacon.GetPositionX()
	float by = beacon.GetPositionY()
	float bz = beacon.GetPositionZ()
	int saved = 0
	int i = 0
	while i < total
		ObjectReference r = refs[i]
		Debug.Trace("OSB: Backup step #" + i + " ref=" + r)
		if r && r != beacon
			Form base = r.GetBaseObject()
			Debug.Trace("OSB: Backup step #" + i + " base=" + base)
			if IsSkippedBase(base)
				Debug.Trace("OSB: Backup skip ref=" + r + " base=" + base)
			elseif Math.Abs(r.GetPositionX() - bx) < 0.05 && Math.Abs(r.GetPositionY() - by) < 0.05 && Math.Abs(r.GetPositionZ() - bz) < 0.05
				; sits exactly on the beacon: part of the beacon pack-in (e.g. its foundation pad)
				Debug.Trace("OSB: Backup skip (on beacon) ref=" + r + " base=" + base)
			else
				; base prints as "[Form < (0006AC92)>]"; make-restore.ps1 parses the hex ID
				string line = "OSB-ITEM " + base + " " + r.GetPositionX() + " " + r.GetPositionY() + " " + r.GetPositionZ() + " " + r.GetAngleX() + " " + r.GetAngleY() + " " + r.GetAngleZ() + " " + r.GetScale()
				Debug.TraceUser(log, line)
				Debug.Trace("OSB: Backup keep ref=" + r + " base=" + base)
				saved += 1
			endif
		endif
		i += 1
	endwhile
	Debug.TraceUser(log, "OSB-END saved=" + saved)
	Debug.Notification("OSB: backed up " + saved + " objects (of " + total + " linked)")
EndFunction

; ---------- restore ----------

; One object by load-order form ID (old backups). Called once per line of OSB_Restore.txt.
Function R(int aiBaseID, float afX, float afY, float afZ, float afAX, float afAY, float afAZ, float afScale) global
	Form base = Game.GetForm(aiBaseID)
	if base == None
		Debug.Trace("OSB: R base not found " + aiBaseID + " (plugin missing or load order changed?)")
		Debug.Notification("OSB: base form missing " + aiBaseID)
		return
	endif
	PlaceBase(base, afX, afY, afZ, afAX, afAY, afAZ, afScale)
EndFunction

; One object by plugin file name + local form ID: independent of the load order.
Function RF(string asPlugin, int aiLocalID, float afX, float afY, float afZ, float afAX, float afAY, float afAZ, float afScale) global
	Form base = Game.GetFormFromFile(aiLocalID, asPlugin)
	if base == None
		Debug.Trace("OSB: RF base not found " + asPlugin + " " + aiLocalID + " (plugin not installed?)")
		Debug.Notification("OSB: missing " + asPlugin + " " + aiLocalID)
		return
	endif
	PlaceBase(base, afX, afY, afZ, afAX, afAY, afAZ, afScale)
EndFunction

; Relative-mode version of RF (see RR).
Function RRF(string asPlugin, int aiLocalID, float afLX, float afLY, float afLZ, float afAX, float afAY, float afRelAZ, float afScale) global
	Form base = Game.GetFormFromFile(aiLocalID, asPlugin)
	if base == None
		Debug.Trace("OSB: RRF base not found " + asPlugin + " " + aiLocalID + " (plugin not installed?)")
		Debug.Notification("OSB: missing " + asPlugin + " " + aiLocalID)
		return
	endif
	ObjectReference beacon = FindBeacon()
	if beacon == None
		Debug.Notification("OSB: no outpost beacon within 300 m")
		Debug.Trace("OSB: RRF no beacon")
		return
	endif
	float yaw = beacon.GetAngleZ()
	float c = Math.cos(yaw)
	float s = Math.sin(yaw)
	PlaceBase(base, beacon.GetPositionX() + afLX * c + afLY * s, beacon.GetPositionY() - afLX * s + afLY * c, beacon.GetPositionZ() + afLZ, afAX, afAY, afRelAZ + yaw, afScale)
EndFunction

Function PlaceBase(Form base, float afX, float afY, float afZ, float afAX, float afAY, float afAZ, float afScale) global
	ObjectReference beacon = FindBeacon()
	Keyword kw = GetWorkshopItemKeyword()
	if beacon == None || kw == None
		Debug.Notification("OSB: no outpost beacon within 300 m")
		Debug.Trace("OSB: R no beacon / keyword")
		return
	endif
	; spawn disabled, move into place, link to the outpost, then enable so 3D loads at the final spot
	ObjectReference newRef = beacon.PlaceAtMe(base, 1, true, true, false)
	if newRef == None
		Debug.Trace("OSB: PlaceAtMe failed base=" + base)
		return
	endif
	newRef.SetPosition(afX, afY, afZ)
	newRef.SetAngle(afAX, afAY, afAZ)
	if afScale > 0.0 && afScale != 1.0
		newRef.SetScale(afScale)
	endif
	newRef.SetLinkedRef(beacon, kw)
	newRef.Enable(false)
	Debug.Trace("OSB: placed new=" + newRef + " base=" + base + " workshop=" + newRef.GetWorkshop() + " inBuildableArea=" + newRef.IsWithinBuildableArea(beacon))
EndFunction

; One object, placed relative to the beacon of the outpost the player is in.
; afLX/afLY/afLZ: offset in the old beacon's local frame (meters); afRelAZ: heading relative to the old beacon.
; make-restore.ps1 computes these, so the layout follows the new beacon's position and heading.
Function RR(int aiBaseID, float afLX, float afLY, float afLZ, float afAX, float afAY, float afRelAZ, float afScale) global
	ObjectReference beacon = FindBeacon()
	if beacon == None
		Debug.Notification("OSB: no outpost beacon within 300 m")
		Debug.Trace("OSB: RR no beacon")
		return
	endif
	float yaw = beacon.GetAngleZ()
	float c = Math.cos(yaw)   ; Papyrus Math.cos/sin take degrees
	float s = Math.sin(yaw)
	; heading is clockwise from +Y: local (x, y) -> world (x*c + y*s, -x*s + y*c)
	float wx = beacon.GetPositionX() + afLX * c + afLY * s
	float wy = beacon.GetPositionY() - afLX * s + afLY * c
	float wz = beacon.GetPositionZ() + afLZ
	R(aiBaseID, wx, wy, wz, afAX, afAY, afRelAZ + yaw, afScale)
EndFunction

; ---------- guidance ----------
; Local coordinates are measured from the center of the current landing zone, so they only
; match the backup when you land at the same spot (e.g. same latitude/longitude).

; Print planet, landing-zone position of the player and of the nearest outpost beacon.
Function Where() global
	; each part is traced on its own: some native calls can hang (see GetFormID), so a hang
	; in a later part must not hide the earlier results
	Actor player = Game.GetPlayer()
	Debug.Trace("OSB: Where player=(" + player.GetPositionX() + "," + player.GetPositionY() + "," + player.GetPositionZ() + ") heading=" + player.GetAngleZ())
	Debug.Notification("OSB: you are at " + Math.Floor(player.GetPositionX()) + ", " + Math.Floor(player.GetPositionY()) + ", " + Math.Floor(player.GetPositionZ()))
	ObjectReference beacon = FindBeacon()
	if beacon
		Debug.Trace("OSB: Where beacon=" + beacon + " pos=(" + beacon.GetPositionX() + "," + beacon.GetPositionY() + "," + beacon.GetPositionZ() + ") yaw=" + beacon.GetAngleZ())
	endif
	Debug.Trace("OSB: Where getting ship")
	ObjectReference ship = Game.GetPlayerHomeSpaceShip()
	Debug.Trace("OSB: Where ship=" + ship)
	if ship
		Debug.Trace("OSB: Where shippos=(" + ship.GetPositionX() + "," + ship.GetPositionY() + "," + ship.GetPositionZ() + ")")
		Debug.Notification("OSB: ship at " + Math.Floor(ship.GetPositionX()) + ", " + Math.Floor(ship.GetPositionY()))
	endif
EndFunction

; atan2 in degrees, heading convention: 0 = +Y, clockwise positive
float Function HeadingTo(float afDX, float afDY) global
	if afDX == 0.0 && afDY == 0.0
		return 0.0
	endif
	float a
	if Math.abs(afDY) >= Math.abs(afDX)
		a = Math.atan(afDX / afDY)
		if afDY < 0.0
			a += 180.0
		endif
	else
		a = 90.0 - Math.atan(afDY / afDX)
		if afDX < 0.0
			a += 180.0
		endif
	endif
	while a > 180.0
		a -= 360.0
	endwhile
	while a <= -180.0
		a += 360.0
	endwhile
	return a
EndFunction

; Walk the player to a landing-zone position (the old beacon spot). Updates every 2 s for up to
; aiSeconds (default 180), stops when within 1.5 m. Notification text is ASCII on purpose.
Function Guide(float afX, float afY, float afZ, int aiSeconds = 180) global
	Actor player = Game.GetPlayer()
	int elapsed = 0
	while elapsed < aiSeconds
		float dx = afX - player.GetPositionX()
		float dy = afY - player.GetPositionY()
		float dz = afZ - player.GetPositionZ()
		float dist = Math.sqrt(dx * dx + dy * dy)
		if dist < 1.5
			Debug.Notification("OSB: you are on the old beacon spot (height " + Math.Floor(dz * 10.0) / 10.0 + " m). Place the beacon here.")
			Debug.Trace("OSB: Guide arrived dz=" + dz)
			return
		endif
		float turn = HeadingTo(dx, dy) - player.GetAngleZ()
		while turn > 180.0
			turn -= 360.0
		endwhile
		while turn <= -180.0
			turn += 360.0
		endwhile
		string dir = "ahead"
		if turn > 20.0
			dir = "turn right " + Math.Floor(turn) + " deg"
		elseif turn < -20.0
			dir = "turn left " + Math.Floor(-turn) + " deg"
		endif
		Debug.Notification("OSB: old beacon " + Math.Floor(dist) + " m, " + dir + ", height " + Math.Floor(dz) + " m")
		Utility.Wait(2.0)
		elapsed += 2
	endwhile
	Debug.Notification("OSB: guide timed out, run bat OSB_Guide again")
EndFunction

Function Done(int aiExpected) global
	ObjectReference beacon = FindBeacon()
	Keyword kw = GetWorkshopItemKeyword()
	int linked = 0
	if beacon && kw
		ObjectReference[] refs = beacon.GetRefsLinkedToMe(kw)
		if refs
			linked = refs.Length
		endif
	endif
	Debug.Trace("OSB: Restore done, expected=" + aiExpected + " beacon=" + beacon + " linkedNow=" + linked)
	Debug.Notification("OSB: restore done (" + aiExpected + " objects), outpost now has " + linked + " linked refs")
EndFunction
