Scriptname OSB_Native Native Hidden
{ Native functions provided by the OutpostBackup SFSE plugin (Data\SFSE\Plugins\OutpostBackup.dll). }

; ---- backup ----
; Start a backup at the current landing zone; false if not on a planet surface.
bool Function BeginBackup(float afBeaconX, float afBeaconY, float afBeaconZ, float afBeaconYaw) global native
; Add one object (plugin + local ID are worked out by the plugin). afPackIn: base is a PackIn.
bool Function AddItem(Form akBase, float afX, float afY, float afZ, float afAX, float afAY, float afAZ, float afScale, bool abPackIn) global native
; Write the backup file; returns the number of objects, or -1 on failure.
int Function EndBackup() global native
bool Function HasBackup() global native

; ---- landing ----
; The next star-map landing goes to the backup's landing-zone center. False if there is no backup.
bool Function ArmLanding() global native
; Old beacon spot in the current landing zone: [x, y, z, distance between landing centers (m)].
; Empty if there is no backup or this is another planet.
float[] Function GetGuideSpot() global native

; ---- restore ----
; Load the backup for restoring; returns the number of objects to place (-1: no backup).
int Function LoadRestore() global native
Form Function GetRestoreForm(int aiIndex) global native
; [x, y, z, ax, ay, az, scale]
float[] Function GetRestoreXform(int aiIndex) global native
string Function GetRestoreName(int aiIndex) global native
