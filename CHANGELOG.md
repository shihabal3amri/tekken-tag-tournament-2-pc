# Changelog

## 0.1.7-community.4 — 2026-10-29

Unofficial Windows prerelease for an existing USA/Europe TU3 installation.

### Added
- Standard Windows update wizard: Next, Browse, Install and Finish.
- Visible version and feature notes in the updater, version in the launcher,
  and version information in Windows file properties.
- Existing game shortcut opens the native launcher. Play Offline uses the
  updated runtime; Play Online starts the preserved original executable.
- Recovery files and the original online runtime stay outside the game folder.
  Normal saves and settings remain in `%LOCALAPPDATA%/TTT2Recomp`.

### Fixed
- Package the runtime as a Windows GUI application, matching the original
  port. No BAT dependency or developer console window; normal asynchronous
  file logging is restored.
- Avoid duplicate launcher EXEs, preview-save folders and backup folders in
  the selected game installation.
- Verify payload checksums and preserve replaced files for failure rollback.

### Validation and limits
Native release/logging, original-process isolation, clean install/reinstall,
rollback/retry, save/settings preservation and wizard-navigation checks pass.
Exact release files receive a fresh Defender gate before upload.

The reported drop to 39–45 FPS still needs a same-scene gameplay comparison.
No FPS recovery, physical VS/Pair/reconnect, new-build cheat acceptance or online
match is claimed. Permanent item grants remain preview-only; the full graphics
checklist is unfinished. Previous alert history is retained in
[the incident record](docs/graphics/ANTIVIRUS_REVIEW_2026-10-29.md).

Earlier experimental release downloads are retired; their git history remains.
