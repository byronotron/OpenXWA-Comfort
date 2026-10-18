# Basis Universal transcoder (subset)

From https://github.com/BinomialLLC/basis_universal, tag v2_50 (Apache 2.0,
see LICENSE): the `transcoder/` folder, unchanged.

OpenXWA uses it to read the KTX2 (Basis Universal, Zstandard
supercompressed) textures in the headset runtime's VR controller models
(Meta's Touch Plus models carry the button letters and their shading that
way), decoded to RGBA at load (`src/xwa_remaster/vr_ktx2_decode.cpp`).

- `zstd/zstd.h` is ours: it forwards to the zstd the engine links instead of
  basisu's bundled single-file zstd.
- Built as `xwa_basisu` (CMakeLists.txt) with the GPU-format targets the
  game does not use switched off (DXT1, DXT5A, PVRTC1/2, ETC2 EAC A8/RG11,
  ATC, FXT1); only RGBA output is used.
