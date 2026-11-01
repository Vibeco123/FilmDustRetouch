#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "resource.h"
#include "film_dust_heal.h"
#include <windowsx.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <wincodec.h>
#include <oleauto.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;
namespace fs = std::filesystem;

namespace {

constexpr wchar_t WINDOW_CLASS[] = L"FilmDustRetouchCppWindow";
constexpr int HISTORY_LIMIT = 8;

ULONG_PTR g_gdiplusToken = 0;
HWND g_hwnd = nullptr;
std::vector<fs::path> g_files;
int g_index = -1;

std::unique_ptr<Bitmap> g_image;
std::unique_ptr<Bitmap> g_previous;
std::unique_ptr<Bitmap> g_next;
std::unique_ptr<Bitmap> g_strokeSource;
std::vector<std::unique_ptr<Bitmap>> g_undo;
std::vector<std::unique_ptr<Bitmap>> g_redo;

int g_brush = 8;
bool g_dirty = false;
bool g_drawing = false;
bool g_panning = false;
bool g_previousHeld = false; // удерживается стрелка вниз
bool g_nextHeld = false;     // удерживается стрелка вверх
bool g_stampSourceSet = false;
bool g_stampStroke = false;
bool g_healingMode = false;
bool g_removeStroke = false;
std::vector<uint8_t> g_removeMask;

bool g_userChangedZoom = false;

using HealFunction = decltype(&fdh_heal_rgba8);
using RemoveFunction = decltype(&fdh_remove_rgba8);
HMODULE g_healModule = nullptr;
HealFunction g_healFunction = nullptr;
RemoveFunction g_removeFunction = nullptr;

float g_zoom = 1.0f;
float g_panX = 0.0f;
float g_panY = 0.0f;
POINT g_lastMouse{};
POINT g_panStart{};
float g_panStartX = 0.0f;
float g_panStartY = 0.0f;
PointF g_stampSource(0.0f, 0.0f);
PointF g_stampStrokeAnchor(0.0f, 0.0f);

std::wstring modeText() {
    if (g_previousHeld) return L"Previous Frame Copy";
    if (g_nextHeld) return L"Next Frame Copy";
    if (g_stampSourceSet && g_healingMode) return L"Healing Brush";
    if (g_stampSourceSet) return L"Clone Stamp";
    return L"Remove";
}

std::unique_ptr<Bitmap> cloneBitmap(Bitmap* source) {
    if (!source) return {};
    const UINT w = source->GetWidth();
    const UINT h = source->GetHeight();
    if (w == 0 || h == 0) return {};

    Bitmap* cloned = source->Clone(0, 0, static_cast<INT>(w), static_cast<INT>(h), PixelFormat32bppARGB);
    if (!cloned || cloned->GetLastStatus() != Ok) {
        delete cloned;
        return {};
    }
    return std::unique_ptr<Bitmap>(cloned);
}

// GDI+ keeps a file-backed image's stream alive. Read into memory, clone it,
// and release the stream so the PNG can be replaced when autosaving.
std::unique_ptr<Bitmap> loadBitmap(const fs::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const std::streamsize length = input.tellg();
    if (length <= 0) return {};
    input.seekg(0, std::ios::beg);

    std::vector<unsigned char> bytes(static_cast<size_t>(length));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), length)) return {};

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!memory) return {};
    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        return {};
    }
    std::memcpy(destination, bytes.data(), bytes.size());
    GlobalUnlock(memory);

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(memory, TRUE, &stream) != S_OK) {
        GlobalFree(memory);
        return {};
    }

    std::unique_ptr<Bitmap> result;
    {
        Bitmap source(stream);
        if (source.GetLastStatus() == Ok)
            result = cloneBitmap(&source);
    }
    stream->Release();
    return result;
}

