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

#if BOOST_OS_WINDOWS
void StartCaptureProcess()
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

	wchar_t command[MAX_PATH + 32]{};
	_snwprintf_s(command, _TRUNCATE, L"\"%s\" %lu", path, GetCurrentProcessId());
	STARTUPINFOW start{};
	start.cb = sizeof(start);
	start.dwFlags = STARTF_USESHOWWINDOW;
	start.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION process{};
	if (!CreateProcessW(path, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &start, &process))
		return;
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
}
#endif
}

void GamePadViewStream_Start()
{
	if (g_started.exchange(true))
		return;
#if BOOST_OS_WINDOWS
	std::thread(StartCaptureProcess).detach();
#endif
}
