#include "input/api/Wiimote/WiimoteControllerProvider.h"
#include "input/api/Wiimote/NativeWiimoteController.h"
#include "input/api/Wiimote/WiimoteMessages.h"

#include <algorithm>
#include <array>
#ifdef HAS_HIDAPI
#include "input/api/Wiimote/hidapi/HidapiWiimote.h"
#endif
#ifdef HAS_BLUEZ
#include "input/api/Wiimote/l2cap/L2CapWiimote.h"
#endif

#include <numbers>
#include <queue>


namespace
{
// Angular rate from an already expanded 16-bit count. Reports are 14-bit;
// calibration zeros and scales are 16-bit.
float MotionPlusAxisRate(float expanded, const MotionPlusData::CalibrationBlock& block, size_t axis)
{
	const int span = int(block.scale[axis]) - int(block.zero[axis]);
	if (span == 0 || block.degrees_div_6 == 0)
		return 0.0f;
	return ((expanded - float(block.zero[axis])) / float(span)) *
		(float(block.degrees_div_6) * 6.0f) * (3.14159265358979323846f / 180.0f);
}

// Slow-mode bias of the learned rest counts, in calibrated pitch, roll, yaw order.
glm::vec3 MotionPlusSlowBias(const MotionPlusData& mp)
{
	return glm::vec3(
		-MotionPlusAxisRate(mp.rest_raw.z, mp.slow_calibration, 2),
		MotionPlusAxisRate(mp.rest_raw.y, mp.slow_calibration, 1),
		-MotionPlusAxisRate(mp.rest_raw.x, mp.slow_calibration, 0));
}

// Integrate at the physical gyro cadence, independently of Nunchuk interleave
// and game FPS. Timestamp each sample before parsing/logging.
// expanded_counts is yaw, roll, pitch already shifted to 16-bit. It is only
// accumulated while learn_zero is set.
void AcceptMotionPlusSample(WiimoteControllerProvider::WiimoteState& state,
    const glm::vec3& measured, const glm::vec3& expanded_counts,
    std::chrono::steady_clock::time_point stamp,
    bool all_slow, bool learn_zero, size_t index)
{
    auto& mp = *state.m_motion_plus;
    glm::vec3 gravity{};
    const auto& cal = state.m_calib_acceleration;
    const glm::vec3 scale = glm::vec3(cal.gravity) - glm::vec3(cal.zero);
    const bool have_gravity = state.m_calibrated &&
        std::abs(scale.x) > 1.0f && std::abs(scale.y) > 1.0f && std::abs(scale.z) > 1.0f;
    if (have_gravity)
        gravity = glm::vec3(state.m_acceleration) / scale;
    const float magnitude = glm::length(gravity);
    // A remote lying still has constant gravity and a gyro that only shows sensor
    // noise around its zero. Factory zeros can be 0.7 rad/s off and drift while
    // the remote warms up, so the zero is relearned on every still window,
    // however far it is from the previous one. Noise is judged by the standard
    // deviation: a single count of slow-mode noise is 0.001 rad/s and the
    // min-max spread of a resting remote exceeds 0.025 rad/s. The IR camera is
    // not consulted: face-down on a table it sees flickering reflections.
    constexpr float kMaxZero = 1.0f;          // rad/s, larger than any factory error seen
    constexpr float kMaxNoiseStdDev = 0.012f; // rad/s per axis; hand tremor is several times larger
    constexpr float kMaxTilt = 0.05f;         // g, about 3 degrees
    const bool jolt = mp.has_last_rate && glm::length(measured - mp.last_rate) > 0.15f;
    if (jolt)
        mp.rest_blocked_until = stamp + std::chrono::milliseconds(500);
    const bool can_learn = learn_zero && all_slow && have_gravity &&
        magnitude > 0.9f && magnitude < 1.1f &&
        std::max({std::abs(measured.x), std::abs(measured.y), std::abs(measured.z)}) < kMaxZero &&
        stamp >= mp.rest_blocked_until;
    if (!can_learn)
        mp.rest_samples = 0;
    else
    {
        if (mp.rest_samples == 0 || glm::length(gravity - mp.rest_gravity) > kMaxTilt)
        {
            mp.rest_started = stamp;
            mp.rest_gravity = gravity;
            mp.rest_sum = {};
            mp.rest_sum_sq = {};
            mp.rest_raw_sum = {};
            mp.rest_samples = 0;
        }
        mp.rest_sum += measured;
        mp.rest_sum_sq += measured * measured;
        mp.rest_raw_sum += expanded_counts;
        ++mp.rest_samples;
        if (mp.rest_samples >= 40 && stamp - mp.rest_started >= std::chrono::seconds(1))
        {
            const float n = float(mp.rest_samples);
            const glm::vec3 variance = glm::max(mp.rest_sum_sq / n - (mp.rest_sum / n) * (mp.rest_sum / n), glm::vec3(0.0f));
            const float noise = std::sqrt(std::max({variance.x, variance.y, variance.z}));
            const glm::vec3 raw_mean = mp.rest_raw_sum / n;
            mp.rest_samples = 0;
            if (noise <= kMaxNoiseStdDev)
            {
                const bool first_zero = !mp.rest_initialized;
                const glm::vec3 previous = MotionPlusSlowBias(mp);
                mp.rest_raw = first_zero ? raw_mean : glm::mix(mp.rest_raw, raw_mean, 0.5f);
                mp.rest_initialized = true;
                mp.rest_offset = MotionPlusSlowBias(mp);
                const float change = glm::length(mp.rest_offset - previous);
                if (first_zero || change > 0.01f)
                    cemuLog_log(LogType::Force, "Wiimote slot {} MotionPlus stationary counts {:.0f},{:.0f},{:.0f} slow-bias {:.3f},{:.3f},{:.3f} rad/s (noise {:.4f}, change {:.3f})",
                        index, mp.rest_raw.x, mp.rest_raw.y, mp.rest_raw.z,
                        mp.rest_offset.x, mp.rest_offset.y, mp.rest_offset.z, noise, change);
            }
        }
    }
    // measured is already expressed relative to the learned rest counts, using
    // the calibration of whichever mode each axis is in. Subtracting rest_offset
    // here would apply the slow-mode bias to fast-mode samples again.
    const glm::vec3 corrected = measured;
    if (mp.last_gyro_timestamp != std::chrono::steady_clock::time_point{})
    {
        const float dt = std::chrono::duration<float>(stamp - mp.last_gyro_timestamp).count();
        if (dt > 0.0f)
        {
            mp.quality_dt_min = std::min(mp.quality_dt_min, dt);
            mp.quality_dt_max = std::max(mp.quality_dt_max, dt);
            mp.quality_dt_sum += dt;
            ++mp.quality_samples;
            mp.quality_bunched += dt < 0.001f ? 1 : 0;
            mp.quality_delayed += dt > 0.015f ? 1 : 0;
            mp.quality_peak_rate = std::max(mp.quality_peak_rate, glm::length(corrected));
        }
        // A missing/reconnected stream must not become a long extrapolation.
        if (dt > 0.0f && dt <= 0.05f)
        {
            // Reconstruct regular 200 Hz samples of the latest received rate.
            // A newly arrived rate must not be averaged backwards over a long
            // USB delivery interval. Reports arriving in a burst simply replace
            // the held value before the next tick. This is a Cemu adaptation of
            // the real-remote path's regular sampling, not a hardware timestamp.
            constexpr auto period = std::chrono::milliseconds(5);
            constexpr float tick_dt = 0.005f;
            const float speed = glm::length(mp.last_integrated_rate);
            const glm::quat step = speed > 1.0e-7f ?
                glm::angleAxis(speed * tick_dt, mp.last_integrated_rate / speed) :
                glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            while (mp.last_sample_tick + period <= stamp)
            {
                state.m_gyro_integral = glm::normalize(state.m_gyro_integral * step);
                state.m_gyro_integral_time += tick_dt;
                mp.last_sample_tick += period;
            }
        }
        else if (dt > 0.05f)
        {
            ++mp.timing_gaps;
            if ((mp.timing_gaps & 0x1f) == 1)
                cemuLog_log(LogType::Force, "Wiimote slot {} gyro gap {:.1f} ms; rebased without extrapolation (count={})",
                    index, dt * 1000.0f, mp.timing_gaps);
            mp.rest_samples = 0;
            mp.last_sample_tick = stamp;
        }
    }
    else
        mp.last_sample_tick = stamp;
    if (mp.quality_last_log == std::chrono::steady_clock::time_point{})
        mp.quality_last_log = stamp;
    if (stamp - mp.quality_last_log >= std::chrono::seconds(2) && mp.quality_samples)
    {
        cemuLog_log(LogType::Force,
            "Wiimote slot {} gyro quality: samples={} dt_ms min/mean/max={:.2f}/{:.2f}/{:.2f} bunched={} delayed={} peak_rad_s={:.2f} range_changes={} gaps={} rejected={}",
            index, mp.quality_samples, mp.quality_dt_min * 1000.0f,
            mp.quality_dt_sum * 1000.0f / mp.quality_samples, mp.quality_dt_max * 1000.0f,
            mp.quality_bunched, mp.quality_delayed, mp.quality_peak_rate,
            mp.quality_range_changes, mp.timing_gaps, mp.rejected_spikes);
        mp.quality_last_log = stamp;
        mp.quality_dt_min = 1.0f;
        mp.quality_dt_max = mp.quality_dt_sum = mp.quality_peak_rate = 0.0f;
        mp.quality_samples = mp.quality_bunched = mp.quality_delayed = mp.quality_range_changes = 0;
    }
    mp.last_gyro_timestamp = stamp;
    mp.last_integrated_rate = corrected;
    mp.angular_velocity = corrected;
    mp.last_rate = measured;
    mp.has_last_rate = true;
}
}

WiimoteControllerProvider::WiimoteControllerProvider()
	: m_running(true)
{
	// The input UI can run without a game, before Cemu opens its log file.
	// Keep the experimental hardware diagnostics visible in that case.
	cemuLog_createLogFile(false);
	cemuLog_log(LogType::Force, "Wiimote motion recovery experiment: latest-rate 200 Hz sampler, per-mode count zero, optical heading");
	m_reader_thread = std::thread(&WiimoteControllerProvider::reader_thread, this);
	m_writer_thread = std::thread(&WiimoteControllerProvider::writer_thread, this);
	m_connectionThread = std::thread(&WiimoteControllerProvider::connectionThread, this);
}

