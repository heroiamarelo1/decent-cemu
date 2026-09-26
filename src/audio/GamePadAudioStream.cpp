#include "GamePadAudioStream.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#if BOOST_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace
{
std::once_flag g_started;

#if BOOST_OS_WINDOWS
constexpr int kPort = 26762;
constexpr int kMaxFrames = 4800;

std::mutex g_mutex;
std::vector<int16_t> g_pending;
std::atomic<bool> g_running{true};

void DropOldFrames()
{
	const int maxSamples = kMaxFrames * 2;
	if (static_cast<int>(g_pending.size()) > maxSamples)
		g_pending.erase(g_pending.begin(), g_pending.end() - maxSamples);
}

void Serve()
{
	WSADATA wsa{};
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return;
	SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSocket == INVALID_SOCKET)
		return;
	int reuse = 1;
	setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse), sizeof(reuse));
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(kPort);
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listenSocket, 1) != 0)
	{
		closesocket(listenSocket);
		return;
	}
	u_long nonblock = 1;
	ioctlsocket(listenSocket, FIONBIO, &nonblock);

	SOCKET client = INVALID_SOCKET;
	std::vector<int16_t> outgoing;
	size_t readByte = 0;
	while (g_running.load())
	{
		if (client == INVALID_SOCKET)
		{
			client = accept(listenSocket, nullptr, nullptr);
			if (client != INVALID_SOCKET)
			{
				int nodelay = 1;
				setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&nodelay), sizeof(nodelay));
				u_long clientNonblock = 1;
				ioctlsocket(client, FIONBIO, &clientNonblock);
				std::lock_guard<std::mutex> lock(g_mutex);
				g_pending.clear();
				outgoing.clear();
				readByte = 0;
			}
		}

		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!g_pending.empty())
			{
				outgoing.insert(outgoing.end(), g_pending.begin(), g_pending.end());
				g_pending.clear();
			}
		}

		if (client != INVALID_SOCKET && readByte < outgoing.size() * sizeof(int16_t))
		{
			const int avail = static_cast<int>(outgoing.size() * sizeof(int16_t) - readByte);
			const int chunk = avail > 4096 ? 4096 : avail;
			const int sent = send(client, reinterpret_cast<char*>(outgoing.data()) + readByte, chunk, 0);
			if (sent > 0)
			{
				readByte += static_cast<size_t>(sent);
				const size_t done = readByte / sizeof(int16_t);
				if (done > 0)
				{
					outgoing.erase(outgoing.begin(), outgoing.begin() + static_cast<std::ptrdiff_t>(done));
					readByte -= done * sizeof(int16_t);
				}
			}
			else
			{
				const int error = WSAGetLastError();
				if (sent == 0 || (error != WSAEWOULDBLOCK && error != WSAETIMEDOUT))
				{
					closesocket(client);
					client = INVALID_SOCKET;
					outgoing.clear();
					readByte = 0;
				}
				else
					Sleep(2);
			}
		}
		else
			Sleep(2);
	}
	if (client != INVALID_SOCKET)
		closesocket(client);
	closesocket(listenSocket);
}
#endif
}

void GamePadAudioStream_Start()
{
#if BOOST_OS_WINDOWS
	std::call_once(g_started, [] { std::thread(Serve).detach(); });
#else
	(void)g_started;
#endif
}

void GamePadAudioStream_Submit(const int16_t* interleavedStereo, int frames)
{
#if BOOST_OS_WINDOWS
	if (!interleavedStereo || frames <= 0)
		return;
	GamePadAudioStream_Start();
	std::lock_guard<std::mutex> lock(g_mutex);
	g_pending.insert(g_pending.end(), interleavedStereo, interleavedStereo + frames * 2);
	DropOldFrames();
#else
	(void)interleavedStereo;
	(void)frames;
#endif
}
