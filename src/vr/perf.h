// Lightweight performance measurement for the VR path: CPU timers and GPU timestamp queries on the game's D3D11 device.
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <cstdint>

namespace uxr::perf {

double NowMs();   // QPC milliseconds

struct Acc {
    double sum = 0, max = 0;
    uint32_t n = 0;
    void add(double v) { sum += v; if (v > max) max = v; n++; }
    double avg() const { return n ? sum / n : 0; }
    void reset() { *this = Acc{}; }
};

// GPU timing of the stereo renders on the game's device. GpuEvent runs on Unity's render thread (GL.IssuePluginEvent):
// id 1 = before the eye renders, id 2 = after them. Results arrive a few frames later.
void SetGameTexture(ID3D11Texture2D* anyGameTexture);   // gives us the device (main thread, once)
void __stdcall GpuEvent(int id);
struct Gpu { Acc stereo, frame; };   // stereo = both eyes (ms); frame = GPU time between consecutive stereo starts (ms)
Gpu TakeGpu();

} // namespace uxr::perf
