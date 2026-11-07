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

Requires: `libx11-dev`

### macOS

```bash
cd VisualPinball.NativeInput
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

Requires: AppKit and ApplicationServices frameworks (provided by macOS)

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

On Windows, NativeInput recognizes Open Pinball Device 1.x HID collections by their report signature. It exposes named X/Y acceleration and velocity axes and the reported plunger fields. Preassigned OPD buttons that match existing VPE actions work alongside keyboard bindings, including physical tilt and digital nudges. An action stays pressed while any connected source holds it.

Pinscape Pico can expose both OPD and a joystick. Each HID collection retains its own mapping identity. VPE's cabinet setup prefers OPD and lets a player explicitly choose either collection. Pico acceleration and velocity ranges depend on its firmware configuration.

Build the report replay tests with `-DNATIVEINPUT_BUILD_TESTS=ON`, then run `ctest --test-dir build -C Release --output-on-failure` from this repository.

## Platform Status

| Platform | Status | API Used |
|----------|--------|----------|
| Windows  | Keyboard, HID joystick axes and Open Pinball Device | GetAsyncKeyState, QueryPerformanceCounter, Windows HID |
| Linux    | ✅ Implemented | X11 keyboard state polling |
| macOS    | ✅ Implemented | CoreGraphics keyboard state polling |

## Performance

- **Latency**: <0.1ms from physical input to event callback
- **CPU Usage**: 5-10% of one core (mostly sleeping)
- **Overhead**: ~10-20ns per P/Invoke call
- **Precision**: Monotonic microsecond timestamps across platforms

## Future Enhancements

1. **Gamepad support**: XInput (Windows), SDL/libinput (Linux), GCController (macOS)
2. **Analog inputs**: Joystick and OPD support on macOS and Linux
3. **Force feedback**: Haptics for nudge, tilt, button feedback
4. **Configuration**: JSON-based key mapping files
5. **Profiles**: Per-table input profiles

## License

GPLv3+ (same as VPE)
