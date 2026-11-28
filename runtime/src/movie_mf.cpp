// GPU VP9 decoding (movie_mf.h). Windows; Linux = movie_va.cpp (VA-API); elsewhere mfvp9_open returns null.
#include "movie_mf.h"
#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <cstring>
#include <memory>
#include <mutex>
#ifndef MFVP9_TEST
#include "rt.h"
#define MLOG rt::log
#else
#include <cstdio>
#define MLOG(...) (printf(__VA_ARGS__), printf("\n"))
#endif

template <class T> static void release(T*& p) { if (p) p->Release(), p = nullptr; }

struct MfVp9 {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IMFDXGIDeviceManager* mgr = nullptr;
    IMFTransform* mft = nullptr;
    ID3D11Texture2D* staging = nullptr;
    uint32_t sw = 0, sh = 0;  // staging size
    bool provides = false;    // the decoder allocates its output samples (D3D mode)
    int64_t t = 0;
    ~MfVp9() { release(staging); release(mft); release(mgr); release(ctx); release(dev); }
};

static bool set_output(MfVp9& d) {
    for (DWORD i = 0;; i++) {
        IMFMediaType* t = nullptr;
        if (FAILED(d.mft->GetOutputAvailableType(0, i, &t))) return false;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        const bool ok = sub == MFVideoFormat_NV12 && SUCCEEDED(d.mft->SetOutputType(0, t, 0));
        t->Release();
        if (ok) break;
    }
    MFT_OUTPUT_STREAM_INFO si{};
    d.mft->GetOutputStreamInfo(0, &si);
    d.provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    return true;
}

MfVp9* mfvp9_open(uint32_t w, uint32_t h) {
    static std::once_flag once;
    static bool mf_ok = false;
    std::call_once(once, [] { mf_ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); });
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // per thread; S_FALSE / RPC_E_CHANGED_MODE are fine
    static bool told = false;
    auto fail = [&](const char* why, HRESULT hr) -> MfVp9* {
        if (!told) told = true, MLOG("movie: GPU VP9 decoding not available (%s, %08lx): 4K movies decode on the CPU", why, (unsigned long)hr);
        return nullptr;
    };
    if (!mf_ok) return fail("Media Foundation", 0);
    auto d = new MfVp9();
    std::unique_ptr<MfVp9> guard(d);
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &d->dev, nullptr, &d->ctx);
    if (FAILED(hr)) return fail("D3D11 device", hr);
    if (ID3D10Multithread* mt = nullptr; SUCCEEDED(d->dev->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt)))
        mt->SetMultithreadProtected(TRUE), mt->Release();
    UINT token = 0;
    if (FAILED(hr = MFCreateDXGIDeviceManager(&token, &d->mgr)) || FAILED(hr = d->mgr->ResetDevice(d->dev, token)))
        return fail("DXGI device manager", hr);
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_VP90}, out{MFMediaType_Video, MFVideoFormat_NV12};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                   &in, &out, &acts, &n);
    if (FAILED(hr) || !n) return fail("no VP9 decoder: install VP9 Video Extensions from the Microsoft Store", hr);
    hr = acts[0]->ActivateObject(IID_PPV_ARGS(&d->mft));
    for (UINT32 i = 0; i < n; i++) acts[i]->Release();
    CoTaskMemFree(acts);
    if (FAILED(hr)) return fail("decoder activation", hr);
    UINT32 aware = 0;
    if (IMFAttributes* a = nullptr; SUCCEEDED(d->mft->GetAttributes(&a))) {
        a->GetUINT32(MF_SA_D3D11_AWARE, &aware);
        a->SetUINT32(MF_LOW_LATENCY, TRUE);
        a->Release();
    }
    if (!aware || FAILED(hr = d->mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)d->mgr)))
        return fail("decoder without D3D11 (GPU) support", hr);
    IMFMediaType* t = nullptr;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_VP90);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, w, h);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = d->mft->SetInputType(0, t, 0);
    t->Release();
    if (FAILED(hr)) return fail("input type", hr);
    if (!set_output(*d)) return fail("NV12 output", 0);
    d->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    static bool said = false;
    if (!said) {
        said = true;
        DXGI_ADAPTER_DESC ad{};
        if (IDXGIDevice* xd = nullptr; SUCCEEDED(d->dev->QueryInterface(IID_PPV_ARGS(&xd)))) {
            if (IDXGIAdapter* xa = nullptr; SUCCEEDED(xd->GetAdapter(&xa))) xa->GetDesc(&ad), xa->Release();
            xd->Release();
        }
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, nullptr, nullptr);
        MLOG("movie: 4K movies decode on the GPU (%s)", name);
    }
    return guard.release();
}

