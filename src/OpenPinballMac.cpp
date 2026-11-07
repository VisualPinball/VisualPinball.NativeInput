// license:GPLv3+
#include "OpenPinballInput.h"

#include <IOKit/IOCFPlugIn.h>
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/usb/IOUSBLib.h>

#include <map>

namespace OpenPinballDevice {
	namespace {
		CFStringRef PollMode() { return CFSTR("VpeOpenPinballInput"); }

		int64_t ReadNumber(io_service_t service, CFStringRef key, int64_t fallback = 0) {
			const auto property = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
			if (property == nullptr) return fallback;
			int64_t value = fallback;
			if (CFGetTypeID(property) == CFNumberGetTypeID()) {
				CFNumberGetValue(static_cast<CFNumberRef>(property), kCFNumberSInt64Type, &value);
			}
			CFRelease(property);
			return value;
		}

		std::string ReadString(IOHIDDeviceRef device, CFStringRef key) {
			const auto property = IOHIDDeviceGetProperty(device, key);
			char bytes[512];
			if (property == nullptr || CFGetTypeID(property) != CFStringGetTypeID()
				|| !CFStringGetCString(static_cast<CFStringRef>(property), bytes, sizeof(bytes), kCFStringEncodingUTF8)) return {};
			return bytes;
		}

		io_service_t FindUsbDevice(io_service_t hid, int& interfaceNumber) {
			IOObjectRetain(hid);
			auto entry = hid;
			while (entry != IO_OBJECT_NULL) {
				if (IOObjectConformsTo(entry, "IOUSBHostDevice") || IOObjectConformsTo(entry, "IOUSBDevice")) return entry;
				if (IOObjectConformsTo(entry, "IOUSBHostInterface") || IOObjectConformsTo(entry, "IOUSBInterface")) {
					interfaceNumber = static_cast<int>(ReadNumber(entry, CFSTR("bInterfaceNumber"), -1));
				}
				io_registry_entry_t parent = IO_OBJECT_NULL;
				IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent);
				IOObjectRelease(entry);
				entry = parent;
			}
			return IO_OBJECT_NULL;
		}

