# VPE Native Input Polling

High-performance native input polling library for Visual Pinball Engine.

## Purpose

Provides OS-level input polling to achieve sub-millisecond input latency by bypassing Unity's Input System buffer. The input polling runs on a dedicated thread at 500-2000 Hz and forwards events to the simulation thread via a callback.

## Architecture

```
┌─────────────────────────────────────┐
│   OS Input Events (Keyboard/HID)    │
└─────────────────────────────────────┘
              ↓
┌─────────────────────────────────────┐
│  Native Input Polling Thread        │
│  (500-2000 Hz)                      │
├─────────────────────────────────────┤
│  • GetAsyncKeyState (Windows)       │
│  • X11 key polling (Linux)          │
│  • CoreGraphics/AppKit (macOS)      │
└─────────────────────────────────────┘
              ↓
┌─────────────────────────────────────┐
│  Lock-Free SPSC Ring Buffer         │
└─────────────────────────────────────┘
              ↓
┌─────────────────────────────────────┐
│  Simulation Thread (1000 Hz)        │
└─────────────────────────────────────┘
```

## Building

### Windows (Visual Studio 2022)

```bash
cd VisualPinball.NativeInput
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

Output: `artifacts/win-x64/VisualPinball.NativeInput.dll`

### Windows (MinGW)

```bash
cd VisualPinball.NativeInput
mkdir build
cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

### Linux

```bash
cd VisualPinball.NativeInput
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

Requires `libx11-dev` and `libudev-dev`. The app user needs read access to the controller's input devices.

### macOS

```bash
cd VisualPinball.NativeInput
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

Requires CMake 3.24 or newer and the macOS SDK.

### CI / NuGet

GitHub Actions builds `win-x64`, `win-x86`, `linux-x64`, `osx-x64`, and `osx-arm64` binaries and packs them into a single NuGet package.

Publishing happens automatically for tags matching `v*`.

### Output directory override

By default, builds are written to `artifacts/<rid>/`.

You can override the output directory with:

- `-DNATIVEINPUT_OUTPUT_DIR=/your/path` (preferred)
- `-DVPE_NATIVEINPUT_OUTPUT_DIR=/your/path` (legacy alias)

## Usage from C#

```csharp
using VisualPinball.Unity.Simulation;

// Initialize
NativeInputApi.VpeInputInit();
if (NativeInputApi.VpeInputGetProtocolVersion() != 2) {
    throw new InvalidOperationException("Unsupported native input protocol.");
}

// Set bindings
var bindings = new[] {
    new NativeInputApi.InputBinding {
        Action = (int)NativeInputApi.InputAction.LeftFlipper,
        BindingType = (int)NativeInputApi.BindingType.Keyboard,
        KeyCode = (int)NativeInputApi.KeyCode.LShift
    }
};
NativeInputApi.VpeInputSetBindings(bindings, bindings.Length);

// Start polling
NativeInputApi.VpeInputStartPolling(OnInputEvent, IntPtr.Zero, 500);

// Callback
[MonoPInvokeCallback(typeof(NativeInputApi.InputEventCallback))]
static void OnInputEvent(ref NativeInputApi.InputEvent evt, IntPtr userData) {
    Console.WriteLine($"Input: {evt.Action} = {evt.Value} @ {evt.TimestampUsec}μs");
}

// Stop polling
NativeInputApi.VpeInputStopPolling();

// Shutdown
NativeInputApi.VpeInputShutdown();
```

## Open Pinball Device

NativeInput recognizes Open Pinball Device 1.x HID collections by their report signature on Windows, Linux, and macOS. The native input thread reads the reports and exposes named X/Y acceleration and velocity axes and the reported plunger fields. Preassigned OPD buttons that match existing VPE actions work alongside keyboard bindings, including physical tilt and digital nudges. An action stays pressed while any connected source holds it.

Pinscape Pico can expose both OPD and a joystick. VPE's cabinet setup prefers OPD and lets a player explicitly choose either collection. On Linux and macOS, OPD mapping identities include the physical USB port so identical boards can be mapped independently. Pico acceleration and velocity ranges depend on its firmware configuration.

On Linux, run VPE with an X11 display. The local user needs read access to the Pico's hidraw node for input and read/write access to its USB device node for the signature request. On a systemd-based desktop, these udev rules grant the active local user access to a Pico using the default IDs. Save them as `/etc/udev/rules.d/70-pinscape-pico.rules`, reload the rules, and reconnect the board. Use the configured VID and PID if the firmware overrides them.

```udev
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="eaeb", TAG+="uaccess"
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="1209", ATTR{idProduct}=="eaeb", TAG+="uaccess"
```

Build the report replay tests with `-DNATIVEINPUT_BUILD_TESTS=ON`, then run `ctest --test-dir build -C Release --output-on-failure` from this repository.

## Platform Status

| Platform | Status | API Used |
|----------|--------|----------|
| Windows  | Keyboard, joystick and gamepad axes, Open Pinball Device | GetAsyncKeyState, Windows HID, SDL |
| Linux    | Keyboard, joystick and gamepad axes, Open Pinball Device | X11, SDL, hidraw, usbfs |
| macOS    | Keyboard, joystick and gamepad axes, Open Pinball Device | CoreGraphics, SDL, IOKit |

SDL is linked into the plugin and only its joystick and gamepad subsystem is started. The native polling thread samples all controller axes. Recognized gamepads expose named stick and trigger axes, and generic joysticks expose raw axes for manual nudge mapping. Windows keeps the existing HID axes and their saved IDs. If a Windows gamepad also exposes HID joystick axes, its standardized axes appear under the separate `(Gamepad)` entry.

Some Nintendo controllers change their report format when SDL opens them. On Windows, use the standardized entry if a saved raw HID mapping for one of those controllers stops receiving samples.

Controller mappings use serial numbers and physical paths where available. When the platform exposes identical devices without a physical identity, the mappings use slots for that model. Recheck those slots after changing the connection order of identical controllers.

The Player's Nudge settings can bind joystick, gamepad, or generic OPD buttons to left, right, and forward nudge, Tilt, and Slam Tilt. Custom buttons supplement keyboard and predefined OPD buttons. Bindings follow the device ID and button ID across polling sessions. Tilt uses the table's physical Tilt Bob routing.

## Performance

- **Latency**: <0.1ms from physical input to event callback
- **CPU Usage**: 5-10% of one core (mostly sleeping)
- **Overhead**: ~10-20ns per P/Invoke call
- **Precision**: Monotonic microsecond timestamps across platforms

## Future Enhancements

1. **Controller buttons**: Configurable bindings for other gameplay actions
2. **Force feedback**: Haptics for nudge, tilt, button feedback
3. **Configuration**: JSON-based key mapping files
4. **Profiles**: Per-table input profiles

## License

GPLv3+ (same as VPE)

SDL is distributed under the [zlib license](native/nuget/SDL-LICENSE.txt).
