Scriptname OSB_Native Native Hidden
{ Native functions provided by the OutpostBackup SFSE plugin (Data\SFSE\Plugins\OutpostBackup.dll). }

; Plugin version (0 if the plugin is not loaded: the call fails and returns the default).
int Function Version() global native

; Only one slate action at a time. A lock left behind expires after 3 minutes.
bool Function TryLock() global native
Function Unlock() global native

; ---- backup ----
; Start a backup at the current landing zone; false if not on a planet surface. akBeacon gives the outpost name.
bool Function BeginBackup(ObjectReference akBeacon, float afBeaconX, float afBeaconY, float afBeaconZ, float afBeaconYaw) global native
; Add one object (plugin + local ID are worked out by the plugin). afPackIn: base is a PackIn.
bool Function AddItem(Form akBase, float afX, float afY, float afZ, float afAX, float afAY, float afAZ, float afScale, bool abPackIn) global native
; Write the backup file; returns the number of objects, 0 if there was nothing to save, -1 on failure.
; Backing up the same outpost again replaces its backup (LastBackupReplaced is then true).
int Function EndBackup() global native
bool Function LastBackupReplaced() global native
bool Function HasBackup() global native

; ---- automatic flow ----
; Start a new land-and-restore flow and return its id; a flow stops when CurrentFlow() no longer matches.
int Function NewFlow() global native
int Function CurrentFlow() global native
; Select the only saved outpost on this planet if there is exactly one. Returns how many are on this planet.
int Function SelectOnlyHere() global native

; ---- choosing a saved outpost ----
; List saved outposts, newest first. abHereOnly: only the planet you are on. Returns the count.
int Function ListBackups(bool abHereOnly) global native
; Put page aiPage of the list into OSB_PickMsg's buttons. Returns the number of outposts on the page.
int Function FillPicker(Message akPickMsg, int aiPage) global native
; Turn the button OSB_PickMsg.Show() returned into: list index, -1 cancel, -2 next page.
int Function PickerResult(int aiButton) global native
bool Function SelectBackup(int aiIndex) global native
string Function GetSelectedName() global native
string Function GetSelectedPlanetName() global native
; Move the selected backup to OutpostBackup\Deleted.
bool Function DeleteBackup() global native

; ---- landing ----
; The next star-map landing on the selected outpost's planet goes to its landing-zone center.
bool Function ArmLanding() global native
; Selected outpost's beacon spot in the current landing zone: [x, y, z, distance between landing centers (m)].
; Empty if nothing is selected or this is another planet.
float[] Function GetGuideSpot() global native

; ---- restore ----
; Load the selected backup for restoring; returns the number of objects to place (-1: none selected or another planet).
int Function LoadRestore() global native
Form Function GetRestoreForm(int aiIndex) global native
; [x, y, z, ax, ay, az, scale]
float[] Function GetRestoreXform(int aiIndex) global native
string Function GetRestoreName(int aiIndex) global native