// Encode PNG through Windows Imaging Component (WIC), using adaptive PNG
// filtering. Opaque frames are written as 24-bit BGR instead of needlessly
// expanding every pixel to 32-bit RGBA. Pixel values are not altered.
bool writePngWic(Bitmap* bitmap, const fs::path& filename) {
    if (!bitmap) return false;

    const UINT width = bitmap->GetWidth();
    const UINT height = bitmap->GetHeight();
    if (width == 0 || height == 0) return false;

    Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    BitmapData locked{};
    if (bitmap->LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &locked) != Ok)
        return false;

    bool opaque = true;
    for (UINT y = 0; y < height && opaque; ++y) {
        const auto* row = static_cast<const BYTE*>(locked.Scan0) +
                          static_cast<std::ptrdiff_t>(y) * locked.Stride;
        for (UINT x = 0; x < width; ++x) {
            if (row[static_cast<size_t>(x) * 4 + 3] != 255) {
                opaque = false;
                break;
            }
        }
    }

    const UINT stride = opaque ? ((width * 3u + 3u) & ~3u) : (width * 4u);
    std::vector<BYTE> pixels(static_cast<size_t>(stride) * height, 0);
    for (UINT y = 0; y < height; ++y) {
        const auto* src = static_cast<const BYTE*>(locked.Scan0) +
                          static_cast<std::ptrdiff_t>(y) * locked.Stride;
        BYTE* dst = pixels.data() + static_cast<size_t>(y) * stride;
        if (opaque) {
            for (UINT x = 0; x < width; ++x) {
                dst[static_cast<size_t>(x) * 3 + 0] = src[static_cast<size_t>(x) * 4 + 0]; // B
                dst[static_cast<size_t>(x) * 3 + 1] = src[static_cast<size_t>(x) * 4 + 1]; // G
                dst[static_cast<size_t>(x) * 3 + 2] = src[static_cast<size_t>(x) * 4 + 2]; // R
            }
        } else {
            std::memcpy(dst, src, static_cast<size_t>(width) * 4);
        }
    }
    bitmap->UnlockBits(&locked);

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* options = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(filename.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &options);

    if (SUCCEEDED(hr) && options) {
        PROPBAG2 property{};
        property.pstrName = const_cast<LPOLESTR>(L"FilterOption");
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_UI1;
        value.bVal = WICPngFilterAdaptive;
        // If a Windows build does not expose this optional encoder setting,
        // continue with the codec's default filter rather than failing save.
        options->Write(1, &property, &value);
        VariantClear(&value);
    }

    if (SUCCEEDED(hr)) hr = frame->Initialize(options);
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);

    WICPixelFormatGUID pixelFormat = opaque ? GUID_WICPixelFormat24bppBGR
                                             : GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&pixelFormat);
    const WICPixelFormatGUID expectedFormat = opaque ? GUID_WICPixelFormat24bppBGR
                                                      : GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr) && !IsEqualGUID(pixelFormat, expectedFormat)) hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    if (SUCCEEDED(hr)) hr = frame->WritePixels(height, stride,
                                                static_cast<UINT>(pixels.size()), pixels.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();

    if (options) options->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return SUCCEEDED(hr);
}

bool saveCurrent() {
    if (!g_dirty || !g_image || g_index < 0) return true;

    const fs::path target = g_files[static_cast<size_t>(g_index)];
    fs::path temporary = target;
    temporary += L".retouch.tmp.png";

    if (!writePngWic(g_image.get(), temporary)) {
        DeleteFileW(temporary.c_str());
        MessageBoxW(g_hwnd, L"Could not write PNG through Windows Imaging Component.",
                    L"Film Dust Retouch", MB_ICONERROR);
        return false;
    }

    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        DeleteFileW(temporary.c_str());
        std::wstring message = L"Could not replace the original PNG. Windows error code: " + std::to_wstring(error);
        MessageBoxW(g_hwnd, message.c_str(), L"Film Dust Retouch", MB_ICONERROR);
        return false;
    }

    g_dirty = false;
    return true;
}

void clearHistory() {
    g_undo.clear();
    g_redo.clear();
}

void pushUndo() {
    auto snapshot = cloneBitmap(g_image.get());
    if (!snapshot) return;
    g_undo.push_back(std::move(snapshot));
    if (g_undo.size() > HISTORY_LIMIT)
        g_undo.erase(g_undo.begin());
    g_redo.clear();
}

void updateStatus() {
    if (!g_hwnd || g_index < 0 || g_index >= static_cast<int>(g_files.size())) return;
    std::wstringstream title;
    title << L"Film Dust Retouch — " << (g_index + 1) << L"/" << g_files.size()
          << L" — " << g_files[static_cast<size_t>(g_index)].filename().wstring()
          << L" — Brush " << g_brush << L"px — " << modeText()
          << L" — " << std::fixed << std::setprecision(2) << g_zoom << L"x";
    if (g_dirty) title << L" *";
    SetWindowTextW(g_hwnd, title.str().c_str());
}

PointF clientToImage(float x, float y) {
    if (g_zoom <= 0.0f) return PointF(-1.0f, -1.0f);
    return PointF((x - g_panX) / g_zoom, (y - g_panY) / g_zoom);
}

bool pointInsideImage(const PointF& p) {
    return g_image && p.X >= 0.0f && p.Y >= 0.0f &&
           p.X < static_cast<float>(g_image->GetWidth()) &&
           p.Y < static_cast<float>(g_image->GetHeight());
}

void fitImage() {
    if (!g_image || !g_hwnd) return;
    RECT client{};
    GetClientRect(g_hwnd, &client);
    const float width = static_cast<float>(client.right - client.left);
    const float height = static_cast<float>(client.bottom - client.top);
    if (width <= 0.0f || height <= 0.0f) return;

    const float sx = width / static_cast<float>(g_image->GetWidth());
    const float sy = height / static_cast<float>(g_image->GetHeight());
    g_zoom = std::clamp(std::min(sx, sy), 0.02f, 16.0f);
    g_panX = (width - g_image->GetWidth() * g_zoom) / 2.0f;
    g_panY = (height - g_image->GetHeight() * g_zoom) / 2.0f;
}

