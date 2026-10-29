"""Smoke-test the native offline updater without opening a launcher or game.

Requires an existing local TU3 installation; no game data is embedded in the test.
"""
import argparse,hashlib,shutil,subprocess,tempfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def snapshot(folder):return {str(p.relative_to(folder)):digest(p) for p in folder.rglob('*') if p.is_file()}
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package',type=Path,required=True)
    parser.add_argument('--installed-game-dir',type=Path,required=True)
    args=parser.parse_args();install=args.installed_game_dir.resolve()
    image=install/'game/tekken.img';originalImage=digest(image)
    with tempfile.TemporaryDirectory(prefix='update-smoke-',dir=ROOT/'.development') as temp:
        folder=Path(temp).resolve();assert folder.is_relative_to(ROOT)
        package=folder/'package';shutil.copytree(args.package,package)
        source=folder/'source-saves';(source/'player/container').mkdir(parents=True)
        (source/'profiles.ini').write_text('[Profiles]\nLast=0\n')
        (source/'player/container/00_000_1.BIN').write_bytes(b'opaque-save-bytes\x00\x01')
        settings=folder/'original-settings.ini';settings.write_text('[Keyboard]\nStart=0x4E\nA=0x4B\n[Online]\nCustomDelay=4\n[Display]\nAntiAliasing=2\n')
        originalSaves=snapshot(source);originalSettings=digest(settings)
        def run(state,game=install,ok=True,reason=''):
            result=subprocess.run([str(package/'TTT2 Offline Update.exe'),'--prepare-only','--install-dir',str(game),
                '--state-dir',str(state),'--source-settings',str(settings),'--source-saves',str(source)],capture_output=True,text=True,timeout=90)
            if ok and result.returncode:raise AssertionError(result.stderr)
            if not ok and (result.returncode!=1 or reason not in result.stderr):raise AssertionError((result.returncode,result.stderr,reason))
        state=folder/'state';run(state)
        assert snapshot(state/'saves')==originalSaves
        ini=(state/'settings.ini').read_text();assert 'Start=0x4E' in ini and 'CustomDelay=4' in ini and 'AntiAliasing=2' in ini
        assert digest(state/'app/tekken_recomp.exe')==digest(package/'bin/game.exe')
        assert digest(state/'app/game/tekken.img')==originalImage
        assert len(list((state/'app/game').iterdir()))==1,'Full assets must be reused, not copied into the updater'
        (state/'saves/preview-progress.bin').write_bytes(b'keep-new-progress');(state/'settings.ini').write_text(ini+'\n[Custom]\nKeep=1\n')
        run(state);assert (state/'saves/preview-progress.bin').read_bytes()==b'keep-new-progress'
        assert '[Custom]' in (state/'settings.ini').read_text()
        wrong=folder/'wrong-install';(wrong/'game').mkdir(parents=True);(wrong/'game/tekken.img').write_bytes(b'wrong image')
        rejected=folder/'rejected';run(rejected,wrong,ok=False,reason='Title Update 3');assert not rejected.exists()
        checksum=package/'payload.sha256';good=checksum.read_text();checksum.write_text('0'*64+good[64:])
        run(folder/'corrupt',ok=False,reason='checksum mismatch');checksum.write_text(good)
        unrelated=folder/'unrelated';unrelated.mkdir();(unrelated/'keep.txt').write_text('untouched')
        run(unrelated,ok=False,reason='unrelated files');assert (unrelated/'keep.txt').read_text()=='untouched'
        assert snapshot(source)==originalSaves and digest(settings)==originalSettings
        assert digest(image)==originalImage
        print('PASS updater: valid TU3 prepare; copied saves/settings; preserved preview progress on rerun; rejected wrong image, corrupt payload and unrelated destination; original files unchanged; no game launched')

if __name__=='__main__':main()
