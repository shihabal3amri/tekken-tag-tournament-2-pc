// Options -> Graphics Settings -> WEATHER EFFECTS (display_settings.h). The stages' particle effects (the rain of
// Tempest and Paris: VERIFIED gone with OFF; Arctic Dream's snow not told apart from its glitter in shots) are
// StageParticle emitters; each frame the game queues one update job per emitter to its StageParticleMT workers
// (guest threads 82A74760>823D94B8) and waits for them. sub_823E6BC8 is that job: +16 the emitter (sub_823E82A8
// updates it), +20 the done flag the queueing thread waits on. OFF marks the job done without running it, so the
// emitters spawn and draw nothing.
// MEASURED 2026-10-31 (4 threads): the jobs cost 0.65 ms of worker time a frame on Riverside Promenade, 0.75 ms on
// Tempest, ~0.1 ms on Tulip Festival; OFF removes the rain cleanly (screenshots). The stage files' own switches
// (StageParticle disp_flag / calc_flag, read by sub_823E0B30 / sub_823E0E90) do NOT stop the jobs. Arctic Dream's
// deformable snow ground is another job (sub_82521E08, ~1.3 ms) and stays: without it the snow looks like flat ice.
// The investigation's test switches (stage-file keys, the crowd, the snow ground): re/diag_2026-10-31/stage_fx_test.cpp.
#include "kernel_shared.h"
#include "display_settings.h"

// The emitter sets pick one of three job kinds (sub_823E6A30 from the set's +816, MEASURED 2026-11-01): 0 (sub_823E6B38,
// updates through sub_823E41A8), 1 (sub_823E6B80, sub_823E53F0) and 2 (sub_823E6BC8, sub_823E82A8), each the same
// layout. Kind 2 is the rain's; Fallen Garden's emitters are kind 1 (its workers' 2.84 ms a frame with WEATHER EFFECTS
// OFF in log 47, against 0.17 on Paris), so OFF skips all three.
// Each body runs one of two things by the work's +552 (the job's +16): 1, the frame's update (sub_823E4000 /
// sub_823E5248 / sub_823E80A0: moves the particles and emits their quads, sub_823E6530 - what is drawn), else the
// stage load's warm-up (sub_823E3908 / sub_823E48D8 / sub_823E78E0: numbers and spawns every particle, no quads). OFF
// skips only the updates: an emitter set loaded with OFF was otherwise never set up, and WEATHER EFFECTS turned ON
// afterwards (the main menu's backdrop, where the option is) updated particle arrays holding whatever the heap held
// (review 2026-11-01).
extern "C" PPC_FUNC(__imp__sub_823E6B38);
extern "C" PPC_FUNC(__imp__sub_823E6B80);
extern "C" PPC_FUNC(__imp__sub_823E6BC8);
static bool SkipUpdate(uint8_t* base, uint32_t job)
{
    if (display_settings::GetWeather()) return false;
    const uint32_t work = GRead32(base, job + 16);
    return work && GRead32(base, work + 552) == 1;
}
PPC_FUNC(sub_823E6B38)
{
    if (SkipUpdate(base, ctx.r3.u32)) { GWrite32(base, ctx.r3.u32 + 20, 1); return; }
    __imp__sub_823E6B38(ctx, base);
}
PPC_FUNC(sub_823E6B80)
{
    if (SkipUpdate(base, ctx.r3.u32)) { GWrite32(base, ctx.r3.u32 + 20, 1); return; }
    __imp__sub_823E6B80(ctx, base);
}
PPC_FUNC(sub_823E6BC8)
{
    if (SkipUpdate(base, ctx.r3.u32)) { GWrite32(base, ctx.r3.u32 + 20, 1); return; }
    __imp__sub_823E6BC8(ctx, base);
}