bool loadFrame(int newIndex) {
    if (newIndex < 0 || newIndex >= static_cast<int>(g_files.size())) return false;
    if (!saveCurrent()) return false;

    auto current = loadBitmap(g_files[static_cast<size_t>(newIndex)]);
    if (!current) {
        MessageBoxW(g_hwnd, L"Could not open the PNG file.", L"Film Dust Retouch", MB_ICONERROR);
        return false;
    }

    std::unique_ptr<Bitmap> previous;
    std::unique_ptr<Bitmap> next;
    if (newIndex > 0)
        previous = loadBitmap(g_files[static_cast<size_t>(newIndex - 1)]);
    if (newIndex + 1 < static_cast<int>(g_files.size()))
        next = loadBitmap(g_files[static_cast<size_t>(newIndex + 1)]);

    g_image = std::move(current);
    g_previous = std::move(previous);
    g_next = std::move(next);
    g_index = newIndex;
    g_dirty = false;
    g_drawing = false;
    g_panning = false;
    g_stampStroke = false;
    g_strokeSource.reset();
    clearHistory();
    g_zoom = 1.0f;
    g_userChangedZoom = false;
    fitImage();
    updateStatus();
    InvalidateRect(g_hwnd, nullptr, TRUE);
    return true;
}

void copyCircle(Bitmap* source, int centerX, int centerY, int radius, int sourceCenterX, int sourceCenterY) {
    if (!g_image || !source) return;
    const int width = static_cast<int>(g_image->GetWidth());
    const int height = static_cast<int>(g_image->GetHeight());
    const int x0 = std::max(0, centerX - radius);
    const int x1 = std::min(width - 1, centerX + radius);
    const int y0 = std::max(0, centerY - radius);
    const int y1 = std::min(height - 1, centerY + radius);
    const int sourceWidth = static_cast<int>(source->GetWidth());
    const int sourceHeight = static_cast<int>(source->GetHeight());

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const int dx = x - centerX;
            const int dy = y - centerY;
            if (dx * dx + dy * dy > radius * radius) continue;
            const int sx = sourceCenterX + dx;
            const int sy = sourceCenterY + dy;
            if (sx < 0 || sy < 0 || sx >= sourceWidth || sy >= sourceHeight) continue;
            Color color;
            if (source->GetPixel(sx, sy, &color) == Ok)
                g_image->SetPixel(x, y, color);
        }
    }
}

void inpaintCircle(int centerX, int centerY, int radius) {
    if (!g_image) return;
    const int width = static_cast<int>(g_image->GetWidth());
    const int height = static_cast<int>(g_image->GetHeight());
    const int x0 = std::max(0, centerX - radius);
    const int x1 = std::min(width - 1, centerX + radius);
    const int y0 = std::max(0, centerY - radius);
    const int y1 = std::min(height - 1, centerY + radius);
    const int ring = std::max(2, radius + 2);

    std::uint64_t red = 0, green = 0, blue = 0, alpha = 0;
    std::uint64_t count = 0;
    for (int y = std::max(0, centerY - ring); y <= std::min(height - 1, centerY + ring); ++y) {
        for (int x = std::max(0, centerX - ring); x <= std::min(width - 1, centerX + ring); ++x) {
            const int dx = x - centerX;
            const int dy = y - centerY;
            const int distance2 = dx * dx + dy * dy;
            if (distance2 <= radius * radius || distance2 > ring * ring) continue;
            Color color;
            if (g_image->GetPixel(x, y, &color) != Ok) continue;
            red += color.GetR(); green += color.GetG(); blue += color.GetB(); alpha += color.GetA();
            ++count;
        }
    }
    if (count == 0) return;

    Color fill(static_cast<BYTE>(alpha / count), static_cast<BYTE>(red / count),
               static_cast<BYTE>(green / count), static_cast<BYTE>(blue / count));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const int dx = x - centerX;
            const int dy = y - centerY;
            if (dx * dx + dy * dy <= radius * radius)
                g_image->SetPixel(x, y, fill);
        }
    }
}

