#pragma once

#include "input/motion/MotionHandler.h"
#include "input/api/Wiimote/WiimoteDevice.h"
#include "input/api/Wiimote/WiimoteMessages.h"

#include "input/api/ControllerProvider.h"
#include "input/api/ControllerState.h"

#include <list>
#include <variant>
#include <boost/ptr_container/ptr_vector.hpp>

#ifndef HAS_WIIMOTE
#define HAS_WIIMOTE 1
#endif

#define WIIMOTE_DEBUG 1

class WiimoteControllerProvider : public ControllerProviderBase
{
	friend class WiimoteController;
public:
	constexpr static uint32 kDefaultPacketDelay = 25;

	WiimoteControllerProvider();
	~WiimoteControllerProvider();

	inline static InputAPI::Type kAPIType = InputAPI::Wiimote;
	InputAPI::Type api() const override { return kAPIType; }

	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;

	bool is_connected(size_t index);
	// Slots that answered with a report. An empty Mayflash/DolphinBar entry stays open and is not included.
	std::vector<size_t> connected_indices();
	size_t opened_slot_count();
	bool is_registered_device(size_t index);
	void set_rumble(size_t index, bool state);
	void set_speaker(size_t index, int command);
	bool is_speaker_enabled(size_t index);
	bool can_send_speaker(size_t index);
	bool send_speaker_data(size_t index, const uint8* data, uint32 size);
	static void set_speaker_volume(uint8 volume);
	void request_status(size_t index);
	void set_led(size_t index, size_t player_index);

	uint32 get_packet_delay(size_t index);
	void set_packet_delay(size_t index, uint32 delay);

	struct WiimoteState
	{
		uint16 buttons = 0;
		uint8 flags = 0;
		uint8 battery_level = 0;

		glm::vec3 m_acceleration{}, m_prev_acceleration{};
		float m_roll = 0;

		std::chrono::steady_clock::time_point m_last_motion_timestamp{};
		glm::quat m_gyro_integral{1.0f, 0.0f, 0.0f, 0.0f};
		double m_gyro_integral_time = 0.0;
		MotionSample motion_sample{};
		WiiUMotionHandler motion_handler{};

		bool m_calibrated = false;
		Calibration m_calib_acceleration{};

		struct IRCamera
		{
			IRMode mode = kIRDisabled;
			std::array<IRDot, 4> dots{}, prev_dots{};

			glm::vec2 position{}, m_prev_position{};
			PositionVisibility m_positionVisibility;
			glm::vec2 middle {};
			float distance = 0;
			std::pair<sint32, sint32> indices{ 0,1 };
		}ir_camera{};

		// Last report mode we'd asked the Wiimote for. Avoids flooding 0x12 on
		// every memory read / status while the desired mode is unchanged.
		InputReportId m_requested_report = kNone;

		std::optional<MotionPlusData> m_motion_plus;
		std::variant<std::monostate, NunchuckData, ClassicData> m_extension{};
		// MotionPlus and Nunchuk reports alternate, so one MotionPlus-only frame is
		// normal. This counts frames with no pass-through so an unplug is noticed.
		uint8 m_motion_plus_only_reports = 0;
		// A400FA has been answered. MotionPlus stays inactive until then so the
		// Nunchuk calibration read still reaches the extension.
		bool m_extension_id_received = false;
		// A MotionPlus attachment can reject the first A600FA read right after connecting.
		uint8 m_motion_plus_probe_retries = 0;
	};
	WiimoteState get_state(size_t index);
	

private:
	std::atomic_bool m_running = false;
	std::thread m_reader_thread, m_writer_thread;
	std::shared_mutex m_device_mutex;

	std::thread m_connectionThread;
	std::mutex m_connectionMutex;
    std::condition_variable m_connectionCond;
	std::vector<WiimoteDevicePtr> m_connectedDevices;
	std::mutex m_connectedDeviceMutex;
	struct Wiimote
	{
		Wiimote(WiimoteDevicePtr device)
			: device(std::move(device)) {}

		WiimoteDevicePtr device;
		// A failed I/O operation marks the current handle disconnected. The
		// handle itself is replaced only while holding m_device_mutex exclusively.
		std::atomic_bool disconnected = false;
		// Set after the first real report. Empty DolphinBar slots never set this.
		std::atomic_bool heard_report = false;
		std::atomic_bool rumble = false;
		std::atomic_bool speaker = false;

		std::shared_mutex mutex;
		WiimoteState state{};
		std::chrono::steady_clock::time_point last_startup_probe{};
		// The hardware only echoes the low 16 address bits in read replies.
		// Keep the full addresses in send order to distinguish A40020/A60020.
		std::mutex pending_reads_mutex;
		std::list<uint32> pending_reads;

		std::atomic_uint32_t data_delay = kDefaultPacketDelay;
		std::chrono::high_resolution_clock::time_point data_ts{};
	};
	boost::ptr_vector<Wiimote> m_wiimotes;

	std::list<std::pair<size_t,std::vector<uint8>>> m_write_queue;
	std::mutex m_writer_mutex;
	std::condition_variable m_writer_cond;

	void reader_thread();
	void writer_thread();
	void connectionThread();

	void calibrate(size_t index);
	// force=true rewrites the IR registers even when the software mode is unchanged
	// (needed after MotionPlus activation, which can leave the camera dark).
	IRMode set_ir_camera(size_t index, bool state, bool force = false);

	void send_packet(size_t index, std::vector<uint8> data);
	void send_read_packet(size_t index, MemoryType type, RegisterAddress address, uint16 size);
	void send_write_packet(size_t index, MemoryType type, RegisterAddress address, const std::vector<uint8>& data);

	void parse_acceleration(WiimoteState& wiimote_state, const uint8*& data);

	void rotate_ir(WiimoteState& wiimote_state);
	void calculate_ir_position(WiimoteState& wiimote_state);
	int parse_ir(WiimoteState& wiimote_state, const uint8* data);

	void request_extension(size_t index);
	void detect_motion_plus(size_t index);
	void set_motion_plus(size_t index, bool state);
	void request_nunchuk_calibration(size_t index, NunchuckData& nunchuck);
	void try_activate_motion_plus(size_t index, WiimoteState& state);

	void update_report_type(size_t index);
};



