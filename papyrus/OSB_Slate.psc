Scriptname OSB_Slate extends ObjectReference
{ Outpost Backup slate: read it from the inventory to back up, land at, restore, or delete saved outposts.
  Needs the OutpostBackup SFSE plugin (OSB_Native) and OSB.psc.
  The extra menus are looked up by form ID in OutpostBackup.esp (written by tools\esp\PatchEsp).

  Land and Restore start an automatic flow: wait until you are outside near the saved outpost,
  move you onto the saved beacon spot, wait for you to build an Outpost Beacon, then restore. }

Message Property OSB_MenuMsg Auto Const Mandatory
{ Buttons: 0 Back up, 1 Land, 2 Restore, 3 Delete, 4 Cancel }

int Property kPickMsg = 0x80B AutoReadOnly Hidden
int Property kFarBeaconMsg = 0x80D AutoReadOnly Hidden
int Property kDupMsg = 0x80E AutoReadOnly Hidden
int Property kDeleteMsg = 0x80F AutoReadOnly Hidden
int Property kNavKeyword = 0x810 AutoReadOnly Hidden
int Property kNavQuest = 0x811 AutoReadOnly Hidden

float Property kMaxGuideMeters = 2000.0 AutoReadOnly Hidden      ; farther than this: land again first
float Property kBeaconNearMeters = 50.0 AutoReadOnly Hidden      ; a beacon this close to the saved spot counts
float Property kBeaconToleranceMeters = 25.0 AutoReadOnly Hidden ; farther than this: ask before restoring
int Property kPageSize = 8 AutoReadOnly Hidden
float Property kProgressSeconds = 3.0 AutoReadOnly Hidden
float Property kPollSeconds = 3.0 AutoReadOnly Hidden
float Property kArriveTimeout = 7200.0 AutoReadOnly Hidden       ; real seconds to wait for the landing
float Property kBeaconTimeout = 1800.0 AutoReadOnly Hidden       ; real seconds to wait for the beacon

Message Function Msg(int aiLocalID)
	return Game.GetFormFromFile(aiLocalID, "OutpostBackup.esp") as Message
EndFunction

Function Notify(string asText)
	Debug.Notification("Outpost Backup: " + asText)
EndFunction

Event OnRead()
	if OSB_Native.Version() < 5
		Debug.MessageBox("Outpost Backup: the SFSE plugin is missing or out of date.\n\nInstall Data\\SFSE\\Plugins\\OutpostBackup.dll and start the game with SFSE (sfse_loader.exe).")
		return
	endif
	int choice = OSB_MenuMsg.Show()
	if choice < 0 || choice > 3
		return
	endif
	if !OSB_Native.TryLock()
		Notify("still busy with the last action, please wait.")
		return
	endif
	int flow = 0
	if choice == 0
		DoBackup()
	elseif choice == 1
		flow = DoLand()
	elseif choice == 2
		flow = DoRestore()
	elseif choice == 3
		DoDelete()
	endif
	OSB_Native.Unlock()
	if flow > 0
		RunFlow(flow)
	endif
EndEvent

; ---------- checks ----------

bool Function IsOutsideOnPlanet()
	Actor player = Game.GetPlayer()
	return !player.IsInSpace() && player.GetCurrentPlanet() != None && !player.IsInInterior()
EndFunction

; True when the player stands outside on a planet and is not fighting. Otherwise says why and returns false.
bool Function CheckOnSurface()
	Actor player = Game.GetPlayer()
	if player.IsInSpace() || player.GetCurrentPlanet() == None
		Notify("land on a planet first.")
		return false
	endif
	if player.IsInInterior()
		Notify("go outside first (not in a ship or building).")
		return false
	endif
	if player.IsInCombat()
		Notify("not during combat.")
		return false
	endif
	return true
EndFunction

; Number of outpost objects linked to the beacon, without the beacon's own parts.
int Function CountOutpostObjects(ObjectReference akBeacon, Keyword akKeyword)
	ObjectReference[] refs = akBeacon.GetRefsLinkedToMe(akKeyword)
	int n = 0
	int i = 0
	while refs && i < refs.Length
		ObjectReference r = refs[i]
		if r && r != akBeacon && OSB_Native.IsBuildable(r.GetBaseObject()) && !OnBeacon(r, akBeacon)
			n += 1
		endif
		i += 1
	endwhile
	return n
