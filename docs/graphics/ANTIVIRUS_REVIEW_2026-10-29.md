# Antivirus release review - 2026-10-29

**Historical incident: withdrawn community.2/community.3 installers.** Microsoft Defender detected
`Trojan:Win32/Sabsik.TE.A!ml` in the downloaded standalone updater and reproduced
the detection against its local build. No false-positive determination exists.
The recent standalone downloads are withdrawn. Functional test passes do not
establish malware clearance. Do not restore the sample or disable protection.

## Local evidence

Real-time protection and antivirus were enabled. Defender's definition updater
reported no updates available: signature `1.463.16.0`, engine `1.1.26100.4`.
Custom scans used the installed Microsoft `MpCmdRun.exe` scanner with ordinary
remediation enabled. No application payload was executed during this review.

| Artifact | SHA-256 recorded before review | Local result |
|---|---|---|
| Game payload | `3c9bb59a81522ac20703f5e30f13d434e2ccf56c8ce26dd80eb411f81968fe2b` | Payload custom scan reported no threats |
| Launcher payload | `81a6c86cb21d29b954e727b0eb81adf7686705f1607b717cd4c13320eb3a0726` | Payload custom scan reported no threats |
| SDL3.dll | `2b4b128d8d510cc2d442693cf848c050d700d43dc667afe9e5fd82b0570d667e` | Payload custom scan reported no threats |
| Plain updater, without embedded payload | `b1e2082e9d02994d0a9b2ceee5e1cc14cec52e0c8e7a3c5892ec027978913dbd` | Custom scan reported no threats |
| Published standalone updater | `ee53e98e0b6fda353d524ed56615d7fe924af42b5cd24ea7e58c0ce5a849e040` | Local standalone custom scan reported one threat; access also blocked by real-time protection |

The standalone scan returned HRESULT `0x80508033` and logged "found 1 threats"
and a cleaning failure. Separate Defender threat records reported successful
actions against the downloaded/local file. These observations must not be
interpreted as a completed machine cleanup or proof of safety.

The standalone hash above is the recorded published SHA-256; protected file
access was blocked when attempting to hash it again. Component hashes remained
readable and matched the recorded payload manifest. The official SDL development
archive digest matched GitHub's release-asset digest:
`78d84602ae616cfe26b33a73b7d3b9b6b56e8a6650e53d92c9c6ca938f747acd`.

The source checkout matched its recorded build revision before this review.
Inspection of launcher process creation found the selected original executable
and installed preview launcher paths. This bounded source inspection and the
component scan results do not identify the cause of the standalone detection.

## Requirements for reinstating the flagged installer

1. Obtain a malware-analysis determination for the flagged standalone sample.
   Do not rebuild or alter packaging merely to obtain a different detection.
2. Run `port/tools/verify_release_antivirus.py` against the exact final updater
   and payloads. The gate fails on disabled protection, unreadable/missing files,
   inconclusive scan output, scanner errors, detections or changed file hashes.
3. Only after that gate passes, run updater/shortcut/save-preservation checks
   against those exact artifacts, then verify uploaded checksums.

The historical gate was run locally: four component/plain-updater checks passed, and the
standalone check failed. No replacement binary was uploaded. The gate and this
report are a source-only contribution; the PR remains draft.

Raw scanner logs and machine-specific incident records are local and excluded
from the repository. Independent review, live gameplay/save acceptance and the
previously recorded feature/platform blockers remain outstanding.

## Current release separation — 2026-10-29

The community.4 update uses the newer clean in-place Windows wizard and native
GUI-subsystem runtime packaging. It does not restore or republish the flagged
installer. Its versioned payload and final updater receive their own exact-file
Defender gate and installation regressions; see `CHANGELOG.md` and
`docs/releases/RELEASING.md` for current behavior and remaining acceptance limits.

The cause of the earlier detection remains undetermined. A successful local
scan of the current release is not a false-positive determination for the old
artifact or an independent safety certification. The old downloads remain
retired; this report preserves the original evidence without placing obsolete
installer instructions at the top of the current README or contribution.
