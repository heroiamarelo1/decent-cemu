#pragma once
#include <cstdint>

// Phone picture size. 0 = 854x480, 1 = 1282x720, 2 = 1922x1080. Aspect stays 854:480.
void PadStream_NotePreset(uint8_t preset);
bool PadStream_TakeSize(int& width, int& height);
