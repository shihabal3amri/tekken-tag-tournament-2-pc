# Launcher visual redesign — 2026-10-28

Implemented in the native launcher, not a mockup: embedded arena artwork, dark
title bar and controls, red primary action, separate Graphics/Input navigation,
visible focus indicators, native keyboard controls and DPI-aware layout.

Artwork: `port/src/launcher/assets/arena-v1.png`, copied into this workspace and
embedded as an executable resource through `launcher.rc`. No runtime asset
download or browser engine is required. Generated with the built-in image tool.

Generation prompt:

> Use case: stylized-concept. Asset type: background artwork for a native Windows Tekken Tag Tournament 2 game launcher. Create a polished cinematic fighting-game arena background, wide 16:9 composition. An empty underground tournament arena at night, glossy black floor, steel architecture, drifting fine embers and smoke, strong crimson light from the left and electric cool blue light from the right, dramatic depth and refined realistic game key-art rendering. No fighters, no people, no logos, no letters, no UI, no watermark. The left half should be very dark and visually quiet for readable overlaid launcher title and text; the more detailed arena entrance and lights should sit toward the center-right. Premium restrained contrast, charcoal textures with red accents, readable dark negative space. This is original decorative launcher art, not a screenshot.

Verification: standalone native build passed; both pages inspected through Computer
Use. Existing preset values loaded correctly. Saving in an isolated settings file
preserved graphics, online netcode values and keyboard bindings. Production save
regression: 302 creation checks and 48 cross-process reopen checks passed, with
negative controls detecting false writes and unsafe paths. The stale test harness
was updated for current rollback/live declarations; production save code unchanged.

Evidence: `.development/launcher-design-build.log`,
`.development/launcher-qa/settings.ini`, `.development/save-regression.log`.
