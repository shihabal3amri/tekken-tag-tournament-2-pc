"""Stage the local in-place updater's fixed embedded files; never publish them."""
import argparse
import json
import os
import struct
import re
from pathlib import Path
import shutil
import subprocess
from package_community_update import ROOT, digest, source_identity

README='''TEKKEN TAG TOURNAMENT 2 - LOCAL UPDATE

Double-click the update EXE. Follow Next, Browse to the existing game folder,
Next, Install, Finish. Windows may request administrator permission for Program Files.
The updater does not install or launch the game until you choose Install.

The game is packaged as a native Windows application, matching the original port;
no BAT file or persistent console window is required.
The normal Tekken Tag Tournament 2.exe opens the native launcher. Play Offline
runs the updated runtime. Play Online runs your preserved original executable.
The only program files written into the game folder are the normal entry EXE,
tekken_recomp.exe and SDL3.dll. Existing game assets and shortcut targets stay in place.
No duplicate launcher EXE, original EXE variants, preview-save directories or
update-backup directories are created in the game folder.

Saves and settings stay at %LOCALAPPDATA%/TTT2Recomp. The updater does not replace them.
When this EXE is run from the local G: project folder, recovery/original runtime
files stay under that project's .development/game-update-recovery folder.
A future downloaded release uses %LOCALAPPDATA%/TTT2Recomp/updates instead.
The launcher needs the external original-runtime folder for original online play.

Use the CLEAN original installation, not the older experimental C:/Games copy.
No game assets, original game executable or user saves are included in this EXE.
No GitHub upload has been performed. Physical gameplay, cheats and online-match
acceptance remain pending. The full graphics checklist is not complete.
The existing permanent item-grant action remains preview-only and will refuse
normal saves. Health/money/temporary controls keep their current runtime behavior.
'''


def windows_app(path):
    """Apply the original port's release-only GUI-subsystem packaging step."""
    before=path.read_bytes()
    if before[:2]!=b'MZ':raise ValueError('Payload is not a Windows executable')
    pe=struct.unpack_from('<I',before,0x3c)[0]
    if before[pe:pe+4]!=b'PE\0\0':raise ValueError('Invalid PE header')
    optional=pe+24
    if struct.unpack_from('<H',before,optional)[0]!=0x20b:raise ValueError('Expected x64 PE32+ game')
    subsystem=optional+68
    if struct.unpack_from('<H',before,subsystem)[0]==2:return
    if struct.unpack_from('<H',before,subsystem)[0]!=3:raise ValueError('Unexpected game subsystem')
    editbin=shutil.which('editbin')
    if not editbin:
        candidates=sorted(Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)')).glob('Microsoft Visual Studio/*/BuildTools/VC/Tools/MSVC/*/bin/Hostx64/x64/editbin.exe'))
        if not candidates:raise RuntimeError('Visual Studio editbin is required for native Windows release packaging')
        editbin=str(candidates[-1])
    subprocess.run([editbin,'/nologo','/SUBSYSTEM:WINDOWS',str(path)],check=True,capture_output=True)
    after=path.read_bytes()
    if len(after)!=len(before) or struct.unpack_from('<H',after,subsystem)[0]!=2:raise RuntimeError('GUI packaging failed')
    allowed=set(range(subsystem,subsystem+2))|set(range(optional+64,optional+68))
    if any(a!=b and i not in allowed for i,(a,b) in enumerate(zip(before,after))):
        raise RuntimeError('GUI packaging unexpectedly changed executable code or data')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game',type=Path,required=True)
    parser.add_argument('--launcher',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();folder=args.output.resolve()
    (folder/'bin').mkdir(parents=True,exist_ok=True);(folder/'LICENSES').mkdir(exist_ok=True)
    files={'bin/game.exe':args.game,'bin/launcher.exe':args.launcher,
           'bin/SDL3.dll':args.game.parent/'SDL3.dll',
           'LICENSES/SMAA-LICENSE.txt':ROOT/'port/third_party/smaa/LICENSE.txt',
           'LICENSES/SMAA-README.md':ROOT/'port/third_party/smaa/README.md',
           'LICENSES/SDL-LICENSE.txt':ROOT/'port/third_party/sdl/LICENSE.txt'}
    for name,source in files.items():
        if not source.is_file():raise FileNotFoundError(source)
        shutil.copy2(source,folder/name)
    windows_app(folder/'bin/game.exe')
    sums={name:digest(folder/'bin'/name) for name in ('game.exe','launcher.exe','SDL3.dll')}
    (folder/'payload.sha256').write_text(''.join(value+'  '+name+'\n' for name,value in sums.items()))
    version=re.search(r'#define TTT2_UPDATE_VERSION \"([^\"]+)\"',(ROOT/'port/src/launcher/update_version.h').read_text()).group(1)
    (folder/'README.txt').write_text('Version '+version+' (2026-10-29)\n\n'+README,encoding='utf-8')
    (folder/'build-info.json').write_text(json.dumps({
        'version':re.search(r'#define TTT2_UPDATE_VERSION \"([^\"]+)\"',(ROOT/'port/src/launcher/update_version.h').read_text()).group(1),'installation_mode':'in-place',
        'source_revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        **source_identity(),'payload_sha256':sums,'steam_enabled':False,
        'game_assets_included':False,'user_acceptance_pending':True},indent=2)+'\n')
    print('Staged in-place updater payload; no upload performed.')

if __name__=='__main__':main()
