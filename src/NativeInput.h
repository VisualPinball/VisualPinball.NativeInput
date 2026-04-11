// license:GPLv3+
// Visual Pinball Engine - Native Input Polling
// Provides OS-level input polling for sub-millisecond latency

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Platform detection
#if defined(_WIN32) || defined(_WIN64)
#define VPE_PLATFORM_WINDOWS 1
#elif defined(__linux__)
#define VPE_PLATFORM_LINUX 1
#elif defined(__APPLE__)
#define VPE_PLATFORM_MACOS 1
#endif

// Export macros
#if defined(VPE_PLATFORM_WINDOWS)
#define VPE_API __declspec(dllexport)
#elif defined(__GNUC__)
#define VPE_API __attribute__((visibility("default")))
#else
#define VPE_API
#endif

// Input action enum (must match C# enum)
typedef enum {
	VPE_INPUT_LEFT_FLIPPER = 0,
	VPE_INPUT_RIGHT_FLIPPER = 1,
	VPE_INPUT_UPPER_LEFT_FLIPPER = 2,
	VPE_INPUT_UPPER_RIGHT_FLIPPER = 3,
	VPE_INPUT_LEFT_MAGNASAVE = 4,
	VPE_INPUT_RIGHT_MAGNASAVE = 5,
	VPE_INPUT_START = 6,
	VPE_INPUT_PLUNGE = 7,
	VPE_INPUT_PLUNGER_ANALOG = 8,
	VPE_INPUT_COIN_INSERT_1 = 9,
	VPE_INPUT_COIN_INSERT_2 = 10,
	VPE_INPUT_COIN_INSERT_3 = 11,
	VPE_INPUT_COIN_INSERT_4 = 12,
	VPE_INPUT_EXIT_GAME = 13,
	VPE_INPUT_SLAM_TILT = 14,
	VPE_INPUT_LEFT_STAGED_FLIPPER = 15,
	VPE_INPUT_RIGHT_STAGED_FLIPPER = 16,
	VPE_INPUT_LEFT_NUDGE = 17,
	VPE_INPUT_RIGHT_NUDGE = 18,
	VPE_INPUT_CENTER_NUDGE = 19,
	VPE_INPUT_TILT = 20,
	VPE_INPUT_EXTRA_BALL = 21,
	VPE_INPUT_LOCKBAR = 22,
	VPE_INPUT_PAUSE_GAME = 23,
	VPE_INPUT_COIN_DOOR = 24,
	VPE_INPUT_RESET = 25,
	VPE_INPUT_SERVICE_1 = 26,
	VPE_INPUT_SERVICE_2 = 27,
	VPE_INPUT_SERVICE_3 = 28,
	VPE_INPUT_SERVICE_4 = 29,
	VPE_INPUT_SERVICE_5 = 30,
	VPE_INPUT_SERVICE_6 = 31,
	VPE_INPUT_SERVICE_7 = 32,
	VPE_INPUT_SERVICE_8 = 33,
	VPE_INPUT_MAX = 64  // Reserve space for future actions
} VpeInputAction;

// Input binding type
typedef enum {
	VPE_BINDING_KEYBOARD = 0,
	VPE_BINDING_GAMEPAD = 1,
	VPE_BINDING_MOUSE = 2
} VpeBindingType;

// Key codes (Windows virtual key codes, mapped on other platforms)
typedef enum {
	VPE_KEY_LSHIFT = 0xA0,
	VPE_KEY_RSHIFT = 0xA1,
	VPE_KEY_LCONTROL = 0xA2,
	VPE_KEY_RCONTROL = 0xA3,
	VPE_KEY_LALT = 0xA4,
	VPE_KEY_RALT = 0xA5,
	VPE_KEY_ESCAPE = 0x1B,
	VPE_KEY_SPACE = 0x20,
	VPE_KEY_PAGEUP = 0x21,
	VPE_KEY_PAGEDOWN = 0x22,
	VPE_KEY_END = 0x23,
	VPE_KEY_HOME = 0x24,
	VPE_KEY_RETURN = 0x0D,
	VPE_KEY_F1 = 0x70,
	VPE_KEY_F2 = 0x71,
	VPE_KEY_F3 = 0x72,
	VPE_KEY_F4 = 0x73,
	VPE_KEY_F5 = 0x74,
	VPE_KEY_F6 = 0x75,
	VPE_KEY_F7 = 0x76,
	VPE_KEY_F8 = 0x77,
	VPE_KEY_F9 = 0x78,
	VPE_KEY_F10 = 0x79,
	VPE_KEY_F11 = 0x7A,
	VPE_KEY_F12 = 0x7B,
	VPE_KEY_D0 = 0x30,
	VPE_KEY_D1 = 0x31,
	VPE_KEY_NUM1 = 0x31,
	VPE_KEY_D2 = 0x32,
	VPE_KEY_D3 = 0x33,
	VPE_KEY_D4 = 0x34,
	VPE_KEY_D5 = 0x35,
	VPE_KEY_NUM5 = 0x35,
	VPE_KEY_D6 = 0x36,
	VPE_KEY_D7 = 0x37,
	VPE_KEY_D8 = 0x38,
	VPE_KEY_D9 = 0x39,
	VPE_KEY_O = 0x4F,
	VPE_KEY_P = 0x50,
	VPE_KEY_T = 0x54,
	VPE_KEY_Y = 0x59,
	VPE_KEY_NUMPAD1 = 0x61,
	VPE_KEY_A = 0x41,
	VPE_KEY_B = 0x42,
	VPE_KEY_S = 0x53,
	VPE_KEY_D = 0x44,
	VPE_KEY_W = 0x57,
	VPE_KEY_MINUS = 0xBD, // VK_OEM_MINUS
	VPE_KEY_QUOTE = 0xDE, // VK_OEM_7
	VPE_KEY_CARET = 0xC0, // VK_OEM_3 (layout dependent)
} VpeKeyCode;

// Input event structure (matches C# struct layout)
typedef struct {
	int64_t timestampUsec;  // Microsecond timestamp
	int32_t action;         // VpeInputAction
	float value;            // 0.0 (released) or 1.0 (pressed), or analog value
	int32_t _padding;       // Ensure 16-byte alignment
} VpeInputEvent;

// Input binding structure
typedef struct {
	int32_t action;         // VpeInputAction
	int32_t bindingType;    // VpeBindingType
	int32_t keyCode;        // VpeKeyCode or gamepad button index
	int32_t _padding;
} VpeInputBinding;

// Callback for input events
typedef void (*VpeInputEventCallback)(const VpeInputEvent* event, void* userData);

// Initialize input system
// Returns 1 on success, 0 on failure
VPE_API int VpeInputInit(void);

// Shutdown input system
VPE_API void VpeInputShutdown(void);

// Set input bindings
// bindings: Array of VpeInputBinding
// count: Number of bindings
VPE_API void VpeInputSetBindings(const VpeInputBinding* bindings, int count);

// Start polling thread
// callback: Function called for each input event
// userData: User data passed to callback
// pollIntervalUs: Polling interval in microseconds (default 500)
VPE_API int VpeInputStartPolling(VpeInputEventCallback callback, void* userData, int pollIntervalUs);

// Stop polling thread
VPE_API void VpeInputStopPolling(void);

// Get high-resolution timestamp in microseconds
VPE_API int64_t VpeGetTimestampUsec(void);

// Set thread priority to time-critical (best effort)
VPE_API void VpeSetThreadPriority(void);

#ifdef __cplusplus
}
#endif
