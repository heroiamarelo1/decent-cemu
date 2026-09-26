#include "MonitorAudioRouting.h"

#include "IAudioAPI.h"
#include "config/CemuConfig.h"
#include "util/helpers/helpers.h"

// Shared with RecreateLiveDevices (AX_SAMPLES_PER_3MS_48KHZ * 4 frames).
static constexpr sint32 kAxOutSamplesPerBlock = 144 * 4;

#if BOOST_OS_WINDOWS
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propkey.h>
#include <initguid.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <wrl/client.h>
#include <algorithm>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "uuid.lib")

namespace
{
	struct EndpointInfo
	{
		std::wstring id;
		std::wstring name;
		GUID containerId{};
		bool hasContainer = false;
	};

	bool GetGdiDeviceName(HMONITOR monitor, std::wstring& outName)
	{
		MONITORINFOEXW info{};
		info.cbSize = sizeof(info);
		if (!GetMonitorInfoW(monitor, &info))
			return false;
		outName = info.szDevice;
		return true;
	}

	bool GetMonitorDevicePath(HMONITOR monitor, std::wstring& outPath, std::wstring* outFriendlyName = nullptr)
	{
		std::wstring gdiName;
		if (!GetGdiDeviceName(monitor, gdiName))
			return false;

		UINT32 pathCount = 0, modeCount = 0;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
			return false;
		if (pathCount == 0 || modeCount == 0)
			return false;

		std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
		std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
		if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
			return false;

		for (UINT32 i = 0; i < pathCount; ++i)
		{
			DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
			sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
			sourceName.header.size = sizeof(sourceName);
			sourceName.header.adapterId = paths[i].sourceInfo.adapterId;
			sourceName.header.id = paths[i].sourceInfo.id;
			if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS)
				continue;
			if (_wcsicmp(sourceName.viewGdiDeviceName, gdiName.c_str()) != 0)
				continue;

			DISPLAYCONFIG_TARGET_DEVICE_NAME targetName{};
			targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
			targetName.header.size = sizeof(targetName);
			targetName.header.adapterId = paths[i].targetInfo.adapterId;
			targetName.header.id = paths[i].targetInfo.id;
			if (DisplayConfigGetDeviceInfo(&targetName.header) != ERROR_SUCCESS)
				continue;
			if (outFriendlyName && targetName.monitorFriendlyDeviceName[0] != L'\0')
				*outFriendlyName = targetName.monitorFriendlyDeviceName;
			if (targetName.monitorDevicePath[0] == L'\0')
				return false; // matched this display; no PnP path (e.g. some virtual monitors)
			outPath = targetName.monitorDevicePath;
			return true;
		}
		return false;
	}

	bool GetContainerIdFromDevicePath(const std::wstring& devicePath, GUID& outId)
	{
		DEVPROPTYPE propType = 0;
		ULONG size = sizeof(GUID);
		const CONFIGRET cr = CM_Get_Device_Interface_PropertyW(
			devicePath.c_str(),
			&DEVPKEY_Device_ContainerId,
			&propType,
			reinterpret_cast<PBYTE>(&outId),
			&size,
			0);
		return cr == CR_SUCCESS && propType == DEVPROP_TYPE_GUID;
	}

	bool GetMonitorContainerId(HMONITOR monitor, GUID& outId, std::wstring* outFriendlyName = nullptr)
	{
		std::wstring path;
		if (!GetMonitorDevicePath(monitor, path, outFriendlyName))
			return false;
		return GetContainerIdFromDevicePath(path, outId);
	}

	const EndpointInfo* FindEndpointByContainer(const std::vector<EndpointInfo>& endpoints, const GUID& containerId)
	{
		for (const auto& ep : endpoints)
		{
			if (ep.hasContainer && InlineIsEqualGUID(ep.containerId, containerId))
				return &ep;
		}
		return nullptr;
	}

	const EndpointInfo* FindEndpointByFriendlyNameHint(const std::vector<EndpointInfo>& endpoints, const std::wstring& monitorName)
	{
		if (monitorName.empty())
			return nullptr;
		for (const auto& ep : endpoints)
		{
			if (ep.name.empty())
				continue;
			if (ep.name.find(monitorName) != std::wstring::npos)
				return &ep;
		}
		return nullptr;
	}

	std::vector<EndpointInfo> EnumerateRenderEndpoints()
	{
		std::vector<EndpointInfo> result;
		Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
		if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
			return result;

		Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
		if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)))
			return result;

		UINT count = 0;
		collection->GetCount(&count);
		for (UINT i = 0; i < count; ++i)
		{
			Microsoft::WRL::ComPtr<IMMDevice> device;
			if (FAILED(collection->Item(i, &device)))
				continue;

			EndpointInfo info;
			LPWSTR id = nullptr;
			if (SUCCEEDED(device->GetId(&id)) && id)
			{
				info.id = id;
				CoTaskMemFree(id);
			}

			Microsoft::WRL::ComPtr<IPropertyStore> props;
			if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)))
			{
				PROPVARIANT var;
				PropVariantInit(&var);
				if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR && var.pwszVal)
					info.name = var.pwszVal;
				PropVariantClear(&var);

				PropVariantInit(&var);
				if (SUCCEEDED(props->GetValue(PKEY_Device_ContainerId, &var)) && var.vt == VT_CLSID && var.puuid)
				{
					info.containerId = *var.puuid;
					info.hasContainer = true;
				}
				PropVariantClear(&var);
			}

			if (!info.id.empty())
				result.push_back(std::move(info));
		}
		return result;
	}

	std::wstring MatchCemuDeviceId(const std::wstring& endpointId, const std::wstring& friendlyName)
	{
		const auto api = static_cast<IAudioAPI::AudioAPI>(GetConfig().audio_api);
		if (!IAudioAPI::IsAudioAPIAvailable(api))
			return {};

		const auto devices = IAudioAPI::GetDevices(api);
		auto pick = [&](const auto& pred) -> std::wstring
		{
			for (const auto& d : devices)
			{
				if (!d)
					continue;
				if (pred(*d))
					return d->GetIdentifier();
			}
			return {};
		};

		// Prefer exact endpoint id (Cubeb / some backends).
		auto id = pick([&](const IAudioAPI::DeviceDescription& d)
		{
			return d.GetIdentifier() == endpointId;
		});
		if (!id.empty())
			return id;

		// XAudio builds a long \\?\...# path that still contains the MMDEVAPI GUID chunk.
		id = pick([&](const IAudioAPI::DeviceDescription& d)
		{
			const auto& ident = d.GetIdentifier();
			return !endpointId.empty() && ident.find(endpointId) != std::wstring::npos;
		});
		if (!id.empty())
			return id;

		// Extract {0.0.0.00000000}.{guid} from the endpoint id and search for that.
		const auto brace = endpointId.find(L"{0.0.0.00000000}");
		if (brace != std::wstring::npos)
		{
			const auto chunk = endpointId.substr(brace);
			id = pick([&](const IAudioAPI::DeviceDescription& d)
			{
				return d.GetIdentifier().find(chunk) != std::wstring::npos;
			});
			if (!id.empty())
				return id;
		}

		if (!friendlyName.empty())
		{
			id = pick([&](const IAudioAPI::DeviceDescription& d)
			{
				return _wcsicmp(d.GetName().c_str(), friendlyName.c_str()) == 0;
			});
			if (!id.empty())
				return id;
		}

		return {};
	}

	struct MonitorPair
	{
		HMONITOR primary = nullptr;
		HMONITOR secondary = nullptr;
	};

	MonitorPair FindPrimaryAndSecondaryMonitors()
	{
		MonitorPair pair;
		struct Ctx { MonitorPair* pair; } ctx{ &pair };
		EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMon, HDC, LPRECT, LPARAM lParam) -> BOOL
		{
			auto* c = reinterpret_cast<Ctx*>(lParam);
			MONITORINFO mi{};
			mi.cbSize = sizeof(mi);
			if (!GetMonitorInfoW(hMon, &mi))
				return TRUE;
			if (mi.dwFlags & MONITORINFOF_PRIMARY)
				c->pair->primary = hMon;
			else if (!c->pair->secondary)
				c->pair->secondary = hMon;
			return TRUE;
		}, reinterpret_cast<LPARAM>(&ctx));
		return pair;
	}
}
#endif // BOOST_OS_WINDOWS

