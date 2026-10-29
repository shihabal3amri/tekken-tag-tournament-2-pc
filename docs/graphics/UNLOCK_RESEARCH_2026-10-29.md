# Offline unlock ownership research - 2026-10-29

This is static source analysis of the locally generated guest translation and
the host hooks. No game, test fixture, build, or save edit was run for this
investigation. REA tools and its CLI are absent in this session; the connection
guide was read and no tooling/configuration was installed or changed. These are
ordinary source observations, not REA Evidence or runtime observations.

## Owning routines found

Generated files below are local disc-derived analysis inputs only. They must
remain ignored and must not be added to the contribution or updater.

| Concern | Static observation | Local evidence |
|---|---|---|
| Wardrobe availability | `827163B8` checks a wardrobe word, then falls back to acquired customization ownership via `8272F190`. A true query result does not establish a saved acquisition. | `re/ppc/ppc_recomp.150.cpp`, function `827163B8` |
| Customization ownership | `8272F190` accepts IDs 0..19999 and reads one ownership bit. It neither writes an acquisition nor requests a save. | `re/ppc/ppc_recomp.153.cpp`, function `8272F190` |
| Owning item setter | `8272F060(object,id,acquired,secondary)` bounds-checks IDs 0..19999. Its ordinary (`secondary=0`) path changes the ownership bit, updates an acquisition-related secondary bitset when newly acquired, and sets the object's leading changed byte. Calling it is more faithful than modifying a raw BIN offset. | `re/ppc/ppc_recomp.153.cpp`, function `8272F060` |
| Grant wrapper | `82731510` forwards to `8272F060` with acquired=1 and the caller's secondary flag. The high-level release-item grant routines use secondary=0. | `re/ppc/ppc_recomp.153.cpp`, function `82731510`; `re/ppc/ppc_recomp.53.cpp`, functions `824116E0` and `824117D0` |
| Panel ownership | `8272F538` accepts IDs 0..99 and reads a distinct bitset. `8272F408` is its setter; `82731530` is a grant wrapper. The guest count routine excludes two special panel IDs, so the numeric storage limit is not proof that all IDs are ordinary content. | `re/ppc/ppc_recomp.153.cpp`, functions `8272F408`, `8272F538`, `8272F588`, `82731530` |
| LED/category grant | `827307D8` accepts categories 0..162, walks 63 signed table entries per category, skips negative IDs, and grants each entry through `8272F060`. `827308C8` queries the corresponding category through that same table. | `re/ppc/ppc_recomp.153.cpp`, functions `827307D8` and `827308C8` |
| LED/category removal | `82730850` walks the same table but calls the item setter with acquired=0. Do not mistake the adjacent routine for a grant-all operation. | `re/ppc/ppc_recomp.153.cpp`, function `82730850` |
| Active customization object | `82718A40` returns the manager's customization-object pointer. This identifies the object accessor, but does not prove profile-load completion or safe timing for unsolicited changes. | `re/ppc/ppc_recomp.150.cpp`, function `82718A40` |
| Progression query/setter | `822ADCB0` bounds-checks 773 IDs and decodes a nibble; `822ADC70` writes one nibble while preserving its neighbor. Directly setting every nibble to unlocked could alter unrelated progression semantics. | `re/ppc/ppc_recomp.11.cpp`, functions `822ADC70`, `822ADCB0` |
| Typed release grants | `824117D0` dispatches by release-item type. Types 0/1/2 use item/category/panel grants; type 5 invokes progression setter `822AE358` with state 2. Other types have different routines and layouts. Blindly treating every type as an item ID is incorrect. | `re/ppc/ppc_recomp.53.cpp`, function `824117D0` |
| Existing roster unlock route | `title_server.cpp` already serves level 3. Guest `826DD7E8` checks that level and submits typed release items using `824117D0`. This is an existing normal guest grant route; a second raw roster-bitmask implementation is unnecessary without evidence the route fails. | `port/src/runtime/kernel/title_server.cpp`; `re/ppc/ppc_recomp.141.cpp`, function `826DD7E8` |

## Why a permanent unlock action is still blocked

The storage limit of 20000 item bits is not an asset catalogue. Setting every
possible bit could grant reserved IDs, content unavailable in the installation,
or unsupported release types. The current temporary setting deliberately changes
availability-query returns only; changing it into an irreversible acquisition
operation would silently change its documented semantics.

The complete path from the customization object's changed state through the
profile save scheduler, serialized record, container flush, fresh-process load,
and ownership query has not been established in this investigation. The host's
`save_store.cpp` preserves opaque container files; container-format correctness
alone cannot prove semantic validity of arbitrary guest ownership changes.

No production permanent writes were added. Existing health/money/temporary
unlock behavior, title-storage roster grants, original installation and real
save containers remain untouched by this investigation.

## Concrete remaining acceptance work

1. Trace a legitimate in-game purchase/grant from `824117D0` or `82731510` to the
   save scheduler and serializer. Record both success and failed/cancelled saves.
2. Enumerate valid installed catalogue/release descriptors, including special
   panels, print patterns and non-item types. Preserve licence and asset checks.
3. Add a distinct opt-in permanent action only for the mapped categories, using
   the owning guest routines on the game thread after a copied profile loads.
4. On copied preview saves, grant known locked IDs, request the normal save,
   restart with temporary overrides disabled, and verify the original ownership
   queries return acquired. Compare metadata and unrelated profile fields.
5. Verify disabled and active-session paths perform no acquisition, and prove
   the original installation/save hashes are unchanged. Do this in the final
   validation phase as requested by the user.

Permanent all-content persistence and complete customization coverage are
unresolved. Temporary availability and the existing level-3 roster route must
not be presented as proof that every content category is permanently unlocked.

## Subsequent mapped grant implementation - 2026-10-29

The earlier blocked conclusion above was superseded for three bounded ownership
groups by deeper source tracing. Original bulk routines 8272EEB0, 8272F260 and
8272F738 fill their own bounded item/panel/print-group bitsets while tracking
acquisition deltas. 82411F08 requests the normal full profile save; 82730EF8 and
82731330 serialize main ownership and deltas; 82731090 reloads primary ownership.

persistent_unlocks.cpp adds a separate confirmation-triggered action: validate
marked isolated preview/save paths, loaded schema and save-busy state, finish a
filesystem backup, recheck offline state, call original bulk grants and request
normal saving. PPCContext is restored. Existing temporary query override remains.
The action refuses ordinary original saves and does not add assets or licences.

657 focused checks passed using the real locally extracted bulk/query/serializer/
loader code, including pre-write backup, guard failures and round-trip ownership.
Only the scheduler is mocked. Live save/restart acceptance and every content
category remain pending. Do not describe the scoped action as all-content proof.