// Photoshop-like Healing Brush: copy texture from the chosen source, while
// matching its average RGB tone to the destination surroundings. A soft edge
// blends the repair into the untouched image. All calculations use the
// untouched stroke snapshot to avoid feedback while dragging.
void healCircleFallback(Bitmap* original, int centerX, int centerY, int radius,
                        int sourceCenterX, int sourceCenterY) {
    if (!g_image || !original) return;
    const int width = static_cast<int>(g_image->GetWidth());
    const int height = static_cast<int>(g_image->GetHeight());
    const int sourceWidth = static_cast<int>(original->GetWidth());
    const int sourceHeight = static_cast<int>(original->GetHeight());
    const int ring = std::max(3, radius / 2 + 3);

    long long dstR = 0, dstG = 0, dstB = 0;
    long long srcR = 0, srcG = 0, srcB = 0;
    int count = 0;
    for (int oy = -ring; oy <= ring; ++oy) {
        for (int ox = -ring; ox <= ring; ++ox) {
            const int d2 = ox * ox + oy * oy;
            if (d2 <= radius * radius || d2 > ring * ring) continue;
            const int dx = centerX + ox, dy = centerY + oy;
            const int sx = sourceCenterX + ox, sy = sourceCenterY + oy;
            if (dx < 0 || dy < 0 || dx >= width || dy >= height ||
                sx < 0 || sy < 0 || sx >= sourceWidth || sy >= sourceHeight) continue;
            Color dc, sc;
            if (original->GetPixel(dx, dy, &dc) != Ok ||
                original->GetPixel(sx, sy, &sc) != Ok) continue;
            dstR += dc.GetR(); dstG += dc.GetG(); dstB += dc.GetB();
            srcR += sc.GetR(); srcG += sc.GetG(); srcB += sc.GetB();
            ++count;
        }
    }
    if (count < 3) return;
    const double shiftR = static_cast<double>(dstR - srcR) / count;
    const double shiftG = static_cast<double>(dstG - srcG) / count;
    const double shiftB = static_cast<double>(dstB - srcB) / count;

    const int x0 = std::max(0, centerX - radius);
    const int x1 = std::min(width - 1, centerX + radius);
    const int y0 = std::max(0, centerY - radius);
    const int y1 = std::min(height - 1, centerY + radius);
    const double feather = std::max(1.0, radius * 0.22);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const int ox = x - centerX, oy = y - centerY;
            const double distance = std::sqrt(static_cast<double>(ox * ox + oy * oy));
            if (distance > radius) continue;
            const int sx = sourceCenterX + ox, sy = sourceCenterY + oy;
            if (sx < 0 || sy < 0 || sx >= sourceWidth || sy >= sourceHeight) continue;
            Color donor, base;
            if (original->GetPixel(sx, sy, &donor) != Ok ||
                original->GetPixel(x, y, &base) != Ok) continue;
            const double edge = std::clamp((radius - distance) / feather, 0.0, 1.0);
            const double blend = 0.5 - 0.5 * std::cos(3.141592653589793 * edge);
            const int r = std::clamp(static_cast<int>(std::lround(base.GetR() * (1.0 - blend) +
                std::clamp(donor.GetR() + shiftR, 0.0, 255.0) * blend)), 0, 255);
            const int g = std::clamp(static_cast<int>(std::lround(base.GetG() * (1.0 - blend) +
                std::clamp(donor.GetG() + shiftG, 0.0, 255.0) * blend)), 0, 255);
            const int b = std::clamp(static_cast<int>(std::lround(base.GetB() * (1.0 - blend) +
                std::clamp(donor.GetB() + shiftB, 0.0, 255.0) * blend)), 0, 255);
            g_image->SetPixel(x, y, Color(base.GetA(), static_cast<BYTE>(r),
                                          static_cast<BYTE>(g), static_cast<BYTE>(b)));
        }
    }
}


