#pragma once

// Sends the GamePad View window to a phone on TCP port 26761.
// Each frame is a big-endian uint32 length followed by a JPEG.
#include <cstdint>
inline constexpr uint32_t kAndroidPadVideoConnectedMessage = 0x8346;
void GamePadViewStream_Start(uintptr_t mainWindow);
bool GamePadViewStream_IsCaptureProcess(uint32_t pid);