MonitorAudioRouting::ApplyResult MonitorAudioRouting::ApplyToConfig(bool enabled)
{
	ApplyResult result;
	auto& config = GetConfig();

	if (!enabled)
	{
		config.pad_device.clear();
		result.applied = true;
		result.message = "GamePad audio routing to the second monitor is off.";
		return result;
	}

#if !BOOST_OS_WINDOWS
	result.message = "Second-monitor audio routing is only available on Windows.";
	return result;
#else
	const auto monitors = FindPrimaryAndSecondaryMonitors();
	if (!monitors.secondary)
	{
		result.noSecondMonitor = true;
		result.message = "No second monitor was found. GamePad audio was not changed.";
		return result;
	}

	const auto endpoints = EnumerateRenderEndpoints();
	if (endpoints.empty())
	{
		result.message = "No active audio output devices were found.";
		return result;
	}

	// TV -> primary monitor audio (or default driver if none is bound).
	std::wstring tvId = L"default";
	if (monitors.primary)
	{
		GUID primaryContainer{};
		std::wstring primaryFriendly;
		const bool hasPrimaryContainer = GetMonitorContainerId(monitors.primary, primaryContainer, &primaryFriendly);
		const EndpointInfo* tvEp = nullptr;
		if (hasPrimaryContainer)
			tvEp = FindEndpointByContainer(endpoints, primaryContainer);
		if (!tvEp)
			tvEp = FindEndpointByFriendlyNameHint(endpoints, primaryFriendly);
		if (tvEp)
		{
			auto matched = MatchCemuDeviceId(tvEp->id, tvEp->name);
			if (!matched.empty())
				tvId = std::move(matched);
		}
	}
	config.tv_device = tvId;

	// GamePad -> second monitor audio (container id, then friendly-name hint).
	GUID secondaryContainer{};
	std::wstring secondaryFriendly;
	const bool hasSecondaryContainer = GetMonitorContainerId(monitors.secondary, secondaryContainer, &secondaryFriendly);
	if (!hasSecondaryContainer && secondaryFriendly.empty())
	{
		// Still try to read the friendly name when the PnP path is missing.
		std::wstring unusedPath;
		GetMonitorDevicePath(monitors.secondary, unusedPath, &secondaryFriendly);
	}

	const EndpointInfo* padEp = nullptr;
	if (hasSecondaryContainer)
		padEp = FindEndpointByContainer(endpoints, secondaryContainer);
	if (!padEp)
		padEp = FindEndpointByFriendlyNameHint(endpoints, secondaryFriendly);

	if (!padEp)
	{
		result.secondMonitorHasNoAudio = true;
		result.message = "The second monitor has no associated audio device. GamePad audio stays disabled.";
		config.pad_device.clear();
		return result;
	}

	auto padId = MatchCemuDeviceId(padEp->id, padEp->name);
	if (padId.empty())
	{
		result.secondMonitorHasNoAudio = true;
		result.message = "Found a second-monitor audio device, but it is not available in the current audio backend. GamePad audio stays disabled.";
		config.pad_device.clear();
		return result;
	}

	config.pad_device = padId;
	result.padDeviceName = padEp->name;
	if (config.pad_volume <= 0)
		config.pad_volume = std::max(config.tv_volume, 50);

	result.applied = true;
	result.message = "GamePad audio plays on the second monitor; TV audio plays on the primary.";
	return result;
#endif
}

