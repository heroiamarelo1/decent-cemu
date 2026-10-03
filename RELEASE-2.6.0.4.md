Decent Cemu 2.6.0.4 is a fork of Cemu 2.6, not an official Cemu release.

## What's new

- **Less GamePad audio delay:** the phone and Cemu keep a shorter audio queue, so GamePad sound starts and stays closer to the picture.
- **GamePad calibration stays put:** when a minigame asks to reset the flat pose, Cemu accepts that only if the phone is actually flat. A phone held in another pose keeps the calibration from the start of the session.
- **MotionPlus holds orientation better:** fast swings no longer pull the resting zero off, and a still remote learns that zero quickly.
- **General bug fixes:** the Wii Remote pointer works when the remote is already connected as Cemu starts.

## Install

1. Close Cemu and back up your existing executable and `PadStream.exe`.
2. Put `Decent.Cemu.2.6.0.4.exe` and `PadStream.exe` in the same Cemu folder. For RetroBat, rename the Cemu executable to `Cemu.exe`.
3. Keep your existing settings, controller profiles, keys, games and saves.
4. Install `Decent.Cemu.Controller.apk` on the phone. It updates the previous app.
5. Connect the phone and PC to the same WiFi network. Open the app, enter the PC's WiFi IP if needed, then press **Video**.

## Notes

- Native Wii Remote, MotionPlus and Nunchuk support and Fast/Compatibility launch modes are retained.
- Cemu code: Mozilla Public License 2.0. Android app: MIT.
