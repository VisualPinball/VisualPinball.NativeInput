// license:GPLv3+
#include "OpenPinballInput.h"
#include "NativeButtons.h"

#include <cstdio>

namespace OpenPinballDevice {
	void Input::Start(VpeInputEventCallback callback, void* userData) {
		m_callback = callback;
		m_userData = userData;
		m_actions = {};
		m_nextDeviceIndex = 0;
	}

	void Input::Stop() {
		std::lock_guard<std::mutex> lock(m_snapshotMutex);
		for (const auto& device : m_devices) NativeButtons::GetState().Disconnect(device.info.deviceIndex);
		m_devices.clear();
	}

	void Input::SetGameplayEnabled(int enabled) {
		std::lock_guard<std::mutex> lock(m_gameplayMutex);
		m_gate = enabled ? Gate::Enabled : Gate::Disabled;
	}

	void Input::ResetGameplayGate() {
		std::lock_guard<std::mutex> lock(m_gameplayMutex);
		m_gate = Gate::Unconfigured;
	}

	void Input::BeginPoll(int64_t timestamp, bool foreground, const Keys& keys) {
		m_foreground = foreground;
		m_keys = keys;
		PublishActions(timestamp);
	}

	int Input::Connect(const std::string& stableId, const std::string& product, Descriptor descriptor, int64_t timestamp) {
		Device device{};
		device.descriptor = descriptor;
		device.info.deviceIndex = m_nextDeviceIndex++;
		device.info.isConnected = 1;
		std::snprintf(device.info.stableId, sizeof(device.info.stableId), "%s", stableId.c_str());
		std::snprintf(device.info.displayName, sizeof(device.info.displayName), "%.*s (OPD)",
			static_cast<int>(sizeof(device.info.displayName) - sizeof(" (OPD)")), product.c_str());

		std::vector<int> axisIds;
		for (const auto& axis : Axes) {
			if (axis.offset + 2 > descriptor.payloadSize) break;
			auto& info = device.axes[device.info.axisCount++];
			axisIds.push_back(axis.id);
			info.axisId = axis.id;
			info.usagePage = UsagePage;
			info.usage = Usage;
			info.kind = axis.kind;
			std::snprintf(info.name, sizeof(info.name), "%s", axis.name);
		}

		{
			std::lock_guard<std::mutex> lock(m_snapshotMutex);
			NativeButtons::GetState().Connect(device.info.deviceIndex, stableId, NativeButtons::OpdGenericButtons(), axisIds);
			m_devices.push_back(device);
		}
		Emit({timestamp, VPE_INPUT_EVENT_DEVICES_CHANGED});
		return device.info.deviceIndex;
	}

	void Input::Disconnect(int deviceIndex, int64_t timestamp) {
		{
			std::lock_guard<std::mutex> lock(m_snapshotMutex);
			const auto device = std::find_if(m_devices.begin(), m_devices.end(), [deviceIndex](const Device& item) {
				return item.info.deviceIndex == deviceIndex;
			});
			if (device == m_devices.end()) return;
			NativeButtons::GetState().Disconnect(deviceIndex);
			m_devices.erase(device);
		}
		PublishActions(timestamp);
		Emit({timestamp, VPE_INPUT_EVENT_DEVICES_CHANGED});
	}