// Poisson healing through the optional Rust DLL. Each brush dab is processed
// as a small patch, not as a full-frame operation.
void healCircle(Bitmap* original, int centerX, int centerY, int radius,
                int sourceCenterX, int sourceCenterY) {
    if (!g_image || !original) return;
    if (!g_healFunction) {
        healCircleFallback(original, centerX, centerY, radius, sourceCenterX, sourceCenterY);
        return;
    }

    const int width = static_cast<int>(g_image->GetWidth());
    const int height = static_cast<int>(g_image->GetHeight());
    const int sourceWidth = static_cast<int>(original->GetWidth());
    const int sourceHeight = static_cast<int>(original->GetHeight());

    // The patch has a one-pixel unmasked border required by the solver.
    const int pad = 1;
    const int side = radius * 2 + 1 + pad * 2;
    const int localCenter = radius + pad;
    const size_t pixels = static_cast<size_t>(side) * side;
    std::vector<uint8_t> src(pixels * 4, 0);
    std::vector<uint8_t> dst(pixels * 4, 0);
    std::vector<uint8_t> mask(pixels, 0);
    std::vector<uint8_t> out(pixels * 4, 0);

    for (int py = 0; py < side; ++py) {
        for (int px = 0; px < side; ++px) {
            const int ox = px - localCenter;
            const int oy = py - localCenter;
            const int dx = centerX + ox;
            const int dy = centerY + oy;
            const int sx = sourceCenterX + ox;
            const int sy = sourceCenterY + oy;
            const size_t i = static_cast<size_t>(py) * side + px;
            Color sc(0, 0, 0, 0), dc(0, 0, 0, 0);
            if (dx >= 0 && dy >= 0 && dx < width && dy < height)
                original->GetPixel(dx, dy, &dc);
            if (sx >= 0 && sy >= 0 && sx < sourceWidth && sy < sourceHeight)
                original->GetPixel(sx, sy, &sc);

            // API expects RGBA; GDI+ Color exposes channels independent of memory order.
            src[i * 4 + 0] = sc.GetR();
            src[i * 4 + 1] = sc.GetG();
            src[i * 4 + 2] = sc.GetB();
            src[i * 4 + 3] = sc.GetA();
            dst[i * 4 + 0] = dc.GetR();
            dst[i * 4 + 1] = dc.GetG();
            dst[i * 4 + 2] = dc.GetB();
            dst[i * 4 + 3] = dc.GetA();

            const int d2 = ox * ox + oy * oy;
            const bool inside = d2 <= radius * radius;
            const bool valid = dx >= 0 && dy >= 0 && dx < width && dy < height &&
                               sx >= 0 && sy >= 0 && sx < sourceWidth && sy < sourceHeight;
            if (inside && valid) mask[i] = 255;
        }
    }

    const int32_t result = g_healFunction(
        static_cast<uint32_t>(side), static_cast<uint32_t>(side),
        src.data(), dst.data(), mask.data(), out.data());

    if (result != 0) {
        // Preserve the existing working brush if the DLL reports an error.
        healCircleFallback(original, centerX, centerY, radius, sourceCenterX, sourceCenterY);
        return;
    }

    for (int py = 0; py < side; ++py) {
        for (int px = 0; px < side; ++px) {
            const size_t i = static_cast<size_t>(py) * side + px;
            if (!mask[i]) continue;
            const int x = centerX + px - localCenter;
            const int y = centerY + py - localCenter;
            if (x < 0 || y < 0 || x >= width || y >= height) continue;
            g_image->SetPixel(x, y, Color(out[i * 4 + 3], out[i * 4 + 0],
                                          out[i * 4 + 1], out[i * 4 + 2]));
        }
    }
}

// Apply one complete Remove stroke using PhotoCraft's patch-based inpainting.
// The mask is collected while dragging, then solved once on mouse release.
void applyRemoveStroke() {
    if (!g_image || !g_removeFunction || g_removeMask.empty()) return;
    const int width = static_cast<int>(g_image->GetWidth());
    const int height = static_cast<int>(g_image->GetHeight());
    int bx0 = width, by0 = height, bx1 = 0, by1 = 0;
    bool any = false;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!g_removeMask[static_cast<size_t>(y) * width + x]) continue;
            any = true;
            bx0 = std::min(bx0, x); by0 = std::min(by0, y);
            bx1 = std::max(bx1, x + 1); by1 = std::max(by1, y + 1);
        }
    }
    if (!any) return;
    const int extent = std::max(bx1 - bx0, by1 - by0);
    const int margin = std::clamp(extent * 3 / 4, 32, 512);
    const int x0 = std::max(0, bx0 - margin);
    const int y0 = std::max(0, by0 - margin);
    const int x1 = std::min(width, bx1 + margin);
    const int y1 = std::min(height, by1 + margin);
    const int w = x1 - x0, h = y1 - y0;
    if (w < 7 || h < 7) return;

    const size_t count = static_cast<size_t>(w) * h;
    std::vector<uint8_t> rgba(count * 4);
    std::vector<uint8_t> mask(count);
    std::vector<uint8_t> output(count * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int ix = x + x0, iy = y + y0;
            const size_t i = static_cast<size_t>(y) * w + x;
            Color c;
            if (g_image->GetPixel(ix, iy, &c) != Ok) continue;
            rgba[i * 4 + 0] = c.GetR();
            rgba[i * 4 + 1] = c.GetG();
            rgba[i * 4 + 2] = c.GetB();
            rgba[i * 4 + 3] = c.GetA();
            mask[i] = g_removeMask[static_cast<size_t>(iy) * width + ix];
        }
    }
    const int32_t result = g_removeFunction(static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                                             rgba.data(), mask.data(), output.data());
    if (result != 0) {
        std::wstring message = L"Automatic Remove failed (code " + std::to_wstring(result) + L").";
        MessageBoxW(g_hwnd, message.c_str(), L"Film Dust Retouch", MB_ICONWARNING);
        return;
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            // PhotoCraft may enlarge the painted hole and close loops. Its output is unchanged
            // outside the resulting hole, so writing the complete crop is safe.
            g_image->SetPixel(x + x0, y + y0,
                Color(output[i * 4 + 3], output[i * 4 + 0], output[i * 4 + 1], output[i * 4 + 2]));
        }
    }
    g_dirty = true;
}

void addRemoveDab(int centerX, int centerY, int radius) {
    if (!g_image || g_removeMask.empty()) return;
    const int width = static_cast<int>(g_image->GetWidth());
    const int height = static_cast<int>(g_image->GetHeight());
    const int x0 = std::max(0, centerX - radius), x1 = std::min(width - 1, centerX + radius);
    const int y0 = std::max(0, centerY - radius), y1 = std::min(height - 1, centerY + radius);
    for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
        const int dx = x - centerX, dy = y - centerY;
        if (dx * dx + dy * dy <= radius * radius)
            g_removeMask[static_cast<size_t>(y) * width + x] = 255;
    }
}

