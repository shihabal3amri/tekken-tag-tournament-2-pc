"""Native clean updater regression: only disposable installations and user data."""
import argparse, hashlib, os, shutil, subprocess, tempfile
from pathlib import Path
from test_vcmpgtuh import ROOT, compile_one

def digest(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def ident(p):
    h=14695981039346656037
    for c in str(p).lower():h=((h^ord(c))*1099511628211)&((1<<64)-1)
    return f'{h:016x}'
CHECK=r'''
#include "clean_install_paths.h"
#include <cstdio>
int wmain(int argc,wchar_t** argv){
 if(argc!=2)return 1;std::filesystem::path root=argv[1];
 SetEnvironmentVariableW(L"TTT2_SAVE_ROOT",L"wrong-preview-saves");
 if(!clean_install::Configure(root/L"Tekken Tag Tournament 2.exe"))return 2;
 wchar_t value[32768]{};
 if(GetEnvironmentVariableW(L"TTT2_SAVE_ROOT",value,32768))return 3;
 GetEnvironmentVariableW(L"TTT2_SETTINGS_INI",value,32768);
 if(std::filesystem::path(value)!=clean_install::UserData()/L"settings.ini")return 4;
 GetEnvironmentVariableW(L"TTT2_LAUNCHER_INI",value,32768);
 if(!std::filesystem::is_regular_file(value))return 5;
 puts("PASS launcher uses normal settings/saves and external install record");return 0;
}
'''
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--updater',type=Path,required=True);ap.add_argument('--payload',type=Path,required=True);ap.add_argument('--installed-game-dir',type=Path,required=True);a=ap.parse_args()
    exe=a.updater.resolve();payload=a.payload.resolve();image=a.installed_game_dir.resolve()/'game/tekken.img';image_hash=digest(image)
    with tempfile.TemporaryDirectory(prefix='ttt2-clean-') as temp:
        base=Path(temp);local=base/'local';data=local/'TTT2Recomp';(data/'saves/profile').mkdir(parents=True)
        save=data/'saves/profile/progress.bin';save.write_bytes(b'keep player progress');settings=data/'settings.ini';settings.write_text('[Keyboard]\nStart=78\nSeparatePlayer=1\n[Display]\nVSync=1\n')
        before=(digest(save),digest(settings));recovery=base/'project recovery';env={**os.environ,'LOCALAPPDATA':str(local)}
        def game(name,valid=True):
            path=base/name;(path/'game').mkdir(parents=True);(path/'Tekken Tag Tournament 2.exe').write_bytes(b'original online binary')
            (path/'game/keep.bin').write_bytes(b'original asset');(path/'steam_api64.dll').write_bytes(b'original steam runtime')
            if valid:shutil.copy2(image,path/'game/tekken.img')
            else:(path/'game/tekken.img').write_bytes(b'incompatible')
            return path
        def run(path,success=True,where=recovery):
            r=subprocess.run([str(exe),'--install-only','--install-dir',str(path),'--recovery-dir',str(where),'--user-data-dir',str(data)],env=env,capture_output=True,text=True,timeout=90)
            assert r.returncode==(0 if success else 1),r.stdout+r.stderr
            return r
        target=game('existing clean game');entry=target/'Tekken Tag Tournament 2.exe';original=digest(entry);run(target)
        assert digest(entry)==digest(payload/'bin/launcher.exe')
        assert digest(target/'tekken_recomp.exe')==digest(payload/'bin/game.exe')
        assert digest(target/'SDL3.dll')==digest(payload/'bin/SDL3.dll')
        assert {p.name for p in target.iterdir()}=={'game','steam_api64.dll','Tekken Tag Tournament 2.exe','tekken_recomp.exe','SDL3.dll'}
        state=recovery/ident(target);assert digest(state/'original/Tekken Tag Tournament 2.exe')==original
        assert digest(state/'original/steam_api64.dll')==digest(target/'steam_api64.dll')
        assert digest(target/'game/tekken.img')==image_hash and (target/'game/keep.bin').read_bytes()==b'original asset'
        save.write_bytes(b'new user progress');before=(digest(save),digest(settings));run(target)
        assert digest(state/'original/Tekken Tag Tournament 2.exe')==original
        assert (digest(save),digest(settings))==before
        checker=compile_one(base,'check_clean_launch',CHECK,[ROOT/'port/src/launcher'])
        subprocess.run([str(checker),str(target)],check=True,env=env,timeout=15)
        subprocess.run([str(entry),'--check-install'],cwd=target,check=True,env=env,timeout=15)
        failed=game('failed update');(failed/'tekken_recomp.exe').write_bytes(b'previous runtime');(failed/'SDL3.dll').write_bytes(b'previous SDL')
        failed_state=recovery/ident(failed);(failed_state/'build-info.json').mkdir(parents=True)
        run(failed,False)
        assert (failed/'Tekken Tag Tournament 2.exe').read_bytes()==b'original online binary'
        assert (failed/'tekken_recomp.exe').read_bytes()==b'previous runtime' and (failed/'SDL3.dll').read_bytes()==b'previous SDL'
        assert not (failed_state/'install.ini').exists()
        (failed_state/'build-info.json').rmdir();run(failed)
        assert (digest(save),digest(settings))==before
        wrong=game('wrong image',False);assert 'TU3' in run(wrong,False).stderr
        assert not (wrong/'tekken_recomp.exe').exists() and not (recovery/ident(wrong)).exists()
        unsafe=game('unsafe recovery');assert 'outside' in run(unsafe,False,unsafe/'backups').stderr
        assert not (unsafe/'backups').exists()
        assert digest(image)==image_hash
    print('PASS clean layout, payloads, unchanged assets/settings/saves, external originals, reinstall/startup, rollback/retry, incompatible image and unsafe recovery rejection')
if __name__=='__main__':main()