// the decoder's frame (a D3D11 texture array slice) -> our staging texture -> tight NV12
static bool read_frame(MfVp9& d, IMFSample* s, std::vector<uint8_t>& o, uint32_t* w, uint32_t* h) {
    IMFMediaBuffer* b = nullptr;
    if (FAILED(s->GetBufferByIndex(0, &b))) return false;
    IMFDXGIBuffer* db = nullptr;
    ID3D11Texture2D* tex = nullptr;
    UINT sub = 0;
    bool ok = SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&db))) && SUCCEEDED(db->GetResource(IID_PPV_ARGS(&tex))) &&
              SUCCEEDED(db->GetSubresourceIndex(&sub));
    if (ok) {
        IMFMediaType* t = nullptr;
        UINT32 fw = 0, fh = 0;
        if (SUCCEEDED(d.mft->GetOutputCurrentType(0, &t))) MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &fw, &fh), t->Release();
        fw &= ~1u, fh &= ~1u;
        if (!d.staging || d.sw != fw || d.sh != fh) {
            release(d.staging);
            D3D11_TEXTURE2D_DESC sd{fw, fh, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
            ok = SUCCEEDED(d.dev->CreateTexture2D(&sd, nullptr, &d.staging)), d.sw = fw, d.sh = fh;
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        if (ok) {
            D3D11_BOX box{0, 0, 0, fw, fh, 1};
            d.ctx->CopySubresourceRegion(d.staging, 0, 0, 0, 0, tex, sub, &box);
            ok = SUCCEEDED(d.ctx->Map(d.staging, 0, D3D11_MAP_READ, 0, &m));
        }
        if (ok) {
            o.resize((size_t)fw * fh * 3 / 2);
            const uint8_t* src = (const uint8_t*)m.pData;
            for (uint32_t y = 0; y < fh; y++) memcpy(&o[(size_t)y * fw], src + (size_t)y * m.RowPitch, fw);
            const uint8_t* uv = src + (size_t)m.RowPitch * fh;
            for (uint32_t y = 0; y < fh / 2; y++) memcpy(&o[(size_t)fw * fh + (size_t)y * fw], uv + (size_t)y * m.RowPitch, fw);
            d.ctx->Unmap(d.staging, 0);
            *w = fw, *h = fh;
        }
    }
    release(tex), release(db), release(b);
    return ok;
}

bool mfvp9_decode(MfVp9* d, const uint8_t* p, uint32_t n, std::vector<uint8_t>& nv12, uint32_t* w, uint32_t* h) {
    thread_local const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || true;  // the decoding thread
    (void)com;
    IMFMediaBuffer* b = nullptr;
    IMFSample* s = nullptr;
    if (FAILED(MFCreateMemoryBuffer(n, &b))) return false;
    BYTE* dst = nullptr;
    b->Lock(&dst, nullptr, nullptr), memcpy(dst, p, n), b->Unlock(), b->SetCurrentLength(n);
    MFCreateSample(&s);
    s->AddBuffer(b), s->SetSampleTime(d->t), s->SetSampleDuration(333333), d->t += 333333;
    HRESULT hr = d->mft->ProcessInput(0, s, 0);
    release(s), release(b);
    if (FAILED(hr)) return false;
    for (int tries = 0; tries < 4; tries++) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        DWORD st = 0;
        hr = d->mft->ProcessOutput(0, 1, &ob, &st);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { if (!set_output(*d)) return false; continue; }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false;  // no frame for this packet (not expected for IVF)
        if (FAILED(hr) || !ob.pSample) return false;
        const bool ok = read_frame(*d, ob.pSample, nv12, w, h);
        ob.pSample->Release();
        return ok;
    }
    return false;
}

void mfvp9_close(MfVp9* d) { delete d; }

#ifdef MFVP9_TEST
// clang++ -std=c++20 -O2 -DMFVP9_TEST movie_mf.cpp -lmfplat -lmfuuid -ld3d11 -lole32 -o mftest.exe; mftest <file.ivf> [frames]
#include <chrono>
#include <fstream>
#include <iterator>
int main(int argc, char** argv) {
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    uint16_t w, h;
    memcpy(&w, &f[12], 2), memcpy(&h, &f[14], 2);
    MfVp9* d = mfvp9_open(w, h);
    if (!d) return 1;
    const int frames = argc > 2 ? atoi(argv[2]) : 300;
    std::vector<uint8_t> o;
    uint32_t ow = 0, oh = 0;
    int got = 0;
    FILETIME c0, e0, k0, u0, c1, e1, k1, u1;
    GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0);
    auto t0 = std::chrono::steady_clock::now();
    size_t pos = 32;
    for (int i = 0; i < frames && pos + 12 <= f.size(); i++) {
        uint32_t n;
        memcpy(&n, &f[pos], 4);
        got += mfvp9_decode(d, &f[pos + 12], n, o, &ow, &oh);
        pos += 12 + n;
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    GetProcessTimes(GetCurrentProcess(), &c1, &e1, &k1, &u1);
    auto t100 = [](FILETIME a) { return (double)(((uint64_t)a.dwHighDateTime << 32) | a.dwLowDateTime) / 1e4; };
    printf("%d frames out (%ux%u), %.2f ms wall per frame, CPU %.2f ms per frame\n", got, ow, oh, ms / frames,
           (t100(u1) - t100(u0) + t100(k1) - t100(k0)) / frames);
    if (got) { FILE* g = fopen("mftest_last.nv12", "wb"); fwrite(o.data(), 1, o.size(), g); fclose(g); }
    mfvp9_close(d);
}
#endif
#elif !defined(__linux__)
MfVp9* mfvp9_open(uint32_t, uint32_t) { return nullptr; }
bool mfvp9_decode(MfVp9*, const uint8_t*, uint32_t, std::vector<uint8_t>&, uint32_t*, uint32_t*) { return false; }
void mfvp9_close(MfVp9*) {}
#endif
