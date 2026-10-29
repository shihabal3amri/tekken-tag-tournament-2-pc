#include "host_settings.h"
#include <chrono>
#include "cheat_overlay.h"
#include "cheat_overlay_policy.h"
#include "sys_ui.h"
#include "display_settings.h"
#include "offline_cheats.h"
#include "persistent_unlocks.h"
#include <cstdlib>
#include <string>
#include <atomic>
#include <cstdint>
bool XSessionAnyActive();
namespace cheat_overlay {
namespace {
uint64_t OverlayNowMs(){return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
CheatOverlayChord chord;
bool owned = false;
bool reopen = false;
bool calibrating = false, reopenCalibration = false;
bool reopenPermanent = false;
uint64_t calibrationUntil = 0;
int calibrationChoice = 0;
uint64_t dialogToken = 0;
std::string lastFrame;
std::atomic<bool> offlineGate{false};
bool Offline() { return CheatOverlayAllowed(lastFrame.rfind("FrameOnline",0)==0,XSessionAnyActive()); }
bool CalibrationAllowed() {
    return Offline() && (lastFrame=="FrameMainMenu" || lastFrame.rfind("FrameMode007-",0)==0);
}
void Open();
void OpenPermanent() {
    if (!CalibrationAllowed() || SysUiBoxBusy()) return;
    SysUiMessageBox box;
    box.text="SAVE ITEM UNLOCKS\nPreview saves only; backup made first.\nItems, panels and print ownership.\nMissing assets are not supplied.";
    box.buttons={"Save unlocks","Cancel"};box.cancel=1;
    const uint64_t token=++dialogToken;
    box.done=[token](int choice){
        if(token!=dialogToken)return;
        ++dialogToken;owned=false;
        if(!Enabled() || !CalibrationAllowed())return;
        if(choice==0)persistent_unlocks::QueuePermanentUnlocks();
        else reopen=true;
    };
    owned=SysUiShowMessageBox(std::move(box));
}
void OpenCalibration() {
    if (!CalibrationAllowed() || SysUiBoxBusy()) return;
    SysUiMessageBox box;
    // The guest information window is about 44 characters wide. Keep its
    // header short and put current values on the adjustment rows.
    box.text = "SDR: black left / white right / gray top\nA adjusts; B returns. Auto-close: 60s.";
    const std::string brightness=std::to_string(display_settings::GetBrightness())+"%";
    const std::string gamma=std::to_string(display_settings::GetGamma())+"%";
    const std::string contrast=std::to_string(display_settings::GetContrast())+"%";
    box.buttons = {"Brightness -5 ("+brightness+")","Brightness +5 ("+brightness+")",
                   "Gamma -5 ("+gamma+")","Gamma +5 ("+gamma+")",
                   "Contrast -5 ("+contrast+")","Contrast +5 ("+contrast+")","Reset to 100%","Back"};
    box.cancel = 7;
    box.active = calibrationChoice;
    const uint64_t token=++dialogToken;
    box.done = [token](int choice) {
        if (token!=dialogToken) return;
        ++dialogToken; // A callback may answer its own dialog only once.
        owned=false;
        if (!calibrating || !Enabled() || !CalibrationAllowed() || OverlayNowMs()>=calibrationUntil) {
            calibrating=false;display_settings::SetCalibrationPattern(false);return;
        }
        if (choice<0 || choice>=7) {
            calibrating=false;display_settings::SetCalibrationPattern(false);reopen=true;return;
        }
        calibrationChoice=choice;
        if (choice<2) display_settings::SetBrightness(display_settings::GetBrightness()+(choice==0?-5:5));
        else if (choice<4) display_settings::SetGamma(display_settings::GetGamma()+(choice==2?-5:5));
        else if (choice<6) display_settings::SetContrast(display_settings::GetContrast()+(choice==4?-5:5));
        else {
            display_settings::SetBrightness(100);display_settings::SetGamma(100);display_settings::SetContrast(100);
        }
        reopenCalibration=true;
    };
    if (SysUiShowMessageBox(std::move(box))) {
        owned=true;calibrating=true;calibrationUntil=OverlayNowMs()+60000;
        display_settings::SetCalibrationPattern(true);
    }
}
void Open() {
    if (!Offline() || SysUiBoxBusy()) return;
    SysUiMessageBox box;
    box.text = "OFFLINE CHEATS\nUP/DOWN: select | A: change | B: close\nInstalled content only; offline use.";
    static const char* healthNames[]={"Off","Player 1","Player 2","Both sides"};
    box.buttons = {
        std::string("Infinite health: ")+healthNames[offline_cheats::HealthSides()],
        std::string("Infinite money: ")+(offline_cheats::InfiniteMoney()?"ON":"OFF"),
        std::string("Unlock content: ")+(offline_cheats::UnlockContent()?"ON":"OFF"),
        CalibrationAllowed()?"SDR calibration":"SDR calibration (main/options menu only)",
        "Save item unlocks (preview only)",
        "Close"
    };
    box.cancel = 5;
    const uint64_t token=++dialogToken;
    box.done = [token](int choice) {
        if (token!=dialogToken) return;
        ++dialogToken;
        owned = false;
        if (!Enabled() || !Offline()) return;
        if (choice == 0) offline_cheats::SetHealthSides((offline_cheats::HealthSides()+1)%4);
        if (choice == 1) offline_cheats::SetInfiniteMoney(!offline_cheats::InfiniteMoney());
        if (choice == 2) offline_cheats::SetUnlockContent(!offline_cheats::UnlockContent());
        if (choice == 3) {
            if (CalibrationAllowed()) {calibrationChoice=0;reopenCalibration=true;}
        }
        if (choice == 4 && CalibrationAllowed()) reopenPermanent=true;
        reopen = (choice >=0 && choice < 3) || ((choice==3 || choice==4) && !CalibrationAllowed());
    };
    owned = SysUiShowMessageBox(std::move(box));
}
}
bool Enabled() {
    static const bool enabled = [] {
        return host_settings::ReadInt("Mods","Overlay",0)==1;
    }();
    return enabled;
}
bool ConsumeChord() { return Enabled() && offlineGate.load(); }
void Frame(const std::string& currentFrame) {
    lastFrame=currentFrame;
    offlineGate.store(Offline());
    offline_cheats::SetOfflineMenu(Offline());
    const bool pressed=chord.Update(HostCheatOverlayChord());
    // Leave reference panels only while their owned dialog is in an allowed
    // front-end menu. Timeout also restores the image after abandoned dialogs.
    if (calibrating && (!Enabled() || !CalibrationAllowed() || OverlayNowMs()>=calibrationUntil ||
                        (!SysUiBoxBusy() && !reopenCalibration))) {
        ++dialogToken;
        calibrating=false;reopenCalibration=false;display_settings::SetCalibrationPattern(false);
        if (owned) {SysUiDismissBox();owned=false;}
    }
    if (!Enabled()) return;
    if (!Offline()) {
        ++dialogToken;
        reopen=false;reopenCalibration=false;reopenPermanent=false;
        if (owned) { SysUiDismissBox();owned=false; }
        return;
    }
    if(reopenPermanent && !SysUiBoxBusy()){reopenPermanent=false;OpenPermanent();}
    if (reopenCalibration && !SysUiBoxBusy()) {reopenCalibration=false;OpenCalibration();}
    if (reopen && !SysUiBoxBusy()) { reopen=false;Open(); }
    if (pressed && !SysUiBoxBusy()) Open();
}
}