	void Input::Receive(int deviceIndex, const unsigned char* bytes, size_t size, int64_t timestamp) {
		std::array<VpeInputEvent, std::size(Axes)> events;
		int eventCount = 0;
		bool buttonsChanged;
		{
			std::lock_guard<std::mutex> lock(m_snapshotMutex);
			const auto device = std::find_if(m_devices.begin(), m_devices.end(), [deviceIndex](const Device& item) {
				return item.info.deviceIndex == deviceIndex;
			});
			if (device == m_devices.end()) return;

			const auto& descriptor = device->descriptor;
			if (size != descriptor.payloadSize + (descriptor.reportId != 0)) return;
			Report report{};
			const bool valid = descriptor.reportId != 0
				? Report::Read(bytes, size, descriptor.reportId, report)
				: Report::ReadPayload(bytes, size, report);
			if (!valid) return;

			const auto genericMask = report.GenericButtons();
			const bool controlsChanged = NativeButtons::GetState().Update(deviceIndex, [&](const auto& button) {
				return (genericMask & (uint32_t{1} << button.buttonId)) != 0;
			}, [&](int axisId, float previous) {
				float value;
				return report.TryReadAxis(Axes[axisId], value) ? value : previous;
			});
			buttonsChanged = controlsChanged || device->buttons != report.PinballButtons();
			device->buttons = report.PinballButtons();
			int64_t sample;
			if (device->clock.Translate(report.Timestamp(), timestamp, sample)) {
				for (int i = 0; i < device->info.axisCount; ++i) {
					auto& axis = device->axes[i];
					report.TryReadAxis(Axes[i], axis.rawValue);
					axis.timestampUsec = sample;
					events[eventCount++] = {sample, VPE_INPUT_EVENT_AXIS, 0, deviceIndex, axis.axisId, axis.rawValue};
				}
			}
		}
		// Controls use every report, including repeated motion timestamps.
		if (buttonsChanged) PublishActions(timestamp);
		if (eventCount > 0) {
			std::lock_guard<std::mutex> lock(m_gameplayMutex);
			if (!m_foreground || m_gate == Gate::Disabled) return;
			for (int i = 0; i < eventCount; ++i) Emit(events[i]);
		}
	}

	int Input::ListDevices(VpeInputDeviceInfo* devices, int maxDevices) {
		std::lock_guard<std::mutex> lock(m_snapshotMutex);
		const int count = static_cast<int>(m_devices.size());
		if (devices != nullptr) {
			for (int i = 0; i < std::min(count, maxDevices); ++i) devices[i] = m_devices[i].info;
		}
		return count;
	}

	int Input::ListAxes(int deviceIndex, VpeInputAxisInfo* axes, int maxAxes) {
		std::lock_guard<std::mutex> lock(m_snapshotMutex);
		for (const auto& device : m_devices) {
			if (device.info.deviceIndex != deviceIndex) continue;
			const int count = device.info.axisCount;
			if (axes != nullptr && maxAxes > 0) std::copy_n(device.axes.begin(), std::min(count, maxAxes), axes);
			return count;
		}
		return 0;
	}

	void Input::PublishActions(int64_t timestamp) {
		// Finish in-flight callbacks before returning from a gate change.
		// Managed input starts dropping events after it disables this gate.
		std::lock_guard<std::mutex> lock(m_gameplayMutex);
		const bool enabled = m_gate == Gate::Unconfigured || (m_foreground && m_gate == Gate::Enabled);
		uint64_t configurable = 0;
		Keys pressed{};
		if (m_foreground || m_gate != Gate::Unconfigured) {
			configurable = NativeButtons::GetState().PressedActions();
			pressed = m_keys;
			uint32_t buttons = 0;
			// Device state only changes on this thread. Snapshot readers never
			// access the button mask.
			for (const auto& device : m_devices) buttons |= device.buttons;
			for (size_t bit = 0; bit < std::size(ButtonActions); ++bit) {
				if ((buttons & (uint32_t{1} << bit)) != 0) pressed[ButtonActions[bit]] = true;
			}
		}
		for (int action = 0; action < VPE_INPUT_MAX; ++action) {
			bool value;
			const bool down = pressed[action] || (configurable & (uint64_t{1} << action)) != 0;
			if (m_actions[action].Update(down, enabled, value)) {
				Emit({timestamp, VPE_INPUT_EVENT_ACTION, action, 0, 0, value ? 1.0f : 0.0f});
			}
		}
	}

	void Input::Emit(VpeInputEvent event) const {
		if (m_callback != nullptr) m_callback(&event, m_userData);
	}

	std::string StableId(const char* platform, const std::string& location,
		const std::string& serial, uint16_t vendor, uint16_t product, uint8_t reportId) {
		// Bound the serial portion to the ABI's fixed-size ID buffer.
		uint64_t serialHash = 14695981039346656037ULL;
		for (unsigned char c : serial) serialHash = (serialHash ^ c) * 1099511628211ULL;
		char id[64];
		std::snprintf(id, sizeof(id), "opd:%s:%04x:%04x:%016llx:%02x:",
			platform, static_cast<unsigned>(vendor), static_cast<unsigned>(product),
			static_cast<unsigned long long>(serialHash), static_cast<unsigned>(reportId));
		return id + location;
	}
}
