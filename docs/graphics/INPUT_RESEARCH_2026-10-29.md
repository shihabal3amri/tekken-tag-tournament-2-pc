# Analog-only input investigation - 2026-10-29

Status: blocked on source separation at the game's gameplay input conversion.
No new input hook or selectable analog-only setting was added. No builds, tests,
game launches or screen control were performed during this investigation.

Evidence below is inspection of the locally generated PPC translation, not REA
runtime evidence. Generated game code remains ignored and is not redistributed.
The REA MCP tools and CLI were unavailable; no global tooling was installed.

## Mapped path

- `re/ppc/ppc_recomp.237.cpp`, `sub_82AF5190`: samples a guest controller into
  object+72 through `sub_82BFC558`; `sub_82AF5088` copies XInput digital button
  bits from object+76 into the current raw button word at object+28.
- `sub_82AF4AD0` keeps analog sources separate in that word. Left stick uses a
  strict +/-16000 threshold and adds 0x00100000 up, 0x00200000 down,
  0x00400000 left and 0x00800000 right. Digital D-pad remains in bits 0..3.
  This proves source information exists before the later button mapping.
- `re/ppc/ppc_recomp.8.cpp`, `sub_8228E870`: loops over six input channels
  (manager channels 9..14), selects a source with `sub_82A8A178`, gets a
  112-byte destination record with `sub_822917F8`, and calls
  `sub_82291878(destination, source, playerConfiguration)`.
- `sub_82291878` takes a button word from source[0]+32, iterates its 32 bits,
  applies the player's binding configuration through `sub_82373AB8`,
  `sub_82373DE0` and `sub_82373A68`, then updates the destination via
  `sub_82A8B278`. The latter writes current buttons at +32, previous at +44
  and pressed edges at +36. This is a candidate source-selection boundary,
  but the relationship from the raw pad object to source[0] is not yet proven.
- `sub_8228DC40(manager, slot)` maps slot-9 through the manager's index vector
  at +84/+88, selects a 112-byte record via `sub_82291838`, and returns its
  button word at +32.
- `re/ppc/ppc_recomp.18.cpp`, `sub_823038E0`: obtains fighter input from
  channels 9..12 via `sub_822745B0`. Offline that wrapper reads
  `sub_8228DC40`; online it reads the network record instead. The gameplay
  routine applies mode/state filters, writes its result into the gameplay
  FIFO with `sub_8225C1E8`, and reads it with `sub_8225BFC0`.
- `port/src/runtime/kernel/rollback.cpp::sub_822745B0` records/replays this
  boundary. It was not modified. Filtering its returned mask using the latest
  host stick state would mix sampling times and interfere with replay input.

## Why a global D-pad mask is not a safe implementation

The same sampled input serves menus, character selection, pause screens and
gameplay. `g_sharedMenuInput=false` includes both fighting and independent
menus. Clearing XInput bits 0..3 globally would break required D-pad navigation.
Clearing the final gameplay direction bits also discards analog directions
already combined with them, and can affect keyboard input in legacy shared mode.

The existing deadzone and diagonal-ratio filter operate only on physical left
stick axes. They preserve digital directions and keyboard bindings. They do
not establish an independent analog-only combat source.

## Remaining work before enabling the option

1. Trace the construction of manager channels 9..14 to prove how raw digital and
   analog bits reach `source[0]+32`, including each controller order and the
   keyboard/shared-input path.
2. Identify whether those channels are used by character selection or pause
   navigation. Preserve all menu paths, even when a fighter input record exists.
3. Add source selection before per-player binding and FIFO insertion, leaving
   the host snapshot, guest menu channels and rollback recorder unchanged.
4. At final validation, cover opposite analog/D-pad directions, simultaneous
   axes, keyboard directions with a physical pad, both join orders, reconnect,
   pause/system dialogs, character selection, VS and Pair. Use copied saves.

Until that mapping is complete, exposing analog-only as functional would be
misleading. The launcher must retain the supported combined and D-pad-only
options, and the master checklist must keep the full DPD-01 task open.
