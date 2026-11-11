#include "mcp.hpp"

#include <d3d11.h>
#include <wincodec.h>
#include <wincrypt.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <thread>
#include <vector>

namespace aimouse {
namespace {

namespace Capture = winrt::Windows::Graphics::Capture;
namespace DirectX = winrt::Windows::Graphics::DirectX;
using namespace std::chrono_literals;
using winrt::check_hresult;
using winrt::com_ptr;

struct Apartment {
    Apartment() { winrt::init_apartment(); }
    ~Apartment() { winrt::uninit_apartment(); }
};

std::string png_base64(UINT width, UINT height, std::vector<BYTE>& pixels) {
    com_ptr<IWICImagingFactory> factory;
    check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(factory.put())));
    com_ptr<IStream> stream;
    check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
    com_ptr<IWICBitmapEncoder> encoder;
    check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    com_ptr<IWICBitmapFrameEncode> frame;
    check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
    check_hresult(frame->Initialize(nullptr));
    check_hresult(frame->SetSize(width, height));
    auto format = GUID_WICPixelFormat32bppBGRA;
    check_hresult(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat32bppBGRA) throw ToolError("capture_failed", "PNG encoder changed pixel format.");
    check_hresult(frame->WritePixels(height, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
    check_hresult(frame->Commit());
    check_hresult(encoder->Commit());
    STATSTG stats{};
    check_hresult(stream->Stat(&stats, STATFLAG_NONAME));
    if (stats.cbSize.QuadPart > 8 * 1024 * 1024) {
        throw ToolError("image_too_large", "The encoded frame exceeded 8 MiB.");
    }
    std::vector<BYTE> encoded(static_cast<std::size_t>(stats.cbSize.QuadPart));
    check_hresult(stream->Seek({}, STREAM_SEEK_SET, nullptr));
    ULONG read{};
    check_hresult(stream->Read(encoded.data(), static_cast<ULONG>(encoded.size()), &read));
    if (read != encoded.size()) throw ToolError("capture_failed", "PNG stream was truncated.");
    DWORD size{};
    constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    if (!CryptBinaryToStringA(encoded.data(), read, flags, nullptr, &size)) {
        throw ToolError("capture_failed", "Could not size the base64 output.");
    }
    std::string base64(size, '\0');
    if (!CryptBinaryToStringA(encoded.data(), read, flags, base64.data(), &size)) {
        throw ToolError("capture_failed", "Could not encode the PNG output.");
    }
    base64.resize(size);
    return base64;
}

Capture::GraphicsCaptureItem capture_item(const std::optional<WindowIdentity>& window) {
    const auto factory = winrt::get_activation_factory<Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    Capture::GraphicsCaptureItem item{nullptr};
    const auto guid = winrt::guid_of<Capture::IGraphicsCaptureItem>();
    if (window) {
        if (!window->alive() || IsIconic(window->hwnd)) throw ToolError("target_lost", "The capture window closed or minimized.");
        check_hresult(factory->CreateForWindow(window->hwnd, guid, winrt::put_abi(item)));
    } else {
        const auto monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        check_hresult(factory->CreateForMonitor(monitor, guid, winrt::put_abi(item)));
    }
    return item;
}

} // namespace

Image capture_image(const std::optional<WindowIdentity>& window) {
    try {
        const Apartment apartment;
        if (!Capture::GraphicsCaptureSession::IsSupported()) {
            throw ToolError("capture_unsupported", "Windows Graphics Capture is unavailable in this session.");
        }
        const auto item = capture_item(window);
        com_ptr<ID3D11Device> device;
        com_ptr<ID3D11DeviceContext> context;
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()));
        com_ptr<::IInspectable> inspectable;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(device.as<IDXGIDevice>().get(), inspectable.put()));
        const auto projected = inspectable.as<DirectX::Direct3D11::IDirect3DDevice>();
        const auto source_size = item.Size();
        if (source_size.Width <= 0 || source_size.Height <= 0 ||
            static_cast<long long>(source_size.Width) * source_size.Height > 40'000'000) {
            throw ToolError("capture_size", "Invalid capture size or source exceeds 40 megapixels.");
        }
        auto pool = Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(projected,
            DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, source_size);
        auto session = pool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(false);
        session.StartCapture();
        Capture::Direct3D11CaptureFrame frame{nullptr};
        const auto deadline = std::chrono::steady_clock::now() + 900ms;
        while (std::chrono::steady_clock::now() < deadline) {
            frame = pool.TryGetNextFrame();
            if (frame) break;
            std::this_thread::sleep_for(10ms);
        }
        if (!frame) throw ToolError("capture_timeout", "No compositor frame arrived within 900 ms.");
        auto access = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        com_ptr<ID3D11Texture2D> source;
        check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), source.put_void()));
        D3D11_TEXTURE2D_DESC description{};
        source->GetDesc(&description);
        const auto size = frame.ContentSize();
        if (size.Width <= 0 || size.Height <= 0 || static_cast<UINT>(size.Width) > description.Width ||
            static_cast<UINT>(size.Height) > description.Height) {
            throw ToolError("capture_resized", "The window resized while being captured. Retry once stable.");
        }
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;
        com_ptr<ID3D11Texture2D> staging;
        check_hresult(device->CreateTexture2D(&description, nullptr, staging.put()));
        context->CopyResource(staging.get(), source.get());
        const double ratio = std::min({1.0, 1920.0 / size.Width, 1080.0 / size.Height});
        const auto width = std::max(1u, static_cast<unsigned>(size.Width * ratio));
        const auto height = std::max(1u, static_cast<unsigned>(size.Height * ratio));
        std::vector<BYTE> pixels(static_cast<std::size_t>(width) * height * 4);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        const auto* bytes = static_cast<const BYTE*>(mapped.pData);
        for (UINT y = 0; y < height; ++y) {
            const auto* row = bytes + (static_cast<std::size_t>(y) * size.Height / height) * mapped.RowPitch;
            for (UINT x = 0; x < width; ++x) {
                const auto* pixel = row + (static_cast<std::size_t>(x) * size.Width / width) * 4;
                auto* output = pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
                std::copy_n(pixel, 3, output);
                output[3] = 255;
            }
        }
        context->Unmap(staging.get(), 0);
        frame.Close();
        session.Close();
        pool.Close();
        return {width, height, png_base64(width, height, pixels)};
    } catch (const winrt::hresult_error& error) {
        throw ToolError("capture_failed", "Windows Graphics Capture: " + utf8(error.message()));
    }
}

} // namespace aimouse