WiimoteControllerProvider::~WiimoteControllerProvider()
{
	if (m_running)
	{
		m_running = false;

		{
			std::scoped_lock lock(m_writer_mutex); 
			m_writer_cond.notify_all();
		}

		{
			std::scoped_lock lock(m_connectionMutex);
			m_connectionCond.notify_all();
		}

		if (m_writer_thread.joinable())
		{
			m_writer_thread.join();
		}

		if (m_reader_thread.joinable())
		{
			m_reader_thread.join();
		}

		if (m_connectionThread.joinable())
		{
			m_connectionThread.join();
		}
	}
}

std::vector<std::shared_ptr<ControllerBase>> WiimoteControllerProvider::get_controllers()
{
	m_connectedDeviceMutex.lock();
	auto devices = m_connectedDevices;
	m_connectedDeviceMutex.unlock();

	std::scoped_lock lock(m_device_mutex);

	for (auto& device : devices)
	{
		bool isDuplicate = false;
		ssize_t lowestReplaceableIndex = -1;
		for (ssize_t i = m_wiimotes.size() - 1; i >= 0; --i)
		{
			auto& wiimote = m_wiimotes[i];
			const auto& wiimoteDevice = wiimote.device;
			if (wiimoteDevice)
			{
				if (*wiimoteDevice == *device)
				{
					if (!wiimote.disconnected.load(std::memory_order_acquire))
						isDuplicate = true;
					else
						lowestReplaceableIndex = i;
					break;
				}
				if (wiimote.disconnected.load(std::memory_order_acquire))
					lowestReplaceableIndex = i;
				continue;
			}

			lowestReplaceableIndex = i;
		}
		if (isDuplicate)
			continue;

		// Register on open. Mayflash empty slots (and some synced remotes) can reject
		// a status probe while still delivering input later; requiring a successful
		// write left those handles unread, so heard_report never fired and a clean PC
		// never got a controller profile. Kick status as best-effort only.
		if (!device->write_data({kStatusRequest, 0x00}))
			cemuLog_logOnce(LogType::Force, "Wiimote HID status probe failed on open; keeping the slot for input reads");

		if (lowestReplaceableIndex != -1)
			m_wiimotes.replace(lowestReplaceableIndex, std::make_unique<Wiimote>(device));
		else
			m_wiimotes.push_back(std::make_unique<Wiimote>(device));
	}

	std::vector<std::shared_ptr<ControllerBase>> result;
	result.reserve(m_wiimotes.size());
	for (size_t i = 0; i < m_wiimotes.size(); ++i)
	{
		result.emplace_back(std::make_shared<NativeWiimoteController>(i));
	}

	return result;
}

bool WiimoteControllerProvider::is_connected(size_t index)
{
	std::shared_lock lock(m_device_mutex);
	return index < m_wiimotes.size() && m_wiimotes[index].device &&
	       !m_wiimotes[index].disconnected.load(std::memory_order_acquire);
}

std::vector<size_t> WiimoteControllerProvider::connected_indices()
{
	std::shared_lock lock(m_device_mutex);
	std::vector<size_t> indices;
	for (size_t i = 0; i < m_wiimotes.size(); ++i)
	{
		if (m_wiimotes[i].device && !m_wiimotes[i].disconnected.load(std::memory_order_acquire) &&
			m_wiimotes[i].heard_report.load(std::memory_order_acquire))
			indices.push_back(i);
	}
	return indices;
}

size_t WiimoteControllerProvider::opened_slot_count()
{
	std::shared_lock lock(m_device_mutex);
	size_t count = 0;
	for (size_t i = 0; i < m_wiimotes.size(); ++i)
	{
		if (m_wiimotes[i].device && !m_wiimotes[i].disconnected.load(std::memory_order_acquire))
			++count;
	}
	return count;
}

bool WiimoteControllerProvider::is_registered_device(size_t index)
{
	std::shared_lock lock(m_device_mutex);
	return index < m_wiimotes.size();
}

void WiimoteControllerProvider::set_rumble(size_t index, bool state)
{
	std::shared_lock lock(m_device_mutex);
	if (index >= m_wiimotes.size())
		return;

	m_wiimotes[index].rumble = state;
	lock.unlock();

	// Report 0x10 is rumble-only; writer also ORs rumble into every other output report.
	send_packet(index, {kRumble, state ? uint8(1) : uint8(0)});
}

namespace
{
uint8 g_speaker_volume = 0x40;

// 4-bit Yamaha ADPCM at 3000 Hz. Volume sits in the fifth byte (0x00-0x40).
std::vector<uint8> SpeakerConfig(uint8 volume)
{
	volume = std::min<uint8>(volume, 0x40);
	return {0x00, 0x00, 0xD0, 0x07, volume, 0x00, 0x00};
}
}

void WiimoteControllerProvider::set_speaker_volume(uint8 volume)
{
	g_speaker_volume = volume;
}

void WiimoteControllerProvider::set_speaker(size_t index, int command)
{
	std::shared_lock lock(m_device_mutex);
	if (index >= m_wiimotes.size())
		return;
	lock.unlock();

	// 0 off, 1/5 on, 2 mute, 3 unmute, 4 play.
	const bool turn_on = command == 1 || command == 5;
	const bool turn_off = command == 0;
	const bool mute = command == 2;
	const bool unmute = command == 3 || command == 4;
	if (!turn_on && !turn_off && !mute && !unmute)
		return;

	if (turn_off)
	{
		m_wiimotes[index].speaker = false;
		send_packet(index, {kSpeakerState, 0x00});
		cemuLog_log(LogType::Force, "Wiimote slot {} speaker off", index);
		return;
	}
	if (mute)
	{
		send_packet(index, {kSpeakerMute, 0x04});
		return;
	}
	if (unmute && !turn_on)
	{
		send_packet(index, {kSpeakerMute, 0x00});
		return;
	}

	m_wiimotes[index].speaker = true;
	send_packet(index, {kSpeakerState, 0x04});
	send_packet(index, {kSpeakerMute, 0x04});
	send_write_packet(index, kRegisterMemory, static_cast<RegisterAddress>(0x4a20009), {0x01});
	send_write_packet(index, kRegisterMemory, static_cast<RegisterAddress>(0x4a20001), {0x08});
	send_write_packet(index, kRegisterMemory, static_cast<RegisterAddress>(0x4a20001), SpeakerConfig(g_speaker_volume));
	send_write_packet(index, kRegisterMemory, static_cast<RegisterAddress>(0x4a20008), {0x01});
	send_packet(index, {kSpeakerMute, 0x00});
	cemuLog_log(LogType::Force, "Wiimote slot {} speaker on, volume {:#04x}", index, g_speaker_volume);
}

bool WiimoteControllerProvider::is_speaker_enabled(size_t index)
{
	std::shared_lock lock(m_device_mutex);
	return index < m_wiimotes.size() && m_wiimotes[index].speaker;
}

bool WiimoteControllerProvider::can_send_speaker(size_t index)
{
	if (!is_speaker_enabled(index))
		return false;
	std::unique_lock lock(m_writer_mutex);
	uint32 queued = 0;
	for (const auto& packet : m_write_queue)
	{
		if (packet.index == index && !packet.data.empty() && packet.data[0] == kSpeakerData)
			++queued;
	}
	return queued < 3;
}

bool WiimoteControllerProvider::send_speaker_data(size_t index, const uint8* data, uint32 size)
{
	if (!data || size == 0 || size > 20 || !is_speaker_enabled(index))
		return false;

	std::vector<uint8> packet(2 + 20, 0);
	packet[0] = kSpeakerData;
	packet[1] = uint8(size << 3);
	std::copy(data, data + size, packet.begin() + 2);

	std::unique_lock lock(m_writer_mutex);
	uint32 queued = 0;
	auto oldest = m_write_queue.end();
	for (auto it = m_write_queue.begin(); it != m_write_queue.end(); ++it)
	{
		if (it->index == index && !it->data.empty() && it->data[0] == kSpeakerData)
		{
			if (oldest == m_write_queue.end())
				oldest = it;
			++queued;
		}
	}
	if (queued >= 6 && oldest != m_write_queue.end())
		m_write_queue.erase(oldest);
	m_write_queue.push_back(OutgoingReport{index, std::move(packet), 0});
	m_writer_cond.notify_one();
	return true;
}

void WiimoteControllerProvider::request_status(size_t index)
{
	send_packet(index, {kStatusRequest, 0x00});
}

void WiimoteControllerProvider::set_led(size_t index, size_t player_index)
{
	uint8 mask = 0;
	mask |= 1 << (4 + (player_index % 4));
	if (player_index >= 4)
		mask |= 1 << (4 + ((player_index - 3) % 4));
	send_packet(index, {kLED, mask});
}

uint32 WiimoteControllerProvider::get_packet_delay(size_t index)
{
	std::shared_lock lock(m_device_mutex);
	if (index < m_wiimotes.size())
	{
		return m_wiimotes[index].data_delay;
	}

	return kDefaultPacketDelay;
}

void WiimoteControllerProvider::set_packet_delay(size_t index, uint32 delay)
{
	std::shared_lock lock(m_device_mutex);
	if (index < m_wiimotes.size())
	{
		m_wiimotes[index].data_delay = delay;
	}
}

WiimoteControllerProvider::WiimoteState WiimoteControllerProvider::get_state(size_t index)
{
	std::shared_lock lock(m_device_mutex);
	if (index < m_wiimotes.size())
	{
		std::shared_lock data_lock(m_wiimotes[index].mutex);
		return m_wiimotes[index].state;
	}

	return {};
}

void WiimoteControllerProvider::connectionThread()
{
	SetThreadName("Wiimote-connect");
	while (m_running.load(std::memory_order_relaxed))
	{
		std::vector<WiimoteDevicePtr> devices;
#ifdef HAS_HIDAPI
		const auto& hidDevices = HidapiWiimote::get_devices();
		std::ranges::move(hidDevices, std::back_inserter(devices));
#endif
#ifdef HAS_BLUEZ
		const auto& l2capDevices = L2CapWiimote::get_devices();
		std::ranges::move(l2capDevices, std::back_inserter(devices));
#endif
		{
			std::scoped_lock lock(m_connectedDeviceMutex);
			m_connectedDevices.clear();
			std::ranges::move(devices, std::back_inserter(m_connectedDevices));
		}
		std::unique_lock<std::mutex> lock(m_connectionMutex);
		m_connectionCond.wait_for(lock, std::chrono::seconds(2));
	}
}


