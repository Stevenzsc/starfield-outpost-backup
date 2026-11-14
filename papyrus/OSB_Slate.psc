Scriptname OSB_Slate extends ObjectReference
{ Outpost Backup slate: read it from the inventory to back up, land at, or restore an outpost.
  Needs the OutpostBackup SFSE plugin (OSB_Native) and OSB.psc. }

Message Property OSB_MenuMsg Auto Const Mandatory
{ Buttons: 0 Back up this outpost, 1 Land at the saved outpost, 2 Restore the saved outpost here, 3 Cancel }

Event OnRead()
	int choice = OSB_MenuMsg.Show()
	if choice == 0
		DoBackup()
	elseif choice == 1
		DoLand()
	elseif choice == 2
		DoRestore()
	endif
EndEvent

Function DoBackup()
	ObjectReference beacon = OSB.FindBeacon()
	Keyword kw = OSB.GetWorkshopItemKeyword()
	if beacon == None || kw == None
		Debug.Notification("Outpost Backup: stand inside the outpost you want to back up.")
		return
	endif
	if !OSB_Native.BeginBackup(beacon.GetPositionX(), beacon.GetPositionY(), beacon.GetPositionZ(), beacon.GetAngleZ())
		Debug.Notification("Outpost Backup: only works on a planet surface.")
		return
	endif
	Debug.Notification("Outpost Backup: backing up, please wait...")
	float bx = beacon.GetPositionX()
	float by = beacon.GetPositionY()
	float bz = beacon.GetPositionZ()
	ObjectReference[] refs = beacon.GetRefsLinkedToMe(kw)
	int i = 0
	while refs && i < refs.Length
		ObjectReference r = refs[i]
		if r && r != beacon
			Form base = r.GetBaseObject()
			bool onBeacon = Math.Abs(r.GetPositionX() - bx) < 0.05 && Math.Abs(r.GetPositionY() - by) < 0.05 && Math.Abs(r.GetPositionZ() - bz) < 0.05
			if !OSB.IsSkippedBase(base) && !onBeacon
				OSB_Native.AddItem(base, r.GetPositionX(), r.GetPositionY(), r.GetPositionZ(), r.GetAngleX(), r.GetAngleY(), r.GetAngleZ(), r.GetScale(), (base as PackIn) != None)
			endif
		endif
		i += 1
	endwhile
	int n = OSB_Native.EndBackup()
	if n < 0
		Debug.Notification("Outpost Backup: could not save the backup.")
	else
		Debug.Notification("Outpost Backup: saved " + n + " objects.")
	endif
EndFunction

Function DoLand()
	if OSB_Native.ArmLanding()
		Debug.MessageBox("Landing set.\n\nOpen the star map on the planet of the saved outpost and land anywhere: your ship will land at the saved spot.\n\nThen read this slate again and choose Restore.")
	else
		Debug.Notification("Outpost Backup: no saved outpost yet.")
	endif
EndFunction

Function DoRestore()
	float[] spot = OSB_Native.GetGuideSpot()
	if spot == None || spot.Length < 4
		Debug.Notification("Outpost Backup: no saved outpost on this planet.")
		return
	endif
	ObjectReference beacon = OSB.FindBeacon()
	if beacon == None
		if spot[3] > 2000.0
			Debug.MessageBox("The saved outpost is " + Math.Floor(spot[3] / 1000.0) + " km from here.\n\nChoose 'Land at the saved outpost' first, then land on this planet again.")
			return
		endif
		Debug.MessageBox("Follow the messages to the saved outpost spot, build an Outpost Beacon there, then read this slate again and choose Restore.")
		OSB.Guide(spot[0], spot[1], spot[2], 300)
		return
	endif
	int n = OSB_Native.LoadRestore()
	if n <= 0
		Debug.Notification("Outpost Backup: nothing to restore.")
		return
	endif
	Debug.Notification("Outpost Backup: restoring " + n + " objects...")
	int placed = 0
	int i = 0
	while i < n
		Form base = OSB_Native.GetRestoreForm(i)
		float[] t = OSB_Native.GetRestoreXform(i)
		if base && t && t.Length >= 7
			if OSB.PlaceBase(base, t[0], t[1], t[2], t[3], t[4], t[5], t[6])
				placed += 1
			endif
		else
			Debug.Trace("OSB: restore skipped " + OSB_Native.GetRestoreName(i) + " (not found: plugin missing?)")
		endif
		i += 1
	endwhile
	Debug.Notification("Outpost Backup: restored " + placed + " of " + n + " objects.")
EndFunction
