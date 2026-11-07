// license:GPLv3+
#include "OpenPinballInput.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <linux/usb/ch9.h>
#include <linux/usbdevice_fs.h>
#include <map>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>

namespace OpenPinballDevice {
	namespace {
		namespace fs = std::filesystem;

		struct File {
			int fd;

			explicit File(const fs::path& path, int flags) : fd(open(path.c_str(), flags | O_CLOEXEC)) {}
			File(File&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
			~File() { if (fd >= 0) close(fd); }
		};

		ProbeResult ReadSignature(const fs::path& usbDevice, uint8_t index) {
			int busNumber, deviceNumber;
			if (!(std::ifstream(usbDevice / "busnum") >> busNumber)
				|| !(std::ifstream(usbDevice / "devnum") >> deviceNumber)) return ProbeResult::Retry;
			char usbPath[32];
			std::snprintf(usbPath, sizeof(usbPath), "/dev/bus/usb/%03d/%03d", busNumber, deviceNumber);
			File usb(usbPath, O_RDWR);
			if (usb.fd < 0) return ProbeResult::Retry;
			// A device-recipient control transfer does not claim the HID
			// interface. The kernel HID driver stays attached for report reads.
			return VerifySignature(index, [&](uint8_t stringIndex, uint16_t language, auto& bytes) {
				usbdevfs_ctrltransfer request{};
				request.bRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
				request.bRequest = USB_REQ_GET_DESCRIPTOR;
				request.wValue = (USB_DT_STRING << 8) | stringIndex;
				request.wIndex = language;
				request.wLength = static_cast<uint16_t>(bytes.size());
				request.timeout = 100;
				request.data = bytes.data();
				return ioctl(usb.fd, USBDEVFS_CONTROL, &request);
			});
		}
	}

	struct Transport::Impl {
		struct Device {
			File hid;
			int index;
		};
		struct PendingProbe {
			fs::path entry;
			fs::path hidDevice;
			File hid;
			Descriptor descriptor;
			std::string id;
			std::string name;
			std::future<ProbeResult> result;
		};

		Input& input;
		std::map<fs::path, Device> devices;
		std::map<fs::path, int64_t> retryAfter;
		std::unique_ptr<PendingProbe> pending;
		int64_t nextScan = 0;

		explicit Impl(Input& input) : input(input) {}

		ProbeResult BeginProbe(const fs::path& entry, const fs::path& hidDevice) {
			std::array<unsigned char, MaxDescriptorSize + 1> descriptorBytes;
			std::ifstream descriptorFile(hidDevice / "report_descriptor", std::ios::binary);
			descriptorFile.read(reinterpret_cast<char*>(descriptorBytes.data()), descriptorBytes.size());
			const auto size = static_cast<size_t>(descriptorFile.gcount());
			if (size == 0) return ProbeResult::Retry;
			if (size > MaxDescriptorSize) return ProbeResult::NotOpd;
			const auto descriptor = FindDescriptor(descriptorBytes.data(), size);
			if (descriptor.stringIndex == 0) return ProbeResult::NotOpd;

			const auto usbInterface = hidDevice.parent_path();
			const auto usbDevice = usbInterface.parent_path();
			File hid(fs::path("/dev") / entry.filename(), O_RDONLY | O_NONBLOCK);
			hidraw_devinfo rawInfo{};
			std::error_code pathError;
			if (hid.fd < 0 || ioctl(hid.fd, HIDIOCGRAWINFO, &rawInfo) < 0
				|| fs::canonical(entry / "device", pathError) != hidDevice) return ProbeResult::Retry;
			if (rawInfo.bustype != BUS_USB) return ProbeResult::NotOpd;
			char name[VPE_INPUT_DEVICE_NAME_SIZE]{};
			if (ioctl(hid.fd, HIDIOCGRAWNAME(sizeof(name) - 1), name) <= 0) {
				std::snprintf(name, sizeof(name), "Open Pinball Device");
			}
			std::string serial;
			std::ifstream serialFile(usbDevice / "serial");
			std::getline(serialFile, serial);
			const auto id = StableId("linux", usbInterface.filename().string(), serial,
				static_cast<uint16_t>(rawInfo.vendor), static_cast<uint16_t>(rawInfo.product), descriptor.reportId);
			try {
				// The USB control transfer may time out. Keep the open hidraw
				// handle here and perform only the signature request off-thread.
				pending = std::make_unique<PendingProbe>(PendingProbe{entry, hidDevice, std::move(hid), descriptor, id, name,
					std::async(std::launch::async, [usbDevice, descriptor] {
						return ReadSignature(usbDevice, descriptor.stringIndex);
					})});
			} catch (const std::system_error&) {
				return ProbeResult::Retry;
			}
			return ProbeResult::Found;
		}

		void FinishProbe(int64_t timestamp) {
			if (!pending || pending->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
			auto probe = std::move(pending);
			const auto result = probe->result.get();
			std::error_code error;
			if (fs::canonical(probe->entry / "device", error) != probe->hidDevice || error) return;
			if (result != ProbeResult::Found) {
				retryAfter[probe->hidDevice] = NextProbe(result, timestamp);
				return;
			}
			retryAfter.erase(probe->hidDevice);
			const int index = input.Connect(probe->id, probe->name, probe->descriptor, timestamp);
			devices.emplace(probe->hidDevice, Device{std::move(probe->hid), index});
		}

		void Scan(int64_t timestamp) {
			std::error_code error;
			const fs::path hidraw("/sys/class/hidraw");
			for (fs::directory_iterator it(hidraw, error), end; !error && it != end; it.increment(error)) {
				const auto& entry = it->path();
				std::error_code pathError;
				const auto hidDevice = fs::canonical(entry / "device", pathError);
				if (pathError || devices.find(hidDevice) != devices.end()) continue;

				const auto retry = retryAfter.find(hidDevice);
				if (retry != retryAfter.end() && timestamp < retry->second) continue;
				const auto result = BeginProbe(entry, hidDevice);
				if (result == ProbeResult::Found) break;
				retryAfter[hidDevice] = NextProbe(result, timestamp);
			}
			for (auto it = retryAfter.begin(); it != retryAfter.end();) {
				std::error_code pathError;
				if (!fs::exists(it->first, pathError) && !pathError) it = retryAfter.erase(it);
				else ++it;
			}
		}

		void Poll(int64_t timestamp) {
			std::array<unsigned char, MaxReportSize> bytes;
			for (auto it = devices.begin(); it != devices.end();) {
				const auto& device = it->second;
				bool disconnected = false;
				for (int i = 0; i < 32; ++i) {
					const auto size = read(device.hid.fd, bytes.data(), bytes.size());
					if (size <= 0) {
						disconnected = size == 0 || (errno != EAGAIN && errno != EINTR);
						break;
					}
					input.Receive(device.index, bytes.data(), static_cast<size_t>(size), timestamp);
				}
				if (disconnected) {
					input.Disconnect(device.index, timestamp);
					it = devices.erase(it);
				} else {
					++it;
				}
			}
			FinishProbe(timestamp);
			if (!pending && timestamp >= nextScan) {
				nextScan = timestamp + 1000000;
				Scan(timestamp);
			}
		}
	};

	Transport::Transport(Input& input) : m_impl(std::make_unique<Impl>(input)) {}
	Transport::~Transport() = default;
	void Transport::Poll(int64_t timestamp) { m_impl->Poll(timestamp); }
}