void WiimoteControllerProvider::reader_thread()
{
	SetThreadName("Wiimote-reader");
	std::chrono::steady_clock::time_point lastCheck = {};
	while (m_running.load(std::memory_order_relaxed))
	{
		const auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::seconds>(now - lastCheck) > std::chrono::milliseconds(500))
		{
			// check for new connected wiimotes
			get_controllers();
			lastCheck = std::chrono::steady_clock::now();
		}

		bool receivedAnyPacket = false;
		std::shared_lock lock(m_device_mutex);
		for (size_t index = 0; index < m_wiimotes.size(); ++index)
		{
			auto& wiimote = m_wiimotes[index];
			if (!wiimote.device || wiimote.disconnected.load(std::memory_order_acquire))
				continue;

			const auto read_data = wiimote.device->read_data();
			if (!read_data)
			{
				wiimote.disconnected.store(true, std::memory_order_release);
				continue;
			}
			if (read_data->empty())
				continue;
			const auto report_timestamp = std::chrono::steady_clock::now();
			wiimote.heard_report.store(true, std::memory_order_release);
			receivedAnyPacket = true;

			std::shared_lock read_lock(wiimote.mutex);
			WiimoteState new_state = wiimote.state;
			read_lock.unlock();

			// DolphinBar exposes empty slots before a remote is synced. The status
			// write on open can fail then; retry after actual input until calibration
			// arrives, rather than leaving the remote permanently in core-only mode.
			if (!new_state.m_calibrated && report_timestamp - wiimote.last_startup_probe >= std::chrono::milliseconds(500))
			{
				wiimote.last_startup_probe = report_timestamp;
				std::scoped_lock writer_lock(m_writer_mutex);
				m_write_queue.push_back(OutgoingReport{index, std::vector<uint8>{kStatusRequest, 0x00}, 0});
				m_writer_cond.notify_one();
			}

			bool update_report = false;
			bool got_acceleration_report = false;
			float gyro[3]{};
			if (new_state.m_motion_plus)
			{
				const auto& angular_velocity = new_state.m_motion_plus->angular_velocity;
				gyro[0] = angular_velocity.x;
				gyro[1] = angular_velocity.y;
				gyro[2] = angular_velocity.z;
			}

			const uint8* data = read_data->data();
			const auto id = (InputReportId)*data;
			static std::atomic_uint64_t observed_report_ids{0};
			const auto report_bit = uint64{1} << (uint8(id) & 0x3f);
			if ((observed_report_ids.fetch_or(report_bit, std::memory_order_relaxed) & report_bit) == 0)
				cemuLog_log(LogType::Force, "Wiimote slot {} first input report id={:#04x} size={}",
					index, uint8(id), read_data->size());
			++data;
			switch (id)
			{
			case kStatus:
				{
					cemuLog_logOnce(LogType::Force, "Wiimote status report received");
					new_state.buttons = *(uint16*)data & (~0x60E0);
					data += 2;
					new_state.flags = *data;
					++data;
					cemuLog_log(LogType::Force, "Wiimote slot {} status flags={:#04x}", index, new_state.flags);
					data += 2; // skip zeroes
					new_state.battery_level = *data;
					++data;

					new_state.ir_camera.mode = set_ir_camera(index, true);
					{
						std::shared_lock sync_lock(wiimote.mutex);
						new_state.m_requested_report = wiimote.state.m_requested_report;
					}
					if(!new_state.m_calibrated)
						calibrate(index);

					if(!new_state.m_motion_plus)
						detect_motion_plus(index);

					if (HAS_FLAG(new_state.flags, kExtensionConnected))
					{
                        cemuLog_logDebug(LogType::Force,"Extension flag is set");
						if(new_state.m_extension.index() == 0 && !new_state.m_motion_plus)
							request_extension(index);
					}
					else
					{
						// The port flag is the unplug signal until MotionPlus is active.
						// After that, MotionPlus-only reports are the signal: the flag can
						// stay clear while a Nunchuk is still plugged in.
						const bool motion_plus_active = new_state.m_motion_plus && new_state.m_motion_plus->activated;
						if (!motion_plus_active && new_state.m_extension.index() != 0)
						{
							cemuLog_log(LogType::Force, "Wiimote slot {} extension disconnected", index);
							new_state.m_extension = {};
							new_state.m_motion_plus_only_reports = 0;
						}
					}

					update_report = true;
				}
				break;
			case kRead:
				{
					if (read_data->size() < 6)
					{
						cemuLog_log(LogType::Force, "Wiimote slot {} truncated memory report: {} bytes",
							index, read_data->size());
						break;
					}
                    cemuLog_logDebug(LogType::Force,"WiimoteControllerProvider::read_thread: kRead");
					new_state.buttons = *(uint16*)data & (~0x60E0);
					data += 2;
					const uint8 error_flag = *data & 0xF, size = (*data >> 4) + 1;
					++data;
					if (!error_flag && read_data->size() < size_t(6 + size))
					{
						cemuLog_log(LogType::Force, "Wiimote slot {} truncated memory report: {} bytes, expected {}",
							index, read_data->size(), size_t(6 + size));
						break;
					}

					const auto reply_address = (*(betype<uint16>*)data).value();
					data += 2;
					uint32 address = reply_address;
					bool matched_read = false;
					{
						std::scoped_lock pending_lock(wiimote.pending_reads_mutex);
						const auto pending = std::find_if(wiimote.pending_reads.begin(), wiimote.pending_reads.end(),
							[reply_address](uint32 full_address) { return (full_address & 0xffff) == reply_address; });
						if (pending != wiimote.pending_reads.end())
						{
							address = *pending;
							wiimote.pending_reads.erase(pending);
							matched_read = true;
						}
					}
					cemuLog_log(LogType::Force, "Wiimote slot {} memory read address={:#08x} size={} matched={} error={:#x}",
						index, address, size, matched_read, error_flag);
					if (error_flag)
					{
						// A failed Nunchuk read must not keep MotionPlus activation waiting.
						if (address == (kRegisterExtensionCalibration & 0xFFFFFF) &&
							std::holds_alternative<NunchuckData>(new_state.m_extension))
						{
							auto& nunchuck = std::get<NunchuckData>(new_state.m_extension);
							nunchuck.calibration_finished = true;
							try_activate_motion_plus(index, new_state);
						}
						else if (address == (kRegisterExtensionType & 0xFFFFFF))
						{
							new_state.m_extension_id_received = true;
							if (std::holds_alternative<NunchuckData>(new_state.m_extension) &&
								!std::get<NunchuckData>(new_state.m_extension).identified)
								new_state.m_extension = {};
							try_activate_motion_plus(index, new_state);
						}
						else if (address == (kRegisterMotionPlusDetect & 0xFFFFFF) && !new_state.m_motion_plus &&
							new_state.m_motion_plus_probe_retries < 5)
						{
							++new_state.m_motion_plus_probe_retries;
							cemuLog_log(LogType::Force, "Wiimote slot {} MotionPlus probe failed; retry {} via status",
								index, new_state.m_motion_plus_probe_retries);
							request_status(index);
						}
						break;
					}
					if (!matched_read)
						continue;
					if (address == (kRegisterCalibration & 0xFFFFFF))
					{
                        cemuLog_logDebug(LogType::Force,"Calibration received");

						cemu_assert(size == 8);

						new_state.m_calib_acceleration.zero.x = (uint16)*data << 2;
						++data;
						new_state.m_calib_acceleration.zero.y = (uint16)*data << 2;
						++data;
						new_state.m_calib_acceleration.zero.z = (uint16)*data << 2;
						++data;
						// --XXYYZZ
						new_state.m_calib_acceleration.zero.x |= (*data >> 4) & 0x3; // 5|4 -> 1|0
						new_state.m_calib_acceleration.zero.y |= (*data >> 2) & 0x3; // 3|4 -> 1|0
						new_state.m_calib_acceleration.zero.z |= *data & 0x3;
						++data;

						new_state.m_calib_acceleration.gravity.x = (uint16)*data << 2;
						++data;
						new_state.m_calib_acceleration.gravity.y = (uint16)*data << 2;
						++data;
						new_state.m_calib_acceleration.gravity.z = (uint16)*data << 2;
						++data;
						new_state.m_calib_acceleration.gravity.x |= (*data >> 4) & 0x3; // 5|4 -> 1|0
						new_state.m_calib_acceleration.gravity.y |= (*data >> 2) & 0x3; // 3|4 -> 1|0
						new_state.m_calib_acceleration.gravity.z |= *data & 0x3;
						++data;

						new_state.m_calibrated = true;
					}
					else if (address == (kRegisterExtensionType & 0xFFFFFF) ||
						address == (kRegisterMotionPlusDetect & 0xFFFFFF))
					{
						if (size == 0xf)
						{
							cemuLog_logDebug(LogType::Force,"Extension type received but no extension connected");
							continue;
						}

						cemu_assert(size == 6);
						auto be_type = *(betype<uint64>*)data;
						data += 6; // 48
						be_type >>= 16;
						be_type &= 0xFFFFFFFFFFFF;
						cemuLog_log(LogType::Force, "Wiimote slot {} extension identifier {:#014x}", index, be_type.value());
						if (address == (kRegisterExtensionType & 0xFFFFFF))
							new_state.m_extension_id_received = true;
						// An already active MotionPlus identifies itself at A400FA. Its
						// fifth byte names the pass-through mode (04 or 05). If it was
						// activated before this process started, deactivate it so the
						// normal A600 calibration sequence can run on the next status.
						const auto active_motion_plus_id = be_type.value() & 0x00FFFFFFFFFF;
						if (active_motion_plus_id == 0x00A4200405 || active_motion_plus_id == 0x00A4200505)
						{
							if (!new_state.m_motion_plus)
							{
								cemuLog_log(LogType::Force, "Wiimote slot {} found an already active MotionPlus; resetting it for calibration", index);
								set_motion_plus(index, false);
								new_state.m_extension = {};
							}
							else if (active_motion_plus_id == 0x00A4200505 &&
								!std::holds_alternative<NunchuckData>(new_state.m_extension))
							{
								// Already in Nunchuk pass-through, so the calibration registers are not reachable.
								NunchuckData nunchuck;
								nunchuck.identified = true;
								nunchuck.calibration_finished = true;
								new_state.m_extension = nunchuck;
							}
							else if (active_motion_plus_id == 0x00A4200405)
								new_state.m_extension = {};
						}
						// An inactive MotionPlus keeps the last pass-through mode in its fifth
						// byte, so an attachment that was used with a Nunchuk reads 0000A6200505.
						else switch ((be_type.value() & 0xFFFF00FF) == 0xA6200005 ? kExtensionMotionPlus : be_type.value())
						{
						case kExtensionNunchuck:
						case kExtensionNunchuckDolphinBar:
                            cemuLog_logDebug(LogType::Force,"Extension Type Received: Nunchuck");
							if (!std::holds_alternative<NunchuckData>(new_state.m_extension))
								new_state.m_extension = NunchuckData{};
							std::get<NunchuckData>(new_state.m_extension).identified = true;
							break;
						case kExtensionClassic:
                            cemuLog_logDebug(LogType::Force,"Extension Type Received: Classic");
							new_state.m_extension = ClassicData{};
							break;
						case kExtensionClassicPro:
                            cemuLog_logDebug(LogType::Force,"Extension Type Received: Classic Pro");
							new_state.m_extension = ClassicData{};
                            break;
						case kExtensionGuitar:
                            cemuLog_logDebug(LogType::Force,"Extension Type Received: Guitar");
                            break;
						case kExtensionDrums:
                            cemuLog_logDebug(LogType::Force,"Extension Type Received: Drums");
                            break;
						case kExtensionBalanceBoard:
                            cemuLog_logDebug(LogType::Force,"Extension Type Received: Balance Board");
                            break;
						case kExtensionMotionPlus:
						case kExtensionMotionPlusIntegrated:
						case kExtensionMotionPlusIntegratedDolphinBar:
						case kExtensionMotionPlusInactive:
							cemuLog_logOnce(LogType::Force, "MotionPlus identifier received");
							if (!new_state.m_motion_plus)
							{
								new_state.m_motion_plus = MotionPlusData{};
								send_read_packet(index, kRegisterMemory, kRegisterMotionPlusCalibration, 0x10);
								send_read_packet(index, kRegisterMemory,
									static_cast<RegisterAddress>(kRegisterMotionPlusCalibration + 0x10), 0x10);
							}
							break;
						case kExtensionPartialyInserted:
                            cemuLog_logDebug(LogType::Force,"Extension only partially inserted");
							new_state.m_extension = {};
							request_status(index);
							break;
						default:
                            cemuLog_logDebug(LogType::Force,"Unknown extension: {:#x}", be_type.value());
                            new_state.m_extension = {};
							break;
						}

						if (std::holds_alternative<NunchuckData>(new_state.m_extension))
						{
							auto& nunchuck = std::get<NunchuckData>(new_state.m_extension);
							if (nunchuck.identified && !nunchuck.calibration_finished &&
								(!new_state.m_motion_plus || !new_state.m_motion_plus->activated))
								request_nunchuk_calibration(index, nunchuck);
						}
						try_activate_motion_plus(index, new_state);
						update_report = true;
					}
					else if ((address == (kRegisterMotionPlusCalibration & 0xFFFFFF) ||
						address == ((kRegisterMotionPlusCalibration + 0x10) & 0xFFFFFF)) && size == 0x10 &&
						new_state.m_motion_plus)
					{
						// MotionPlus has separate fast and slow calibration blocks at A60020/A60030.
						auto read_block = [](MotionPlusData::CalibrationBlock& block, const uint8* block_data)
						{
							for (size_t axis = 0; axis < 3; ++axis)
								block.zero[axis] = (*(betype<uint16>*)(block_data + axis * 2)).value();
							for (size_t axis = 0; axis < 3; ++axis)
								block.scale[axis] = (*(betype<uint16>*)(block_data + 6 + axis * 2)).value();
							block.degrees_div_6 = block_data[12];
						};
						auto& mp = *new_state.m_motion_plus;
						if (address == (kRegisterMotionPlusCalibration & 0xFFFFFF))
							read_block(mp.fast_calibration, data);
						else
						{
							read_block(mp.slow_calibration, data);
							mp.calibration_valid = mp.fast_calibration.degrees_div_6 != 0 &&
								mp.slow_calibration.degrees_div_6 != 0;
							cemuLog_log(LogType::Force,
								"MotionPlus calibration fast zero={},{},{} scale={},{},{} deg/6={}; slow zero={},{},{} scale={},{},{} deg/6={}",
								mp.fast_calibration.zero.x, mp.fast_calibration.zero.y, mp.fast_calibration.zero.z,
								mp.fast_calibration.scale.x, mp.fast_calibration.scale.y, mp.fast_calibration.scale.z,
								mp.fast_calibration.degrees_div_6,
								mp.slow_calibration.zero.x, mp.slow_calibration.zero.y, mp.slow_calibration.zero.z,
								mp.slow_calibration.scale.x, mp.slow_calibration.scale.y, mp.slow_calibration.scale.z,
								mp.slow_calibration.degrees_div_6);
							cemuLog_log(LogType::Force, "MotionPlus calibration complete; valid={}", mp.calibration_valid);
							try_activate_motion_plus(index, new_state);
							update_report = true;
						}
					}
					else if (address == (kRegisterExtensionCalibration & 0xFFFFFF))
					{
                        cemuLog_logDebug(LogType::Force,"Extension calibration received");
						if (size != 0x10)
						{
							cemuLog_log(LogType::Force, "Wiimote slot {} Nunchuk calibration size {} rejected", index, size);
							if (std::holds_alternative<NunchuckData>(new_state.m_extension))
								std::get<NunchuckData>(new_state.m_extension).calibration_finished = true;
						}
						else
						{
							std::string raw_hex;
							raw_hex.reserve(16 * 3);
							for (int byte_index = 0; byte_index < 16; ++byte_index)
								raw_hex += fmt::format("{:02x} ", data[byte_index]);
							cemuLog_log(LogType::Force, "Wiimote slot {} Nunchuk calibration bytes {}", index, raw_hex);
							std::visit(
								overloaded
								{
									[](auto)
									{
									},
									[data](NunchuckData& nunchuck)
									{
										nunchuck.calibration_finished = true;
										std::array<uint8, 14> zero{};
										if (memcmp(zero.data(), data, zero.size()) == 0)
										{
											cemuLog_log(LogType::Force, "Nunchuk calibration data is zero; keeping default stick scale");
											return;
										}

										// The accelerometer and the stick blocks are judged separately:
										// a bad stick block must not discard a good accelerometer one.
										glm::vec<3, uint16> acc_zero, acc_gravity;
										acc_zero.x = uint16((uint16)data[0] << 2 | ((data[3] >> 4) & 0x3));
										acc_zero.y = uint16((uint16)data[1] << 2 | ((data[3] >> 2) & 0x3));
										acc_zero.z = uint16((uint16)data[2] << 2 | (data[3] & 0x3));
										acc_gravity.x = uint16((uint16)data[4] << 2 | ((data[7] >> 4) & 0x3));
										acc_gravity.y = uint16((uint16)data[5] << 2 | ((data[7] >> 2) & 0x3));
										acc_gravity.z = uint16((uint16)data[6] << 2 | (data[7] & 0x3));
										const auto acc_axis_ok = [](uint16 zero_value, uint16 gravity_value)
										{
											return gravity_value > zero_value + 80 && gravity_value < zero_value + 400;
										};
										if (acc_axis_ok(acc_zero.x, acc_gravity.x) && acc_axis_ok(acc_zero.y, acc_gravity.y) &&
											acc_axis_ok(acc_zero.z, acc_gravity.z))
										{
											nunchuck.calibration.zero = acc_zero;
											nunchuck.calibration.gravity = acc_gravity;
										}
										cemuLog_log(LogType::Force, "Nunchuk accelerometer zero={},{},{} 1g={},{},{}",
											nunchuck.calibration.zero.x, nunchuck.calibration.zero.y, nunchuck.calibration.zero.z,
											nunchuck.calibration.gravity.x, nunchuck.calibration.gravity.y, nunchuck.calibration.gravity.z);

										const uint8 max_x = data[8];
										const uint8 min_x = data[9];
										const uint8 center_x = data[10];
										const uint8 max_y = data[11];
										const uint8 min_y = data[12];
										const uint8 center_y = data[13];
										const auto axis_ordered = [](uint8 min_value, uint8 center_value, uint8 max_value)
										{
											return min_value + 8 < center_value && center_value + 8 < max_value;
										};
										if (!axis_ordered(min_x, center_x, max_x) || !axis_ordered(min_y, center_y, max_y))
										{
											cemuLog_log(LogType::Force,
												"Nunchuk calibration stick x min/center/max={}/{}/{} y={}/{}/{} rejected; keeping default stick scale",
												min_x, center_x, max_x, min_y, center_y, max_y);
											return;
										}

										nunchuck.calibration.max.x = max_x;
										nunchuck.calibration.max.y = max_y;
										nunchuck.calibration.min.x = min_x;
										nunchuck.calibration.min.y = min_y;
										nunchuck.calibration.center.x = center_x;
										nunchuck.calibration.center.y = center_y;
										nunchuck.calibration_valid = true;
										cemuLog_log(LogType::Force, "Nunchuk calibration stick x min/center/max={}/{}/{} y={}/{}/{}",
											nunchuck.calibration.min.x, nunchuck.calibration.center.x, nunchuck.calibration.max.x,
											nunchuck.calibration.min.y, nunchuck.calibration.center.y, nunchuck.calibration.max.y);
									}
								}, new_state.m_extension);
						}
						try_activate_motion_plus(index, new_state);
					}
					else
					{
                        cemuLog_logDebug(LogType::Force,"Unhandled read data received");
                        continue;
					}

					update_report = true;
				}
				break;
            case kAcknowledge:
                {
                    new_state.buttons = *(uint16*)data & (~0x60E0);
                    data += 2;
                    const auto report_id = *data++;
                    const auto error = *data++;
					if (error)
						cemuLog_log(LogType::Force, "Wiimote slot {} output report {:#04x} rejected: {:#x}",
							index, report_id, error);
                    break;
                }
			case kDataCore:
				{
					// 30 BB BB
					new_state.buttons = *(uint16*)data & (~0x60E0);
					data += 2;
					break;
				}
			case kDataCoreAcc:
				{
					// 31 BB BB AA AA AA
					new_state.buttons = *(uint16*)data & (~0x60E0);
					parse_acceleration(new_state, data);
					got_acceleration_report = true;
					break;
				}
			case kDataCoreExt8:
				{
					// 32 BB BB EE EE EE EE EE EE EE EE
					new_state.buttons = *(uint16*)data & (~0x60E0);
					data += 2;
					break;
				}
			case kDataCoreAccIR:
				{
					// 33 BB BB AA AA AA II II II II II II II II II II II II 
					new_state.buttons = *(uint16*)data & (~0x60E0);
					parse_acceleration(new_state, data);
					got_acceleration_report = true;
					data += parse_ir(new_state, data);
					break;
				}
			case kDataCoreExt19:
				{
					// 34 BB BB EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE 
					new_state.buttons = *(uint16*)data & (~0x60E0);
					data += 2;
					break;
				}
			case kDataCoreAccExt:
				{
					// 35 BB BB AA AA AA EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE
					new_state.buttons = *(uint16*)data & (~0x60E0);
					parse_acceleration(new_state, data);
					got_acceleration_report = true;
					break;
				}
			case kDataCoreIRExt:
				{
					// 36 BB BB II II II II II II II II II II EE EE EE EE EE EE EE EE EE
					new_state.buttons = *(uint16*)data & (~0x60E0);
					data += 2;
					break;
				}
			case kDataCoreAccIRExt:
				{
					// 37 BB BB AA AA AA II II II II II II II II II II EE EE EE EE EE EE
					if (read_data->size() < 22)
					{
						cemuLog_log(LogType::Force, "Wiimote slot {} truncated motion report: {} bytes",
							index, read_data->size());
						break;
					}
					new_state.buttons = *(uint16*)data & (~0x60E0);
					parse_acceleration(new_state, data);
					got_acceleration_report = true;
					// This report always has 10 bytes of basic IR. Parsing it as extended
					// mode would consume two bytes of the extension and shift the gyro.
					const IRMode saved_ir_mode = new_state.ir_camera.mode;
					new_state.ir_camera.mode = kBasicIR;
					parse_ir(new_state, data);
					new_state.ir_camera.mode = saved_ir_mode;
					data += 10;
					if (saved_ir_mode != kBasicIR && saved_ir_mode != kIRDisabled)
						update_report = true;
					const uint8* extension_data = data;
					const bool motion_plus_active = new_state.m_motion_plus && new_state.m_motion_plus->activated;
					if (motion_plus_active && (data[5] & 0x02))
					{
						auto& mp = *new_state.m_motion_plus;
						const auto raw_yaw = uint16(data[0]) | (uint16(data[3] & 0xfc) << 6);
						const auto raw_roll = uint16(data[1]) | (uint16(data[4] & 0xfc) << 6);
						const auto raw_pitch = uint16(data[2]) | (uint16(data[5] & 0xfc) << 6);
						mp.slow_yaw = (data[3] & 0x02) != 0;
						mp.slow_pitch = (data[3] & 0x01) != 0;
						mp.slow_roll = (data[4] & 0x02) != 0;
						mp.extension_connected = (data[4] & 0x01) != 0;
						if (new_state.m_motion_plus_only_reports < 255)
							++new_state.m_motion_plus_only_reports;
						// Pass-through stops when the Nunchuk is removed. A short run of
						// MotionPlus-only frames is that unplug, not the usual interleave.
						if (new_state.m_motion_plus_only_reports >= 8 && new_state.m_extension.index() != 0)
						{
							cemuLog_log(LogType::Force, "Wiimote slot {} extension removed", index);
							new_state.m_extension = {};
							mp.extension_connected = false;
						}
						const bool invalid_sentinel = raw_yaw == 0x3fff || raw_roll == 0x3fff || raw_pitch == 0x3fff;
						if (!invalid_sentinel)
							mp.orientation = glm::vec3(raw_yaw, raw_roll, raw_pitch);

						const auto expand = [](uint16 raw)
						{
							return float((int(raw) << 2) | ((raw & 1) ? 3 : 0));
						};
						// Yaw, roll, pitch. The same rest counts are removed with the
						// calibration block of the mode this axis is in right now.
						const glm::vec3 expanded(expand(raw_yaw), expand(raw_roll), expand(raw_pitch));
						const auto corrected_rate = [&](uint16 raw, float rest_count, const MotionPlusData::CalibrationBlock& block, size_t axis)
						{
							const float value = MotionPlusAxisRate(expand(raw), block, axis);
							if (!mp.rest_initialized)
								return value;
							return value - MotionPlusAxisRate(rest_count, block, axis);
						};
						const auto& yaw_calibration = mp.slow_yaw ? mp.slow_calibration : mp.fast_calibration;
						const auto& roll_calibration = mp.slow_roll ? mp.slow_calibration : mp.fast_calibration;
						const auto& pitch_calibration = mp.slow_pitch ? mp.slow_calibration : mp.fast_calibration;
						const glm::vec3 calibrated(
							-corrected_rate(raw_pitch, mp.rest_raw.z, pitch_calibration, 2),
							corrected_rate(raw_roll, mp.rest_raw.y, roll_calibration, 1),
							-corrected_rate(raw_yaw, mp.rest_raw.x, yaw_calibration, 0));


						// Delay only a large jump accompanied by a range transition.
						// Confirmed swings retain their original timestamp.
						const auto near_rate = [](const glm::vec3& a, const glm::vec3& b)
						{
							const glm::vec3 d = glm::abs(a - b);
							return std::max({d.x, d.y, d.z}) <= 30.0f;
						};
						const uint8 slow_flags = (mp.slow_pitch ? 1 : 0) |
							(mp.slow_roll ? 2 : 0) | (mp.slow_yaw ? 4 : 0);
						if (mp.has_last_rate && slow_flags != mp.last_slow_flags)
							++mp.quality_range_changes;
						if (mp.calibration_valid && !invalid_sentinel &&
							std::isfinite(calibrated.x) && std::isfinite(calibrated.y) && std::isfinite(calibrated.z))
						{
							if (mp.has_held_rate)
							{
								if (mp.has_last_rate && near_rate(calibrated, mp.last_rate))
								{
									++mp.rejected_spikes;
									cemuLog_log(LogType::Force, "Wiimote slot {} isolated MotionPlus range spike rejected (count={})", index, mp.rejected_spikes);
								}
								else
									AcceptMotionPlusSample(new_state, mp.held_rate, {}, mp.held_timestamp, false, false, index);
								mp.has_held_rate = false;
								AcceptMotionPlusSample(new_state, calibrated, expanded, report_timestamp, slow_flags == 7, false, index);
							}
							else if (mp.has_last_rate && slow_flags != mp.last_slow_flags && !near_rate(calibrated, mp.last_rate))
							{
								mp.held_rate = calibrated;
								mp.held_timestamp = report_timestamp;
								mp.has_held_rate = true;
								mp.rest_samples = 0;
							}
							else
								AcceptMotionPlusSample(new_state, calibrated, expanded, report_timestamp, slow_flags == 7, true, index);
							mp.last_slow_flags = slow_flags;
							gyro[0] = mp.angular_velocity.x;
							gyro[1] = mp.angular_velocity.y;
							gyro[2] = mp.angular_velocity.z;
						}
						data += 6;
						static std::atomic_uint32_t motion_plus_log_counter{0};
						if ((motion_plus_log_counter.fetch_add(1, std::memory_order_relaxed) & 0x3f) == 0)
							cemuLog_log(LogType::Force, "MotionPlus sample: raw={},{},{} gyro={:.3f},{:.3f},{:.3f} rad/s",
								raw_pitch, raw_roll, raw_yaw, gyro[0], gyro[1], gyro[2]);
					}
					else
					{
						std::array<uint8, 6> passthrough_data{};
						if (motion_plus_active)
						{
							new_state.m_motion_plus_only_reports = 0;
							if (new_state.m_motion_plus)
								new_state.m_motion_plus->extension_connected = true;
							// A Nunchuk plugged in after activation only shows up here, and the
							// active MotionPlus hides its calibration registers.
							if (!std::holds_alternative<NunchuckData>(new_state.m_extension))
							{
								NunchuckData nunchuck;
								nunchuck.identified = true;
								nunchuck.calibration_finished = true;
								new_state.m_extension = nunchuck;
								cemuLog_log(LogType::Force,
									"Wiimote slot {} Nunchuk found through MotionPlus pass-through; using default calibration", index);
							}
							// Undo the Nunchuk pass-through bit packing before using the common parser.
							std::copy_n(extension_data, passthrough_data.size(), passthrough_data.begin());
							// Restore the bits moved by MotionPlus. The three lost
							// accelerometer LSBs are approximated from the next bit.
							const uint8 packed = extension_data[5];
							passthrough_data[4] = uint8((passthrough_data[4] & 0xfe) | (packed >> 7));
							passthrough_data[5] = uint8(((packed >> 2) & 1) |
								(((packed >> 3) & 1) << 1) | (((packed >> 4) & 1) << 2) |
								(((packed >> 4) & 1) << 3) | (((packed >> 5) & 1) << 4) |
								(packed & 0x20) | (((packed >> 6) & 1) << 6) |
								(((packed >> 6) & 1) << 7));
							data = passthrough_data.data();
						}
					std::visit(
						overloaded
						{
							[](auto)
							{
							},
							[data](NunchuckData& nunchuck) mutable
							{
								nunchuck.raw_axis.x = *data;
								++data;
								nunchuck.raw_axis.y = *data;
								++data;

								glm::vec<3, uint16> raw_acc;
								raw_acc.x = (uint16)*data << 2;
								++data;
								raw_acc.y = (uint16)*data << 2;
								++data;
								raw_acc.z = (uint16)*data << 2;
								++data;
								nunchuck.z = (*data & 1) == 0;
								nunchuck.c = (*data & 2) == 0;

								raw_acc.x |= (*data >> 2) & 0x3; // 3|2 -> 1|0
								raw_acc.y |= (*data >> 4) & 0x3; // 5|4 -> 1|0
								raw_acc.z |= (*data >> 6) & 0x3; // 7|6 -> 1|0

								auto& calib = nunchuck.calibration;

								if (nunchuck.raw_axis.x < nunchuck.calibration.center.x) // [-1, 0]
									nunchuck.axis.x = ((float)nunchuck.raw_axis.x - calib.min.x) / ((float)nunchuck.
										calibration.center.x - calib.min.x + 0.012f) - 1.0f;
								else // [0, 1]
									nunchuck.axis.x = (float)(nunchuck.raw_axis.x - nunchuck.calibration.center.x) / (
										nunchuck.calibration.max.x - nunchuck.calibration.center.x + 0.012f);

								if (nunchuck.raw_axis.y <= nunchuck.calibration.center.y) // [-1, 0]
									nunchuck.axis.y = ((float)nunchuck.raw_axis.y - calib.min.y) / ((float)nunchuck.
										calibration.center.y - calib.min.y + 0.012f) - 1.0f;
								else // [0, 1]
									nunchuck.axis.y = (float)(nunchuck.raw_axis.y - nunchuck.calibration.center.y) / (
										nunchuck.calibration.max.y - nunchuck.calibration.center.y);
								glm::vec3 acceleration = raw_acc;
								nunchuck.prev_acceleration = nunchuck.acceleration;
								nunchuck.acceleration = acceleration - glm::vec3(calib.zero);

								float acc[3]{ -nunchuck.acceleration.x, -nunchuck.acceleration.z, nunchuck.acceleration.y };
								const auto grav = nunchuck.calibration.gravity - nunchuck.calibration.zero;

								auto tacc = nunchuck.acceleration;
								auto pacc = nunchuck.prev_acceleration;
								if (grav != glm::vec<3, uint16>{})
								{
									acc[0] /= (float)grav.x;
									acc[1] /= (float)grav.y;
									acc[2] /= (float)grav.z;

									tacc.x /= (float)grav.x;
									pacc.x /= (float)grav.x;

									tacc.y /= (float)grav.y;
									pacc.y /= (float)grav.y;

									tacc.z /= (float)grav.z;
									pacc.z /= (float)grav.z;
								}
								static std::atomic_uint32_t nunchuk_log_counter{0};
								if ((nunchuk_log_counter.fetch_add(1, std::memory_order_relaxed) & 0x3f) == 0)
									cemuLog_log(LogType::Force, "Nunchuk: stick={:.2f},{:.2f} C={} Z={} raw_acc={},{},{} acc={:.2f},{:.2f},{:.2f}",
										nunchuck.axis.x, nunchuck.axis.y, nunchuck.c, nunchuck.z,
										raw_acc.x, raw_acc.y, raw_acc.z, acc[0], acc[1], acc[2]);
								float zero3[3]{};
								float zero4[4]{};


								nunchuck.motion_sample = MotionSample(
									acc,
									glm::length(tacc - pacc),
									zero3,
									zero3,
									zero4
								);
                                cemuLog_logDebug(LogType::Force,"Nunchuck: Z={}, C={} | {}, {} | {:.2f}, {:.2f}, {:.2f}",
                                                 nunchuck.z, nunchuck.c,
                                                 nunchuck.axis.x, nunchuck.axis.y,
                                                 RadToDeg(nunchuck.acceleration.x), RadToDeg(nunchuck.acceleration.y),
                                                 RadToDeg(nunchuck.acceleration.z));
							},
							[data](ClassicData& classic) mutable
							{
								classic.left_raw_axis.x = *data & 0x3F;
								classic.right_raw_axis.x = (*data & 0xC0) >> 3; // 7|6 -> 4|3
								++data;

								classic.left_raw_axis.y = *data & 0x3F;
								classic.right_raw_axis.x |= (*data & 0xC0) >> 5; // 7|6 -> 2|1
								++data;

								classic.right_raw_axis.y = *data & 0x1F;
								classic.raw_trigger.x = (*data & 0x60) >> 2; // 6|5 -> 4|3
								classic.right_raw_axis.x |= (*data & 0x80) >> 7; // 7 -> 0
								++data;

								classic.raw_trigger.x |= (*data & 0xE0) >> 5; // 7|5 -> 2|0
								classic.raw_trigger.y = (*data & 0x1F);
								++data;

								classic.buttons = ~(*(uint16*)data);
								data += 2;

								classic.left_axis = classic.left_raw_axis;
								classic.left_axis /= 63.0f;
								classic.left_axis = classic.left_axis * 2.0f - 1.0f;

								classic.right_axis = classic.right_raw_axis;
								classic.right_axis /= 31.0f;
								classic.right_axis = classic.right_axis * 2.0f - 1.0f;

								classic.trigger = classic.raw_trigger;
								classic.trigger /= 31.0f;
                                cemuLog_logDebug(LogType::Force,"Classic Controller: Buttons={:b} | {}, {} | {}, {} | {}, {}",
                                                 classic.buttons, classic.left_axis.x, classic.left_axis.y,
                                                 classic.right_axis.x, classic.right_axis.y, classic.trigger.x,
                                                 classic.trigger.y);

							}
						}, new_state.m_extension);
						data = extension_data + 6;
					}


					break;
				}
			case kDataExt:
				{
					// 3d EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE
					break;
				}
			default:
                cemuLog_logDebug(LogType::Force,"unhandled input packet id {} for wiimote {}", id, index);
			}

			// Integrate only fresh accelerometer reports. Status and memory replies
			// must not advance the IMU or replace the last motion sample.
			if (got_acceleration_report && new_state.m_calibrated)
			{
				const auto& calibration = new_state.m_calib_acceleration;
				const float scale_x = float(calibration.gravity.x) - float(calibration.zero.x);
				const float scale_y = float(calibration.gravity.y) - float(calibration.zero.y);
				const float scale_z = float(calibration.gravity.z) - float(calibration.zero.z);
				if (std::abs(scale_x) > 1.0f && std::abs(scale_y) > 1.0f && std::abs(scale_z) > 1.0f)
				{
					const float acc_x = -new_state.m_acceleration.x / scale_x;
					const float acc_y = -new_state.m_acceleration.z / scale_z;
					const float acc_z = new_state.m_acceleration.y / scale_y;
					const auto motion_now = report_timestamp;
					float delta_time = 0.01f;
					if (new_state.m_last_motion_timestamp != std::chrono::steady_clock::time_point{})
						delta_time = std::chrono::duration<float>(motion_now - new_state.m_last_motion_timestamp).count();
					new_state.m_last_motion_timestamp = motion_now;
					if (delta_time <= 0.0f || delta_time > 0.2f)
						delta_time = 0.01f;
					new_state.motion_handler.processMotionSample(delta_time, gyro[0], gyro[1], gyro[2],
						acc_x, acc_y, acc_z);
					// Physical stationary zero is already removed for native MotionPlus.
					new_state.motion_sample = new_state.motion_handler.getMotionSample(!new_state.m_motion_plus.has_value());
					if (new_state.m_motion_plus)
						new_state.motion_sample.setGyroIntegral(new_state.m_gyro_integral, new_state.m_gyro_integral_time);
					static std::atomic_uint32_t fused_motion_log_counter{0};
					if ((fused_motion_log_counter.fetch_add(1, std::memory_order_relaxed) & 0x7f) == 0)
					{
						float fused_gyro[3]{}, orientation[3]{};
						new_state.motion_sample.getGyrometer(fused_gyro);
						new_state.motion_sample.getVPADOrientation(orientation);
						cemuLog_log(LogType::Force,
							"Wiimote fused motion: gyro={:.3f},{:.3f},{:.3f} rad/s orientation={:.3f},{:.3f},{:.3f} turns",
							fused_gyro[0], fused_gyro[1], fused_gyro[2],
							orientation[0], orientation[1], orientation[2]);
					}
				}
			}

			std::unique_lock data_lock(wiimote.mutex);
			wiimote.state = new_state;
			data_lock.unlock();

			if (update_report)
				update_report_type(index);
			// After the copy is committed, so this handshake is not overwritten by new_state.
			advance_pointer_startup(index, id, report_timestamp);
		}

		lock.unlock();
		if (!receivedAnyPacket)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
}

