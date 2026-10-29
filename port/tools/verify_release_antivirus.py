"""Fail closed unless Windows Defender scans the exact release files successfully.

Run BEFORE executing functional updater tests or uploading a release. This never
executes the payload, changes protection, restores quarantine, or submits files.
A passing local scan is evidence for this signature version, not a safety guarantee.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from datetime import datetime, timezone


def fingerprint(path):
    with path.open('rb') as stream:
        return {'sha256': hashlib.file_digest(stream, 'sha256').hexdigest(),
                'bytes': path.stat().st_size}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--updater', type=Path, required=True)
    parser.add_argument('--payload-dir', type=Path, required=True,
                        help='Staged bin folder with game.exe, launcher.exe and SDL3.dll')
    parser.add_argument('--plain-updater', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    report = {'utc': datetime.now(timezone.utc).isoformat(), 'passed': False,
              'files': [], 'limitations': 'Local Defender result only; no external malware determination.'}
    try:
        if os.name != 'nt':
            raise RuntimeError('Windows Defender is required; this check cannot be skipped.')
        status = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command',
            'Get-MpComputerStatus | Select-Object AntivirusEnabled,RealTimeProtectionEnabled,'
            'AntivirusSignatureVersion,AntivirusSignatureLastUpdated,AMEngineVersion | ConvertTo-Json'],
            capture_output=True, text=True, timeout=30, check=True)
        report['defender'] = json.loads(status.stdout)
        if not all(report['defender'].get(key) is True
                   for key in ('AntivirusEnabled', 'RealTimeProtectionEnabled')):
            raise RuntimeError('Antivirus and real-time protection must both be enabled.')
        scanner = Path(os.environ['ProgramData']) / 'Microsoft/Windows Defender/Platform/CurrentVersion/MpCmdRun.exe'
        if not scanner.is_file():
            scanner = Path(os.environ['ProgramFiles']) / 'Windows Defender/MpCmdRun.exe'
        if not scanner.is_file():
            raise RuntimeError('Defender scanner not found.')
        targets = [(name, args.payload_dir / name) for name in ('game.exe', 'launcher.exe', 'SDL3.dll')]
        if args.plain_updater:
            targets.append(('plain-updater.exe', args.plain_updater))
        targets.append(('standalone-updater.exe', args.updater))
        for label, path in targets:
            entry = {'file': label, 'passed': False}
            report['files'].append(entry)
            try:
                before = fingerprint(path)
                entry.update(before)
                result = subprocess.run([str(scanner), '-Scan', '-ScanType', '3',
                    '-File', str(path.resolve()), '-ReturnHR'],
                    capture_output=True, text=True, errors='replace', timeout=300)
                entry['scanner_exit_code'] = result.returncode
                # Console logs can contain personal absolute paths. Keep them
                # local and redact the selected filename in the JSON report.
                entry['scanner_output'] = (result.stdout + result.stderr).replace(str(path.resolve()), label)
                if result.returncode != 0:
                    raise RuntimeError('Defender did not return a successful scan.')
                if 'found no threats.' not in result.stdout.lower():
                    raise RuntimeError('Scan completion was inconclusive; inspect Defender logs.')
                if fingerprint(path) != before:
                    raise RuntimeError('File changed or was removed during scanning.')
                entry['passed'] = True
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                entry['error'] = type(error).__name__ + ': release file could not be read or cleared by Defender'
            print(('PASS ' if entry['passed'] else 'FAIL ') + label, flush=True)
        report['passed'] = len(report['files']) == len(targets) and all(e['passed'] for e in report['files'])
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError, KeyError) as error:
        report['error'] = str(error)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    if not report['passed']:
        print('Release blocked: do not execute or upload these artifacts. Review the local report.', file=sys.stderr)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
