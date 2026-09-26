#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
constexpr int kPort = 26761;

struct LatestFrame
{
	std::mutex mutex;
	std::vector<uint8_t> bgra;
	int width = 0;
	int height = 0;
	uint64_t serial = 0;
};

bool EncodeJpeg(const uint8_t* bgra, int width, int height, int stride, std::vector<uint8_t>& jpeg)
{
	IWICImagingFactory* factory = nullptr;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
		return false;
	IStream* stream = nullptr;
	if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
	{
		factory->Release();
		return false;
	}
	IWICBitmapEncoder* encoder = nullptr;
	IWICBitmapFrameEncode* frame = nullptr;
	IPropertyBag2* props = nullptr;
	bool ok = false;
	if (SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) &&
		SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
		SUCCEEDED(encoder->CreateNewFrame(&frame, &props)))
	{
		PROPBAG2 option{};
		option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
		VARIANT value;
		VariantInit(&value);
		value.vt = VT_R4;
		value.fltVal = 0.6f;
		props->Write(1, &option, &value);
		VariantClear(&value);
		if (SUCCEEDED(frame->Initialize(props)) &&
			SUCCEEDED(frame->SetSize(width, height)))
	{
		WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
		std::vector<uint8_t> bgr(static_cast<size_t>(width) * height * 3);
		for (int i = 0; i < width * height; ++i)
		{
			bgr[static_cast<size_t>(i) * 3 + 0] = bgra[static_cast<size_t>(i) * 4 + 0];
			bgr[static_cast<size_t>(i) * 3 + 1] = bgra[static_cast<size_t>(i) * 4 + 1];
			bgr[static_cast<size_t>(i) * 3 + 2] = bgra[static_cast<size_t>(i) * 4 + 2];
		}
		if (SUCCEEDED(frame->SetPixelFormat(&format)) &&
			format == GUID_WICPixelFormat24bppBGR &&
			SUCCEEDED(frame->WritePixels(height, static_cast<UINT>(width * 3), static_cast<UINT>(bgr.size()), bgr.data())) &&
			SUCCEEDED(frame->Commit()) &&
			SUCCEEDED(encoder->Commit()))
		{
			HGLOBAL memory = nullptr;
			if (SUCCEEDED(GetHGlobalFromStream(stream, &memory)))
			{
				const SIZE_T size = GlobalSize(memory);
				void* data = GlobalLock(memory);
				if (data && size > 0)
				{
					jpeg.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
					ok = true;
				}
				if (data)
					GlobalUnlock(memory);
			}
		}
	}
	}
	if (props)
		props->Release();
	if (frame)
		frame->Release();
	if (encoder)
		encoder->Release();
	stream->Release();
	factory->Release();
	return ok;
}

BOOL CALLBACK FindPadWindow(HWND window, LPARAM param)
{
	wchar_t title[256]{};
	if (GetWindowTextW(window, title, 256) <= 0)
		return TRUE;
	if (wcsncmp(title, L"GamePad View", 12) != 0)
		return TRUE;
	*reinterpret_cast<HWND*>(param) = window;
	return FALSE;
}

struct Capture
{
	winrt::com_ptr<ID3D11Device> device;
	winrt::com_ptr<ID3D11DeviceContext> context;
	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice{nullptr};
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool{nullptr};
	winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{nullptr};
	winrt::com_ptr<ID3D11Texture2D> staging;
	UINT stageW = 0;
	UINT stageH = 0;
	std::atomic<bool> pending{false};
	HWND target = nullptr;