void WiimoteControllerProvider::parse_acceleration(WiimoteState& wiimote_state, const uint8*& data)
{
	glm::vec<3, uint16> raw_acc;
	// acc encoded in BB BB
	raw_acc.x = (*data >> 5) & 3; // 6|5 -> 1|0
	++data;
	raw_acc.y = (*data >> 4) & 2; // 5 -> 1
	raw_acc.z = (*data >> 5) & 2; // 6 -> 1
	++data;

	raw_acc.x |= (uint16)*data << 2;
	++data;
	raw_acc.y |= (uint16)*data << 2;
	++data;
	raw_acc.z |= (uint16)*data << 2;
	++data;

	wiimote_state.m_prev_acceleration = wiimote_state.m_acceleration;
	glm::vec3 acceleration = raw_acc;

	const auto& calib = wiimote_state.m_calib_acceleration;
	wiimote_state.m_acceleration = acceleration - glm::vec3(calib.zero);

	glm::vec3 tmp = calib.gravity;
	tmp -= calib.zero;
	acceleration = (wiimote_state.m_acceleration / tmp);

	const float pi_2 = (float)std::numbers::pi / 2.0f;
	wiimote_state.m_roll = std::atan2(acceleration.z, acceleration.x) - pi_2;
}

void WiimoteControllerProvider::rotate_ir(WiimoteState& wiimote_state)
{
	const float rot = wiimote_state.m_roll;
	if (rot == 0.0f)
		return;

	const float sin = std::sin(rot);
	const float cos = std::cos(rot);
	int i = -1;
	for (auto& dot : wiimote_state.ir_camera.dots)
	{
		i++;
		if (!dot.visible)
			continue;
		// move to center, rotate and move back
		dot.pos -= 0.5f;
		auto tmp = dot.pos;
		dot.pos.x = (tmp.x * cos) + (tmp.y * (-sin));
		dot.pos.y = (tmp.x * sin) + (tmp.y * cos);
		dot.pos += 0.5f;
	}
}

