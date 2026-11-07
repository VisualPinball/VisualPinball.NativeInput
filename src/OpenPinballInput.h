// license:GPLv3+
#pragma once

#include "OpenPinballDescriptor.h"

#include <iterator>
#include <memory>
#include <mutex>
#include <string>

namespace OpenPinballDevice {
	using Keys = std::array<bool, VPE_INPUT_MAX>;

	// Shared Linux/macOS polling state. Only the polling thread mutates device
	// state and delivers callbacks. Snapshot queries can run on either thread.
	class Input {
	public:
		void Start(VpeInputEventCallback callback, void* userData);
		void Stop();
		void SetGameplayEnabled(int enabled);
		void ResetGameplayGate();
		void BeginPoll(int64_t timestamp, bool foreground, const Keys& keys);

		int Connect(const std::string& stableId, const std::string& product, Descriptor descriptor, int64_t timestamp);
		void Disconnect(int deviceIndex, int64_t timestamp);
		void Receive(int deviceIndex, const unsigned char* bytes, size_t size, int64_t timestamp);
		int ListDevices(VpeInputDeviceInfo* devices, int maxDevices);
		int ListAxes(int deviceIndex, VpeInputAxisInfo* axes, int maxAxes);

	private:
		struct Device {
			VpeInputDeviceInfo info{};
			Descriptor descriptor;
			std::array<VpeInputAxisInfo, std::size(Axes)> axes{};
			Clock clock;
			uint32_t buttons = 0;
		};

		void PublishActions(int64_t timestamp);
		void Emit(VpeInputEvent event) const;

		std::vector<Device> m_devices;
		std::mutex m_snapshotMutex;
		int m_nextDeviceIndex = 0;
		VpeInputEventCallback m_callback = nullptr;
		void* m_userData = nullptr;
		Keys m_keys{};
		std::array<ActionLatch, VPE_INPUT_MAX> m_actions{};
		bool m_foreground = false;
		enum class Gate { Unconfigured, Enabled, Disabled };
		std::mutex m_gameplayMutex;
		Gate m_gate = Gate::Unconfigured;
	};

	// IDs keep the USB port as part of the identity so two identical devices
	// with missing or duplicated serial numbers can be mapped independently.
	std::string StableId(const char* platform, const std::string& location,
		const std::string& serial, uint16_t vendor, uint16_t product, uint8_t reportId);

	// The platform transport is created and destroyed on the native polling
	// thread, including the run loop and report callbacks on macOS.
	class Transport {
	public:
		explicit Transport(Input& input);
		~Transport();
		void Poll(int64_t timestamp);

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
}
