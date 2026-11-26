#include "ImageModel.hpp"
#include <windows.h>
#include <wincodec.h>
#include <algorithm>
#include <new>
#include <unordered_set>

std::vector<int> GrayImage::levels() const {
    std::vector<int> l;
    for (int g = 0; g < 256; g++)
        if (count[g] > 0) l.push_back(g);
    return l;
}

namespace {

template <class T> struct Com {   // minimal COM pointer
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

} // namespace

bool loadGrayImage(const std::string& path, GrayImage& out, std::string& err) try {
    // COM may already be initialised on this thread (file dialogs); only undo our own init
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct Uninit {
        bool on;
        ~Uninit() { if (on) CoUninitialize(); }
    } uninit{SUCCEEDED(init)};

    Com<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        err = "Windows görüntü bileşeni (WIC) başlatılamadı.";
        return false;
    }
    Com<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(wide(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) {
        err = "Görsel açılamadı ya da biçimi tanınmadı (PNG, JPG, BMP, GIF, TIFF desteklenir).";
        return false;
    }
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICFormatConverter> conv;
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.p, GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        err = "Görselin pikselleri okunamadı.";
        return false;
    }
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (w < 2 || h < 2) { err = "Görsel en az 2 × 2 piksel olmalı."; return false; }
    // No size limit of our own; WIC copies pixels in one call whose buffer size is a 32-bit
    // count, so the image is read in bands of rows
    const unsigned long long stride64 = ((unsigned long long)w * 3 + 3) & ~3ull;
    if (stride64 > 0xFFFFFFFFull) { err = "Görselin bir satırı okunamayacak kadar geniş."; return false; }
    const UINT stride = (UINT)stride64;
    GrayImage g;
    g.w = (int)w;
    g.h = (int)h;
    g.gray.resize((size_t)w * h);
    std::unordered_set<uint32_t> colours;
    const UINT band = std::max<UINT>(1, (UINT)std::min<unsigned long long>(h, (64ull << 20) / stride));   // ~64 MB at a time
    std::vector<uint8_t> rgb((size_t)stride * band);
    for (UINT y0 = 0; y0 < h; y0 += band) {
        const UINT n = std::min(band, h - y0);
        const WICRect rc{0, (INT)y0, (INT)w, (INT)n};
        if (FAILED(conv->CopyPixels(&rc, stride, stride * n, rgb.data()))) { err = "Görselin pikselleri okunamadı."; return false; }
        for (UINT y = 0; y < n; y++)
            for (UINT x = 0; x < w; x++) {
                const uint8_t* p = rgb.data() + (size_t)y * stride + (size_t)x * 3;
                const uint8_t v = (uint8_t)((299 * p[0] + 587 * p[1] + 114 * p[2] + 500) / 1000);
                g.gray[(size_t)(y0 + y) * w + x] = v;
                g.count[v]++;
                if (colours.size() < 100000) colours.insert((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]);
            }
    }
    g.colours = (int)colours.size();
    out = std::move(g);
    return true;
} catch (const std::bad_alloc&) {
    err = "Görsel belleğe sığmadı; daha küçük bir görselle deneyin.";
    return false;
}

GridModel modelFromGray(const GrayImage& img, const std::array<float, 256>& velocity, double widthM, double depthM,
                        const std::string& name) {
    GridModel m;
    m.nx = img.w;
    m.nz = img.h;
    m.dx = widthM / (img.w - 1);
    m.dz = depthM / (img.h - 1);
    m.source = name;
    m.v.resize((size_t)img.w * img.h);
    for (int ix = 0; ix < img.w; ix++)
        for (int iz = 0; iz < img.h; iz++) m.v[(size_t)ix * img.h + iz] = velocity[img.gray[(size_t)iz * img.w + ix]];
    return m;
}
