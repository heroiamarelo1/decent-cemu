#pragma once

#include <cstdint>

// Sends the GamePad mix to the phone speaker. The TV mix is not included.
void GamePadAudioStream_Start();
void GamePadAudioStream_Submit(const int16_t* interleavedStereo, int frames);