EndFunction

bool Function OnBeacon(ObjectReference akRef, ObjectReference akBeacon)
	return Math.Abs(akRef.GetPositionX() - akBeacon.GetPositionX()) < 0.05 && Math.Abs(akRef.GetPositionY() - akBeacon.GetPositionY()) < 0.05 && Math.Abs(akRef.GetPositionZ() - akBeacon.GetPositionZ()) < 0.05
EndFunction

float Function Distance2D(float afX1, float afY1, float afX2, float afY2)
	float dx = afX1 - afX2
	float dy = afY1 - afY2
	return Math.Sqrt(dx * dx + dy * dy)
EndFunction

; The outpost beacon near the saved spot, or None.
ObjectReference Function BeaconNearSpot(float[] akSpot)
	ObjectReference b = OSB.FindBeacon()
	if b && Distance2D(b.GetPositionX(), b.GetPositionY(), akSpot[0], akSpot[1]) <= kBeaconNearMeters
		return b
	endif
	return None
EndFunction

; ---------- choosing a saved outpost ----------

; Shows the saved outposts and selects the chosen one. Returns true when one is selected.
bool Function PickBackup(bool abHereOnly)
	int n = OSB_Native.ListBackups(abHereOnly)
	if n <= 0
		if abHereOnly
			Debug.MessageBox("No saved outpost on this planet.\n\nChoose 'Land at a saved outpost' to fly to one first.")
		else
			Debug.MessageBox("No saved outposts yet.\n\nStand inside an outpost and choose 'Back up this outpost'.")
		endif
		return false
	endif
	Message pick = Msg(kPickMsg)
	if pick == None
		Notify("OutpostBackup.esp is out of date.")
		return false
	endif
	int page = 0
	while true
		OSB_Native.FillPicker(pick, page)
		int r = OSB_Native.PickerResult(pick.Show())
		if r == -2
			page += 1
			if page * kPageSize >= n
				page = 0
			endif
		elseif r < 0
			return false
		else
			return OSB_Native.SelectBackup(r)
		endif
	endwhile
	return false
EndFunction

; ---------- actions ----------

Function DoBackup()
	if !CheckOnSurface()
		return
	endif
	ObjectReference beacon = OSB.FindBeacon()
	Keyword kw = OSB.GetWorkshopItemKeyword()
	if beacon == None || kw == None
		Notify("stand inside the outpost you want to back up.")
		return
	endif
	Actor player = Game.GetPlayer()
	Location planetLoc = None
	Location[] planetParents = None
	Planet p = player.GetCurrentPlanet()
	if p
		planetLoc = p.GetLocation()
		if planetLoc
			planetParents = planetLoc.GetParentLocations()
		endif
	endif
	if !OSB_Native.BeginBackup(beacon, planetLoc, planetParents, player.GetCurrentLocation(), beacon.GetPositionX(), beacon.GetPositionY(), beacon.GetPositionZ(), beacon.GetAngleZ())
		Notify("only works on a planet surface.")
		return
	endif
	Notify("backing up, please wait...")
	ObjectReference[] refs = beacon.GetRefsLinkedToMe(kw)
	int total = 0
	if refs
		total = refs.Length
	endif
	float lastShown = Utility.GetCurrentRealTime()
	int i = 0
	while i < total
		if Utility.GetCurrentRealTime() - lastShown >= kProgressSeconds
			lastShown = Utility.GetCurrentRealTime()
			Notify("backing up... " + (i * 100 / total) + "% (" + i + " of " + total + ")")
		endif
		ObjectReference r = refs[i]
		if r && r != beacon
			Form base = r.GetBaseObject()
			if OSB_Native.IsBuildable(base) && !OSB.IsSkippedBase(base) && !OnBeacon(r, beacon)
				OSB_Native.AddItem(base, r.GetPositionX(), r.GetPositionY(), r.GetPositionZ(), r.GetAngleX(), r.GetAngleY(), r.GetAngleZ(), r.GetScale(), (base as PackIn) != None)
			endif
		endif
		i += 1
	endwhile
	int n = OSB_Native.EndBackup()
	if n < 0
		Notify("could not save the backup.")
	elseif n == 0
		Notify("this outpost has nothing to back up yet.")
	elseif OSB_Native.LastBackupReplaced()
		Notify("updated the backup of this outpost (" + n + " objects).")
	else
		Notify("saved " + n + " objects.")
	endif
