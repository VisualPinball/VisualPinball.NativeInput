// license:GPLv3+
// macOS implementation of native input polling

#ifdef __APPLE__

#include "OpenPinballInput.h"

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
	};

	std::thread g_pollingThread;
	std::atomic<bool> g_running(false);
	std::atomic<bool> g_protocolVersionChecked(false);
	std::condition_variable g_waitCondition;
	std::mutex g_waitMutex;
	OpenPinballDevice::Input g_input;
	std::mutex g_bindingsMutex;
	int g_pollIntervalUs = 500;
	Clock::time_point g_startTime;
	std::vector<KeyBindingState> g_bindings;

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
			case VPE_KEY_LALT:
				*nativeKeyCode = 58;
				return true;
			case VPE_KEY_RALT:
				*nativeKeyCode = 61;
				return true;
			case VPE_KEY_ESCAPE:
				*nativeKeyCode = 53;
				return true;
			case VPE_KEY_SPACE:
				*nativeKeyCode = 49;
				return true;
			case VPE_KEY_PAGEUP:
				*nativeKeyCode = 116;
				return true;
			case VPE_KEY_PAGEDOWN:
				*nativeKeyCode = 121;
				return true;
			case VPE_KEY_END:
				*nativeKeyCode = 119;
				return true;
			case VPE_KEY_HOME:
				*nativeKeyCode = 115;
				return true;
			case VPE_KEY_RETURN:
				*nativeKeyCode = 36;
				return true;
			case VPE_KEY_F1:
				*nativeKeyCode = 122;
				return true;
			case VPE_KEY_F2:
				*nativeKeyCode = 120;
				return true;
			case VPE_KEY_F3:
				*nativeKeyCode = 99;
				return true;
			case VPE_KEY_F4:
				*nativeKeyCode = 118;
				return true;
			case VPE_KEY_F5:
				*nativeKeyCode = 96;
				return true;
			case VPE_KEY_F6:
				*nativeKeyCode = 97;
				return true;
			case VPE_KEY_F7:
				*nativeKeyCode = 98;
				return true;
			case VPE_KEY_F8:
				*nativeKeyCode = 100;
				return true;
			case VPE_KEY_F9:
				*nativeKeyCode = 101;
				return true;
			case VPE_KEY_F10:
				*nativeKeyCode = 109;
				return true;
			case VPE_KEY_F11:
				*nativeKeyCode = 103;
				return true;
			case VPE_KEY_F12:
				*nativeKeyCode = 111;
				return true;
			case VPE_KEY_D0:
				*nativeKeyCode = 29;
				return true;
			case VPE_KEY_D1:
				*nativeKeyCode = 18;
				return true;
			case VPE_KEY_D2:
				*nativeKeyCode = 19;
				return true;
			case VPE_KEY_D3:
				*nativeKeyCode = 20;
				return true;
			case VPE_KEY_D4:
				*nativeKeyCode = 21;
				return true;
			case VPE_KEY_D5:
				*nativeKeyCode = 23;
				return true;
			case VPE_KEY_D6:
				*nativeKeyCode = 22;
				return true;
			case VPE_KEY_D7:
				*nativeKeyCode = 26;
				return true;
			case VPE_KEY_D8:
				*nativeKeyCode = 28;
				return true;
			case VPE_KEY_D9:
				*nativeKeyCode = 25;
				return true;
			case VPE_KEY_NUMPAD1:
				*nativeKeyCode = 83;
				return true;
			case VPE_KEY_A:
				*nativeKeyCode = 0;
				return true;
			case VPE_KEY_B:
				*nativeKeyCode = 11;
				return true;
			case VPE_KEY_O:
				*nativeKeyCode = 31;
				return true;
			case VPE_KEY_P:
				*nativeKeyCode = 35;
				return true;
			case VPE_KEY_S:
				*nativeKeyCode = 1;
				return true;
			case VPE_KEY_D:
				*nativeKeyCode = 2;
				return true;
			case VPE_KEY_T:
				*nativeKeyCode = 17;
				return true;
			case VPE_KEY_W:
				*nativeKeyCode = 13;
				return true;
			case VPE_KEY_Y:
				*nativeKeyCode = 16;
				return true;
			case VPE_KEY_Z:
				*nativeKeyCode = 6;
				return true;
			case VPE_KEY_MINUS:
				*nativeKeyCode = 27;
				return true;
			case VPE_KEY_SLASH:
				*nativeKeyCode = 44;
				return true;
			case VPE_KEY_QUOTE:
				*nativeKeyCode = 39;
				return true;
			case VPE_KEY_CARET:
				*nativeKeyCode = 50;
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

	void PollingThreadFunc() {
		@autoreleasepool {
			VpeSetThreadPriority();
			OpenPinballDevice::Transport transport(g_input);
			while (g_running.load(std::memory_order_acquire)) {
				const int64_t timestamp = GetTimestampUsecInternal();
				OpenPinballDevice::Keys keys{};
				bool foreground;
				{
					std::lock_guard<std::mutex> lock(g_bindingsMutex);
					foreground = IsCurrentProcessForeground();
					if (foreground) {
						for (const auto& binding : g_bindings) {
							keys[binding.action] |= IsPressed(binding.nativeKeyCode);
						}
					}
				}
				g_input.BeginPoll(timestamp, foreground, keys);
				transport.Poll(timestamp);

				std::unique_lock<std::mutex> lock(g_waitMutex);
				g_waitCondition.wait_for(lock, std::chrono::microseconds(g_pollIntervalUs),
					[] { return !g_running.load(std::memory_order_acquire); });
			}
		}
	}
}

