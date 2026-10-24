# HideHIDDevices

An [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) plugin that hides selected HID devices from DirectInput games.

It was written for **Need for Speed: Hot Pursuit 2**, which crashes on start-up on Windows 11 when certain HID devices are connected (for example the ASRock RGB LED controller, `HID\VID_26CE&PID_01A2`). It works with any 32-bit game that uses DirectInput 3, 5, 7 or 8.

## Installation

1. Install Ultimate ASI Loader in the game folder (for NFS HP2, as `dinput.dll`).
2. Copy `HideHIDDevices.asi` and `HideHIDDevices.ini` from the `scripts` folder of this repo into the game's `scripts` folder.
3. Edit `HideHIDDevices.ini` and list the devices to hide, one per line.

## Configuration

Each line in `HideHIDDevices.ini` is a Hardware ID as shown in Device Manager (device → Properties → Details → *Hardware Ids*). Lines starting with `;` or `#` are comments.

| Line | Effect |
| --- | --- |
| `HID\VID_26CE&PID_01A2` | Hides every device with that vendor/product ID |
| `HID\VID_26CE&PID_01A2&REV_0000` | Hides it only if a connected device has exactly this ID (`REV`, `MI`, `Col` parts must match) |

### Hiding everything that isn't a game controller

`HideNonGameControllers=1` (off by default) also hides every HID device that DirectInput doesn't report as a game controller: extra HID mice and keyboards, RGB/lighting controllers, coolers, media keys and vendor-defined devices. The game still gets the mouse and keyboard through Windows' combined system *Mouse* and *Keyboard* devices, and joysticks, gamepads and wheels stay visible.

Gaming mice and keyboards often expose several HID devices each. Turn this on if a game has trouble with the number of devices DirectInput reports. These devices are hidden from the game only; DirectInput itself still reads them.

## How it works

- On load, the plugin redirects the game's calls to `DirectInputCreateA/W/Ex` (`dinput.dll`), `DirectInput8Create` (`dinput8.dll`) and `GetProcAddress` through itself.
- When the game creates a DirectInput object, the plugin reads the `.ini` and patches the object's `EnumDevices`, `CreateDevice`, `CreateDeviceEx` and `GetDeviceStatus` methods.
- Matching devices are skipped during enumeration, and attempts to open them fail with `DIERR_DEVICENOTREG`.
- Devices listed in the `.ini` are also hidden from DirectInput itself. Some devices, such as the ASRock LED controller, make Windows' `dinput.dll` corrupt memory while it reads them during enumeration, even if the game never uses them, which causes random crashes. The plugin therefore patches the system `dinput.dll`'s own imports, so its SetupAPI device enumeration (`SetupDiEnumDeviceInterfaces`) skips these devices and opening them (`CreateFile`) fails.

DirectInput only exposes a device's VID/PID, so that is what decides what gets hidden. The `REV`/`MI`/`Col` parts are checked against the devices Windows reports as connected.

## Log file

Logging is on by default; set `EnableLog=0` in the `.ini` to turn it off, along with the diagnostic heap checks and crash logging.

Each launch writes `HideHIDDevices.log` next to the `.asi`, and keeps the previous run's log as `HideHIDDevices.prev.log`. The log contains:

- every HID device Windows reports as connected, with its Hardware ID
- every device the game enumerated, with its VID/PID and DirectInput type, and whether it was hidden and why
- each device the game opened
- details of any crash: the module and offset where it happened, and likely callers

Use it to find the IDs of other devices that cause trouble.

## Building

The plugin is a single C file with no SDK dependencies. Build it as a 32-bit DLL:

```sh
# MinGW / llvm-mingw
i686-w64-mingw32-gcc -O2 -shared -s -static -o HideHIDDevices.asi src/HideHIDDevices.c -lsetupapi
```

```bat
:: MSVC (x86 Native Tools Command Prompt)
cl /O2 /LD src\HideHIDDevices.c setupapi.lib /Fe:HideHIDDevices.asi
```

## Limitations

Only DirectInput is filtered. Devices are not hidden from the legacy `winmm` joystick API, Raw Input or XInput.

## AI disclosure

This project was built entirely by Claude.
