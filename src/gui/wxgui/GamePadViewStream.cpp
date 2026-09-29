#include "wxgui/GamePadViewStream.h"

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
	if (wcslen(path) + 16 >= MAX_PATH)
		return;
	wcscat_s(path, L"PadCapture.exe");
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
		return;

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