extern "C" {

VPE_API int VpeInputInit(void) {
	g_protocolVersionChecked.store(false, std::memory_order_release);
	g_input.ResetGameplayGate();
	g_startTime = Clock::now();
	return 1;
}

VPE_API int VpeInputGetProtocolVersion(void) {
	g_protocolVersionChecked.store(true, std::memory_order_release);
	return VPE_INPUT_PROTOCOL_VERSION;
}

VPE_API void VpeInputShutdown(void) {
	VpeInputStopPolling();
	g_protocolVersionChecked.store(false, std::memory_order_release);
	g_bindings.clear();
}

VPE_API void VpeInputSetBindings(const VpeInputBinding* bindings, int count) {
	std::lock_guard<std::mutex> lock(g_bindingsMutex);
	g_bindings.clear();

	if (bindings == nullptr || count <= 0) {
		return;
	}

	for (int i = 0; i < count; ++i) {
		if (bindings[i].bindingType != VPE_BINDING_KEYBOARD
			|| bindings[i].action < 0 || bindings[i].action >= VPE_INPUT_MAX) {
			continue;
		}

		CGKeyCode nativeKeyCode = 0;
		if (!MapKeyCode(bindings[i].keyCode, &nativeKeyCode)) {
			continue;
		}

		g_bindings.push_back({
			static_cast<VpeInputAction>(bindings[i].action),
			nativeKeyCode,
		});
	}
}

VPE_API void VpeInputSetGameplayInputEnabled(int enabled) {
	g_input.SetGameplayEnabled(enabled);
}

VPE_API int VpeInputStartPolling(VpeInputEventCallback callback, void* userData, int pollIntervalUs) {
	if (!g_protocolVersionChecked.load(std::memory_order_acquire) ||
		g_running.load(std::memory_order_acquire)) {
		return 0;
	}

	g_input.Start(callback, userData);
	g_pollIntervalUs = pollIntervalUs > 0 ? pollIntervalUs : 500;
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
	g_input.Stop();
}

VPE_API int VpeInputListDevices(VpeInputDeviceInfo* devices, int maxDevices) {
	return g_input.ListDevices(devices, maxDevices);
}

VPE_API int VpeInputListDeviceAxes(int deviceIndex, VpeInputAxisInfo* axes, int maxAxes) {
	return g_input.ListAxes(deviceIndex, axes, maxAxes);
}

VPE_API int64_t VpeGetTimestampUsec(void) {
	return GetTimestampUsecInternal();
}

VPE_API void VpeSetThreadPriority(void) {
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

} // extern "C"

#endif // __APPLE__
