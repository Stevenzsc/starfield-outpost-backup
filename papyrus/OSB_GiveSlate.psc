Scriptname OSB_GiveSlate extends Quest
{ Start-game-enabled, run-once quest: gives the player the Outpost Backup slate the first time the mod loads. }

Book Property OSB_SlateItem Auto Const Mandatory

Event OnQuestInit()
	Actor player = Game.GetPlayer()
	if player.GetItemCount(OSB_SlateItem) == 0
		player.AddItem(OSB_SlateItem, 1)
	endif
	Debug.Notification("Outpost Backup: slate added to your inventory (Notes / Slates).")
EndEvent