	bool EnsureDevice()
	{
		if (device)
			return true;
		D3D_FEATURE_LEVEL level{};
		if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), &level, context.put())))
			return false;
		winrt::com_ptr<IDXGIDevice> dxgi;
		if (FAILED(device->QueryInterface(dxgi.put())))
			return false;
		winrt::com_ptr<::IInspectable> inspectable;
		if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put())))
			return false;
		winrtDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
		return true;
	}

	void Close()
	{
		try
		{
			if (session)
				session.Close();
			if (pool)
				pool.Close();
		}
		catch (...)
		{
		}
		session = nullptr;
		pool = nullptr;
		staging = nullptr;
		stageW = 0;
		stageH = 0;
		pending.store(false);
		target = nullptr;
	}

	bool Open(HWND window)
	{
		Close();
		if (!EnsureDevice() || !window)
			return false;
		try
		{
			auto factory = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
			winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
			winrt::check_hresult(factory->CreateForWindow(
				window,
				winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
				reinterpret_cast<void**>(winrt::put_abi(item))));
			pool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
				winrtDevice,
				winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
				2,
				item.Size());
			pool.FrameArrived([this](auto&&, auto&&) { pending.store(true); });
			session = pool.CreateCaptureSession(item);
			session.IsCursorCaptureEnabled(false);
			try
			{
				session.IsBorderRequired(false);
			}
			catch (...)
			{
			}
			session.StartCapture();
			target = window;
			return true;
		}
		catch (...)
		{
			Close();
			return false;
		}
	}

	bool Take(LatestFrame& latest)
	{
		if (!pending.exchange(false) || !pool)
			return false;
		winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{nullptr};
		try
		{
			frame = pool.TryGetNextFrame();
		}
		catch (...)
		{
			return false;
		}
		if (!frame)
			return false;
		auto access = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (FAILED(access->GetInterface(IID_PPV_ARGS(texture.put()))))
			return false;
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		if (desc.Width < 8 || desc.Height < 8)
			return false;
		if (!staging || stageW != desc.Width || stageH != desc.Height)
		{
			D3D11_TEXTURE2D_DESC stage = desc;
			stage.Usage = D3D11_USAGE_STAGING;
			stage.BindFlags = 0;
			stage.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			stage.MiscFlags = 0;
			staging = nullptr;
			if (FAILED(device->CreateTexture2D(&stage, nullptr, staging.put())))
				return false;
			stageW = desc.Width;
			stageH = desc.Height;
		}
		context->CopyResource(staging.get(), texture.get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
			return false;
		int cropX = 0;
		int cropY = 0;
		int cropW = static_cast<int>(desc.Width);
		int cropH = static_cast<int>(desc.Height);
		if (target)
		{
			RECT windowRect{};
			RECT clientRect{};
			POINT origin{};
			if (GetWindowRect(target, &windowRect) && GetClientRect(target, &clientRect) && ClientToScreen(target, &origin))
			{
				const int windowW = windowRect.right - windowRect.left;
				const int windowH = windowRect.bottom - windowRect.top;
				int clientX = origin.x - windowRect.left;
				int clientY = origin.y - windowRect.top;
				int clientW = clientRect.right;
				int clientH = clientRect.bottom;
				if (std::abs(static_cast<int>(desc.Width) - clientW) < 8 && std::abs(static_cast<int>(desc.Height) - clientH) < 8)
				{
					clientX = 0;
					clientY = 0;
				}
				else if (windowW > 0 && windowH > 0)
				{
					clientX = clientX * static_cast<int>(desc.Width) / windowW;
					clientY = clientY * static_cast<int>(desc.Height) / windowH;
					clientW = clientW * static_cast<int>(desc.Width) / windowW;
					clientH = clientH * static_cast<int>(desc.Height) / windowH;
				}
				if (clientW > 8 && clientH > 8)
				{
					const double aspect = 854.0 / 480.0;
					double contentW;
					double contentH;
					if (clientW / static_cast<double>(clientH) > aspect)
					{
						contentH = clientH;
						contentW = clientH * aspect;
					}
					else
					{
						contentW = clientW;
						contentH = clientW / aspect;
					}
					cropX = clientX + static_cast<int>((clientW - contentW) / 2.0);
					cropY = clientY + static_cast<int>((clientH - contentH) / 2.0);
					cropW = static_cast<int>(contentW);
					cropH = static_cast<int>(contentH);
				}
			}
		}
		if (cropX < 0)
			cropX = 0;
		if (cropY < 0)
			cropY = 0;
		if (cropX + cropW > static_cast<int>(desc.Width))
			cropW = static_cast<int>(desc.Width) - cropX;
		if (cropY + cropH > static_cast<int>(desc.Height))
			cropH = static_cast<int>(desc.Height) - cropY;
		constexpr int kOutW = 854;
		constexpr int kOutH = 480;
		std::vector<uint8_t> bgra(static_cast<size_t>(kOutW) * kOutH * 4);
		const auto* src = static_cast<const uint8_t*>(mapped.pData);
		for (int y = 0; y < kOutH; ++y)
		{
			const int sy = cropY + y * cropH / kOutH;
			for (int x = 0; x < kOutW; ++x)
			{
				const int sx = cropX + x * cropW / kOutW;
				memcpy(bgra.data() + (static_cast<size_t>(y) * kOutW + x) * 4, src + static_cast<size_t>(sy) * mapped.RowPitch + static_cast<size_t>(sx) * 4, 4);
			}
		}
		context->Unmap(staging.get(), 0);
		std::lock_guard<std::mutex> lock(latest.mutex);
		latest.bgra.swap(bgra);
		latest.width = kOutW;
		latest.height = kOutH;
		latest.serial++;
		return true;
	}
};

