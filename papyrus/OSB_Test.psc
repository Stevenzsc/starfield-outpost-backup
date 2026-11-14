Scriptname OSB_Test
{ Outpost Backup - verification script. Call from console with cgf, no esp needed. }

; Usage (console):
;   cgf "OSB_Test.List" <beaconRefID decimal>
;   cgf "OSB_Test.PlaceTest" <beaconRefID decimal> <outpostObjectRefID decimal>
; Output: notifications + Papyrus log（Documents\My Games\Starfield\Logs\Script\Papyrus.0.log）

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
	newRef.SetPosition(src.GetPositionX() + 300.0, src.GetPositionY(), src.GetPositionZ())
	newRef.SetAngle(src.GetAngleX(), src.GetAngleY(), src.GetAngleZ())
	bool inArea = newRef.IsWithinBuildableArea(beacon)
	ObjectReference ws = newRef.GetWorkshop()
	Debug.Trace("OSB: PlaceTest new=" + newRef + " base=" + base + " workshop=" + ws + " inBuildableArea=" + inArea + " srcWorkshop=" + src.GetWorkshop())
	Debug.Notification("OSB place: workshop=" + ws + " inArea=" + inArea)
EndFunction
