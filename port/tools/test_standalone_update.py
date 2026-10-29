"""Exercise a standalone offline updater with a local TU3 installation."""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path
from test_vcmpgtuh import compile_one


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--updater", type=Path, required=True)
    parser.add_argument("--installed-game-dir", type=Path, required=True)
    parser.add_argument("--expected-game", type=Path)
    parser.add_argument("--expected-launcher", type=Path)
    args = parser.parse_args()
    install = args.installed_game_dir.resolve()
    image = install / "game/tekken.img"
    original_image = digest(image)
    with tempfile.TemporaryDirectory(prefix="ttt2-standalone-test-") as temporary:
        root = Path(temporary)
        package = root / "download"
        package.mkdir()
        updater = package / "update.exe"
        shutil.copy2(args.updater, updater)
        saves = root / "original-saves"
        saves.mkdir()
        (saves / "opaque.bin").write_bytes(b"original save")
        settings = root / "original.ini"
        settings.write_text("[Keyboard]\nStart=78\n[Online]\nCustomDelay=4\n")
        state = root / "preview"

        def run(game, destination):
            return subprocess.run([str(updater), "--prepare-only", "--install-dir", str(game),
                                   "--state-dir", str(destination), "--source-settings", str(settings),
                                   "--source-saves", str(saves)], capture_output=True, text=True, timeout=120)

        result = run(install, state)
        assert result.returncode == 0, result.stderr
        assert (state / "saves/opaque.bin").read_bytes() == b"original save"
        assert "CustomDelay=4" in (state / "settings.ini").read_text()
        assert digest(state / "app/game/tekken.img") == original_image
        assert (state / "app/tekken_recomp.exe").is_file()
        assert (state / "app/TTT2 Launcher.exe").is_file()
        assert (state / "app/SDL3.dll").is_file()
        assert (state / "app/LICENSES/SDL-LICENSE.txt").is_file()
        info=json.loads((state / "app/build-info.json").read_text())
        assert (state / "app/LICENSES/SMAA-LICENSE.txt").is_file()
        assert (state / "app/README.txt").is_file()
        assert info['payload_sha256']['game.exe']==digest(state / "app/tekken_recomp.exe")
        assert info['payload_sha256']['launcher.exe']==digest(state / "app/TTT2 Launcher.exe")
        if args.expected_game:
            assert digest(args.expected_game)==digest(state / "app/tekken_recomp.exe"),"Embedded game is stale"
        if args.expected_launcher:
            assert digest(args.expected_launcher)==digest(state / "app/TTT2 Launcher.exe"),"Embedded launcher is stale"
        (state / "saves/opaque.bin").write_bytes(b"preview progress")
        assert run(install, state).returncode == 0
        assert (state / "saves/opaque.bin").read_bytes() == b"preview progress"
        # Replacing a preview's accidental hard link must not alter its original.
        sentinel=root / "original-hardlink-sentinel.bin"
        sentinel.write_bytes(b"do not modify the original target")
        (state / "app/tekken_recomp.exe").unlink()
        os.link(sentinel,state / "app/tekken_recomp.exe")
        assert run(install,state).returncode==0
        assert sentinel.read_bytes()==b"do not modify the original target"
        assert digest(state / "app/tekken_recomp.exe")==info['payload_sha256']['game.exe']
        wrong = root / "wrong"
        (wrong / "game").mkdir(parents=True)
        (wrong / "game/tekken.img").write_bytes(b"not TU3")
        result = run(wrong, root / "rejected")
        assert result.returncode == 1 and "Title Update 3" in result.stderr
        assert (saves / "opaque.bin").read_bytes() == b"original save"
        assert digest(image) == original_image
        assert list(package.iterdir()) == [updater], "Updater required external files"
        # The online choice must start the original executable, in its own
        # folder, without inheriting the offline preview overrides.
        mock_source = r'''
#include <windows.h>
#include <cstdio>
#include <cstdlib>
int main() {
    const char* out=std::getenv("TTT2_TEST_MARKER"); if(!out)return 2;
    char cwd[32768]{}; GetCurrentDirectoryA(sizeof(cwd),cwd);
    FILE* f=std::fopen(out,"w"); if(!f)return 3;
    const char* vars[]={"TTT2_SETTINGS_INI","TTT2_SAVE_ROOT","TTT2_VFS_ROOT","TTT2_NET","TTT2_INPUT_SCRIPT","TTT2_AUTOSTART"};
    std::fprintf(f,"%s\n",cwd);
    for(auto v:vars) std::fprintf(f,"%s=%s\n",v,std::getenv(v)?std::getenv(v):"<unset>");
    std::fclose(f);return 0;
}
'''
        mock_install = root / "original-game"
        mock_install.mkdir()
        mock = compile_one(mock_install, "original", mock_source, [])
        marker = root / "original-launched.txt"
        env = dict(os.environ, TTT2_TEST_MARKER=str(marker), TTT2_NET="loopback",
                   TTT2_SETTINGS_INI="preview-settings", TTT2_SAVE_ROOT="preview-saves",
                   TTT2_VFS_ROOT="preview-assets", TTT2_INPUT_SCRIPT="preview-input", TTT2_AUTOSTART="1")
        result = subprocess.run([str(updater), "--launch-original", "--install-dir", str(mock_install),
                                 "--original-exe", str(mock), "--state-dir", str(root / "unused-state")],
                                env=env, capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stderr
        for _ in range(100):
            if marker.exists():
                break
            time.sleep(.05)
        lines = marker.read_text().splitlines()
        assert Path(lines[0]).resolve() == mock_install.resolve()
        assert len(lines) == 7 and all(line.endswith("=<unset>") for line in lines[1:]), lines
        assert not (root / "unused-state").exists()
    print("PASS standalone updater: offline preparation and original launch are isolated; original files unchanged")


if __name__ == "__main__":
    main()
