# SMAA 1x vendor source

Upstream: https://github.com/iryoku/smaa
Pinned commit: 71c806a838bdd7d517df19192a20f0c61b3ca29d
License: LICENSE.txt (MIT, including upstream binary-notice clarification).

AreaTex.h and SearchTex.h are copied unchanged from upstream Textures/.
shader_source.h embeds upstream SMAA.hlsl as a C++ raw string for standalone binary
packaging. The only functional upstream edits are explicit s0/s1 register bindings
on LinearSampler/PointSampler, matching the renderer's D3D11 sampler bindings.
Original copyright notices and shader integration instructions are retained.

Integration: port/src/runtime/kernel/smaa_d3d11.h implements SMAA 1x HIGH using
luma-edge, blend-weight and neighborhood passes and the upstream lookup textures.
No temporal jitter, motion vectors, frame generation or simulation changes.
The current presentation chain filters display-encoded color (gamma-space blending),
a permitted upstream fallback rather than a new HDR/linear-light pipeline.

Regression: python port/tools/test_smaa_d3d11.py runs the production passes on WARP
against flat-color and diagonal-edge fixtures, alpha, repeated frames and resize.

Vendored source formatting normalizes trailing whitespace; lookup values and shader algorithms are unchanged.