void paintStroke(POINT from, POINT to) {
    if (!g_image) return;
    const PointF a = clientToImage(static_cast<float>(from.x), static_cast<float>(from.y));
    const PointF b = clientToImage(static_cast<float>(to.x), static_cast<float>(to.y));
    const int x1 = static_cast<int>(std::lround(a.X));
    const int y1 = static_cast<int>(std::lround(a.Y));
    const int x2 = static_cast<int>(std::lround(b.X));
    const int y2 = static_cast<int>(std::lround(b.Y));
    const int distance = std::max(std::abs(x2 - x1), std::abs(y2 - y1));
    const int spacing = std::max(1, g_brush / 4);
    const int steps = std::max(1, distance / spacing);
    const int radius = std::max(1, g_brush / 2);

    for (int k = 0; k <= steps; ++k) {
        const int x = x1 + (x2 - x1) * k / steps;
        const int y = y1 + (y2 - y1) * k / steps;
        if (g_previousHeld || g_nextHeld) {
            Bitmap* source = g_previousHeld ? g_previous.get() : g_next.get();
            if (source && source->GetWidth() == g_image->GetWidth() &&
                source->GetHeight() == g_image->GetHeight())
                copyCircle(source, x, y, radius, x, y);
        } else if (g_stampStroke && g_strokeSource) {
            const int sx = static_cast<int>(std::lround(g_stampSource.X + x - g_stampStrokeAnchor.X));
            const int sy = static_cast<int>(std::lround(g_stampSource.Y + y - g_stampStrokeAnchor.Y));
            if (g_healingMode)
                healCircle(g_strokeSource.get(), x, y, radius, sx, sy);
            else
                copyCircle(g_strokeSource.get(), x, y, radius, sx, sy);
        } else if (g_removeStroke) {
            addRemoveDab(x, y, radius);
        } else {
            inpaintCircle(x, y, radius);
        }
    }

    if (!g_removeStroke) g_dirty = true;
    updateStatus();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void beginStroke(POINT location) {
    if (!g_image) return;
    const PointF point = clientToImage(static_cast<float>(location.x), static_cast<float>(location.y));
    if (!pointInsideImage(point)) return;

    pushUndo();
    g_drawing = true;
    SetCapture(g_hwnd);
    g_lastMouse = location;
    g_stampStroke = g_stampSourceSet && !g_previousHeld && !g_nextHeld;
    g_removeStroke = !g_stampStroke && !g_previousHeld && !g_nextHeld && g_removeFunction;
    g_removeMask.clear();
    if (g_removeStroke) {
        const size_t pixels = static_cast<size_t>(g_image->GetWidth()) * g_image->GetHeight();
        g_removeMask.assign(pixels, 0);
    }
    g_strokeSource = g_stampStroke ? cloneBitmap(g_image.get()) : nullptr;
    g_stampStrokeAnchor = point;
    paintStroke(location, location);
}

void undoRedo(bool redoAction) {
    if (!g_image) return;
    auto& from = redoAction ? g_redo : g_undo;
    auto& to = redoAction ? g_undo : g_redo;
    if (from.empty()) return;

    auto current = cloneBitmap(g_image.get());
    if (!current) return;
    to.push_back(std::move(current));
    g_image = std::move(from.back());
    from.pop_back();
    g_dirty = true;
    updateStatus();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void endStroke() {
    if (!g_drawing) return;
    if (g_removeStroke) applyRemoveStroke();
    g_removeStroke = false;
    g_removeMask.clear();
    g_drawing = false;
    g_stampStroke = false;
    g_strokeSource.reset();
    if (GetCapture() == g_hwnd) ReleaseCapture();
    updateStatus();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void chooseFolderAndLoad() {
    // Use the modern Windows folder picker. Unlike SHBrowseForFolder, it has
    // the Explorer address/location bar; Ctrl+L or Alt+D lets the user paste a path.
    IFileDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        MessageBoxW(g_hwnd, L"Could not open the folder picker.", L"Film Dust Retouch", MB_ICONERROR);
        return;
    }

    DWORD options = 0;
    hr = dialog->GetOptions(&options);
    if (SUCCEEDED(hr)) {
        hr = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    if (SUCCEEDED(hr)) dialog->SetTitle(L"Select a folder containing PNG files");
    if (SUCCEEDED(hr)) hr = dialog->Show(g_hwnd);

    IShellItem* result = nullptr;
    if (SUCCEEDED(hr)) hr = dialog->GetResult(&result);
    dialog->Release();
    if (FAILED(hr) || !result) return; // Cancel or dialog error.

    PWSTR folderPath = nullptr;
    hr = result->GetDisplayName(SIGDN_FILESYSPATH, &folderPath);
    result->Release();
    if (FAILED(hr) || !folderPath) return;

    const fs::path folder(folderPath);
    CoTaskMemFree(folderPath);

    try {
        for (const auto& entry : fs::directory_iterator(folder)) {
            if (!entry.is_regular_file()) continue;
            std::wstring extension = entry.path().extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (extension == L".png") g_files.push_back(entry.path());
        }
    } catch (...) {
        MessageBoxW(g_hwnd, L"Could not read the selected folder.", L"Film Dust Retouch", MB_ICONERROR);
        return;
    }

    std::sort(g_files.begin(), g_files.end(), [](const fs::path& a, const fs::path& b) {
        return CompareStringOrdinal(a.filename().c_str(), -1, b.filename().c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });

    if (g_files.empty()) {
        MessageBoxW(g_hwnd, L"No PNG files found in the selected folder.", L"Film Dust Retouch", MB_ICONINFORMATION);
        return;
    }
    loadFrame(0);
}

void paintWindow(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC windowDC = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    if (width <= 0 || height <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }

    // Render the complete frame into an off-screen bitmap first. Only the
    // finished image is copied to the window, preventing flicker/tearing
    // while the mouse moves and the brush cursor is continuously redrawn.
    HDC memoryDC = CreateCompatibleDC(windowDC);
    HBITMAP backBuffer = CreateCompatibleBitmap(windowDC, width, height);
    if (!memoryDC || !backBuffer) {
        if (backBuffer) DeleteObject(backBuffer);
        if (memoryDC) DeleteDC(memoryDC);
        EndPaint(hwnd, &ps);
        return;
    }

    HGDIOBJ oldBitmap = SelectObject(memoryDC, backBuffer);
    {
        Graphics graphics(memoryDC);
        graphics.SetCompositingMode(CompositingModeSourceCopy);
        graphics.Clear(Color(255, 35, 35, 35));
        graphics.SetCompositingMode(CompositingModeSourceOver);

        if (g_image) {
            graphics.SetInterpolationMode(InterpolationModeNearestNeighbor);
            graphics.SetPixelOffsetMode(PixelOffsetModeHalf);
            const RectF destination(g_panX, g_panY,
                g_image->GetWidth() * g_zoom, g_image->GetHeight() * g_zoom);
            graphics.DrawImage(g_image.get(), destination);

            POINT mouse{};
            GetCursorPos(&mouse);
            ScreenToClient(hwnd, &mouse);
            const PointF imagePoint = clientToImage(static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            if (pointInsideImage(imagePoint)) {
                const float diameter = g_brush * g_zoom;
                Pen brushPen(Color(255, 255, 255, 255), 1.0f);
                graphics.DrawEllipse(&brushPen,
                    g_panX + imagePoint.X * g_zoom - diameter / 2.0f,
                    g_panY + imagePoint.Y * g_zoom - diameter / 2.0f,
                    diameter, diameter);

                if (g_stampSourceSet && !g_previousHeld && !g_nextHeld) {
                    Pen sourcePen(Color(255, 50, 220, 100), 1.0f);
                    const float sx = g_panX + g_stampSource.X * g_zoom;
                    const float sy = g_panY + g_stampSource.Y * g_zoom;
                    graphics.DrawLine(&sourcePen, sx - 5, sy, sx + 5, sy);
                    graphics.DrawLine(&sourcePen, sx, sy - 5, sx, sy + 5);
                }
            }
        }
    }

    BitBlt(windowDC, 0, 0, width, height, memoryDC, 0, 0, SRCCOPY);
    SelectObject(memoryDC, oldBitmap);
    DeleteObject(backBuffer);
    DeleteDC(memoryDC);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_hwnd = hwnd;
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        paintWindow(hwnd);
        return 0;

    case WM_SIZE:
        if (g_image && !g_userChangedZoom) fitImage();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        beginStroke(p);
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (g_drawing && (wParam & MK_LBUTTON)) {
            paintStroke(g_lastMouse, p);
            g_lastMouse = p;
        } else if (g_panning && (wParam & MK_MBUTTON)) {
            g_panX = g_panStartX + static_cast<float>(p.x - g_panStart.x);
            g_panY = g_panStartY + static_cast<float>(p.y - g_panStart.y);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        endStroke();
        return 0;

    case WM_RBUTTONDOWN: {
        if (!g_image) return 0;
        const PointF p = clientToImage(static_cast<float>(GET_X_LPARAM(lParam)),
                                       static_cast<float>(GET_Y_LPARAM(lParam)));
        g_stampSourceSet = pointInsideImage(p);
        if (g_stampSourceSet) g_stampSource = p;
        updateStatus();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MBUTTONDOWN:
        g_panning = true;
        SetCapture(hwnd);
        g_panStart = POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        g_panStartX = g_panX;
        g_panStartY = g_panY;
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return 0;

    case WM_MBUTTONUP:
        if (g_panning) {
            g_panning = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (g_drawing) {
            if (g_removeStroke) applyRemoveStroke();
            g_removeStroke = false;
            g_removeMask.clear();
            g_drawing = false;
            g_stampStroke = false;
            g_strokeSource.reset();
        }
        g_panning = false;
        return 0;

    case WM_MOUSEWHEEL: {
        if (!g_image) return 0;
        POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &p);
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (GetKeyState(VK_SHIFT) & 0x8000) {
            g_brush = std::clamp(g_brush + (delta > 0 ? 1 : -1), 1, 256);
        } else {
            const PointF before = clientToImage(static_cast<float>(p.x), static_cast<float>(p.y));
            const float factor = delta > 0 ? 1.15f : (1.0f / 1.15f);
            g_zoom = std::clamp(g_zoom * factor, 0.02f, 32.0f);
            g_panX = p.x - before.X * g_zoom;
            g_panY = p.y - before.Y * g_zoom;
            g_userChangedZoom = true;
        }
        updateStatus();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_DOWN) {
            g_previousHeld = true;
            g_nextHeld = false;
            updateStatus();
            return 0;
        }
        if (wParam == VK_UP) {
            g_nextHeld = true;
            g_previousHeld = false;
            updateStatus();
            return 0;
        }
        if (wParam == VK_LEFT) {
            if (g_index > 0) loadFrame(g_index - 1);
            return 0;
        }
        if (wParam == VK_RIGHT) {
            if (g_index + 1 < static_cast<int>(g_files.size())) loadFrame(g_index + 1);
            return 0;
        }
        if (wParam == 'S' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            saveCurrent();
            return 0;
        }
        if (wParam == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            undoRedo((GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        }
        if (wParam == '1') {
            g_stampSourceSet = false;
            g_healingMode = false;
            updateStatus();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == '2' || wParam == '3' || wParam == 'H') {
            if (g_stampSourceSet) {
                if (wParam == '2') g_healingMode = false;
                else if (wParam == '3') g_healingMode = true;
                else if (!(lParam & (1LL << 30))) g_healingMode = !g_healingMode;
                updateStatus();
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                MessageBeep(MB_ICONWARNING);
            }
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            g_stampSourceSet = false;
            g_healingMode = false;
            updateStatus();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == 'F') {
            g_userChangedZoom = false;
            fitImage();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_KEYUP:
        if (wParam == VK_DOWN || wParam == VK_UP) {
            g_previousHeld = false;
            g_nextHeld = false;
            updateStatus();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_KILLFOCUS:
        g_previousHeld = false;
        g_nextHeld = false;
        updateStatus();
        InvalidateRect(hwnd, nullptr, FALSE);
        break;

    case WM_CLOSE:
        if (saveCurrent()) DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace


bool loadHealingLibrary() {
    wchar_t exePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    fs::path dllPath(exePath);
    dllPath = dllPath.parent_path() / L"film_dust_heal.dll";
    g_healModule = LoadLibraryW(dllPath.c_str());
    if (!g_healModule) return false;
    g_healFunction = reinterpret_cast<HealFunction>(
        GetProcAddress(g_healModule, "fdh_heal_rgba8"));
    g_removeFunction = reinterpret_cast<RemoveFunction>(
        GetProcAddress(g_healModule, "fdh_remove_rgba8"));
    if (!g_healFunction || !g_removeFunction) {
        FreeLibrary(g_healModule);
        g_healModule = nullptr;
        return false;
    }
    return true;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    GdiplusStartupInput startupInput;
    if (GdiplusStartup(&g_gdiplusToken, &startupInput, nullptr) != Ok) {
        MessageBoxW(nullptr, L"Could not start GDI+.", L"Film Dust Retouch", MB_ICONERROR);
        return 1;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    loadHealingLibrary();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = WINDOW_CLASS;
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"Could not register the application window.", L"Film Dust Retouch", MB_ICONERROR);
        CoUninitialize();
        GdiplusShutdown(g_gdiplusToken);
        return 2;
    }

    g_hwnd = CreateWindowExW(0, WINDOW_CLASS, L"Film Dust Retouch",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 800,
        nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) {
        MessageBoxW(nullptr, L"Could not create the application window.", L"Film Dust Retouch", MB_ICONERROR);
        CoUninitialize();
        GdiplusShutdown(g_gdiplusToken);
        return 3;
    }

    ShowWindow(g_hwnd, showCommand);
    UpdateWindow(g_hwnd);
    chooseFolderAndLoad();

    if (g_files.empty() || !g_image) {
        DestroyWindow(g_hwnd);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_image.reset();
    g_previous.reset();
    g_next.reset();
    g_strokeSource.reset();
    clearHistory();
    if (g_healModule) FreeLibrary(g_healModule);
    CoUninitialize();
    GdiplusShutdown(g_gdiplusToken);
    return static_cast<int>(message.wParam);
}
