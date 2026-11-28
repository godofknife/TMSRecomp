#pragma once
// GPU VP9 decoding for the 4K replacement movies (s117, Reddit: "4K movies are very heavy on the CPU"): Windows' VP9
// decoder (Media Foundation, "VP9 Video Extensions") on a D3D11 device, i.e. the GPU's video engine; frames are read
// back as tight NV12. Linux: VA-API (movie_va.cpp). Null / false whenever it is not available: the caller keeps libvpx.
#include <cstdint>
#include <vector>

struct MfVp9;
MfVp9* mfvp9_open(uint32_t w, uint32_t h);  // null: no decoder / no GPU decoding on this PC (logged once)
// one IVF packet in; true and a frame when one came out (w, h: its even size)
bool mfvp9_decode(MfVp9* d, const uint8_t* p, uint32_t n, std::vector<uint8_t>& nv12, uint32_t* w, uint32_t* h);
void mfvp9_close(MfVp9* d);
