// Writes the code generator's inputs (BUILDING.md step 1) from the disc image and Title Update 3,
// with the same parsers and checks as the installer (port/src/setup):
//   <re>/Default.xex                                   base executable from the ISO (SHA-1 checked)
//   <re>/extracted_tu/<TU file name>/Default.xexp      TU3 delta patch (version + SHA-1 checked)
//   <re>/decrypted/Default_patched_tu3.bin             base + TU3 image = the runtime's tekken.img (SHA-1 checked)
#include "setup/xdvdfs.h"
#include "setup/stfs.h"
#include "setup/game_data.h"
#include <cstdio>
#include <fstream>

namespace fs = std::filesystem;
using namespace ttt2::setup;

static bool Write(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    out.close();
    if (!out) { fprintf(stderr, "cannot write %s\n", path.c_str()); return false; }
    printf("%s  %s\n", Sha1Hex(bytes.data(), bytes.size()).c_str(), path.c_str());
    return true;
}

int main(int argc, char** argv) {
    if (argc != 4) { fprintf(stderr, "Usage: ttt2_inputs disc.iso TU3_package re_dir\n"); return 2; }
    std::string error;
    XdvdfsImage iso;
    if (!iso.Open(argv[1], error)) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
    const IsoEntry* entry = iso.Find("Default.xex");
    std::vector<uint8_t> xex, patch, image;
    if (!entry || !iso.ReadEntry(*entry, xex)) { fprintf(stderr, "no readable Default.xex in the ISO\n"); return 1; }
    if (Sha1Hex(xex.data(), xex.size()) != kBaseXexSha1) { fprintf(stderr, "Default.xex is not the supported base (0.0.0.2)\n"); return 1; }
    StfsPackage tu;
    if (!tu.Open(argv[2], error)) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
    if (tu.TitleId() != kTitleId || tu.MediaId() != kMediaId || !tu.ReadFile("Default.xexp", patch) ||
        XexpTargetVersion(patch) != "0.0.3.2") { fprintf(stderr, "not Title Update 3 for this disc\n"); return 1; }
    if (!BuildGameImage(xex, patch, image, error)) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
    const fs::path re = argv[3];
    const std::string tuName = fs::path(argv[2]).filename().string();
    bool ok = Write(re / "Default.xex", xex) &&
              Write(re / "extracted_tu" / tuName / "Default.xexp", patch) &&
              Write(re / "decrypted" / "Default_patched_tu3.bin", image);
    return ok ? 0 : 1;
}
