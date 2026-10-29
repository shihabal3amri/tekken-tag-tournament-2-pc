#include "host_input_sdl.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"FAIL line %d: %s (%s)\n",__LINE__,#x,SDL_GetError());return 1;} } while(0)
int main() {
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    CHECK(host_input::Start());
    host_input::SetFocused(true);
    SDL_Event key{};key.type=SDL_EVENT_KEY_DOWN;key.key.key=SDLK_N;
    host_input::Event(key);CHECK(host_input::Read().keys['N']);
    host_input::SetFocused(false);CHECK(!host_input::Read().keys['N']);
    host_input::Event(key);CHECK(!host_input::Read().keys['N']);
    host_input::SetFocused(true);key.type=SDL_EVENT_KEY_UP;host_input::Event(key);
    CHECK(!host_input::Read().keys['N']);
    host_input::Pad pad;
    host_input::ApplyGamepadAxis(pad,SDL_GAMEPAD_AXIS_LEFTY,-32768);
    CHECK(pad.leftY==32767);
    host_input::ApplyGamepadButton(pad,SDL_GAMEPAD_BUTTON_START,true);CHECK(pad.buttons==0x10);
    host_input::ApplyGamepadButton(pad,SDL_GAMEPAD_BUTTON_START,false);CHECK(!pad.buttons);
    const char* script=std::getenv("TTT2_SDL_TEST_PAD_SCRIPT");
    if(script && *script) {
        const uint16_t expected=std::strstr(script,"START")?0x10:0x1000;
        bool sawPress=false,sawRelease=false;
        for(unsigned i=0;i<150;++i) {
            host_input::Tick();SDL_Event event{};
            while(SDL_PollEvent(&event))host_input::Event(event);
            for(const auto& p:host_input::Read().pads)if(p.connected) {
                if(p.buttons&expected)sawPress=true;
                else if(sawPress)sawRelease=true;
            }
            SDL_Delay(2);
        }
        CHECK(sawPress && sawRelease);
    }
    host_input::Stop();SDL_Quit();
    std::puts("PASS SDL keyboard/focus, axis/button mapping and requested virtual-pad press/release");
}
