#include "input/api/Wiimote/NativeWiimoteController.h"

#include "input/api/Wiimote/WiimoteControllerProvider.h"
#include "Cemu/Logging/CemuLogging.h"

#include <limits>
#include <pugixml.hpp>

NativeWiimoteController::NativeWiimoteController(size_t index)
	: base_type(fmt::format("{}", index), fmt::format("Controller {}", index + 1)), m_configured_index(index), m_index(index)
{
	m_settings.motion = true;
	m_settings.rumble = 1.0f;
}

void NativeWiimoteController::save(pugi::xml_node& node)
{
	base_type::save(node);

	node.append_child("packet_delay").append_child(pugi::node_pcdata).set_value(
		fmt::format("{}", m_packet_delay).c_str());
}

void NativeWiimoteController::load(const pugi::xml_node& node)
{
	base_type::load(node);

	if (const auto value = node.child("packet_delay"))
		m_packet_delay = ConvertString<uint32>(value.child_value());
}

bool NativeWiimoteController::connect()
{
	if (!m_runtime_enabled.load(std::memory_order_acquire))
		return false;

	if (is_connected())
		return true;

	if (!m_provider->is_registered_device(index()))
	{
		m_provider->get_controllers();
	}

	if (m_provider->is_connected(index()))
	{
		m_provider->set_packet_delay(index(), m_packet_delay);
		m_provider->set_led(index(), m_player_index);
		return true;
	}

	return false;
}

bool NativeWiimoteController::is_connected()
{
	if (!m_runtime_enabled.load(std::memory_order_acquire))
		return false;

	if (m_provider->is_connected(index()))
	{
		m_provider->set_packet_delay(index(), m_packet_delay);
		return true;
	}

	return false;
}

void NativeWiimoteController::set_player_index(size_t player_index)
{
	m_player_index = player_index;
	if (m_runtime_enabled.load(std::memory_order_acquire))
		m_provider->set_led(index(), m_player_index);
}

void NativeWiimoteController::apply_slot(size_t device, bool enabled, size_t led_player)
{
	const size_t next = enabled ? device : std::numeric_limits<size_t>::max();
	if (m_slot_applied && m_runtime_enabled.load(std::memory_order_relaxed) == enabled &&
		index() == next && m_player_index == led_player)
		return;

	m_slot_applied = true;
	m_runtime_enabled.store(enabled, std::memory_order_release);
	m_index.store(next, std::memory_order_release);
	m_player_index = led_player;
	if (!enabled)
		return;

	m_provider->set_packet_delay(next, m_packet_delay);
	m_provider->set_led(next, led_player);
	cemuLog_log(LogType::Force, "Wiimote player {} uses device {} (light {})", led_player + 1, next, led_player + 1);
}

bool NativeWiimoteController::slot_active()
{
	if (!m_runtime_enabled.load(std::memory_order_acquire))
		return false;
	return m_provider->is_connected(index());
}

NativeWiimoteController::Extension NativeWiimoteController::get_extension() const
{
	Extension result = None;
	const auto state = m_provider->get_state(index());
	const auto ext = state.m_extension;
	if (std::holds_alternative<NunchuckData>(ext))
	{
		// MotionPlus frames and Nunchuk frames alternate. The extension bit on a
		// MotionPlus frame is often clear even though the Nunchuk is still plugged in.
		result = Nunchuck;
	}
	else if (std::holds_alternative<ClassicData>(ext))
		result = Classic;

	return result;
}

bool NativeWiimoteController::is_mpls_attached() const
{
	return m_provider->get_state(index()).m_motion_plus.has_value();
}

bool NativeWiimoteController::get_motion_plus_raw(uint16& yaw, uint16& roll, uint16& pitch) const
{
	const auto state = m_provider->get_state(index());
	if (!state.m_motion_plus || !state.m_motion_plus->calibration_valid)
		return false;
	const auto& raw = state.m_motion_plus->orientation;
	yaw = static_cast<uint16>(raw.x);
	roll = static_cast<uint16>(raw.y);
	pitch = static_cast<uint16>(raw.z);
	return true;
}

bool NativeWiimoteController::has_position()
{
	const auto state = m_provider->get_state(index());
	return std::any_of(state.ir_camera.dots.cbegin(), state.ir_camera.dots.cend(),
	                   [](const IRDot& v) { return v.visible; });
}

glm::vec2 NativeWiimoteController::get_position()
{
	const auto state = m_provider->get_state(index());
	return state.ir_camera.position;
}

glm::vec2 NativeWiimoteController::get_prev_position()
{
	const auto state = m_provider->get_state(index());
	return state.ir_camera.m_prev_position;
}
PositionVisibility NativeWiimoteController::GetPositionVisibility()
{
	const auto state = m_provider->get_state(index());
	return state.ir_camera.m_positionVisibility;
}

int NativeWiimoteController::get_ir_points(IRPoint points[4])
{
	const auto state = m_provider->get_state(index());
	for (int i = 0; i < 4; ++i)
	{
		const auto& dot = state.ir_camera.dots[i];
		points[i].visible = dot.visible;
		points[i].x = dot.visible ? dot.raw.x : 0x3FF;
		points[i].y = dot.visible ? dot.raw.y : 0x3FF;
	}
	return 4;
}

