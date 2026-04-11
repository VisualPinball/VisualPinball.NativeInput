// license:GPLv3+
// Linux implementation of native input polling

#ifdef __linux__

#include "NativeInput.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
	using Clock = std::chrono::steady_clock;

	struct KeyBindingState {
		VpeInputAction action;
		KeyCode nativeKeyCode;
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
	Display* g_display = nullptr;
	Atom g_activeWindowAtom = None;
	Atom g_wmPidAtom = None;
	std::vector<KeyBindingState> g_bindings;
	bool g_wasForeground = false;

	int64_t GetTimestampUsecInternal() {
		return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_startTime).count();
	}

	KeySym MapKeyCodeToKeySym(int keyCode) {
		switch (keyCode) {
			case VPE_KEY_LSHIFT:
				return XK_Shift_L;
			case VPE_KEY_RSHIFT:
				return XK_Shift_R;
			case VPE_KEY_LCONTROL:
				return XK_Control_L;
			case VPE_KEY_RCONTROL:
				return XK_Control_R;
			case VPE_KEY_LALT:
				return XK_Alt_L;
			case VPE_KEY_RALT:
				return XK_Alt_R;
			case VPE_KEY_ESCAPE:
				return XK_Escape;
			case VPE_KEY_SPACE:
				return XK_space;
			case VPE_KEY_PAGEUP:
				return XK_Prior;
			case VPE_KEY_PAGEDOWN:
				return XK_Next;
			case VPE_KEY_HOME:
				return XK_Home;
			case VPE_KEY_END:
				return XK_End;
			case VPE_KEY_RETURN:
				return XK_Return;
			case VPE_KEY_F1:
				return XK_F1;
			case VPE_KEY_F2:
				return XK_F2;
			case VPE_KEY_F3:
				return XK_F3;
			case VPE_KEY_F4:
				return XK_F4;
			case VPE_KEY_F5:
				return XK_F5;
			case VPE_KEY_F6:
				return XK_F6;
			case VPE_KEY_F7:
				return XK_F7;
			case VPE_KEY_F8:
				return XK_F8;
			case VPE_KEY_F9:
				return XK_F9;
			case VPE_KEY_F10:
				return XK_F10;
			case VPE_KEY_F11:
				return XK_F11;
			case VPE_KEY_F12:
				return XK_F12;
			case VPE_KEY_D0:
				return XK_0;
			case VPE_KEY_D1:
				return XK_1;
			case VPE_KEY_D2:
				return XK_2;
			case VPE_KEY_D3:
				return XK_3;
			case VPE_KEY_D4:
				return XK_4;
			case VPE_KEY_D5:
				return XK_5;
			case VPE_KEY_D6:
				return XK_6;
			case VPE_KEY_D7:
				return XK_7;
			case VPE_KEY_D8:
				return XK_8;
			case VPE_KEY_D9:
				return XK_9;
			case VPE_KEY_NUMPAD1:
				return XK_KP_1;
			case VPE_KEY_A:
				return XK_a;
			case VPE_KEY_B:
				return XK_b;
			case VPE_KEY_O:
				return XK_o;
			case VPE_KEY_P:
				return XK_p;
			case VPE_KEY_S:
				return XK_s;
			case VPE_KEY_D:
				return XK_d;
			case VPE_KEY_T:
				return XK_t;
			case VPE_KEY_W:
				return XK_w;
			case VPE_KEY_Y:
				return XK_y;
			case VPE_KEY_MINUS:
				return XK_minus;
			case VPE_KEY_QUOTE:
				return XK_apostrophe;
			case VPE_KEY_CARET:
				return XK_asciicircum;
			default:
				return NoSymbol;
		}
	}

	bool GetWindowPid(Window window, pid_t* pid) {
		if (window == None || g_wmPidAtom == None) {
			return false;
		}

		Atom actualType = None;
		int actualFormat = 0;
		unsigned long itemCount = 0;
		unsigned long bytesAfter = 0;
		unsigned char* data = nullptr;

		int status = XGetWindowProperty(
			g_display,
			window,
			g_wmPidAtom,
			0,
			1,
			False,
			XA_CARDINAL,
			&actualType,
			&actualFormat,
			&itemCount,
			&bytesAfter,
			&data);

		if (status != Success || actualType != XA_CARDINAL || actualFormat != 32 || itemCount == 0 || data == nullptr) {
			if (data != nullptr) {
				XFree(data);
			}
			return false;
		}

		*pid = static_cast<pid_t>(*reinterpret_cast<unsigned long*>(data));
		XFree(data);
		return true;
	}

	bool IsCurrentProcessForeground() {
		if (g_display == nullptr || g_activeWindowAtom == None || g_wmPidAtom == None) {
			return true;
		}

		Atom actualType = None;
		int actualFormat = 0;
		unsigned long itemCount = 0;
		unsigned long bytesAfter = 0;
		unsigned char* data = nullptr;

		int status = XGetWindowProperty(
			g_display,
			DefaultRootWindow(g_display),
			g_activeWindowAtom,
			0,
			1,
			False,
			XA_WINDOW,
			&actualType,
			&actualFormat,
			&itemCount,
			&bytesAfter,
			&data);

		if (status != Success || actualType != XA_WINDOW || actualFormat != 32 || itemCount == 0 || data == nullptr) {
			if (data != nullptr) {
				XFree(data);
			}
			return true;
		}

		Window activeWindow = *reinterpret_cast<Window*>(data);
		XFree(data);

		pid_t activePid = 0;
		if (!GetWindowPid(activeWindow, &activePid)) {
			return true;
		}

		return activePid == getpid();
	}

	bool IsPressed(const unsigned char* keyBits, KeyCode nativeKeyCode) {
		if (keyBits == nullptr || nativeKeyCode == 0) {
			return false;
		}

		const int index = nativeKeyCode / 8;
		const int bit = nativeKeyCode % 8;
		if (index < 0 || index >= 32) {
			return false;
		}

		return (keyBits[index] & (1u << bit)) != 0;
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
				char keymap[32] = {};
				XQueryKeymap(g_display, keymap);
				const auto* keyBits = reinterpret_cast<const unsigned char*>(keymap);

				for (auto& binding : g_bindings) {
					const float currentValue = IsPressed(keyBits, binding.nativeKeyCode) ? 1.0f : 0.0f;
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

extern "C" {

VPE_API int VpeInputInit(void) {
	g_startTime = Clock::now();
	g_display = XOpenDisplay(nullptr);
	if (g_display == nullptr) {
		return 0;
	}

	g_activeWindowAtom = XInternAtom(g_display, "_NET_ACTIVE_WINDOW", True);
	g_wmPidAtom = XInternAtom(g_display, "_NET_WM_PID", True);
	g_wasForeground = false;
	return 1;
}

VPE_API void VpeInputShutdown(void) {
	VpeInputStopPolling();
	g_bindings.clear();
	g_wasForeground = false;

	if (g_display != nullptr) {
		XCloseDisplay(g_display);
		g_display = nullptr;
	}
}

VPE_API void VpeInputSetBindings(const VpeInputBinding* bindings, int count) {
	g_bindings.clear();
	g_wasForeground = false;

	if (g_display == nullptr || bindings == nullptr || count <= 0) {
		return;
	}

	for (int i = 0; i < count; ++i) {
		if (bindings[i].bindingType != VPE_BINDING_KEYBOARD) {
			continue;
		}

		const KeySym keysym = MapKeyCodeToKeySym(bindings[i].keyCode);
		if (keysym == NoSymbol) {
			continue;
		}

		const KeyCode nativeKeyCode = XKeysymToKeycode(g_display, keysym);
		if (nativeKeyCode == 0) {
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
	if (g_display == nullptr || g_running.load(std::memory_order_acquire)) {
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
	(void)nice(-5);
	struct sched_param params {};
	params.sched_priority = 1;
	pthread_setschedparam(pthread_self(), SCHED_RR, &params);
}

} // extern "C"

#endif // __linux__
