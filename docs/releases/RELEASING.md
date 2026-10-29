# Windows update releases

For every downloadable update:

1. Bump `port/src/launcher/update_version.h`, including its numeric Windows file
   version, and add a dated section to `CHANGELOG.md` listing new features,
   fixes, validation and known issues.
2. Build the launcher and stage the payload using `stage_inplace_update.py`.
   It copies the runtime, applies the original GUI-subsystem release step,
   verifies that executable code is unchanged and writes matching version/hashes.
3. Build the embedded updater. Run `verify_release_antivirus.py` against those
   exact files before updater execution/upload. Scan failure blocks publishing;
   never disable protection or repackage solely to evade a detection.
4. Run native packaging/logging, clean-update, original-launch and wizard tests
   against the final payload. Record live-game acceptance separately.
5. Publish a version-tagged prerelease with one EXE, SHA256SUMS.txt and concise
   notes copied from the changelog. Keep it a prerelease while acceptance remains
   incomplete. Verify GitHub asset digests match local files.
6. Keep the README current. Retire obsolete installer downloads while preserving
   source history and material incident records. A current local scan does not
   establish that an earlier detection was a false positive.

Do not upload game assets, generated PPC source, saves, proprietary SDK files,
or machine-specific diagnostic records. Do not label unresolved FPS or live
online/gameplay acceptance as fixed.
