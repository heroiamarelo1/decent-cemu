# Decent Cemu - The Cemu that sorts it out

![Nintendo Land](banner.png)

This is a vibe-coded fork version of Cemu, build so you can have features the official release just doesn't give you. AI contributions are welcome.

## What it has

- Native Wii Remote, MotionPlus and Nunchuk support, with automatic setup (Dolphinbar recommended)
- An Android app that acts as the GamePad, including gyroscope, touchscreen and audio
- A MotionPlus infrared simulator for a remote pointed at the floor (Minus + B turns it on and off)
- Fixes for Wii Sports Club and Wii Party U. Launch them by right-clicking the game and choosing **Start (Compatibility Mode)**
- Wiimote DSU compatible so you can use a phone a Wii Remote!
- 
The phone and the PC have to be on the same network. In Cemu, **Gamepad on Android**
Connect a bluetooth controller to the phone and add it as a DSUController. Put both IPs on both places and enjoy the ride.

## Latest update — 2.6.0.3

[Download the latest release](https://github.com/heroiamarelo1/decent-cemu/releases/latest).

Connect the phone app's Video stream and Cemu now discovers and connects that
phone as the GamePad automatically when no GamePad controller is connected.


This update also fixes microphone input for Nintendo Land's Donkey Kong blow
section. Confirmed in hands-on testing.

Download the Cemu executable, `PadStream.exe` and Android APK from the latest
release. Put both Windows files in the same Cemu folder, then install the APK.
The motion-axis fixes confirmed in 2.6.0.2 remain included.

## Android app

The phone app is in [`gamepad-android`](gamepad-android). Its name is Decent Cemu Controller. It is licensed under the [MIT License](gamepad-android/LICENSE).

The Cemu code stays under the original [Mozilla Public License 2.0](LICENSE.txt).

This was vibe-coded. I do not morally authorize the official development team to use my code, because of their anti-artificial-intelligence policy, and because the Discord mod is an idiot who is going to stay single for the rest of his life.

## Build

See [BUILD.md](BUILD.md).