		ProbeResult ReadSignature(io_service_t service, uint8_t index) {
			IOCFPlugInInterface** plugin = nullptr;
			SInt32 score;
			if (IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
				kIOCFPlugInInterfaceID, &plugin, &score) != kIOReturnSuccess) return ProbeResult::Retry;
			IOUSBDeviceInterface245** usb = nullptr;
			const auto query = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID245),
				reinterpret_cast<LPVOID*>(&usb));
			IODestroyPlugInInterface(plugin);
			if (query != 0 || usb == nullptr) return ProbeResult::Retry;

			// Use a temporary non-seizing device open, with no interface claim.
			// The descriptor request may also succeed if the device is busy.
			const bool opened = (*usb)->USBDeviceOpen(usb) == kIOReturnSuccess;
			const auto signature = VerifySignature(index, [&](uint8_t stringIndex, uint16_t language, auto& bytes) {
				IOUSBDevRequestTO request{};
				request.bmRequestType = USBmakebmRequestType(kUSBIn, kUSBStandard, kUSBDevice);
				request.bRequest = kUSBRqGetDescriptor;
				request.wValue = (kUSBStringDesc << 8) | stringIndex;
				request.wIndex = language;
				request.wLength = static_cast<uint16_t>(bytes.size());
				request.pData = bytes.data();
				request.noDataTimeout = 20;
				request.completionTimeout = 100;
				return (*usb)->DeviceRequestTO(usb, &request) == kIOReturnSuccess
					? static_cast<int>(request.wLenDone) : -1;
			});
			if (opened) (*usb)->USBDeviceClose(usb);
			(*usb)->Release(usb);
			return signature;
		}
	}

	struct Transport::Impl {
		struct Device {
			Impl& owner;
			IOHIDDeviceRef hid;
			bool opened = false;
			bool removed = false;
			int index = -1;
			std::array<unsigned char, MaxReportSize> bytes;

			Device(Impl& owner, IOHIDDeviceRef hid) : owner(owner), hid(hid) {}
			~Device() {
				if (index >= 0) IOHIDDeviceUnscheduleFromRunLoop(hid, CFRunLoopGetCurrent(), PollMode());
				if (opened) IOHIDDeviceClose(hid, kIOHIDOptionsTypeNone);
				CFRelease(hid);
			}

			static void OnRemoval(void* context, IOReturn, void*) {
				static_cast<Device*>(context)->removed = true;
			}

			static void OnReport(void* context, IOReturn result, void*, IOHIDReportType type,
				uint32_t, uint8_t* report, CFIndex size) {
				auto& device = *static_cast<Device*>(context);
				if (!device.removed && result == kIOReturnSuccess && type == kIOHIDReportTypeInput && size > 0) {
					// IOKit supplies the HID report ID byte for numbered reports.
					device.owner.input.Receive(device.index, report, static_cast<size_t>(size), device.owner.timestamp);
				}
			}
		};

		Input& input;
		std::map<uint64_t, std::unique_ptr<Device>> devices;
		std::map<uint64_t, int64_t> retryAfter;
		int64_t timestamp = 0;
		int64_t nextScan = 0;

		explicit Impl(Input& input) : input(input) {}

		ProbeResult Connect(io_service_t service, uint64_t registryId) {
			const auto property = IORegistryEntryCreateCFProperty(service, CFSTR(kIOHIDReportDescriptorKey), kCFAllocatorDefault, 0);
			if (property == nullptr) return ProbeResult::Retry;
			Descriptor descriptor;
			if (CFGetTypeID(property) == CFDataGetTypeID()) {
				const auto data = static_cast<CFDataRef>(property);
				const auto size = CFDataGetLength(data);
				if (size > 0 && size <= MaxDescriptorSize) descriptor = FindDescriptor(CFDataGetBytePtr(data), size);
			}
			CFRelease(property);
			if (descriptor.stringIndex == 0) return ProbeResult::NotOpd;

			int interfaceNumber = -1;
			const auto usb = FindUsbDevice(service, interfaceNumber);
			if (usb == IO_OBJECT_NULL) return ProbeResult::NotOpd;
			const auto location = static_cast<uint32_t>(ReadNumber(usb, CFSTR("locationID")));
			const auto vendor = static_cast<uint16_t>(ReadNumber(usb, CFSTR("idVendor")));
			const auto product = static_cast<uint16_t>(ReadNumber(usb, CFSTR("idProduct")));

			const auto hid = IOHIDDeviceCreate(kCFAllocatorDefault, service);
			if (hid == nullptr) {
				IOObjectRelease(usb);
				return ProbeResult::Retry;
			}
			auto device = std::make_unique<Device>(*this, hid);
			device->opened = IOHIDDeviceOpen(hid, kIOHIDOptionsTypeNone) == kIOReturnSuccess;
			const auto signature = device->opened && location != 0
				? ReadSignature(usb, descriptor.stringIndex) : ProbeResult::Retry;
			IOObjectRelease(usb);
			if (signature != ProbeResult::Found) return signature;
			const auto serial = ReadString(hid, CFSTR(kIOHIDSerialNumberKey));
			const auto name = ReadString(hid, CFSTR(kIOHIDProductKey));
			const auto id = StableId("mac", std::to_string(location) + ":" + std::to_string(interfaceNumber),
				serial, vendor, product, descriptor.reportId);
			device->index = input.Connect(id, name.empty() ? "Open Pinball Device" : name, descriptor, timestamp);
			IOHIDDeviceRegisterRemovalCallback(hid, Device::OnRemoval, device.get());
			IOHIDDeviceRegisterInputReportCallback(hid, device->bytes.data(), device->bytes.size(), Device::OnReport, device.get());
			IOHIDDeviceScheduleWithRunLoop(hid, CFRunLoopGetCurrent(), PollMode());
			devices.emplace(registryId, std::move(device));
			return ProbeResult::Found;
		}

		void RemoveDisconnected(const std::map<uint64_t, io_service_t>* present = nullptr) {
			for (auto it = devices.begin(); it != devices.end();) {
				if (it->second->removed || (present != nullptr && present->count(it->first) == 0)) {
					input.Disconnect(it->second->index, timestamp);
					it = devices.erase(it);
				} else {
					++it;
				}
			}
		}

		void Poll(int64_t now) {
			timestamp = now;
			for (int i = 0; i < 32; ++i) {
				if (CFRunLoopRunInMode(PollMode(), 0, true) != kCFRunLoopRunHandledSource) break;
			}
			RemoveDisconnected();
			if (timestamp < nextScan) return;
			nextScan = timestamp + 1000000;
			io_iterator_t iterator;
			if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching(kIOHIDDeviceKey), &iterator) != kIOReturnSuccess) return;
			std::map<uint64_t, io_service_t> present;
			while (const auto service = IOIteratorNext(iterator)) {
				uint64_t registryId;
				if (IORegistryEntryGetRegistryEntryID(service, &registryId) != kIOReturnSuccess
					|| !present.emplace(registryId, service).second) {
					IOObjectRelease(service);
				}
			}
			// A device can disappear between opening it and registering the
			// removal callback. Retire those handles before connecting replacements.
			if (IOIteratorIsValid(iterator)) {
				RemoveDisconnected(&present);
				for (const auto& [id, service] : present) {
					if (devices.find(id) != devices.end()) continue;
					const auto retry = retryAfter.find(id);
					if (retry != retryAfter.end() && timestamp < retry->second) continue;
					const auto result = Connect(service, id);
					if (result == ProbeResult::Found) retryAfter.erase(id);
					else retryAfter[id] = NextProbe(result, timestamp);
				}
				for (auto it = retryAfter.begin(); it != retryAfter.end();) {
					if (present.count(it->first) == 0) it = retryAfter.erase(it);
					else ++it;
				}
			}
			IOObjectRelease(iterator);
			for (const auto& item : present) IOObjectRelease(item.second);
		}
	};

	Transport::Transport(Input& input) : m_impl(std::make_unique<Impl>(input)) {}
	Transport::~Transport() = default;
	void Transport::Poll(int64_t timestamp) { m_impl->Poll(timestamp); }
}