EndFunction

; Returns a flow id to run after the menu closes, or 0.
int Function DoLand()
	if !PickBackup(false)
		return 0
	endif
	if !OSB_Native.ArmLanding()
		Notify("could not read that backup.")
		return 0
	endif
	int flow = OSB_Native.NewFlow()
	float[] spot = OSB_Native.GetGuideSpot()
	if IsOutsideOnPlanet() && spot && spot.Length >= 4 && spot[3] <= kMaxGuideMeters
		return flow   ; already there: the flow moves you onto the spot
	endif
	string planetName = Where()
	bool marked = StartNav()
	if Game.IsFarTravelControlsEnabled()
		if marked
			Debug.Notification("Outpost Backup: " + planetName + " has a quest marker in the star map. Set course and land anywhere on it.")
		else
			Debug.Notification("Outpost Backup: open " + planetName + " in the star map and land anywhere on it.")
		endif
		Game.ShowGalaxyStarMapMenu()
	else
		Debug.MessageBox("Landing set.\n\nWhen you can travel again, open " + planetName + " in the star map and land anywhere on it: your ship lands at the saved outpost.")
	endif
	return flow
EndFunction

; ---------- star-map marker ----------
; OSB_NavQuest finds the location tagged with OSB_NavTargetKeyword and puts a quest target on that planet.

Location navLoc = None

Quest Function NavQuest()
	return Game.GetFormFromFile(kNavQuest, "OutpostBackup.esp") as Quest
EndFunction

Keyword Function NavKeyword()
	return Game.GetFormFromFile(kNavKeyword, "OutpostBackup.esp") as Keyword
EndFunction

; Mark the selected outpost's planet. False for backups without a planet location (back up again to fix).
bool Function StartNav()
	Location loc = OSB_Native.GetSelectedPlanetLocation()
	Quest q = NavQuest()
	Keyword kw = NavKeyword()
	StopNav()
	if loc == None || q == None || kw == None
		Debug.Trace("OSB: no star-map marker (location " + loc + ", quest " + q + ", keyword " + kw + ")")
		return false
	endif
	navLoc = loc
	loc.AddKeyword(kw)
	if !q.Start()
		Debug.Trace("OSB: OSB_NavQuest did not start for " + loc)
		StopNav()
		return false
	endif
	q.SetObjectiveDisplayed(10)
	q.SetActive(true)
	Debug.Trace("OSB: star-map marker set on " + loc)
	return true
EndFunction

Function StopNav()
	Quest q = NavQuest()
	if q && q.IsRunning()
		q.SetObjectiveCompleted(10)
		q.Stop()
		int i = 0
		while !q.IsStopped() && i < 50
			Utility.Wait(0.1)
			i += 1
		endwhile
	endif
	Keyword kw = NavKeyword()
	if navLoc && kw
		navLoc.RemoveKeyword(kw)
	endif
	navLoc = None
EndFunction

; Returns a flow id to run after the menu closes, or 0.
int Function DoRestore()
	if !CheckOnSurface()
		return 0
	endif
	int here = OSB_Native.SelectOnlyHere()
	if here == 0
		Debug.MessageBox("No saved outpost on this planet.\n\nChoose 'Land at a saved outpost' to fly to one first.")
		return 0
	elseif here > 1 && !PickBackup(true)
		return 0
	endif
	float[] spot = OSB_Native.GetGuideSpot()
	if spot == None || spot.Length < 4
		Notify("that outpost is on another planet.")
		return 0
	endif
	if spot[3] > kMaxGuideMeters
		Debug.MessageBox("The saved outpost is " + Math.Floor(spot[3] / 1000.0) + " km from here.\n\nChoose 'Land at a saved outpost': the star map opens and your ship lands right at it.")
		return 0
	endif
	return OSB_Native.NewFlow()