void WiimoteControllerProvider::calculate_ir_position(WiimoteState& wiimote_state)
{
	auto& ir = wiimote_state.ir_camera;
	ir.m_prev_position = ir.position;

	std::pair indices = ir.indices;
	const bool have_middle = ir.middle.x != 0.0f || ir.middle.y != 0.0f;
	float best_distance = std::numeric_limits<float>::max();
	bool found_pair = false;
	for (size_t i = 0; i < ir.dots.size(); ++i)
	{
		if (!ir.dots[i].visible)
			continue;

		for (size_t j = i + 1; j < ir.dots.size(); ++j)
		{
			if (!ir.dots[j].visible)
				continue;

			const auto mid = (ir.dots[i].pos + ir.dots[j].pos) / 2.0f;
			if (have_middle)
			{
				if (mid.x == 0.0f && ir.middle.x != 0.0f)
					continue;
				const float last_angle = std::atan2(ir.middle.y, ir.middle.x == 0.0f ? 1e-6f : ir.middle.x);
				float angle = std::atan2(mid.y, mid.x == 0.0f ? 1e-6f : mid.x);
				if (std::abs(last_angle - angle) > DegToRad(10.0f))
					continue;

				const float distance = std::abs(ir.distance - glm::length(ir.dots[i].pos - ir.dots[j].pos));
				if (distance > 0.1f && distance > best_distance)
					continue;
				best_distance = distance;
			}
			else
			{
				// Cold start: pick the first pair of visible dots.
				best_distance = 0.0f;
			}

			found_pair = true;
			indices = {(sint32)i, (sint32)j};
			if (!have_middle)
				break;
		}
		if (found_pair && !have_middle)
			break;
	}
	if (found_pair)
		ir.indices = indices;

	if (ir.dots[indices.first].visible && ir.dots[indices.second].visible)
	{
		ir.prev_dots[indices.first] = ir.dots[indices.first];
		ir.prev_dots[indices.second] = ir.dots[indices.second];
		ir.position = (ir.dots[indices.first].pos + ir.dots[indices.second].pos) / 2.0f;

		ir.middle = ir.position;
		ir.distance = glm::length(ir.dots[indices.first].pos - ir.dots[indices.second].pos);
		ir.indices = indices;
		ir.m_positionVisibility = PositionVisibility::FULL;
	}
	else if (ir.dots[indices.first].visible)
	{
		ir.position = ir.middle + (ir.dots[indices.first].pos - ir.prev_dots[indices.first].pos);
		ir.m_positionVisibility = PositionVisibility::PARTIAL;
	}
	else if (ir.dots[indices.second].visible)
	{
		ir.position = ir.middle + (ir.dots[indices.second].pos - ir.prev_dots[indices.second].pos);
		ir.m_positionVisibility = PositionVisibility::PARTIAL;
	}
	else {
		ir.m_positionVisibility = PositionVisibility::NONE;
	}
}


