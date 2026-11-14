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
	Debug.TraceUser(log, "OSB-BEGIN beacon=" + beacon + " pos=(" + beacon.GetPositionX() + "," + beacon.GetPositionY() + "," + beacon.GetPositionZ() + ") linked=" + total)
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

; One object. Called once per line of OSB_Restore.txt.
Function R(int aiBaseID, float afX, float afY, float afZ, float afAX, float afAY, float afAZ, float afScale) global
	Form base = Game.GetForm(aiBaseID)
	if base == None
		Debug.Trace("OSB: R base not found " + aiBaseID + " (plugin missing or load order changed?)")
		Debug.Notification("OSB: base form missing " + aiBaseID)
		return
	endif
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
		Debug.Trace("OSB: R PlaceAtMe failed base=" + base)
		return
	endif
	newRef.SetPosition(afX, afY, afZ)
	newRef.SetAngle(afAX, afAY, afAZ)
	if afScale > 0.0 && afScale != 1.0
		newRef.SetScale(afScale)
	endif
	newRef.SetLinkedRef(beacon, kw)
	newRef.Enable(false)
	Debug.Trace("OSB: R new=" + newRef + " base=" + base + " workshop=" + newRef.GetWorkshop() + " inBuildableArea=" + newRef.IsWithinBuildableArea(beacon))
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
