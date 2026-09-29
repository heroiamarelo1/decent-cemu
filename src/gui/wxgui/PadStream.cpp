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
#include "GamePadViewStream.h"
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

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
	int poolW = 0;
	int poolH = 0;
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
		poolW = 0;
		poolH = 0;
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
			const auto itemSize = item.Size();
			poolW = itemSize.Width;
			poolH = itemSize.Height;
			pool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
				winrtDevice,
				winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
				2,
				itemSize);
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
		const auto content = frame.ContentSize();
		if (pool && content.Width >= 8 && content.Height >= 8 && (content.Width != poolW || content.Height != poolH))
		{
			pool.Recreate(
				winrtDevice,
				winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
				2,
				content);
			poolW = content.Width;
			poolH = content.Height;
			staging = nullptr;
			stageW = 0;
			stageH = 0;
			return false;
		}
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
		const int cropX = 0;
		const int cropY = 0;
		const int cropW = static_cast<int>(desc.Width);
		const int cropH = static_cast<int>(desc.Height);
		(void)target;
		int kOutW = 854;
		int kOutH = 480;
		HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\DecentCemuPadStream");
		if (mapping)
		{
			auto* config = static_cast<uint32_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 16));
			if (config && config[0] == 0x50535452 && config[1] >= 320 && config[2] >= 180)
			{
				kOutW = static_cast<int>(config[1] & ~1u);
				kOutH = static_cast<int>(config[2] & ~1u);
			}
			if (config)
				UnmapViewOfFile(config);
			CloseHandle(mapping);
		}
		std::vector<uint8_t> bgra(static_cast<size_t>(kOutW) * kOutH * 4, 0);
		const auto* src = static_cast<const uint8_t*>(mapped.pData);
		const double scaleW = static_cast<double>(kOutW) / cropW;
		const double scaleH = static_cast<double>(kOutH) / cropH;
		const double scale = scaleW < scaleH ? scaleW : scaleH;
		int destW = static_cast<int>(cropW * scale);
		int destH = static_cast<int>(cropH * scale);
		if (destW < 2)
			destW = 2;
		if (destH < 2)
			destH = 2;
		if (destW > kOutW)
			destW = kOutW;
		if (destH > kOutH)
			destH = kOutH;
		const int destX = (kOutW - destW) / 2;
		const int destY = (kOutH - destH) / 2;
		for (int y = 0; y < destH; ++y)
		{
			const int sy = cropY + y * cropH / destH;
			for (int x = 0; x < destW; ++x)
			{
				const int sx = cropX + x * cropW / destW;
				memcpy(bgra.data() + (static_cast<size_t>(destY + y) * kOutW + destX + x) * 4, src + static_cast<size_t>(sy) * mapped.RowPitch + static_cast<size_t>(sx) * 4, 4);
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

void KeepParameterSets(const std::vector<uint8_t>& annexb, std::vector<uint8_t>& config)
{
	if (!config.empty() || annexb.size() < 4)
		return;
	std::vector<uint8_t> sets;
	size_t i = 0;
	while (i + 3 < annexb.size())
	{
		size_t start = annexb.size();
		size_t nal = annexb.size();
		if (annexb[i] == 0 && annexb[i + 1] == 0 && annexb[i + 2] == 1)
		{
			start = i;
			nal = i + 3;
		}
		else if (i + 4 < annexb.size() && annexb[i] == 0 && annexb[i + 1] == 0 && annexb[i + 2] == 0 && annexb[i + 3] == 1)
		{
			start = i;
			nal = i + 4;
		}
		if (start == annexb.size())
		{
			++i;
			continue;
		}
		size_t next = nal;
		while (next + 3 < annexb.size())
		{
			const bool three = annexb[next] == 0 && annexb[next + 1] == 0 && annexb[next + 2] == 1;
			const bool four = next + 3 < annexb.size() && annexb[next] == 0 && annexb[next + 1] == 0 && annexb[next + 2] == 0 && annexb[next + 3] == 1;
			if (three || four)
				break;
			++next;
		}
		if (next + 3 >= annexb.size())
			next = annexb.size();
		if (nal < annexb.size() && ((annexb[nal] & 0x1F) == 7 || (annexb[nal] & 0x1F) == 8))
			sets.insert(sets.end(), annexb.begin() + static_cast<std::ptrdiff_t>(start), annexb.begin() + static_cast<std::ptrdiff_t>(next));
		i = next == start ? start + 1 : next;
	}
	if (!sets.empty())
		config.swap(sets);
}

void ToAnnexB(const uint8_t* data, size_t size, std::vector<uint8_t>& out)
{
	out.clear();
	if (size >= 4 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || (data[2] == 0 && data[3] == 1)))
	{
		out.assign(data, data + size);
		return;
	}
	size_t offset = 0;
	while (offset + 4 <= size)
	{
		const uint32_t nal = (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) | (uint32_t(data[offset + 2]) << 8) | data[offset + 3];
		offset += 4;
		if (nal == 0 || offset + nal > size)
			break;
		out.push_back(0);
		out.push_back(0);
		out.push_back(0);
		out.push_back(1);
		out.insert(out.end(), data + offset, data + offset + nal);
		offset += nal;
	}
}

bool SendPacket(SOCKET socket, uint8_t kind, const uint8_t* data, int size)
{
	if (size < 0 || size > 4000000)
		return false;
	const uint32_t length = htonl(static_cast<uint32_t>(size + 1));
	if (!SendAll(socket, reinterpret_cast<const char*>(&length), 4))
		return false;
	const char kindByte = static_cast<char>(kind);
	if (!SendAll(socket, &kindByte, 1))
		return false;
	return size == 0 || SendAll(socket, reinterpret_cast<const char*>(data), size);
}

class H264Encoder
{
public:
	~H264Encoder() { Close(); }

	bool Matches(int width, int height) const { return encoder && width == m_width && height == m_height; }

	bool Open(int width, int height)
	{
		Close();
		if ((width % 2) || (height % 2))
			return false;
		if (!started)
		{
			if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)))
				return false;
			started = true;
		}
		if (!CreateEncoder(width, height))
			return false;
		m_width = width;
		m_height = height;
		nv12.resize(static_cast<size_t>(width) * height * 3 / 2);
		return true;
	}

	bool HasConfig() const { return !config.empty(); }
	const std::vector<uint8_t>& Config() const { return config; }

	bool Encode(const uint8_t* bgra, std::vector<uint8_t>& annexb)
	{
		annexb.clear();
		if (!encoder)
			return false;
		ConvertNv12(bgra);
		IMFSample* sample = nullptr;
		IMFMediaBuffer* buffer = nullptr;
		if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(nv12.size()), &buffer)))
		{
			if (buffer) buffer->Release();
			if (sample) sample->Release();
			return false;
		}
		BYTE* dest = nullptr;
		DWORD maxLength = 0;
		if (FAILED(buffer->Lock(&dest, &maxLength, nullptr)))
		{
			buffer->Release();
			sample->Release();
			return false;
		}
		memcpy(dest, nv12.data(), nv12.size());
		buffer->Unlock();
		buffer->SetCurrentLength(static_cast<DWORD>(nv12.size()));
		sample->AddBuffer(buffer);
		buffer->Release();
		const LONGLONG stamp = frameIndex * 333333;
		sample->SetSampleTime(stamp);
		sample->SetSampleDuration(333333);
		frameIndex++;
		const HRESULT pushed = encoder->ProcessInput(0, sample, 0);
		sample->Release();
		if (FAILED(pushed))
			return false;

		MFT_OUTPUT_STREAM_INFO info{};
		encoder->GetOutputStreamInfo(0, &info);
		const bool provides = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
		IMFSample* owned = nullptr;
		IMFMediaBuffer* outBuffer = nullptr;
		MFT_OUTPUT_DATA_BUFFER output{};
		if (!provides)
		{
			if (FAILED(MFCreateSample(&owned)) || FAILED(MFCreateMemoryBuffer(info.cbSize ? info.cbSize : static_cast<DWORD>(nv12.size()), &outBuffer)))
			{
				if (outBuffer) outBuffer->Release();
				if (owned) owned->Release();
				return false;
			}
			owned->AddBuffer(outBuffer);
			output.pSample = owned;
		}
		DWORD status = 0;
		const HRESULT pulled = encoder->ProcessOutput(0, 1, &output, &status);
		if (pulled == MF_E_TRANSFORM_NEED_MORE_INPUT)
		{
			if (outBuffer) outBuffer->Release();
			if (owned) owned->Release();
			if (output.pSample && output.pSample != owned) output.pSample->Release();
			return false;
		}
		if (FAILED(pulled) || !output.pSample)
		{
			if (outBuffer) outBuffer->Release();
			if (owned) owned->Release();
			if (output.pSample && output.pSample != owned) output.pSample->Release();
			return false;
		}
		IMFMediaBuffer* encoded = nullptr;
		output.pSample->ConvertToContiguousBuffer(&encoded);
		BYTE* bytes = nullptr;
		DWORD length = 0;
		if (encoded && SUCCEEDED(encoded->Lock(&bytes, nullptr, &length)) && length > 0)
		{
			ToAnnexB(bytes, length, annexb);
			encoded->Unlock();
			KeepParameterSets(annexb, config);
		}
		if (encoded) encoded->Release();
		if (outBuffer) outBuffer->Release();
		if (owned) owned->Release();
		else if (output.pSample) output.pSample->Release();
		return !annexb.empty();
	}