sint32 WiimoteControllerProvider::parse_ir(WiimoteState& wiimote_state, const uint8* data)
{
	switch (wiimote_state.ir_camera.mode)
	{
	case kIRDisabled:
		wiimote_state.ir_camera.dots = {};
		return 0;
	case kBasicIR:
		{
			const auto ir = (BasicIR*)data;
			for (int i = 0; i < 2; ++i)
			{
				auto& dot1 = wiimote_state.ir_camera.dots[i * 2];
				auto& dot2 = wiimote_state.ir_camera.dots[i * 2 + 1];

				dot1.raw.x = ir[i].x1 | (ir[i].bits.x1 << 8); // 9|8
				dot1.raw.y = ir[i].y1 | (ir[i].bits.y1 << 8);
				dot1.size = 0;

				dot2.raw.x = ir[i].x2 | (ir[i].bits.x2 << 8);
				dot2.raw.y = ir[i].y2 | (ir[i].bits.y2 << 8);
				dot2.size = 0;

				dot1.visible = dot1.raw != glm::vec<2, uint16>(0x3ff, 0x3ff);
				if (dot1.visible)
					dot1.pos = glm::vec2(1.0f - dot1.raw.x / 1023.0f, (float)dot1.raw.y / 768.0f);
				else
					dot1.pos = {};

				dot2.visible = dot2.raw != glm::vec<2, uint16>(0x3ff, 0x3ff);
				if (dot2.visible)
					dot2.pos = glm::vec2(1.0f - dot2.raw.x / 1023.0f, (float)dot2.raw.y / 768.0f);
				else
					dot2.pos = {};
			}

			rotate_ir(wiimote_state);
			calculate_ir_position(wiimote_state);
			static std::atomic_uint32_t ir_log_counter{0};
			if ((ir_log_counter.fetch_add(1, std::memory_order_relaxed) & 0x7f) == 0)
			{
				int visible = 0;
				for (const auto& dot : wiimote_state.ir_camera.dots)
					visible += dot.visible ? 1 : 0;
				const auto& dots = wiimote_state.ir_camera.dots;
				cemuLog_log(LogType::Force,
					"Wiimote IR basic visible={} pos={:.2f},{:.2f} raw0={},{} raw1={},{}",
					visible, wiimote_state.ir_camera.position.x, wiimote_state.ir_camera.position.y,
					dots[0].raw.x, dots[0].raw.y, dots[1].raw.x, dots[1].raw.y);
			}
			return sizeof(BasicIR) * 2;
		}
	case kExtendedIR:
		{
			const auto ir = (ExtendedIR*)data;
			for (int i = 0; i < 4; ++i)
			{
				auto& dot = wiimote_state.ir_camera.dots[i];
				dot.raw.x = ir[i].x;
				dot.raw.y = ir[i].y;

				dot.raw.x |= (uint16)ir[i].bits.x << 8; // 9|8
				dot.raw.y |= (uint16)ir[i].bits.y << 8; // 9|8

				dot.size = ir[i].bits.size;

				dot.visible = dot.raw != glm::vec<2, uint16>(0x3ff, 0x3ff);
				if (dot.visible)
					dot.pos = glm::vec2(1.0f - dot.raw.x / 1023.0f, (float)dot.raw.y / 768.0f);
				else
					dot.pos = {};
			}

			rotate_ir(wiimote_state);
			calculate_ir_position(wiimote_state);
			return sizeof(ExtendedIR) * 4;
		}
	default:
		cemu_assert(false);
		break;
	}
	return 0;
}

