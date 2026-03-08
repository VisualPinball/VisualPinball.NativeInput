// license:GPLv3+
// macOS implementation of native input polling

#ifdef __APPLE__

#include "NativeInput.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <vector>

namespace {
	using Clock = std::chrono::steady_clock;

	struct KeyBindingState {
		VpeInputAction action;
		CGKeyCode nativeKeyCode;
		float previousValue;
	};

	std::thread g_pollingThread;
	std::atomic<bool> g_running(false);
	std::condition_variable g_waitCondition;
	std::mutex g_waitMutex;
	VpeInputEventCallback g_callback = nullptr;
	void* g_userData = nullptr;
	int g_pollIntervalUs = 500;
	Clock::time_point g_startTime;
	std::vector<KeyBindingState> g_bindings;
	bool g_wasForeground = false;

	int64_t GetTimestampUsecInternal() {
		return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_startTime).count();
	}

	bool MapKeyCode(int keyCode, CGKeyCode* nativeKeyCode) {
		switch (keyCode) {
			case VPE_KEY_LSHIFT:
				*nativeKeyCode = 56;
				return true;
			case VPE_KEY_RSHIFT:
				*nativeKeyCode = 60;
				return true;
			case VPE_KEY_LCONTROL:
				*nativeKeyCode = 59;
				return true;
			case VPE_KEY_RCONTROL:
				*nativeKeyCode = 62;
				return true;
			case VPE_KEY_SPACE:
				*nativeKeyCode = 49;
				return true;
			case VPE_KEY_RETURN:
				*nativeKeyCode = 36;
				return true;
			case VPE_KEY_D1:
				*nativeKeyCode = 18;
				return true;
			case VPE_KEY_D5:
				*nativeKeyCode = 23;
				return true;
			case VPE_KEY_NUMPAD1:
				*nativeKeyCode = 83;
				return true;
			case VPE_KEY_A:
				*nativeKeyCode = 0;
				return true;
			case VPE_KEY_S:
				*nativeKeyCode = 1;
				return true;
			case VPE_KEY_D:
				*nativeKeyCode = 2;
				return true;
			case VPE_KEY_W:
				*nativeKeyCode = 13;
				return true;
			default:
				return false;
		}
	}

	bool IsCurrentProcessForeground() {
		@autoreleasepool {
			NSRunningApplication* currentApplication = [NSRunningApplication currentApplication];
			NSRunningApplication* frontmostApplication = [[NSWorkspace sharedWorkspace] frontmostApplication];
			if (currentApplication == nil || frontmostApplication == nil) {
				return true;
			}

			return currentApplication.processIdentifier == frontmostApplication.processIdentifier;
		}
	}

	bool IsPressed(CGKeyCode nativeKeyCode) {
		return CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, nativeKeyCode);
	}

	void EmitReleaseForPressedKeys(int64_t timestampUsec) {
		if (g_callback == nullptr) {
			for (auto& binding : g_bindings) {
				binding.previousValue = 0.0f;
			}
			return;
		}

		for (auto& binding : g_bindings) {
			if (binding.previousValue <= 0.0f) {
				continue;
			}

			binding.previousValue = 0.0f;

			VpeInputEvent event{};
			event.timestampUsec = timestampUsec;
			event.action = static_cast<int32_t>(binding.action);
			event.value = 0.0f;
			g_callback(&event, g_userData);
		}
	}

	void PollingThreadFunc() {
		@autoreleasepool {
			VpeSetThreadPriority();

			while (g_running.load(std::memory_order_acquire)) {
				const int64_t timestampUsec = GetTimestampUsecInternal();
				const bool isForeground = IsCurrentProcessForeground();

				if (!isForeground) {
					if (g_wasForeground) {
						EmitReleaseForPressedKeys(timestampUsec);
					}
					g_wasForeground = false;
				} else {
					g_wasForeground = true;

					for (auto& binding : g_bindings) {
						const float currentValue = IsPressed(binding.nativeKeyCode) ? 1.0f : 0.0f;
						if (currentValue == binding.previousValue) {
							continue;
						}

						binding.previousValue = currentValue;
						if (g_callback == nullptr) {
							continue;
						}

						VpeInputEvent event{};
						event.timestampUsec = timestampUsec;
						event.action = static_cast<int32_t>(binding.action);
						event.value = currentValue;
						g_callback(&event, g_userData);
					}
				}

				std::unique_lock<std::mutex> lock(g_waitMutex);
				g_waitCondition.wait_for(
					lock,
					std::chrono::microseconds(g_pollIntervalUs > 0 ? g_pollIntervalUs : 500),
					[] { return !g_running.load(std::memory_order_acquire); });
			}
		}
	}
}

extern "C" {

VPE_API int VpeInputInit(void) {
	g_startTime = Clock::now();
	g_wasForeground = false;
	return 1;
}

VPE_API void VpeInputShutdown(void) {
	VpeInputStopPolling();
	g_bindings.clear();
	g_wasForeground = false;
}

VPE_API void VpeInputSetBindings(const VpeInputBinding* bindings, int count) {
	g_bindings.clear();
	g_wasForeground = false;

	if (bindings == nullptr || count <= 0) {
		return;
	}

	for (int i = 0; i < count; ++i) {
		if (bindings[i].bindingType != VPE_BINDING_KEYBOARD) {
			continue;
		}

		CGKeyCode nativeKeyCode = 0;
		if (!MapKeyCode(bindings[i].keyCode, &nativeKeyCode)) {
			continue;
		}

		g_bindings.push_back({
			static_cast<VpeInputAction>(bindings[i].action),
			nativeKeyCode,
			0.0f,
		});
	}
}

VPE_API int VpeInputStartPolling(VpeInputEventCallback callback, void* userData, int pollIntervalUs) {
	if (g_running.load(std::memory_order_acquire)) {
		return 0;
	}

	g_callback = callback;
	g_userData = userData;
	g_pollIntervalUs = pollIntervalUs > 0 ? pollIntervalUs : 500;
	g_wasForeground = false;
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
	if (!g_running.load(std::memory_order_acquire)) {
		return;
	}

	g_running.store(false, std::memory_order_release);
	g_waitCondition.notify_all();
	if (g_pollingThread.joinable()) {
		g_pollingThread.join();
	}
}

VPE_API int64_t VpeGetTimestampUsec(void) {
	return GetTimestampUsecInternal();
}

VPE_API void VpeSetThreadPriority(void) {
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

} // extern "C"

#endif // __APPLE__
