#include "input/emulated/WiimoteController.h"

#include "input/api/Controller.h"
#include "input/api/Wiimote/NativeWiimoteController.h"

WiimoteController::WiimoteController(size_t player_index)
	: WPADController(player_index, kDataFormat_CORE_ACC_DPD)
{
}

void WiimoteController::set_device_type(WPADDeviceType device_type)
{
	if (m_device_type == device_type)
		return;
	m_device_type = device_type;
	if (!m_format_from_game)
		m_data_format = get_default_data_format();
}

WPADDeviceType WiimoteController::reported_device_type() const
{
	// A game that never asks for MotionPlus still has to see a Nunchuk, or it
	// keeps the sideways D-pad. Asking for MotionPlus switches the report to the
	// real MotionPlus type. The gyro samples are filled either way.
	if (!m_mpls_enabled)
	{
		if (m_device_type == kWAPDevMPLSFreeStyle)
			return kWAPDevFreestyle;
		if (m_device_type == kWAPDevMPLS)
			return kWAPDevCore;
		if (m_device_type == kWAPDevMPLSClassic)
			return kWAPDevClassic;
	}
	return m_device_type;
}

void WiimoteController::update()
{
	base_type::update();
	if (!m_auto_detect_extensions)
		return;

	// Nunchuk reports alternate with MotionPlus reports, so the extension bit is
	// often clear for a frame. Require several agreeing samples before changing the
	// type the game sees, in either direction. Also detect remotes without MotionPlus.
	WPADDeviceType seen = m_device_type;
	bool have_wiimote = false;
	for (const auto& controller : get_controllers())
	{
		if (!controller || controller->api() != InputAPI::Wiimote)
			continue;
		auto* wiimote = static_cast<NativeWiimoteController*>(controller.get());
		if (!wiimote->slot_active())
			continue;
		have_wiimote = true;
		const bool motion_plus = wiimote->is_mpls_attached();
		const auto extension = wiimote->get_extension();
		if (extension == NativeWiimoteController::Nunchuck)
			seen = motion_plus ? kWAPDevMPLSFreeStyle : kWAPDevFreestyle;
		else if (extension == NativeWiimoteController::Classic)
			seen = motion_plus ? kWAPDevMPLSClassic : kWAPDevClassic;
		else
			seen = motion_plus ? kWAPDevMPLS : kWAPDevCore;
		break;
	}

	if (!have_wiimote)
		return;

	if (seen == m_extension_candidate)
		++m_extension_candidate_samples;
	else
	{
		m_extension_candidate = seen;
		m_extension_candidate_samples = 1;
	}

	if (seen != m_device_type && m_extension_candidate_samples >= 8)
	{
		cemuLog_log(LogType::Force, "Wiimote player {} device type {} -> {} to match the connected extension",
			player_index(), (int)m_device_type, (int)seen);
		// Keep the format the game already chose. Only the device type has to
		// change so a Nunchuk unplug is visible on the next read.
		if (m_format_from_game)
			m_device_type = seen;
		else
			set_device_type(seen);
	}

	// A profile can already say MotionPlus+Nunchuk while the read format is still
	// the core remote, which has no stick. Put the Nunchuk stick in the sample
	// until the game chooses a format itself.
	if (!m_format_from_game && (m_device_type == kWAPDevMPLSFreeStyle || m_device_type == kWAPDevFreestyle))
	{
		if (m_data_format == kDataFormat_CORE || m_data_format == kDataFormat_CORE_ACC ||
			m_data_format == kDataFormat_CORE_ACC_DPD || m_data_format == kDataFormat_CORE_ACC_DPD_FULL)
			m_data_format = kDataFormat_FREESTYLE_ACC_DPD;
	}
}

bool WiimoteController::is_mpls_attached()
{
	return m_device_type == kWAPDevMPLS || m_device_type == kWAPDevMPLSClassic || m_device_type == kWAPDevMPLSFreeStyle;
}

bool WiimoteController::get_motion_plus_raw(uint16& pitch, uint16& yaw, uint16& roll) const
{
	for (const auto& controller : get_controllers())
	{
		if (!controller || controller->api() != InputAPI::Wiimote)
			continue;
		const auto* wiimote = static_cast<const NativeWiimoteController*>(controller.get());
		uint16 raw_yaw = 0, raw_roll = 0, raw_pitch = 0;
		if (!wiimote->get_motion_plus_raw(raw_yaw, raw_roll, raw_pitch))
			continue;
		pitch = raw_pitch;
		yaw = raw_yaw;
		roll = raw_roll;
		return true;
	}
	return false;
}

uint32 WiimoteController::get_emulated_button_flag(uint32 id) const
{
	return s_get_emulated_button_flag(id);
}