void WiimoteControllerProvider::request_extension(size_t index)
{
	// send_write_packet(index, kRegisterMemory, kRegisterExtensionEncrypted, { 0x00 });
	send_write_packet(index, kRegisterMemory, kRegisterExtension1, {0x55});
	send_write_packet(index, kRegisterMemory, kRegisterExtension2, {0x00});
	send_read_packet(index, kRegisterMemory, kRegisterExtensionType, 6);
}

void WiimoteControllerProvider::detect_motion_plus(size_t index)
{
	send_read_packet(index, kRegisterMemory, kRegisterMotionPlusDetect, 6);
}

void WiimoteControllerProvider::request_nunchuk_calibration(size_t index, NunchuckData& nunchuck)
{
	if (nunchuck.calibration_requested || nunchuck.calibration_finished)
		return;
	nunchuck.calibration_requested = true;
	cemuLog_log(LogType::Force, "Wiimote slot {} requesting Nunchuk calibration before MotionPlus activation", index);
	send_read_packet(index, kRegisterMemory, kRegisterExtensionCalibration, 0x10);
}

void WiimoteControllerProvider::try_activate_motion_plus(size_t index, WiimoteState& state)
{
	if (!state.m_motion_plus || state.m_motion_plus->activated || !state.m_motion_plus->calibration_valid)
		return;
	if (!state.m_extension_id_received && HAS_FLAG(state.flags, kExtensionConnected))
		return;

	if (std::holds_alternative<NunchuckData>(state.m_extension))
	{
		auto& nunchuck = std::get<NunchuckData>(state.m_extension);
		// The identifier read is what proves the extension init writes have completed.
		if (!nunchuck.identified)
			return;
		if (!nunchuck.calibration_finished)
		{
			request_nunchuk_calibration(index, nunchuck);
			return;
		}
	}

	set_motion_plus(index, true);
	state.m_motion_plus->activated = true;
	cemuLog_log(LogType::Force, "Wiimote slot {} MotionPlus activation sent", index);
	// Those enable writes can leave the IR camera dark, and they often land while
	// the remote is still streaming. Restart the handshake so the camera registers
	// are written only after a core-button report proves the remote is listening.
	restart_pointer_startup(index);
}

void WiimoteControllerProvider::set_motion_plus(size_t index, bool state)
{
	if (state) {
		send_write_packet(index, kRegisterMemory, kRegisterMotionPlusInit, { 0x55 });
		// 0x05 enables Nunchuk pass-through so gyro and Nunchuk reports can be interleaved.
		send_write_packet(index, kRegisterMemory, kRegisterMotionPlusEnable, { 0x05 });
	}
	else
	{
		send_write_packet(index, kRegisterMemory, kRegisterExtension1, { 0x55 });
	}
}


void WiimoteControllerProvider::writer_thread()
{
	SetThreadName("Wiimote-writer");
	while (m_running.load(std::memory_order_relaxed))
	{
		std::unique_lock writer_lock(m_writer_mutex);
		while (m_write_queue.empty())
		{
			if (m_writer_cond.wait_for(writer_lock, std::chrono::milliseconds(250)) == std::cv_status::timeout)
			{
				if (!m_running.load(std::memory_order_relaxed))
					return;
			}
		}

		auto index = (size_t)-1;
		std::vector<uint8> data;
		uint32 mode_cookie = 0;
		std::shared_lock device_lock(m_device_mutex);

		// get first packet of device which is ready to be sent
		const auto now = std::chrono::high_resolution_clock::now();
		std::array<bool, 8> waiting{};
		for (auto it = m_write_queue.begin(); it != m_write_queue.end(); ++it)
		{
			if (it->index >= m_wiimotes.size() || it->index >= waiting.size() || waiting[it->index])
				continue;

			// A packet that is not due yet blocks later packets for the same remote,
			// so speaker audio cannot overtake the configuration that enables it.
			// Speaker data itself is paced at 13 ms (20 bytes of 3000 Hz ADPCM).
			const uint32 delay = (!it->data.empty() && it->data[0] == kSpeakerData)
				? 13
				: m_wiimotes[it->index].data_delay.load(std::memory_order_relaxed);
			if (now < m_wiimotes[it->index].data_ts + std::chrono::milliseconds(delay))
			{
				waiting[it->index] = true;
				continue;
			}
			if (now >= m_wiimotes[it->index].data_ts + std::chrono::milliseconds(delay))
			{
				index = it->index;
				data = std::move(it->data);
				mode_cookie = it->mode_cookie;
				m_write_queue.erase(it);
				break;
			}
		}
		writer_lock.unlock();

		if (index != (size_t)-1 && !data.empty())
		{
			auto& wiimote = m_wiimotes[index];
			if (!wiimote.device || wiimote.disconnected.load(std::memory_order_acquire))
				continue;
			if (wiimote.rumble)
				data[1] |= 1;
			if (data[0] == kReadMemory && data.size() >= 7)
			{
				const uint32 address = (uint32(data[2]) << 16) | (uint32(data[3]) << 8) | data[4];
				std::scoped_lock pending_lock(wiimote.pending_reads_mutex);
				wiimote.pending_reads.push_back(address);
			}
			if (!wiimote.device->write_data(data))
			{
				wiimote.disconnected.store(true, std::memory_order_release);
				wiimote.rumble = false;
				std::scoped_lock pending_lock(wiimote.pending_reads_mutex);
				wiimote.pending_reads.clear();
			}
			else
			{
				wiimote.data_ts = std::chrono::high_resolution_clock::now();
				// Publish only after the HID write. Reports already buffered by the
				// adapter must not count as the remote accepting this mode change.
				if (mode_cookie != 0 && data.size() >= 3 && data[0] == kType)
				{
					wiimote.published_report.store(data[2], std::memory_order_relaxed);
					wiimote.published_ns.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
					wiimote.published_cookie.store(mode_cookie, std::memory_order_release);
				}
			}
		}
		device_lock.unlock();

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		std::this_thread::yield();
	}
}

void WiimoteControllerProvider::calibrate(size_t index)
{
	send_read_packet(index, kEEPROMMemory, kRegisterCalibration, 8);
}

