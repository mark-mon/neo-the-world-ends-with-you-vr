#include "perf.h"
#include <mutex>
#include "../common/log.h"

namespace uxr::perf {

double NowMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

namespace {
ID3D11Texture2D* g_tex = nullptr;
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
constexpr int kRing = 8;
struct Entry { ID3D11Query *dj = nullptr, *t0 = nullptr, *t1 = nullptr; bool pending = false, open = false; };
Entry g_e[kRing];
int g_w = 0, g_r = 0;
uint64_t g_lastT0 = 0;
bool g_failed = false;
std::mutex g_m;
Gpu g_acc;

bool Ensure() {
    if (g_ctx) return true;
    if (g_failed || !g_tex) return false;
    g_tex->GetDevice(&g_dev);
    g_dev->GetImmediateContext(&g_ctx);
    D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, ts{D3D11_QUERY_TIMESTAMP, 0};
    for (auto& e : g_e) {
        if (FAILED(g_dev->CreateQuery(&dj, &e.dj)) || FAILED(g_dev->CreateQuery(&ts, &e.t0)) || FAILED(g_dev->CreateQuery(&ts, &e.t1))) {
            LOGW("perf: timestamp queries unavailable");
            g_failed = true;
            return false;
        }
    }
    return true;
}

void Poll() {
    while (g_r != g_w && g_e[g_r].pending) {
        Entry& e = g_e[g_r];
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
        uint64_t a = 0, b = 0;
        if (g_ctx->GetData(e.dj, &d, sizeof(d), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
        if (g_ctx->GetData(e.t0, &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
        if (g_ctx->GetData(e.t1, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
        if (!d.Disjoint && d.Frequency) {
            double ms = (double)(b - a) * 1000.0 / (double)d.Frequency;
            std::lock_guard<std::mutex> l(g_m);
            g_acc.stereo.add(ms);
            if (g_lastT0 && a > g_lastT0) g_acc.frame.add((double)(a - g_lastT0) * 1000.0 / (double)d.Frequency);
        }
        g_lastT0 = a;
        e.pending = false;
        g_r = (g_r + 1) % kRing;
    }
}
} // namespace

void SetGameTexture(ID3D11Texture2D* t) { if (!g_tex) g_tex = t; }

void __stdcall GpuEvent(int id) {
    if (!Ensure()) return;
    Poll();
    Entry& e = g_e[g_w];
    if (id == 1) {
        if (e.pending || e.open) return;   // ring full: skip this frame's measurement
        g_ctx->Begin(e.dj);
        g_ctx->End(e.t0);
        e.open = true;
    } else if (id == 2 && e.open) {
        g_ctx->End(e.t1);
        g_ctx->End(e.dj);
        e.open = false;
        e.pending = true;
        g_w = (g_w + 1) % kRing;
    }
}

Gpu TakeGpu() {
    std::lock_guard<std::mutex> l(g_m);
    Gpu g = g_acc;
    g_acc = Gpu{};
    return g;
}

} // namespace uxr::perf
