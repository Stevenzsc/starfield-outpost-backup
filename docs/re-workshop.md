# Workshop build RE notes (Starfield 1.16.244)

Goal: restore habs the way the game builds them (snapping picks door/wall/roof variants), instead of
`PlaceAtMe`, which spawns a hab's default PackIn (sealed doors, missing walls and roof pieces).

Tools: `tools/xref` (Iced; `refs`, `calls`, `func`, `dis`, `strs`, `vtref`), `tools/id2off.sh`, `tools/off2id.sh`
(Address Library `versionlib-1-16-244-0.bin`: 0x60 header, then `uint32 offset[id]`).

## Found

| What | RVA | ID |
|---|---|---|
| `BSTEventSource<Workshop::ItemPlacedEvent>` getter | `F4F840` | 73963 |
| `BSTEventSource<Workshop::WorkshopItemPlacedEvent>` getter | `F4F7A0` | 73962 |
| `BSTEventSource::RegisterSink` | `22C8E80` | 123821 |
| `Workshop::PackinWorkshopItem` vtable | `4C254A0` | |
| `PackinWorkshopItem` constructor (no args, size >= 0xF0) | `F5B8F0` | 74063 |
| `PackinWorkshopItem::vfunc[0xA0]` = commit/place | `F66220` | 74152 |
| register placed item + send ItemPlacedEvent (via `F73C20`) | `F66EA0` | 74153 |
| WorkshopEngine commit: `this->[+0x268]->vfunc[0xA0](this)`, then WorkshopItemPlaced | `21DE3F0` | 121618 |
| `TCmd<lambda, WorkshopEngine::CommitEvent>` handler | `21BFD30` | 121448 |

Constructor callers: `F5B260`, `F6F140`, `21BE540` (WorkshopEngine, creating the preview item).

Call stack of one in-game build (diagnostic sink, `OutpostBackup\diag.txt` enables it):
`F66EA0 <- F73C20 <- F66220 <- 21DE3F0 <- 21B10B0 <- 21B2360 <- 21BFD30 (CommitEvent) <- UI dispatch`

Engine state in `21DE3F0` (rdi): `+0x268` current PackinWorkshopItem, `+0x270` ref-counted object,
`+0x608` workshop ref, `+0x78` float.

Placing a hab group creates `OutpostGroupPackinDummy` (STAT `0x15804`): the group the game uses to
compute the connected structure (`ExtraCalcedPackIn` 0xD2 / `ExtraSourcePackIn` 0xD4 exist in ExtraDataTypes).

## Next

- How the engine fills a PackinWorkshopItem from a build recipe and moves/snaps the preview
  (`21BE540`, `F5B260`, `F6F140`).
- Whether commit works outside the player's build mode (engine object lifetime), or whether the restore
  must run inside build mode and drive the engine's own commands (select item, move, commit).
