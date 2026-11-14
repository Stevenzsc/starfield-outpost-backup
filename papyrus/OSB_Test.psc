Scriptname OSB_Test
{ Outpost Backup - verification script. Call from console with cgf, no esp needed. }

; Usage (console):
;   cgf "OSB_Test.List" <beaconRefID decimal>
;   cgf "OSB_Test.PlaceTest" <beaconRefID decimal> <outpostObjectRefID decimal>
;   cgf "OSB_Test.CheckLink" <beaconRefID decimal> <objectRefID decimal>
;   cgf "OSB_Test.PlaceLinkTest" <beaconRefID decimal> <outpostObjectRefID decimal>
; WorkshopItemKeyword = Starfield.esm KYWD 00054BA6 (found by scanning the ESM)
; Output: notifications + Papyrus log (Documents\My Games\Starfield\Logs\Script\Papyrus.0.log)

ObjectReference Function GetRef(int aiFormID) global
	ObjectReference ref = Game.GetForm(aiFormID) as ObjectReference
	if ref == None
		Debug.Notification("OSB: refID not found " + aiFormID)
		Debug.Trace("OSB: GetForm failed for " + aiFormID)
	endif
	return ref
EndFunction

Function DumpArray(string asLabel, ObjectReference[] akRefs, ObjectReference akBeacon) global
	if akRefs == None
		Debug.Trace("OSB: [" + asLabel + "] = None")
		return
	endif
	int n = akRefs.Length
	int sameWorkshop = 0
	int i = 0
	while i < n
		ObjectReference r = akRefs[i]
		if r && r.GetWorkshop() == akBeacon
			sameWorkshop += 1
		endif
		if i < 60
			Debug.Trace("OSB: [" + asLabel + "] #" + i + " ref=" + r + " base=" + r.GetBaseObject() + " pos=(" + r.GetPositionX() + "," + r.GetPositionY() + "," + r.GetPositionZ() + ") ang=(" + r.GetAngleX() + "," + r.GetAngleY() + "," + r.GetAngleZ() + ") scale=" + r.GetScale() + " workshop=" + r.GetWorkshop())
		endif
		i += 1
	endwhile
	Debug.Trace("OSB: [" + asLabel + "] total=" + n + " belongToBeacon=" + sameWorkshop)
	Debug.Notification("OSB " + asLabel + ": " + n + " found (in this outpost: " + sameWorkshop + ")")
EndFunction

Function List(int aiBeaconID) global
	ObjectReference beacon = GetRef(aiBeaconID)
	if beacon == None
		return
	endif
	Debug.Trace("OSB: ===== List beacon=" + beacon + " base=" + beacon.GetBaseObject() + " workshop=" + beacon.GetWorkshop())
	DumpArray("ResourceObjects", beacon.GetWorkshopResourceObjects(None, 0), beacon)
	DumpArray("OwnedByPlayer", beacon.GetWorkshopOwnedObjects(Game.GetPlayer()), beacon)
	DumpArray("LinkedToMe", beacon.GetRefsLinkedToMe(None), beacon)
	Keyword kw = GetWorkshopItemKeyword()
	if kw
		DumpArray("LinkedToMe_WorkshopItemKeyword", beacon.GetRefsLinkedToMe(kw), beacon)
	endif
	DumpArray("Destructible", beacon.GetDestructibleOutpostObjects(), beacon)
	Debug.Trace("OSB: ===== List done")
EndFunction