bool WiimoteController::set_default_mapping(const std::shared_ptr<ControllerBase>& controller)
{
	std::vector<std::pair<uint64, uint64>> mapping;
	switch (controller->api())
	{
	case InputAPI::Wiimote: {
		const auto sdl_controller = std::static_pointer_cast<NativeWiimoteController>(controller);
		mapping =
		{
			{kButtonId_A, kWiimoteButton_A},
			{kButtonId_B, kWiimoteButton_B},
			{kButtonId_1, kWiimoteButton_One},
			{kButtonId_2, kWiimoteButton_Two},

			{kButtonId_Home, kWiimoteButton_Home},

			{kButtonId_Plus, kWiimoteButton_Plus},
			{kButtonId_Minus, kWiimoteButton_Minus},

			{kButtonId_Up, kWiimoteButton_Up},
			{kButtonId_Down, kWiimoteButton_Down},
			{kButtonId_Left, kWiimoteButton_Left},
			{kButtonId_Right, kWiimoteButton_Right},

			{kButtonId_Nunchuck_Z, kWiimoteButton_Z},
			{kButtonId_Nunchuck_C, kWiimoteButton_C},

			{kButtonId_Nunchuck_Up, kAxisYP},
			{kButtonId_Nunchuck_Down, kAxisYN},
			{kButtonId_Nunchuck_Left, kAxisXN},
			{kButtonId_Nunchuck_Right, kAxisXP},
		};
		break;
	}
	case InputAPI::DSUClient:
		mapping =
		{
			{kButtonId_A, 13},
			{kButtonId_B, 12},
			{kButtonId_1, 15},
			{kButtonId_2, 14},
			{kButtonId_Minus, 0},
			{kButtonId_Plus, 3},
			{kButtonId_Up, 4},
			{kButtonId_Right, 5},
			{kButtonId_Down, 6},
			{kButtonId_Left, 7},
		};
		break;
	}

	bool mapping_updated = false;
	std::for_each(mapping.cbegin(), mapping.cend(), [this, &controller, &mapping_updated](const auto& m)
		{
			if (m_mappings.find(m.first) == m_mappings.cend())
			{
				set_mapping(m.first, controller, m.second);
				mapping_updated = true;
			}
		});

	return mapping_updated;
}

glm::vec2 WiimoteController::get_axis() const
{
	const auto left = get_axis_value(kButtonId_Nunchuck_Left);
	const auto right = get_axis_value(kButtonId_Nunchuck_Right);

	const auto up = get_axis_value(kButtonId_Nunchuck_Up);
	const auto down = get_axis_value(kButtonId_Nunchuck_Down);

	glm::vec2 result;
	result.x = (left > right) ? -left : right;
	result.y = (up > down) ? up : -down;
	return result;
}

glm::vec2 WiimoteController::get_rotation() const
{
	return {};
}

glm::vec2 WiimoteController::get_trigger() const
{
	return {};
}

void WiimoteController::load(const pugi::xml_node& node)
{
	base_type::load(node);

	if (const auto value = node.child("device_type"))
		m_device_type = ConvertString<WPADDeviceType>(value.child_value());
	if (const auto value = node.child("auto_detect_extensions"))
		m_auto_detect_extensions = ConvertString<bool>(value.child_value());
}

void WiimoteController::save(pugi::xml_node& node)
{
	base_type::save(node);

	node.append_child("device_type").append_child(pugi::node_pcdata).set_value(fmt::format("{}", (int)m_device_type).c_str());
	node.append_child("auto_detect_extensions").append_child(pugi::node_pcdata).set_value(m_auto_detect_extensions ? "true" : "false");
}

uint32 WiimoteController::s_get_emulated_button_flag(uint32 id)
{
	switch (id)
	{
		case kButtonId_A:
			return kWPADButton_A;
		case kButtonId_B:
			return kWPADButton_B;
		case kButtonId_1:
			return kWPADButton_1;
		case kButtonId_2:
			return kWPADButton_2;

		case kButtonId_Plus:
			return kWPADButton_Plus;
		case kButtonId_Minus:
			return kWPADButton_Minus;
		case kButtonId_Home:
			return kWPADButton_Home;

		case kButtonId_Up:
			return kWPADButton_Up;
		case kButtonId_Down:
			return kWPADButton_Down;
		case kButtonId_Left:
			return kWPADButton_Left;
		case kButtonId_Right:
			return kWPADButton_Right;

		case kButtonId_Nunchuck_Z:
			return kWPADButton_Z;
		case kButtonId_Nunchuck_C:
			return kWPADButton_C;
	}

	return 0;
}

std::string_view WiimoteController::get_button_name(ButtonId id)
{
	switch (id)
	{
	case kButtonId_A: return "A";
	case kButtonId_B: return "B";
	case kButtonId_1: return "1";
	case kButtonId_2: return "2";

	case kButtonId_Home: return TR_NOOP("home");
	case kButtonId_Plus: return "+";
	case kButtonId_Minus: return "-";

	case kButtonId_Up: return TR_NOOP("up");
	case kButtonId_Down: return TR_NOOP("down");
	case kButtonId_Left: return TR_NOOP("left");
	case kButtonId_Right: return TR_NOOP("right");

	case kButtonId_Nunchuck_Z: return "Z";
	case kButtonId_Nunchuck_C: return "C";

	case kButtonId_Nunchuck_Up: return TR_NOOP("up");
	case kButtonId_Nunchuck_Down: return TR_NOOP("down");
	case kButtonId_Nunchuck_Left: return TR_NOOP("left");
	case kButtonId_Nunchuck_Right: return TR_NOOP("right");

	default:
		return "";
	}
}