bool NativeWiimoteController::has_low_battery()
{
	const auto state = m_provider->get_state(index());
	return HAS_FLAG(state.flags, kBatteryEmpty);
}

void NativeWiimoteController::start_rumble()
{
	if (m_settings.rumble <= 0)
		return;

	m_provider->set_rumble(index(), true);
}

void NativeWiimoteController::stop_rumble()
{
	m_provider->set_rumble(index(), false);
}

void NativeWiimoteController::set_speaker(int command)
{
	m_provider->set_speaker(index(), command);
}

bool NativeWiimoteController::is_speaker_enabled()
{
	return m_provider->is_speaker_enabled(index());
}

bool NativeWiimoteController::can_send_speaker()
{
	return m_provider->can_send_speaker(index());
}

bool NativeWiimoteController::send_speaker_data(const uint8* data, uint32 size)
{
	return m_provider->send_speaker_data(index(), data, size);
}

MotionSample NativeWiimoteController::get_motion_sample()
{
	const auto state = m_provider->get_state(index());
	return state.motion_sample;
}

MotionSample NativeWiimoteController::get_nunchuck_motion_sample() const
{
	const auto state = m_provider->get_state(index());
	if (std::holds_alternative<NunchuckData>(state.m_extension))
	{
		return std::get<NunchuckData>(state.m_extension).motion_sample;
	}

	return {};
}

std::string NativeWiimoteController::get_button_name(uint64 button) const
{
	switch (button)
	{
	case kWiimoteButton_A: return "A";
	case kWiimoteButton_B: return "B";

	case kWiimoteButton_One: return "1";
	case kWiimoteButton_Two: return "2";

	case kWiimoteButton_Plus: return "+";
	case kWiimoteButton_Minus: return "-";

	case kWiimoteButton_Home: return "HOME";

	case kWiimoteButton_Up: return "UP";
	case kWiimoteButton_Down: return "DOWN";
	case kWiimoteButton_Left: return "LEFT";
	case kWiimoteButton_Right: return "RIGHT";

	// nunchuck
	case kWiimoteButton_C: return "C";
	case kWiimoteButton_Z: return "Z";

	// classic
	case kHighestWiimote + kClassicButton_A: return "A";
	case kHighestWiimote + kClassicButton_B: return "B";
	case kHighestWiimote + kClassicButton_Y: return "Y";
	case kHighestWiimote + kClassicButton_X: return "X";

	case kHighestWiimote + kClassicButton_Plus: return "+";
	case kHighestWiimote + kClassicButton_Minus: return "-";

	case kHighestWiimote + kClassicButton_Home: return "HOME";

	case kHighestWiimote + kClassicButton_Up: return "UP";
	case kHighestWiimote + kClassicButton_Down: return "DOWN";
	case kHighestWiimote + kClassicButton_Left: return "LEFT";
	case kHighestWiimote + kClassicButton_Right: return "RIGHT";

	case kHighestWiimote + kClassicButton_L: return "L";
	case kHighestWiimote + kClassicButton_R: return "R";

	case kHighestWiimote + kClassicButton_ZL: return "ZL";
	case kHighestWiimote + kClassicButton_ZR: return "ZR";
	}

	return base_type::get_button_name(button);
}

uint32 NativeWiimoteController::get_packet_delay()
{
	m_packet_delay = m_provider->get_packet_delay(index());
	return m_packet_delay;
}

void NativeWiimoteController::set_packet_delay(uint32 delay)
{
	m_packet_delay = delay;
	m_provider->set_packet_delay(index(), delay);
}

ControllerState NativeWiimoteController::raw_state()
{
	ControllerState result{};
	if (!is_connected())
		return result;

	const auto state = m_provider->get_state(index());
	for (int i = 0; i < std::numeric_limits<uint16>::digits; i++)
		result.buttons.SetButtonState(i, (state.buttons & (1 << i)) != 0);

	if (std::holds_alternative<NunchuckData>(state.m_extension))
	{
		const auto nunchuck = std::get<NunchuckData>(state.m_extension);
		if (nunchuck.c)
			result.buttons.SetButtonState(kWiimoteButton_C, true);

		if (nunchuck.z)
			result.buttons.SetButtonState(kWiimoteButton_Z, true);

		result.axis = nunchuck.axis;
	}
	else if (std::holds_alternative<ClassicData>(state.m_extension))
	{
		const auto classic = std::get<ClassicData>(state.m_extension);
		uint64 buttons = (uint64)classic.buttons << kHighestWiimote;
		for (int i = 0; i < std::numeric_limits<uint64>::digits; i++)
		{
			// OR with base buttons
			if((buttons & (1ULL << i)))
				result.buttons.SetButtonState(i, true);
		}
		result.axis = classic.left_axis;
		result.rotation = classic.right_axis;
		result.trigger = classic.trigger;
	}

	return result;
}