private:
	IMFTransform* encoder = nullptr;
	int m_width = 0;
	int m_height = 0;
	long long frameIndex = 0;
	std::vector<uint8_t> nv12;
	std::vector<uint8_t> config;
	bool started = false;

	void Close()
	{
		if (encoder)
		{
			encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
			encoder->Release();
			encoder = nullptr;
		}
		m_width = 0;
		m_height = 0;
		frameIndex = 0;
		config.clear();
	}

	bool ConfigureEncoder(IMFTransform* candidate, int width, int height)
	{
		ICodecAPI* codec = nullptr;
		if (SUCCEEDED(candidate->QueryInterface(IID_PPV_ARGS(&codec))))
		{
			VARIANT value;
			VariantInit(&value);
			value.vt = VT_BOOL;
			value.boolVal = VARIANT_TRUE;
			codec->SetValue(&CODECAPI_AVLowLatencyMode, &value);
			value.vt = VT_UI4;
			value.ulVal = 0;
			codec->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &value);
			const uint32_t pixels = static_cast<uint32_t>(width) * static_cast<uint32_t>(height);
			value.ulVal = pixels >= 1920 * 1000 ? 10000000u : pixels >= 1200 * 700 ? 6000000u : 2500000u;
			codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &value);
			VariantClear(&value);
			codec->Release();
		}

		IMFMediaType* outType = nullptr;
		if (FAILED(MFCreateMediaType(&outType)))
			return false;
		outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
		outType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<uint32_t>(width) * static_cast<uint32_t>(height) >= 1920u * 1000u ? 10000000u : 4000000u);
		MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, width, height);
		MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, 30, 1);
		MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
		HRESULT hr = candidate->SetOutputType(0, outType, 0);
		UINT32 blob = 0;
		if (SUCCEEDED(hr) && SUCCEEDED(outType->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blob)) && blob > 0)
		{
			std::vector<uint8_t> raw(blob);
			outType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, raw.data(), blob, &blob);
			ToAnnexB(raw.data(), blob, config);
		}
		outType->Release();
		if (FAILED(hr))
			return false;

		IMFMediaType* inType = nullptr;
		if (FAILED(MFCreateMediaType(&inType)))
			return false;
		inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
		MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, width, height);
		MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, 30, 1);
		MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		inType->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width));
		hr = candidate->SetInputType(0, inType, 0);
		inType->Release();
		if (FAILED(hr))
			return false;
		candidate->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
		candidate->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
		return true;
	}

	bool CreateEncoder(int width, int height)
	{
		MFT_REGISTER_TYPE_INFO inputInfo{ MFMediaType_Video, MFVideoFormat_NV12 };
		MFT_REGISTER_TYPE_INFO outputInfo{ MFMediaType_Video, MFVideoFormat_H264 };
		IMFActivate** activate = nullptr;
		UINT32 count = 0;
		if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &inputInfo, &outputInfo, &activate, &count)))
		{
			for (UINT32 i = 0; i < count && !encoder; ++i)
			{
				IMFTransform* candidate = nullptr;
				if (SUCCEEDED(activate[i]->ActivateObject(IID_PPV_ARGS(&candidate))) && candidate)
				{
					if (ConfigureEncoder(candidate, width, height))
						encoder = candidate;
					else
						candidate->Release();
				}
			}
			for (UINT32 i = 0; i < count; ++i)
				activate[i]->Release();
			CoTaskMemFree(activate);
		}
		if (!encoder)
		{
			IMFTransform* candidate = nullptr;
			if (SUCCEEDED(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&candidate))) && candidate)
			{
				if (ConfigureEncoder(candidate, width, height))
					encoder = candidate;
				else
					candidate->Release();
			}
		}
		return encoder != nullptr;
	}

	void ConvertNv12(const uint8_t* bgra)
	{
		uint8_t* y = nv12.data();
		uint8_t* uv = y + static_cast<size_t>(m_width) * m_height;
		for (int row = 0; row < m_height; ++row)
		{
			for (int col = 0; col < m_width; ++col)
			{
				const uint8_t* pixel = bgra + (static_cast<size_t>(row) * m_width + col) * 4;
				const int b = pixel[0];
				const int g = pixel[1];
				const int r = pixel[2];
				y[static_cast<size_t>(row) * m_width + col] = static_cast<uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
				if ((row % 2) == 0 && (col % 2) == 0)
				{
					const size_t uvIndex = static_cast<size_t>(row / 2) * m_width + col;
					uv[uvIndex] = static_cast<uint8_t>(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
					uv[uvIndex + 1] = static_cast<uint8_t>(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
				}
			}
		}
	}
};

int wmain(int argc, wchar_t** argv)
{
	const DWORD parentId = argc > 1 ? static_cast<DWORD>(_wtol(argv[1])) : 0;
	const HWND mainWindow = argc > 2 ? reinterpret_cast<HWND>(static_cast<uintptr_t>(_wcstoui64(argv[2], nullptr, 10))) : nullptr;
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
	H264Encoder encoder;
	HWND opened = nullptr;
	while (true)
	{
		if (parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0)
			break;
		sockaddr_in peer{};
		int peerLength = sizeof(peer);
		SOCKET client = accept(listenSocket, reinterpret_cast<sockaddr*>(&peer), &peerLength);
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
		DWORD windowProcessId = 0;
		if (mainWindow && GetWindowThreadProcessId(mainWindow, &windowProcessId) && windowProcessId == parentId)
			PostMessageW(mainWindow, kAndroidPadVideoConnectedMessage, ntohl(peer.sin_addr.s_addr), GetCurrentProcessId());
		u_long peek = 1;
		ioctlsocket(client, FIONBIO, &peek);
		int nodelay = 1;
		setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&nodelay), sizeof(nodelay));
		uint64_t seen = 0;
		bool headerSent = false;
		bool preferJpeg = false;
		int encodeMisses = 0;
		int sentW = 0;
		int sentH = 0;
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
					bgra.swap(latest.bgra);
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
			if (width != sentW || height != sentH)
			{
				sentW = width;
				sentH = height;
				headerSent = false;
				encodeMisses = 0;
				preferJpeg = false;
			}
			// Native stays JPEG. Larger presets use H.264.
			if ((width == 854 && height == 480) || preferJpeg || !encoder.Matches(width, height))
			{
				headerSent = false;
				if ((width == 854 && height == 480) || preferJpeg || !encoder.Open(width, height))
				{
					if (!(width == 854 && height == 480))
						preferJpeg = true;
					std::vector<uint8_t> jpeg;
					if (!EncodeJpeg(bgra.data(), width, height, width * 4, jpeg) || jpeg.empty())
						continue;
					const uint32_t length = htonl(static_cast<uint32_t>(jpeg.size()));
					if (!SendAll(client, reinterpret_cast<const char*>(&length), 4) || !SendAll(client, reinterpret_cast<const char*>(jpeg.data()), static_cast<int>(jpeg.size())))
						break;
					continue;
				}
			}
			const std::vector<uint8_t>* config = encoder.HasConfig() ? &encoder.Config() : nullptr;
			std::vector<uint8_t> access;
			if (!encoder.Encode(bgra.data(), access) || access.empty())
			{
				if (++encodeMisses > 20)
					preferJpeg = true;
				continue;
			}
			encodeMisses = 0;
			if (!config)
				config = &access;
			if (!headerSent)
			{
				uint8_t sizePacket[4] = {
					static_cast<uint8_t>((width >> 8) & 0xFF), static_cast<uint8_t>(width & 0xFF),
					static_cast<uint8_t>((height >> 8) & 0xFF), static_cast<uint8_t>(height & 0xFF)
				};
				if (!SendPacket(client, 0, sizePacket, 4) || !SendPacket(client, 1, config->data(), static_cast<int>(config->size())))
					break;
				headerSent = true;
			}
			if (!SendPacket(client, 2, access.data(), static_cast<int>(access.size())))
				break;
		}
		closesocket(client);
		DWORD disconnectedWindowProcessId = 0;
		if (mainWindow && GetWindowThreadProcessId(mainWindow, &disconnectedWindowProcessId) && disconnectedWindowProcessId == parentId)
			PostMessageW(mainWindow, kAndroidPadVideoConnectedMessage, 0, GetCurrentProcessId());
	}
	capture.Close();
	closesocket(listenSocket);
	if (parent)
		CloseHandle(parent);
	return 0;
}
