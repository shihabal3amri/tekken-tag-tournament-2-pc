#pragma once
#include <cstdint>
struct DisplayChangeGuard {
    struct Choice { int mode=0, resolution=0, refresh=0; bool operator==(const Choice&) const = default; };
    Choice accepted{}, staged{};
    bool initialized=false, pending=false;
    uint64_t deadline=0;
    bool Observe(Choice current,uint64_t now) {
        if(!initialized){accepted=staged=current;initialized=true;return false;}
        if(current==staged)return false;
        staged=current;pending=!(current==accepted);deadline=now+15000;
        return pending;
    }
    void Confirm(){accepted=staged;pending=false;}
    Choice Revert(){staged=accepted;pending=false;return accepted;}
    bool Expired(uint64_t now) const{return pending&&now>=deadline;}
};