void MonitorAudioRouting::RecreateLiveDevices()
{
	std::unique_lock lock(g_audioMutex);
	const bool hadTv = static_cast<bool>(g_tvAudio);
	const bool hadPad = static_cast<bool>(g_padAudio);
	if (!hadTv && !hadPad)
		return;

	const bool tvPlaying = hadTv && g_tvAudio;
	const bool padPlaying = hadPad && g_padAudio;
	// Preserve play state approximately: if a device existed, recreate and Play().
	(void)tvPlaying;
	(void)padPlaying;

	if (g_tvAudio)
	{
		g_tvAudio->Stop();
		g_tvAudio.reset();
	}
	if (g_padAudio)
	{
		g_padAudio->Stop();
		g_padAudio.reset();
	}

	try
	{
		g_tvAudio = IAudioAPI::CreateDeviceFromConfig(IAudioAPI::AudioType::TV, 48000, kAxOutSamplesPerBlock, 16);
		if (g_tvAudio)
			g_tvAudio->Play();
	}
	catch (const std::runtime_error& ex)
	{
		cemuLog_log(LogType::Force, "MonitorAudioRouting: can't recreate TV audio: {}", ex.what());
	}

	g_padVolume = GetConfig().pad_volume;
	try
	{
		g_padAudio = IAudioAPI::CreateDeviceFromConfig(IAudioAPI::AudioType::Gamepad, 48000, kAxOutSamplesPerBlock, 16);
		if (g_padAudio)
			g_padAudio->Play();
	}
	catch (const std::runtime_error& ex)
	{
		cemuLog_log(LogType::Force, "MonitorAudioRouting: can't recreate GamePad audio: {}", ex.what());
	}
}
