// license:GPLv3+
// Stub implementation for non-Windows platforms (Linux/macOS implementation TODO)

#ifndef _WIN32

#include "NativeInput.h"
#include <chrono>
#include <thread>

extern "C" {

VPE_API int VpeInputInit(void) {
	// TODO: Implement for Linux (evdev) and macOS (IOKit)
	return 0; // Not implemented
}

VPE_API void VpeInputShutdown(void) {
	// TODO
}

VPE_API void VpeInputSetBindings(const VpeInputBinding* bindings, int count) {
	// TODO
}

VPE_API int VpeInputStartPolling(VpeInputEventCallback callback, void* userData, int pollIntervalUs) {
	// TODO
	return 0;
}

VPE_API void VpeInputStopPolling(void) {
	// TODO
}

VPE_API int64_t VpeGetTimestampUsec(void) {
	auto now = std::chrono::high_resolution_clock::now();
	auto duration = now.time_since_epoch();
	return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

VPE_API void VpeSetThreadPriority(void) {
	// TODO: pthread_setschedparam on Linux/macOS
}

} // extern "C"

#endif // !_WIN32