void WiimoteControllerProvider::update_report_type(size_t index)
{
	auto& wm = m_wiimotes[index];
	// The handshake owns report 0x12 until the remote has shown core buttons.
	// Sending 0x33/0x37 here used to race that pause and get ignored.
	if (wm.pointer_startup == PointerStartup::PauseStream || wm.pointer_startup == PointerStartup::WaitCore)
		return;

	std::shared_lock read_lock(m_wiimotes[index].mutex);
	auto& state = m_wiimotes[index].state;

	// 0x36/0x37 only have room for basic IR. Switch before requesting the report.
	if (state.ir_camera.mode != kIRDisabled)
	{
		const bool packed = state.m_extension.index() != 0 || state.m_motion_plus.has_value();
		const IRMode want = packed ? kBasicIR : kExtendedIR;
		if (state.ir_camera.mode != want)
		{
			read_lock.unlock();
			set_ir_camera(index, true, false);
			return;
		}
	}

	const bool extension = state.m_extension.index() != 0 || state.m_motion_plus.has_value();
	const bool ir = state.ir_camera.mode != kIRDisabled;
	const bool motion = true; // UseMotion();

	InputReportId report_type;
	if (extension && ir && motion)
		report_type = kDataCoreAccIRExt;
	else if (extension && ir)
		report_type = kDataCoreIRExt;
	else if (extension && motion)
		report_type = kDataCoreAccExt;
	else if (ir && motion)
		report_type = kDataCoreAccIR;
	else if (extension)
		report_type = kDataCoreExt19;
	else if (ir)
		report_type = kDataCoreAccIR;
	else if (motion)
		report_type = kDataCoreAcc;
	else
		report_type = kDataCore;

	if (state.m_requested_report == report_type && wm.pointer_startup != PointerStartup::ArmCamera)
		return;

	state.m_requested_report = report_type;
	cemuLog_log(LogType::Force, "Wiimote slot {} requesting input report {:#04x} (extension={}, IR={})",
		index, uint8(report_type), extension, ir);
	const uint32 cookie = arm_pointer_report(index, report_type);
	wm.pointer_startup = PointerStartup::WaitReport;
	read_lock.unlock();
	send_packet(index, {kType, 0x04, report_type}, cookie);
}

IRMode WiimoteControllerProvider::set_ir_camera(size_t index, bool state, bool force)
{
	auto& wm = m_wiimotes[index];
	// Register writes during the initial stream are the ones the remote drops.
	// Wait until advance_pointer_startup has seen core-button reports.
	if (wm.pointer_startup == PointerStartup::PauseStream || wm.pointer_startup == PointerStartup::WaitCore)
	{
		std::shared_lock read_lock(wm.mutex);
		return wm.state.ir_camera.mode;
	}

	std::shared_lock read_lock(m_wiimotes[index].mutex);
	auto& wiimote_state = m_wiimotes[index].state;

	IRMode mode;
	if (!state)
		mode = kIRDisabled;
	else
	{
		// Reports that also carry an extension (0x36/0x37) only have room for basic IR.
		const bool packed_with_extension = wiimote_state.m_extension.index() != 0 || wiimote_state.m_motion_plus.has_value();
		mode = packed_with_extension ? kBasicIR : kExtendedIR;
	}

	const bool mode_changed = wiimote_state.ir_camera.mode != mode;
	if (!mode_changed && !force)
		return mode;

	wiimote_state.ir_camera.mode = mode;
	// Report format depends on IR being on and on basic vs extended; clear so
	// update_report_type will send a fresh 0x12 when needed.
	wiimote_state.m_requested_report = kNone;

	const uint8_t data = state ? 0x04 : 0x00;
	send_packet(index, {kIR, data});
	send_packet(index, {kIR2, data});
	if (state)
	{
		send_write_packet(index, kRegisterMemory, kRegisterIR, {0x08});
		send_write_packet(index, kRegisterMemory, kRegisterIRSensitivity1,
		                  {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xaa, 0x00, 0x64});
		send_write_packet(index, kRegisterMemory, kRegisterIRSensitivity2, {0x63, 0x03});
		send_write_packet(index, kRegisterMemory, kRegisterIRMode, {(uint8)mode});
		send_write_packet(index, kRegisterMemory, kRegisterIR, {0x08});
	}

	read_lock.unlock();
	update_report_type(index);
	return mode;
}

void WiimoteControllerProvider::send_packet(size_t index, std::vector<uint8> data, uint32 mode_cookie)
{
	cemu_assert(data.size() > 1);

	std::shared_lock device_lock(m_device_mutex);
	if (index >= m_wiimotes.size())
		return;

	device_lock.unlock();

	std::unique_lock lock(m_writer_mutex);
	if (mode_cookie != 0)
	{
		// A stale 0x12 still in the queue would be published first and could be
		// mistaken for the mode change this cookie is waiting on.
		m_write_queue.remove_if([index](const OutgoingReport& packet)
		{
			return packet.index == index && !packet.data.empty() && packet.data[0] == kType;
		});
	}
	m_write_queue.push_back(OutgoingReport{index, std::move(data), mode_cookie});
	m_writer_cond.notify_one();
}

uint32 WiimoteControllerProvider::arm_pointer_report(size_t index, InputReportId report)
{
	auto& wm = m_wiimotes[index];
	uint32 cookie = wm.next_cookie.fetch_add(1, std::memory_order_relaxed) + 1;
	if (cookie == 0)
		cookie = wm.next_cookie.fetch_add(1, std::memory_order_relaxed) + 1;
	wm.expected_cookie = cookie;
	wm.expected_report = report;
	wm.pointer_step_started = std::chrono::steady_clock::now();
	return cookie;
}

void WiimoteControllerProvider::restart_pointer_startup(size_t index)
{
	auto& wm = m_wiimotes[index];
	if (wm.pointer_startup == PointerStartup::PauseStream || wm.pointer_startup == PointerStartup::WaitCore)
		return;
	wm.pointer_startup = PointerStartup::PauseStream;
	wm.pointer_mismatch = 0;
	cemuLog_log(LogType::Force, "Wiimote slot {} repeating pointer setup after MotionPlus enable", index);
}

void WiimoteControllerProvider::advance_pointer_startup(size_t index, InputReportId id,
	std::chrono::steady_clock::time_point report_time)
{
	auto& wm = m_wiimotes[index];
	const auto is_data_report = [](InputReportId report)
	{
		return report >= kDataCore && report <= kDataCoreAccIRExt;
	};
	const auto published_cookie = wm.published_cookie.load(std::memory_order_acquire);
	const auto published_time = std::chrono::steady_clock::time_point(
		std::chrono::steady_clock::duration(wm.published_ns.load(std::memory_order_relaxed)));
	const bool write_published = wm.expected_cookie != 0 && published_cookie == wm.expected_cookie &&
		wm.published_report.load(std::memory_order_relaxed) == uint8(wm.expected_report);
	const bool accepted = write_published && id == wm.expected_report && report_time > published_time;
	const auto now = std::chrono::steady_clock::now();
	const bool timed_out = wm.pointer_step_started != std::chrono::steady_clock::time_point{} &&
		(write_published ? now - published_time > std::chrono::milliseconds(700)
			: now - wm.pointer_step_started > std::chrono::seconds(2));

	switch (wm.pointer_startup)
	{
	case PointerStartup::PauseStream:
	{
		// Continuous core buttons. The remote was often already in 0x37; camera
		// register writes in that stream are rejected, and one 0x12 was never repeated.
		const uint32 cookie = arm_pointer_report(index, kDataCore);
		wm.pointer_startup = PointerStartup::WaitCore;
		if (wm.pointer_retries < 8)
			cemuLog_log(LogType::Force, "Wiimote slot {} pointer setup attempt {}: core report before IR",
				index, wm.pointer_retries + 1);
		send_packet(index, {kType, 0x04, kDataCore}, cookie);
		break;
	}
	case PointerStartup::WaitCore:
		if (accepted)
		{
			wm.pointer_startup = PointerStartup::ArmCamera;
			// force rewrites the camera even when software already says basic IR.
			set_ir_camera(index, true, true);
			wm.pointer_mismatch = 0;
		}
		else if (timed_out)
		{
			if (wm.pointer_retries < 255)
				++wm.pointer_retries;
			// Two pauses that the remote never answered: write the camera in the
			// stream it is already sending, instead of waiting forever for 0x30.
			if ((wm.pointer_retries % 3) == 2)
			{
				wm.pointer_startup = PointerStartup::ArmCamera;
				set_ir_camera(index, true, true);
			}
			else
				wm.pointer_startup = PointerStartup::PauseStream;
			if (wm.pointer_retries <= 8)
				cemuLog_log(LogType::Force, "Wiimote slot {} pointer setup was not accepted; repeating IR camera init", index);
		}
		break;
	case PointerStartup::ArmCamera:
		set_ir_camera(index, true, true);
		break;
	case PointerStartup::WaitReport:
		if (accepted)
		{
			wm.pointer_startup = PointerStartup::Ready;
			wm.pointer_retries = 0;
			wm.pointer_mismatch = 0;
			cemuLog_log(LogType::Force, "Wiimote slot {} pointer report {:#04x} accepted", index, uint8(id));
		}
		else if (timed_out)
		{
			if (wm.pointer_retries < 255)
				++wm.pointer_retries;
			wm.pointer_startup = PointerStartup::PauseStream;
			if (wm.pointer_retries <= 8)
				cemuLog_log(LogType::Force, "Wiimote slot {} pointer setup was not accepted; repeating IR camera init", index);
		}
		break;
	case PointerStartup::Ready:
		// Memory replies sit between data reports. They must not clear the streak.
		if (!is_data_report(id))
			break;
		if (wm.expected_report == kNone || id == wm.expected_report)
			wm.pointer_mismatch = 0;
		else if (wm.pointer_mismatch < 255)
			++wm.pointer_mismatch;
		if (wm.pointer_mismatch >= 8)
		{
			wm.pointer_mismatch = 0;
			wm.pointer_startup = PointerStartup::PauseStream;
			cemuLog_log(LogType::Force, "Wiimote slot {} pointer report mode lost; repeating IR camera init", index);
		}
		break;
	}
}

void WiimoteControllerProvider::send_read_packet(size_t index, MemoryType type, RegisterAddress address, uint16 size)
{
	cemuLog_log(LogType::Force, "Wiimote slot {} requested read address={:#08x} size={}", index,
		static_cast<uint32>(address) & 0xffffff, size);
	std::vector<uint8> data(7);
	data[0] = kReadMemory;
	data[1] = type;
	*(betype<uint32>*)(data.data() + 2) = (address & 0xFFFFFF) << 8; // only uint24
	*(betype<uint16>*)(data.data() + 2 + 3) = size;

	send_packet(index, std::move(data));
}

void WiimoteControllerProvider::send_write_packet(size_t index, MemoryType type, RegisterAddress address,
                                                  const std::vector<uint8>& data)
{
	cemu_assert(data.size() <= 16);
	std::vector<uint8> packet(6 + 16);
	packet[0] = kWriteMemory;
	packet[1] = type;
	*(betype<uint32>*)(packet.data() + 2) = (address & 0xFFFFFF) << 8; // only uint24
	*(packet.data() + 2 + 3) = (uint8)data.size();
	std::copy(data.begin(), data.end(), packet.data() + 2 + 3 + 1);
	send_packet(index, std::move(packet));
}
