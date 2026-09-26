#pragma once

#include <string>

// Routes Wii U TV audio to the primary monitor's output and GamePad audio to the
// second monitor's output when Windows exposes a device for that display.
namespace MonitorAudioRouting
{
	struct ApplyResult
	{
		bool applied = false;
		// True when a second monitor exists but no render endpoint is tied to it.
		bool secondMonitorHasNoAudio = false;
		bool noSecondMonitor = false;
		std::wstring padDeviceName;
		std::string message;
	};

	// Updates GetConfig() tv_device / pad_device / pad_volume from the current
	// display layout. When enabled is false, clears pad_device (GamePad muted).
	ApplyResult ApplyToConfig(bool enabled);

	// Recreates g_tvAudio / g_padAudio from the current config if they already exist.
	void RecreateLiveDevices();
}
