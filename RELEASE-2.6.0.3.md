Decent Cemu 2.6.0.3 is a fork of Cemu 2.6, not an official Cemu release.

## What's new

- **Automatic Android GamePad connection:** connect the phone app's Video stream and Cemu discovers that phone's GamePad. Cemu attaches it automatically when no GamePad controller is connected. Existing game controller profiles and mappings are preserved.
- **Android Back exits fullscreen:** the system Back button leaves fullscreen; the two-finger gesture continues to work.
- **Nintendo Land microphone:** fixed the simulated microphone input used by the Donkey Kong blow section. Confirmed in hands-on play.
- Includes the Android motion-axis fixes from 2.6.0.2.

## Install

1. Close Cemu and back up your existing executable and `PadStream.exe`.
2. Put `Decent.Cemu.2.6.0.3.exe` and the attached `PadStream.exe` in the same Cemu folder. For RetroBat, rename the Cemu executable to `Cemu.exe`.
3. Keep your existing settings, controller profiles, keys, games and saves.
4. Install the attached `Decent.Cemu.Controller.apk` on the phone.
5. Connect the phone and PC to the same WiFi network. Open the app, enter the PC's WiFi IP if needed, then press **Video**. Cemu will connect its GamePad input automatically when no other GamePad controller is connected.

## Notes

- Native Wii Remote, MotionPlus and Nunchuk support and Fast/Compatibility launch modes are retained.
- Confirmed motion tests: Nintendo Land — Donkey Kong steering; Game & Wario — Fronks; Wii Party U — Spiked-Ball Brawl. Motion remains beta; full original GamePad compatibility is not claimed.
- Cemu code: Mozilla Public License 2.0. Android app: MIT.