Function PlaceTest(int aiBeaconID, int aiSourceID) global
	ObjectReference beacon = GetRef(aiBeaconID)
	ObjectReference src = GetRef(aiSourceID)
	if beacon == None || src == None
		return
	endif
	Form base = src.GetBaseObject()
	ObjectReference newRef = src.PlaceAtMe(base, 1, true, false, false)
	if newRef == None
		Debug.Notification("OSB: PlaceAtMe failed")
		Debug.Trace("OSB: PlaceTest PlaceAtMe returned None, base=" + base)
		return
	endif
	newRef.SetPosition(src.GetPositionX() + 3.0, src.GetPositionY(), src.GetPositionZ())
	newRef.SetAngle(src.GetAngleX(), src.GetAngleY(), src.GetAngleZ())
	bool inArea = newRef.IsWithinBuildableArea(beacon)
	ObjectReference ws = newRef.GetWorkshop()
	Debug.Trace("OSB: PlaceTest new=" + newRef + " base=" + base + " workshop=" + ws + " inBuildableArea=" + inArea + " srcWorkshop=" + src.GetWorkshop())
	Debug.Notification("OSB place: workshop=" + ws + " inArea=" + inArea)
EndFunction

Keyword Function GetWorkshopItemKeyword() global
	Keyword kw = Game.GetFormFromFile(0x00054BA6, "Starfield.esm") as Keyword
	if kw == None
		Debug.Notification("OSB: WorkshopItemKeyword not found")
		Debug.Trace("OSB: GetFormFromFile 00054BA6 is not a Keyword")
	endif
	return kw
EndFunction

; Does an existing outpost object point to the beacon through WorkshopItemKeyword?
Function CheckLink(int aiBeaconID, int aiObjectID) global
	ObjectReference beacon = GetRef(aiBeaconID)
	ObjectReference obj = GetRef(aiObjectID)
	Keyword kw = GetWorkshopItemKeyword()
	if beacon == None || obj == None || kw == None
		return
	endif
	ObjectReference linkedKw = obj.GetLinkedRef(kw)
	ObjectReference linkedNone = obj.GetLinkedRef(None)
	Debug.Trace("OSB: CheckLink obj=" + obj + " base=" + obj.GetBaseObject() + " workshop=" + obj.GetWorkshop() + " linked(WorkshopItemKeyword)=" + linkedKw + " linked(None)=" + linkedNone + " isBeacon=" + (linkedKw == beacon))
	Debug.Notification("OSB link: kw=" + linkedKw + " isBeacon=" + (linkedKw == beacon))
EndFunction

; Copy an outpost object, link it to the beacon with WorkshopItemKeyword, refresh 3D, report.
Function PlaceLinkTest(int aiBeaconID, int aiSourceID) global
	ObjectReference beacon = GetRef(aiBeaconID)
	ObjectReference src = GetRef(aiSourceID)
	Keyword kw = GetWorkshopItemKeyword()
	if beacon == None || src == None || kw == None
		return
	endif
	Form base = src.GetBaseObject()
	; spawn disabled, move, link, then enable so 3D loads at the final position
	ObjectReference newRef = src.PlaceAtMe(base, 1, true, true, false)
	if newRef == None
		Debug.Notification("OSB: PlaceAtMe failed")
		Debug.Trace("OSB: PlaceLinkTest PlaceAtMe returned None, base=" + base)
		return
	endif
	newRef.SetPosition(src.GetPositionX() + 3.0, src.GetPositionY(), src.GetPositionZ())
	newRef.SetAngle(src.GetAngleX(), src.GetAngleY(), src.GetAngleZ())
	ObjectReference wsBefore = newRef.GetWorkshop()
	newRef.SetLinkedRef(beacon, kw)
	newRef.Enable(false)
	ObjectReference wsAfter = newRef.GetWorkshop()
	bool inArea = newRef.IsWithinBuildableArea(beacon)
	bool listed = false
	ObjectReference[] linked = beacon.GetRefsLinkedToMe(kw)
	int i = 0
	while linked && i < linked.Length
		if linked[i] == newRef
			listed = true
		endif
		i += 1
	endwhile
	Debug.Trace("OSB: PlaceLinkTest new=" + newRef + " base=" + base + " workshopBefore=" + wsBefore + " workshopAfter=" + wsAfter + " inBuildableArea=" + inArea + " inLinkedList=" + listed + " disabled=" + newRef.IsDisabled())
	Debug.Notification("OSB placeLink: ws=" + wsAfter + " listed=" + listed + " inArea=" + inArea)
EndFunction
