# meshoptimizer (subset)

From https://github.com/zeux/meshoptimizer, tag v0.21 (MIT, see LICENSE.md):
`src/meshoptimizer.h`, `src/simplifier.cpp`, `src/allocator.cpp`,
`src/vcacheoptimizer.cpp`, `src/vfetchoptimizer.cpp`, unchanged.

OpenXWA uses the simplifier at run time to make light cut-out meshes of
the headset runtime's VR controller models (src/xwa_remaster/vr_controllers.c;
the `xwa_meshopt` library builds only the simplifier and the allocator).
tools/vr/decimate_glove_mesh.cpp uses all five offline for the glove meshes
(`decimate_glove_mesh 0.35` reproduces src/xwa_remaster/vr_glove_mesh.h).