EndFunction

; "Planet (System)" of the selected outpost.
string Function Where()
	string planetName = OSB_Native.GetSelectedPlanetName()
	string systemName = OSB_Native.GetSelectedSystemName()
	if systemName != ""
		return planetName + " in the " + systemName + " system"
	endif
	return planetName
EndFunction

; Arrive near the saved outpost -> stand on the beacon spot -> wait for a beacon -> restore.
Function RunFlow(int aiFlow)
	Actor player = Game.GetPlayer()
	float[] spot

	; 1. wait until you are outside near the saved outpost (after the landing)
	Planet warnedOn = None
	float waited = 0.0
	while true
		if OSB_Native.CurrentFlow() != aiFlow
			return
		endif
		if IsOutsideOnPlanet()
			spot = OSB_Native.GetGuideSpot()
			if spot == None || spot.Length < 4
				Planet here = player.GetCurrentPlanet()
				if here != warnedOn
					warnedOn = here
					Debug.MessageBox("This is not the saved outpost's planet.\n\nThe outpost is on " + Where() + ". Open the star map and land there.")
				endif
			endif
			if spot && spot.Length >= 4 && spot[3] <= kMaxGuideMeters
				Utility.Wait(1.0)
				StopNav()   ; arrived: drop the star-map marker
				; 2. stand on the saved beacon spot (unless a beacon is already there)
				ObjectReference beacon = BeaconNearSpot(spot)
				if beacon == None
					if Distance2D(player.GetPositionX(), player.GetPositionY(), spot[0], spot[1]) > 3.0
						player.SetPosition(spot[0], spot[1], spot[2] + 1.0)
					endif
					Debug.MessageBox("You are on the saved outpost spot.\n\nBuild an Outpost Beacon right here. The outpost is restored as soon as the beacon is built.")
					beacon = WaitForBeacon(aiFlow, spot)
				endif
				if beacon
					RestoreAt(beacon, spot)
				endif
				return
			endif
		endif
		Utility.Wait(kPollSeconds)
		waited += kPollSeconds
		if waited > kArriveTimeout
			Notify("stopped waiting for the landing. Choose Land again when you are ready.")
			return
		endif
	endwhile
EndFunction

ObjectReference Function WaitForBeacon(int aiFlow, float[] akSpot)
	float waited = 0.0
	while waited < kBeaconTimeout
		Utility.Wait(kPollSeconds)
		waited += kPollSeconds
		if OSB_Native.CurrentFlow() != aiFlow
			return None
		endif
		ObjectReference b = BeaconNearSpot(akSpot)
		if b
			Utility.Wait(2.0)   ; let the build finish
			return b
		endif
	endwhile
	Notify("no beacon built. Choose Restore when the beacon is ready.")
	return None
EndFunction

; Remove every outpost object that is not in akBefore (everything a cancelled build added, hab parts included).
int Function UndoBuild(ObjectReference akBeacon, ObjectReference[] akBefore)
	ObjectReference[] now = akBeacon.GetRefsLinkedToMe(OSB.GetWorkshopItemKeyword())
	int removed = 0
	int i = 0
	while now && i < now.Length
		ObjectReference r = now[i]
		if r && r != akBeacon && (akBefore == None || akBefore.Find(r) < 0)
			r.Disable()
			r.Delete()
			removed += 1
		endif
		i += 1
	endwhile
	Debug.Trace("OSB: undo removed " + removed + " objects")
	return removed
EndFunction

