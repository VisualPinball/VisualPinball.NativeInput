// license:GPLv3+
// Windows implementation of native input polling

#ifdef _WIN32

#include "NativeInput.h"
#include <windows.h>
#include <thread>
#include <atomic>
#include <vector>

namespace {
	struct KeyBindingState {
		int keyCode;
		VpeInputAction action;
		float previousValue;
	};

	std::thread g_pollingThread;
	std::atomic<bool> g_running(false);
	VpeInputEventCallback g_callback = nullptr;
	void* g_userData = nullptr;
	int g_pollIntervalUs = 500;
	HANDLE g_timer = NULL;
	HANDLE g_stopEvent = NULL;

	// Input bindings: one key can map to multiple actions.
	std::vector<KeyBindingState> g_bindings;

	// High-resolution timing
	LARGE_INTEGER g_frequency;
	LARGE_INTEGER g_startTime;
	bool g_wasForeground = false;

	bool IsCurrentProcessForeground() {
		HWND foreground = GetForegroundWindow();
		if (foreground == NULL) {
			return false;
		}

		DWORD foregroundPid = 0;
		GetWindowThreadProcessId(foreground, &foregroundPid);
		return foregroundPid == GetCurrentProcessId();
	}

	void EmitReleaseForPressedKeys(int64_t timestampUsec) {
		for (auto& binding : g_bindings) {
			if (binding.previousValue <= 0.0f) {
				continue;
			}

			binding.previousValue = 0.0f;
			if (g_callback == nullptr) {
				continue;
			}

			VpeInputEvent event;
			event.timestampUsec = timestampUsec;
			event.action = static_cast<int32_t>(binding.action);
			event.value = 0.0f;
			event._padding = 0;
			g_callback(&event, g_userData);
		}
	}

	// Polling thread function
	void PollingThreadFunc() {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

		const HANDLE handles[2] = { g_timer, g_stopEvent };
		while (g_running.load(std::memory_order_acquire)) {
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			int64_t timestampUsec = ((now.QuadPart - g_startTime.QuadPart) * 1000000LL) / g_frequency.QuadPart;

			bool isForeground = IsCurrentProcessForeground();
			if (!isForeground) {
				if (g_wasForeground) {
					EmitReleaseForPressedKeys(timestampUsec);
				}
				g_wasForeground = false;

				if (g_pollIntervalUs > 0 && g_timer != NULL) {
					LARGE_INTEGER dueTime;
					dueTime.QuadPart = -(static_cast<LONGLONG>(g_pollIntervalUs) * 10);
					SetWaitableTimer(g_timer, &dueTime, 0, NULL, NULL, FALSE);
					DWORD waitResult = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
					if (waitResult == WAIT_OBJECT_0 + 1) {
						break;
					}
				} else {
					DWORD waitResult = WaitForSingleObject(g_stopEvent, 1);
					if (waitResult == WAIT_OBJECT_0) {
						break;
					}
				}
				continue;
			}

			g_wasForeground = true;

			// Poll all bound keys
			for (auto& binding : g_bindings) {
				SHORT keyState = GetAsyncKeyState(binding.keyCode);
				bool isPressed = (keyState & 0x8000) != 0;
				float currentValue = isPressed ? 1.0f : 0.0f;

				if (currentValue != binding.previousValue) {
					binding.previousValue = currentValue;

					// Fire event
					if (g_callback) {
						VpeInputEvent event;
						event.timestampUsec = timestampUsec;
						event.action = static_cast<int32_t>(binding.action);
						event.value = currentValue;
						event._padding = 0;
						g_callback(&event, g_userData);
					}
				}
			}

			// Wait for next poll interval or stop event.
			if (g_pollIntervalUs > 0 && g_timer != NULL) {
				// Relative due time in 100ns units (negative = relative)
				LARGE_INTEGER dueTime;
				dueTime.QuadPart = -(static_cast<LONGLONG>(g_pollIntervalUs) * 10);
				SetWaitableTimer(g_timer, &dueTime, 0, NULL, NULL, FALSE);
				DWORD waitResult = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
				if (waitResult == WAIT_OBJECT_0 + 1) {
					break; // stop event
				}
			} else {
				DWORD waitResult = WaitForSingleObject(g_stopEvent, 1);
				if (waitResult == WAIT_OBJECT_0) {
					break;
				}
			}
		}
	}
}

extern "C" {

VPE_API int VpeInputInit(void) {
	// Initialize high-resolution timer
	if (!QueryPerformanceFrequency(&g_frequency)) {
		return 0;
	}
	QueryPerformanceCounter(&g_startTime);
	if (g_timer == NULL) {
		g_timer = CreateWaitableTimer(NULL, FALSE, NULL);
	}
	if (g_stopEvent == NULL) {
		g_stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	}
	return 1;
}

VPE_API void VpeInputShutdown(void) {
	VpeInputStopPolling();
	g_bindings.clear();
	if (g_timer != NULL) {
		CloseHandle(g_timer);
		g_timer = NULL;
	}
	if (g_stopEvent != NULL) {
		CloseHandle(g_stopEvent);
		g_stopEvent = NULL;
	}
}

VPE_API void VpeInputSetBindings(const VpeInputBinding* bindings, int count) {
	g_bindings.clear();
	g_wasForeground = false;

	for (int i = 0; i < count; i++) {
		if (bindings[i].bindingType == VPE_BINDING_KEYBOARD) {
			g_bindings.push_back({
				bindings[i].keyCode,
				static_cast<VpeInputAction>(bindings[i].action),
				0.0f,
			});
		}
		// TODO: Add gamepad support
	}
}

VPE_API int VpeInputStartPolling(VpeInputEventCallback callback, void* userData, int pollIntervalUs) {
	if (g_running.load(std::memory_order_acquire)) {
		return 0; // Already running
	}

	g_callback = callback;
	g_userData = userData;
	g_pollIntervalUs = pollIntervalUs;
	g_wasForeground = false;
	if (g_stopEvent != NULL) {
		ResetEvent(g_stopEvent);
	}
	g_running.store(true, std::memory_order_release);

	try {
		g_pollingThread = std::thread(PollingThreadFunc);
		return 1;
	} catch (...) {
		g_running.store(false, std::memory_order_release);
		return 0;
	}
}

VPE_API void VpeInputStopPolling(void) {
	if (g_running.load(std::memory_order_acquire)) {
		g_running.store(false, std::memory_order_release);
		if (g_stopEvent != NULL) {
			SetEvent(g_stopEvent);
		}
		if (g_pollingThread.joinable()) {
			g_pollingThread.join();
		}
	}
}

VPE_API int64_t VpeGetTimestampUsec(void) {
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return ((now.QuadPart - g_startTime.QuadPart) * 1000000LL) / g_frequency.QuadPart;
}

VPE_API void VpeSetThreadPriority(void) {
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

} // extern "C"

#endif // _WIN32
