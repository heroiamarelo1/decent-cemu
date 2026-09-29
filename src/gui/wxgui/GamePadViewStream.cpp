#include "wxgui/GamePadViewStream.h"
#include "input/api/DSU/PadStreamControl.h"

#include <atomic>
#include <thread>

#if BOOST_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
std::atomic<bool> g_started{false};
std::atomic<uint32_t> g_captureProcessId{0};

#if BOOST_OS_WINDOWS
void StartCaptureProcess(uintptr_t mainWindow)
{
	wchar_t path[MAX_PATH]{};
	if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0)
		return;
	wchar_t* slash = wcsrchr(path, L'\\');
	if (!slash)
		return;
	*(slash + 1) = 0;
	if (wcslen(path) + 20 >= MAX_PATH)
		return;
	wcscat_s(path, L"PadStream.exe");
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
	{
		*(slash + 1) = 0;
		wcscat_s(path, L"PadCapture.exe");
		if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
			return;
	}

	wchar_t command[MAX_PATH + 80]{};
	_snwprintf_s(command, _TRUNCATE, L"\"%s\" %lu %llu", path, GetCurrentProcessId(), static_cast<unsigned long long>(mainWindow));
	STARTUPINFOW start{};
	start.cb = sizeof(start);
	start.dwFlags = STARTF_USESHOWWINDOW;
	start.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION process{};
	if (!CreateProcessW(path, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &start, &process))
		return;
	g_captureProcessId.store(process.dwProcessId);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
}
#endif
}

void GamePadViewStream_Start(uintptr_t mainWindow)
{
	if (g_started.exchange(true))
		return;
#if BOOST_OS_WINDOWS
	std::thread(StartCaptureProcess, mainWindow).detach();
#endif
}

bool GamePadViewStream_IsCaptureProcess(uint32_t pid)
{
	return pid != 0 && pid == g_captureProcessId.load();
}

namespace
{
std::atomic<int> g_streamWidth{0};
std::atomic<int> g_streamHeight{0};
std::atomic<bool> g_streamPending{false};

void PublishStream(int width, int height)
{
#if BOOST_OS_WINDOWS
	HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 16, L"Local\\DecentCemuPadStream");
	if (!mapping)
		return;
	auto* config = static_cast<uint32_t*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 16));
	if (config)
	{
		config[0] = 0x50535452;
		config[1] = static_cast<uint32_t>(width);
		config[2] = static_cast<uint32_t>(height);
		config[3]++;
		UnmapViewOfFile(config);
	}
	// Keep the mapping alive for the process lifetime.
	static HANDLE retained = nullptr;
	if (!retained)
		retained = mapping;
	else
		CloseHandle(mapping);
#else
	(void)width;
	(void)height;
#endif
}
}

void PadStream_NotePreset(uint8_t preset)
{
	int width = 854;
	int height = 480;
	if (preset == 1)
	{
		width = 1282;
		height = 720;
	}
	else if (preset == 2)
	{
		width = 1922;
		height = 1080;
	}
	else if (preset != 0)
		return;
	if (g_streamWidth.load() == width && g_streamHeight.load() == height)
		return;
	g_streamWidth.store(width);
	g_streamHeight.store(height);
	g_streamPending.store(true);
	PublishStream(width, height);
}

bool PadStream_TakeSize(int& width, int& height)
{
	if (!g_streamPending.exchange(false))
		return false;
	width = g_streamWidth.load();
	height = g_streamHeight.load();
	return width > 0 && height > 0;
}
