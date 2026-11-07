// license:GPLv3+
#pragma once

#include "NativeInput.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

// A private SDL joystick subsystem. The platform input thread owns the SDL
// lifecycle and calls Poll. Other threads can only read the published snapshot.
class ControllerInput {
public:
	static constexpr int FirstDeviceIndex = 0x40000000;

	ControllerInput();
	~ControllerInput();
	void Start();
	void Stop();
	void SetGameplayEnabled(bool enabled);
	void Poll(int64_t timestamp, bool foreground, VpeInputEventCallback callback, void* userData);
	int AppendDevices(VpeInputDeviceInfo* devices, int maxDevices, int existingCount);
	int ListAxes(int deviceIndex, VpeInputAxisInfo* axes, int maxAxes);

private:
	struct Device;
	bool Reconcile(int64_t timestamp);

	std::mutex m_snapshotMutex;
	std::vector<std::unique_ptr<Device>> m_devices;
	std::vector<VpeInputEvent> m_axisEvents;
	std::atomic<bool> m_gameplayEnabled{true};
	std::atomic<bool> m_refreshOnResume{true};
	bool m_started = false;
	bool m_wasPublishing = false;
	int m_nextIndex = FirstDeviceIndex;
	int64_t m_nextScan = 0;
};
