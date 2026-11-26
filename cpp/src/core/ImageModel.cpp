#include "ImageModel.hpp"
#include <windows.h>
#include <wincodec.h>
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

bool loadGrayImage(const std::string& path, GrayImage& out, std::string& err) {
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
    if ((unsigned long long)w * h > 40000000ull) { err = "Görsel çok büyük (en fazla 40 milyon piksel)."; return false; }
    const UINT stride = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> rgb((size_t)stride * h);
    if (FAILED(conv->CopyPixels(nullptr, stride, (UINT)rgb.size(), rgb.data()))) { err = "Görselin pikselleri okunamadı."; return false; }

    GrayImage g;
    g.w = (int)w;
    g.h = (int)h;
    g.gray.resize((size_t)w * h);
    std::unordered_set<uint32_t> colours;
    for (UINT y = 0; y < h; y++)
        for (UINT x = 0; x < w; x++) {
            const uint8_t* p = rgb.data() + (size_t)y * stride + x * 3;
            const uint8_t v = (uint8_t)((299 * p[0] + 587 * p[1] + 114 * p[2] + 500) / 1000);
            g.gray[(size_t)y * w + x] = v;
            g.count[v]++;
            if (colours.size() < 100000) colours.insert((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]);
        }
    g.colours = (int)colours.size();
    out = std::move(g);
    return true;
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
