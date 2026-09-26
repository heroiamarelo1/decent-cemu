#pragma once

#include "input/api/Controller.h"
#include "input/api/Wiimote/WiimoteControllerProvider.h"

#include <atomic>

// todo: find better name because of emulated nameclash
class NativeWiimoteController : public Controller<WiimoteControllerProvider>
{
public:
	NativeWiimoteController(size_t index);

	enum Extension
	{
		None,
		Nunchuck,
		Classic,
		MotionPlus,
	};
	
	std::string_view api_name() const override
	{
		static_assert(to_string(InputAPI::Wiimote) == "Wiimote");
		return to_string(InputAPI::Wiimote);
	}
	InputAPI::Type api() const override { return InputAPI::Wiimote; }

	void save(pugi::xml_node& node) override;
	void load(const pugi::xml_node& node) override;

	bool connect() override;
	bool is_connected() override;

	void set_player_index(size_t player_index);
	// Runtime binding. The saved profile uuid stays on m_configured_index.
	// led_player 0 lights LED 1, which is what the game shows as player 1.
	void apply_slot(size_t device, bool enabled, size_t led_player);
	bool slot_active();
	size_t configured_index() const { return m_configured_index; }
	size_t index() const { return m_index.load(std::memory_order_acquire); }

	Extension get_extension() const;
	bool is_mpls_attached() const;
	// Raw 14-bit MotionPlus counts stored as yaw, roll, pitch.
	bool get_motion_plus_raw(uint16& yaw, uint16& roll, uint16& pitch) const;

	ControllerState raw_state() override;

	bool has_position() override;
	glm::vec2 get_position() override;
	glm::vec2 get_prev_position() override;
	PositionVisibility GetPositionVisibility() override;
	int get_ir_points(IRPoint points[4]) override;
	
	bool has_motion() override { return true; }
	bool has_rumble() override { return true; }

	bool has_battery() override { return true; }
	bool has_low_battery() override;
	
	void start_rumble() override;
	void stop_rumble() override;

	bool has_speaker() override { return true; }
	void set_speaker(int command) override;
	bool is_speaker_enabled() override;
	bool can_send_speaker() override;
	bool send_speaker_data(const uint8* data, uint32 size) override;

	MotionSample get_motion_sample() override;
	MotionSample get_nunchuck_motion_sample() const;

	std::string get_button_name(uint64 button) const override;

	uint32 get_packet_delay();
	void set_packet_delay(uint32 delay);

private:
	size_t m_configured_index = 0;
	std::atomic<size_t> m_index{0};
	std::atomic<bool> m_runtime_enabled{true};
	size_t m_player_index = 0;
	bool m_slot_applied = false;
	uint32 m_packet_delay = WiimoteControllerProvider::kDefaultPacketDelay;
};

