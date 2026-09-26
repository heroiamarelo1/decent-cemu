#pragma once

// Sends the GamePad View window to a phone on TCP port 26761.
// Each frame is a big-endian uint32 length followed by a JPEG.
void GamePadViewStream_Start();