; Build the loaded restore list through the workshop engine, like the player would: opens build mode at
; the beacon, the plugin selects, positions and commits each object. False if it could not start
; (the caller then falls back to placing objects directly).
bool Function AutoBuildAt(ObjectReference akBeacon, int aiCount)
	Notify("building " + aiCount + " objects. Press the exit-build-mode key to cancel and undo.")
	; what the outpost has now, so a cancelled build can be undone
	ObjectReference[] before = akBeacon.GetRefsLinkedToMe(OSB.GetWorkshopItemKeyword())
	; lock movement and looking during the build (the plugin also ignores build-mode keys, except exit = cancel)
	InputEnableLayer lock = InputEnableLayer.Create()
	lock.DisablePlayerControls(true, true, true, true, true, true, true, true, true, true, true)
	akBeacon.StartWorkshop(true)
	Utility.Wait(3.0)
	if !OSB_Native.AutoBuild()
		lock.Delete()
		akBeacon.StartWorkshop(false)
		Debug.Trace("OSB: auto-build could not start, placing objects directly")
		return false
	endif
	int[] p = OSB_Native.AutoBuildProgress()
	float lastShown = Utility.GetCurrentRealTime()
	while p && p.Length >= 5 && p[4] == 1
		Utility.Wait(1.0)
		p = OSB_Native.AutoBuildProgress()
		if p && p.Length >= 5 && Utility.GetCurrentRealTime() - lastShown >= kProgressSeconds
			lastShown = Utility.GetCurrentRealTime()
			Notify("building... " + p[0] + " of " + p[3])
		endif
	endwhile
	akBeacon.StartWorkshop(false)
	if p && p.Length >= 6 && p[5] == 1
		int removed = UndoBuild(akBeacon, before)
		lock.Delete()
		Debug.MessageBox("Restore cancelled.\n\nRemoved the " + removed + " objects it had built; the outpost is as before.")
		return true
	endif
	lock.Delete()
	if p && p.Length >= 5
		if p[2] > 0
			Debug.MessageBox("Built " + p[1] + " of " + p[3] + " objects.\n\n" + p[2] + " could not be built (not in the build menu, e.g. a mod that is not installed).")
		else
			Notify("built " + p[1] + " of " + p[3] + " objects.")
		endif
	endif
	return true
EndFunction

Function RestoreAt(ObjectReference akBeacon, float[] akSpot)
	while !OSB_Native.TryLock()
		Utility.Wait(1.0)
	endwhile
	Keyword kw = OSB.GetWorkshopItemKeyword()
	float beaconOff = Distance2D(akBeacon.GetPositionX(), akBeacon.GetPositionY(), akSpot[0], akSpot[1])
	if beaconOff > kBeaconToleranceMeters && Msg(kFarBeaconMsg).Show(beaconOff) != 0
		OSB_Native.Unlock()
		return
	endif
	int existing = CountOutpostObjects(akBeacon, kw)
	if existing > 0 && Msg(kDupMsg).Show(existing) != 0
		OSB_Native.Unlock()
		return
	endif

	int n = OSB_Native.LoadRestore()
	if n <= 0
		Notify("nothing to restore.")
		OSB_Native.Unlock()
		return
	endif
	if AutoBuildAt(akBeacon, n)
		OSB_Native.Unlock()
		return
	endif
	Notify("restoring " + n + " objects...")
	int placed = 0
	int missing = 0
	float lastShown = Utility.GetCurrentRealTime()
	int i = 0
	while i < n
		if Utility.GetCurrentRealTime() - lastShown >= kProgressSeconds
			lastShown = Utility.GetCurrentRealTime()
			Notify("restoring... " + (i * 100 / n) + "% (" + i + " of " + n + ")")
		endif
		Form base = OSB_Native.GetRestoreForm(i)
		float[] t = OSB_Native.GetRestoreXform(i)
		if base && t && t.Length >= 7
			if OSB.PlaceBase(base, t[0], t[1], t[2], t[3], t[4], t[5], t[6])
				placed += 1
			endif
		else
			missing += 1
			Debug.Trace("OSB: restore skipped " + OSB_Native.GetRestoreName(i) + " (not found: plugin missing?)")
		endif
		i += 1
	endwhile
	OSB_Native.Unlock()
	if missing > 0
		Debug.MessageBox("Restored " + placed + " of " + n + " objects.\n\n" + missing + " objects come from mods that are not installed now.")
	else
		Notify("restored " + placed + " of " + n + " objects.")
	endif
EndFunction

Function DoDelete()
	if !PickBackup(false)
		return
	endif
	string name = OSB_Native.GetSelectedName()
	if Msg(kDeleteMsg).Show() != 0
		return
	endif
	if OSB_Native.DeleteBackup()
		Notify("deleted " + name)
	else
		Notify("could not delete that backup.")
	endif
EndFunction
