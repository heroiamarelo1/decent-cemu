Decent Cemu 2.6.0.2 is a fork of Cemu 2.6, not an official Cemu release.

## Android GamePad motion fixes

- Corrected motion axes, including reversed left/right tilt.
- Orientation, acceleration and gyro now use a consistent coordinate conversion.
- Improved sensor timing, slow movement and orientation calibration handling.
- Confirmed by hands-on testing: Nintendo Land — Donkey Kong steering; Game & Wario — Fronks; Wii Party U — Spiked-Ball Brawl.

## Install

1. Close Cemu and back up your existing executable.
2. Put `Decent.Cemu.2.6.0.2.exe` in your existing Cemu folder. For RetroBat, rename it to `Cemu.exe`.
3. Keep your existing settings, controller profiles, keys, games and saves.
4. Install the attached `Decent.Cemu.Controller.apk`. If you already use the tested `1.1-motion-preview` app, you can keep it.
5. Calibrate the phone flat with its screen facing up. Keep the app visible during motion gameplay.

## Notes

- Native Wii Remote, MotionPlus and Nunchuk support and Fast/Compatibility launch modes are retained.
- The experimental Donkey Kong horizontal guard is not included. Local testers should leave it disabled in Graphic packs. Donkey Kong should be played with the GamePad upright; its near-horizontal behavior remains under investigation.
- Motion is still beta. Not every game, camera axis or motion feature has been verified; full original GamePad compatibility is not claimed.
- Existing phone limitations remain, including microphone, camera, NFC and heading drift without a validated compass reference.
- Sensor traces are opt-in through `DECENT_MOTION_TRACE`; field-isolation experiments are removed from this release.

Cemu code: Mozilla Public License 2.0. Android app: MIT.