bool SendAll(SOCKET socket, const char* data, int size)
{
	int sent = 0;
	while (sent < size)
	{
		const int n = send(socket, data + sent, size - sent, 0);
		if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK)
		{
			Sleep(1);
			continue;
		}
		if (n <= 0)
			return false;
		sent += n;
	}
	return true;
}
}

int wmain(int argc, wchar_t** argv)
{
	const DWORD parentId = argc > 1 ? static_cast<DWORD>(_wtol(argv[1])) : 0;
	HANDLE parent = parentId ? OpenProcess(SYNCHRONIZE, FALSE, parentId) : nullptr;
	SetProcessDPIAware();
	winrt::init_apartment(winrt::apartment_type::multi_threaded);
	WSADATA wsa{};
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return 1;
	SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSocket == INVALID_SOCKET)
		return 1;
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(kPort);
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	int reuse = 1;
	setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse), sizeof(reuse));
	u_long nonblock = 1;
	ioctlsocket(listenSocket, FIONBIO, &nonblock);
	if (bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listenSocket, 1) != 0)
		return 1;

	Capture capture;
	LatestFrame latest;
	std::vector<uint8_t> jpeg;
	HWND opened = nullptr;
	while (true)
	{
		if (parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0)
			break;
		SOCKET client = accept(listenSocket, nullptr, nullptr);
		if (client == INVALID_SOCKET)
		{
			HWND window = nullptr;
			EnumWindows(FindPadWindow, reinterpret_cast<LPARAM>(&window));
			if (window && window != opened && capture.Open(window))
				opened = window;
			if (opened)
				capture.Take(latest);
			Sleep(4);
			continue;
		}
		u_long peek = 1;
		ioctlsocket(client, FIONBIO, &peek);
		uint64_t seen = 0;
		while (true)
		{
			if (parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0)
				break;
			char probe = 0;
			const int n = recv(client, &probe, 1, MSG_PEEK);
			if (n == 0 || (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK))
				break;
			HWND window = nullptr;
			EnumWindows(FindPadWindow, reinterpret_cast<LPARAM>(&window));
			if (window && window != opened && capture.Open(window))
				opened = window;
			capture.Take(latest);
			std::vector<uint8_t> bgra;
			int width = 0;
			int height = 0;
			uint64_t serial = 0;
			{
				std::lock_guard<std::mutex> lock(latest.mutex);
				serial = latest.serial;
				if (serial != seen && latest.width > 0)
				{
					bgra = latest.bgra;
					width = latest.width;
					height = latest.height;
				}
			}
			if (bgra.empty())
			{
				Sleep(4);
				continue;
			}
			seen = serial;
			if (!EncodeJpeg(bgra.data(), width, height, width * 4, jpeg) || jpeg.empty())
				continue;
			uint32_t size = htonl(static_cast<uint32_t>(jpeg.size()));
			if (!SendAll(client, reinterpret_cast<char*>(&size), 4) || !SendAll(client, reinterpret_cast<char*>(jpeg.data()), static_cast<int>(jpeg.size())))
				break;
		}
		closesocket(client);
	}
	capture.Close();
	closesocket(listenSocket);
	if (parent)
		CloseHandle(parent);
	return 0;
}
