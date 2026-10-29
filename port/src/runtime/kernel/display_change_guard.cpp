#include "display_change_guard.h"
#include "display_settings.h"
#include "sys_ui.h"
#include <chrono>
namespace {
DisplayChangeGuard guard;
bool ownsDisplayBox=false;
void RevertDisplay(){
 const auto old=guard.Revert();
 display_settings::SetRefreshRate(old.refresh);display_settings::SetMode(old.mode);display_settings::SetResolutionIndex(old.resolution);
}
}
void DisplayChangeGuardFrame(){
 const auto now=uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
 guard.Observe({display_settings::GetMode(),display_settings::GetResolutionIndex(),display_settings::GetRefreshRate()},now);
 if(guard.Expired(now)){
  RevertDisplay();
  if(ownsDisplayBox){SysUiDismissBox();ownsDisplayBox=false;}
  return;
 }
 if(!guard.pending||ownsDisplayBox||SysUiBoxBusy())return;
 SysUiMessageBox box;
 box.text="Keep these display settings?\n\nThe previous display mode and resolution\nwill return automatically after 15 seconds\nif you do not confirm.";
 box.buttons={"Keep","Revert"};box.cancel=1;
 box.done=[](int choice){ownsDisplayBox=false;if(!guard.pending)return;if(choice==0)guard.Confirm();else RevertDisplay();};
 ownsDisplayBox=SysUiShowMessageBox(std::move(box));
}
