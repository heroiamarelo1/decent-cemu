#include <api/Controller.h>
#include "input/emulated/WPADController.h"

#include "input/emulated/ClassicController.h"
#include "input/emulated/ProController.h"
#include "input/emulated/WiimoteController.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace
{
struct PointerCal
{
	// Used when portable/pointer-cal.txt is absent. scale 1 maps the whole
	// camera onto the screen, so the lights leave the camera at about 75%
	// and the cursor disappears. 1.35 and a 0.7 bar offset reach the edges.
	float x_sign = 1.0f;
	float y_sign = 1.0f;
	float bar_widths = 0.7f;
	float scale = 1.35f;
};

PointerCal LoadPointerCal()
{
	static PointerCal cal;
	static std::chrono::steady_clock::time_point next{};
	const auto now = std::chrono::steady_clock::now();
	if (now < next)
		return cal;
	next = now + std::chrono::seconds(1);

	std::ifstream in(ActiveSettings::GetUserDataPath("pointer-cal.txt"));
	if (!in)
		return cal;

	PointerCal loaded;
	std::string line;
	while (std::getline(in, line))
	{
		if (line.empty() || line[0] == '#')
			continue;
		std::istringstream ss(line);
		std::string key;
		float value = 0;
		if (!(ss >> key >> value))
			continue;
		if (key == "x_sign")
			loaded.x_sign = value;
		else if (key == "y_sign")
			loaded.y_sign = value;
		else if (key == "bar_widths")
			loaded.bar_widths = value;
		else if (key == "scale")
			loaded.scale = value;
	}
	cal = loaded;
	return cal;
}

int ReadIRPoints(const EmulatedController& controller, ControllerBase::IRPoint points[4])
{
	for (const auto& api : controller.get_controllers())
	{
		if (api && api->get_ir_points(points) > 0)
			return 4;
	}
	return 0;
}

void WriteDPDObjects(WPADStatus_t* status, const ControllerBase::IRPoint points[4])
{
	for (int i = 0; i < 4; ++i)
	{
		status->obj[i].x = points[i].x;
		status->obj[i].y = points[i].y;
		status->obj[i].size = points[i].visible ? 8 : 0;
		status->obj[i].traceId = points[i].visible ? (uint8)i : 0;
	}
}

constexpr float kTau = 6.28318531f;

glm::mat3 AxisAngle(glm::vec3 axis, float angle)
{
	const float len = glm::length(axis);
	if (len < 1.0e-6f || angle == 0.0f)
		return glm::mat3(1.0f);
	axis /= len;
	const float c = std::cos(angle);
	const float s = std::sin(angle);
	const float t = 1.0f - c;
	glm::mat3 m;
	m[0] = glm::vec3(t * axis.x * axis.x + c, t * axis.x * axis.y + s * axis.z, t * axis.x * axis.z - s * axis.y);
	m[1] = glm::vec3(t * axis.x * axis.y - s * axis.z, t * axis.y * axis.y + c, t * axis.y * axis.z + s * axis.x);
	m[2] = glm::vec3(t * axis.x * axis.z + s * axis.y, t * axis.y * axis.z - s * axis.x, t * axis.z * axis.z + c);
	return m;
}

// Rotation that moves unit vector `from` the given fraction of the way to `to`.
glm::mat3 TurnToward(const glm::vec3& from, const glm::vec3& to, float fraction)
{
	glm::vec3 axis = glm::cross(from, to);
	const float cos_angle = glm::dot(from, to);
	if (glm::length(axis) < 1.0e-4f)
	{
		if (cos_angle > 0.0f)
			return glm::mat3(1.0f);
		axis = glm::cross(from, std::abs(from.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0));
	}
	return AxisAngle(axis, std::atan2(glm::length(glm::cross(from, to)), cos_angle) * fraction);
}

glm::vec3 Horizontal(const glm::vec3& v, const glm::vec3& up)
{
	return v - up * glm::dot(v, up);
}

// Rotation about `up` that moves the heading of `from` the given fraction of the
// way to the heading of `to`. Identity when either points nearly straight up or down.
glm::mat3 TurnHeading(const glm::vec3& from, const glm::vec3& to, const glm::vec3& up, float fraction, float max_step)
{
	const glm::vec3 a = Horizontal(from, up);
	const glm::vec3 b = Horizontal(to, up);
	if (glm::length(a) < 0.2f || glm::length(b) < 0.2f)
		return glm::mat3(1.0f);
	const glm::vec3 an = glm::normalize(a);
	const glm::vec3 bn = glm::normalize(b);
	const float angle = std::atan2(glm::dot(glm::cross(an, bn), up), glm::dot(an, bn));
	return AxisAngle(up, std::clamp(angle * fraction, -max_step, max_step));
}

glm::mat3 Orthonormalize(const glm::mat3& m)
{
	const glm::vec3 z = glm::normalize(m[2]);
	glm::vec3 x = m[0] - z * glm::dot(m[0], z);
	if (glm::length(x) < 1.0e-4f)
		x = glm::cross(m[1], z);
	x = glm::normalize(x);
	return glm::mat3(x, glm::cross(z, x), z);
}

// Orientation whose world up matches the measured gravity, facing the TV.
glm::mat3 DirFromGravity(const glm::vec3& up)
{
	const glm::vec3 world_y = -up;
	glm::vec3 world_z = glm::vec3(0, 0, 1) - world_y * world_y.z;
	if (glm::length(world_z) < 0.3f)
		world_z = glm::cross(glm::vec3(1, 0, 0), world_y);
	world_z = glm::normalize(world_z);
	const glm::vec3 world_x = glm::cross(world_y, world_z);
	// Rows are the world axes in remote coordinates.
	return glm::transpose(glm::mat3(world_x, world_y, world_z));
}

// A per-sample weight applied once per read; KPAD revises at the 200 Hz sample rate.
float ReviseFraction(float weight, float dt)
{
	weight = std::clamp(weight, 0.0f, 1.0f);
	return 1.0f - std::pow(1.0f - weight, dt * 200.0f);
}

// Optical heading correction weight, scaled by elapsed sensor time.
float HeadingReviseFraction(float weight, float dt, float per_second)
{
	weight = std::clamp(weight, 0.0f, 1.0f);
	return 1.0f - std::pow(1.0f - weight, dt * per_second);
}
}

WPADController::WPADController(size_t player_index, WPADDataFormat data_format)
	: EmulatedController(player_index), m_data_format(data_format)
{
}

WPADDataFormat WPADController::get_default_data_format() const
{
	switch (get_device_type())
	{
	case kWAPDevCore:
		return kDataFormat_CORE_ACC_DPD;
	case kWAPDevFreestyle:
		return kDataFormat_FREESTYLE_ACC;
	case kWAPDevClassic:
		return kDataFormat_CLASSIC;
	case kWAPDevMPLS:
		return kDataFormat_MPLS;
	case kWAPDevMPLSFreeStyle:
		return kDataFormat_FREESTYLE_ACC_DPD;
	case kWAPDevMPLSClassic:
		return kDataFormat_CLASSIC_ACC_DPD;
	case kWAPDevURCC:
		return kDataFormat_URCC;
	default:
		return kDataFormat_CORE;
	}
}

uint32 WPADController::get_emulated_button_flag(WPADDataFormat format, uint32 id) const
{
	switch(format)
	{
	case kDataFormat_CORE:
	case kDataFormat_CORE_ACC:
	case kDataFormat_CORE_ACC_DPD:
	case kDataFormat_CORE_ACC_DPD_FULL:
	case kDataFormat_FREESTYLE:
	case kDataFormat_FREESTYLE_ACC:
	case kDataFormat_FREESTYLE_ACC_DPD:
	case kDataFormat_MPLS:
		return WiimoteController::s_get_emulated_button_flag(id);
	case kDataFormat_CLASSIC:
	case kDataFormat_CLASSIC_ACC:
	case kDataFormat_CLASSIC_ACC_DPD:
		return ClassicController::s_get_emulated_button_flag(id);
	
	case kDataFormat_TRAIN: break;
	case kDataFormat_GUITAR: break;
	case kDataFormat_BALANCE_CHECKER: break;
	case kDataFormat_DRUM: break;
	
	case kDataFormat_TAIKO: break;
	case kDataFormat_URCC:
		return ProController::s_get_emulated_button_flag(id);

	}

	return 0;
}

void WPADController::WPADRead(WPADStatus_t* status)
{
	controllers_update_states();
	const bool gamepad_bar_combo = update_gamepad_bar_shortcut();
	uint32 button = 0;
	for (uint32 i = 1; i < get_highest_mapping_id(); ++i)
	{
		if (is_mapping_down(i))
		{
			const uint32 value = get_emulated_button_flag(m_data_format, i);
			button |= value;
		}
	}

	if (gamepad_bar_combo)
		button &= ~(kWPADButton_Minus | kWPADButton_B);

	m_homebutton_down |= is_home_down();

	// todo fill position api from wiimote

	switch (m_data_format)
	{
	case kDataFormat_CORE:
	case kDataFormat_CORE_ACC:
	case kDataFormat_CORE_ACC_DPD:
	case kDataFormat_CORE_ACC_DPD_FULL:
	{
		memset(status, 0x00, sizeof(*status));
		status->button = button;
		break;
	}

	case kDataFormat_FREESTYLE:
	case kDataFormat_FREESTYLE_ACC:
	case kDataFormat_FREESTYLE_ACC_DPD:
	{
		WPADFSStatus_t* ex_status = (WPADFSStatus_t*)status;
		memset(ex_status, 0x00, sizeof(*ex_status));
		ex_status->button = button;

		auto axis = get_axis();
		axis *= 127.0f;
		ex_status->fsStickX = (sint8)axis.x;
		ex_status->fsStickY = (sint8)axis.y;
		break;
	}

	case kDataFormat_CLASSIC:
	case kDataFormat_CLASSIC_ACC:
	case kDataFormat_CLASSIC_ACC_DPD:
	case kDataFormat_GUITAR:
	case kDataFormat_DRUM:
	case kDataFormat_TAIKO:
	{
		WPADCLStatus_t* ex_status = (WPADCLStatus_t*)status;
		memset(ex_status, 0x00, sizeof(*ex_status));
		ex_status->clButton = button;
		
		auto axis = get_axis();
		axis *= 2048.0f;
		ex_status->clLStickX = (uint16)axis.x;
		ex_status->clLStickY = (uint16)axis.y;

		auto rotation = get_rotation();
		rotation *= 2048.0f;
		ex_status->clRStickX = (uint16)rotation.x;
		ex_status->clRStickY = (uint16)rotation.y;
		break;
	}
	case kDataFormat_TRAIN:
	{
		WPADTRStatus_t* ex_status = (WPADTRStatus_t*)status;
		// TODO
		break;
	}
	case kDataFormat_BALANCE_CHECKER:
	{
		WPADBLStatus_t* ex_status = (WPADBLStatus_t*)status;
		// TODO
		break;
	}
	case kDataFormat_MPLS:
	{
		WPADMPStatus_t* ex_status = (WPADMPStatus_t*)status;
		memset(ex_status, 0x00, sizeof(*ex_status));
		ex_status->button = button;
		// Attached and valid. EXT_VALID tells the game the Nunchuk or Classic
		// bytes in this sample are the extension, not an empty MotionPlus slot.
		ex_status->stat = (get_device_type() == kWAPDevMPLS) ? 0x81 : 0xC1;
		auto axis = get_axis();
		axis *= 127.0f;
		ex_status->status.fs.fsStickX = (sint8)axis.x;
		ex_status->status.fs.fsStickY = (sint8)axis.y;
		uint16 pitch = 0, yaw = 0, roll = 0;
		if (get_motion_plus_raw(pitch, yaw, roll))
		{
			ex_status->pitch = pitch;
			ex_status->yaw = yaw;
			ex_status->roll = roll;
		}
		break;
	}
	case kDataFormat_URCC:
	{
		WPADUCStatus_t* ex_status = (WPADUCStatus_t*)status;
		memset(ex_status, 0x00, sizeof(*ex_status));
		ex_status->ucButton = button;

		ex_status->cable = TRUE;
		ex_status->charge = TRUE;

		auto axis = get_axis();
		axis *= 2048.0f;
		ex_status->ucLStickX = (uint16)axis.x;
		ex_status->ucLStickY = (uint16)axis.y;

		auto rotation = get_rotation();
		rotation *= 2048.0f;
		ex_status->ucRStickX = (uint16)rotation.x;
		ex_status->ucRStickY = (uint16)rotation.y;

		break;
	}
	default:
		cemu_assert(false);
	}

	ControllerBase::IRPoint points[4]{};
	uint16 bar_x[4]{};
	uint16 bar_y[4]{};
	bool bar_visible[4]{};
	if (fill_inverted_sensor_bar(bar_x, bar_y, bar_visible))
	{
		for (int i = 0; i < 4; ++i)
			points[i] = {bar_x[i], bar_y[i], bar_visible[i]};
		WriteDPDObjects(status, points);
	}
	else if (ReadIRPoints(*this, points) > 0)
		WriteDPDObjects(status, points);

	status->dev = reported_device_type();
	status->err = WPAD_ERR_NONE;
}

bool WPADController::update_gamepad_bar_shortcut()
{
	if (type() != Type::Wiimote)
		return false;
	const bool minus = is_mapping_down(WiimoteController::kButtonId_Minus);
	const bool b = is_mapping_down(WiimoteController::kButtonId_B);
	if (minus && b && !m_gamepad_bar_combo_latched)
	{
		m_gamepad_bar_combo_latched = true;
		m_gamepad_bar_enabled = !m_gamepad_bar_enabled;
		m_inverted_bar_latched = false;
		m_has_dot_track = false;
		cemuLog_log(LogType::Force, "Wiimote player {}: virtual GamePad sensor bar {} (Minus+B)",
			player_index() + 1, m_gamepad_bar_enabled ? "enabled" : "disabled");
	}
	// Release both buttons before another toggle; polling by WPAD and KPAD
	// must not toggle twice or let either shortcut button reach the game.
	if (!minus && !b)
		m_gamepad_bar_combo_latched = false;
	return m_gamepad_bar_combo_latched;
}

bool WPADController::fill_inverted_sensor_bar(uint16 xs[4], uint16 ys[4], bool lit[4])
{
	if (!m_gamepad_bar_enabled || !GetConfig().inverted_sensor_bar.GetValue() || !is_mpls_attached() || !has_motion())
	{
		m_inverted_bar_latched = false;
		return false;
	}

	glm::vec3 acc{};
	get_motion_data().getAccelerometer(&acc.x);
	const float mag = glm::length(acc);
	if (mag < 0.55f)
	{
		m_inverted_bar_latched = false;
		return false;
	}

	// KPAD +Z points toward the player: camera-down gives gravity along +Z.
	// Face-down (+Y) is a different pose and must not create a GamePad bar.
	const float camera_down = acc.z / mag;
	const float need = m_inverted_bar_latched ? 0.72f : 0.85f;
	if (camera_down < need || mag < 0.8f || mag > 1.2f)
	{
		m_inverted_bar_latched = false;
		return false;
	}

	ControllerBase::IRPoint real[4]{};
	int visible = 0;
	if (ReadIRPoints(*this, real) > 0)
	{
		for (int i = 0; i < 4; ++i)
		{
			if (real[i].visible)
				++visible;
		}
	}
	// A real bar, including the television, stays in charge.
	if (visible >= 2)
		return false;

	m_inverted_bar_latched = true;
	const auto cal = LoadPointerCal();
	const float nx = acc.x / mag;
	const float ny = acc.y / mag;
	const float cx = std::clamp(512.0f + nx * 380.0f, 160.0f, 860.0f);
	// Place the virtual bar above the screen, so straight-down aim centers
	// on the GamePad screen rather than on its sensor bar.
	const float cy = std::clamp(384.0f - 280.0f * cal.bar_widths - ny * 380.0f, 20.0f, 740.0f);
	for (int i = 0; i < 4; ++i)
	{
		xs[i] = 0x3FF;
		ys[i] = 0x3FF;
		lit[i] = false;
	}
	xs[0] = (uint16)std::lround(cx - 140.0f);
	ys[0] = (uint16)std::lround(cy);
	lit[0] = true;
	xs[1] = (uint16)std::lround(cx + 140.0f);
	ys[1] = (uint16)std::lround(cy);
	lit[1] = true;
	return true;
}

void WPADController::KPADRead(KPADStatus_t& status, const BtnRepeat& repeat)
{
	uint32be* hold, *release, *trigger;
	switch (type())
	{
	case Pro:
		hold = &status.ex_status.uc.hold;
		release = &status.ex_status.uc.release;
		trigger = &status.ex_status.uc.trig;
		break;
	case Classic:
		hold = &status.ex_status.cl.hold;
		release = &status.ex_status.cl.release;
		trigger = &status.ex_status.cl.trig;
		break;
	default:
		hold = &status.hold;
		release = &status.release;
		trigger = &status.trig;
	}

	controllers_update_states();
	const bool gamepad_bar_combo = update_gamepad_bar_shortcut();
	for (uint32 i = 1; i < get_highest_mapping_id(); ++i)
	{
		if (is_mapping_down(i))
		{
			const uint32 value = get_emulated_button_flag(m_data_format, i);
			*hold |= value;
		}
	}

	if (gamepad_bar_combo)
		*hold = uint32(*hold) & ~(kWPADButton_Minus | kWPADButton_B);

	m_homebutton_down |= is_home_down();
	
	// button repeat
	const auto now = std::chrono::steady_clock::now();
	if (*hold != m_last_holdvalue)
	{
		m_last_hold_change = m_last_pulse = now;
	}

	if (repeat.pulse > 0)
	{
		if (m_last_hold_change + std::chrono::milliseconds(repeat.delay) >= now)
		{
			if ((m_last_pulse + std::chrono::milliseconds(repeat.pulse)) < now)
			{
				m_last_pulse = now;
				*hold |= kWPADButtonRepeat;
			}
		}
	}

	// axis
	const auto axis = get_axis();
	const auto rotation = get_rotation();

	*release = m_last_holdvalue & ~*hold;
	//status.release = m_last_holdvalue & ~*hold;
	*trigger = ~m_last_holdvalue & *hold;
	//status.trig = ~m_last_holdvalue & *hold;
	m_last_holdvalue = *hold;

	// Wii Remote samples are already in KPAD axes: X right, Y out of the bottom,
	// Z toward the player. Lying buttons-down is (0, 1, 0).
	glm::vec3 kpad_acc{};
	float kpad_acc_speed = 0.0f;
	if (has_motion())
	{
		auto motion_sample = get_motion_data();

		motion_sample.getAccelerometer(&kpad_acc[0]);
		status.acc.x = kpad_acc.x;
		status.acc.y = kpad_acc.y;
		status.acc.z = kpad_acc.z;

		status.acc_value = motion_sample.getVPADAccMagnitude();
		kpad_acc_speed = motion_sample.getVPADAccAcceleration();
		status.acc_speed = kpad_acc_speed;

		const float acc_mag = glm::length(kpad_acc);
		if (acc_mag > 0.0001f)
		{
			status.accVertical.x = std::sqrt(kpad_acc.x * kpad_acc.x + kpad_acc.y * kpad_acc.y) / acc_mag;
			status.accVertical.y = -kpad_acc.z / acc_mag;
		}
		else
		{
			status.accVertical.x = 1.0f;
			status.accVertical.y = 0.0f;
		}
	}
	uint16 forced_x[4]{};
	uint16 forced_y[4]{};
	bool forced_visible[4]{};
	const bool forced_bar = fill_inverted_sensor_bar(forced_x, forced_y, forced_visible);
	m_mpls_real_ir_valid = false;
	if (forced_bar != m_last_bar_was_virtual)
		m_has_dot_track = false;
	m_last_bar_was_virtual = forced_bar;
	auto visibility = GetPositionVisibility();
	// Derive every KPAD pointer field from the real IR-camera dots. The provider
	// midpoint alone is insufficient: Wii titles also consume distance and
	// horizon, and screen aim needs the physical sensor-bar offset.
	if ((has_position() && visibility != PositionVisibility::NONE) || forced_bar)
	{
		if (forced_bar || visibility == PositionVisibility::FULL)
			status.dpd_valid_fg = 2;
		else
			status.dpd_valid_fg = -1;

		const auto position = get_position();
		ControllerBase::IRPoint points[4]{};
		bool have_points = false;
		if (forced_bar)
		{
			for (int i = 0; i < 4; ++i)
				points[i] = {forced_x[i], forced_y[i], forced_visible[i]};
			have_points = true;
		}
		else
			have_points = ReadIRPoints(*this, points) > 0;
		int first = -1;
		int second = -1;

		// Tracked midpoint is the pair chosen by the camera, not the first two slots.
		// A reflection in another slot used to pull the cursor to the opposite side.
		const float track_x = (1.0f - position.x) * 1023.0f;
		const float track_y = position.y * 768.0f;
		// Once the two lights are known, follow their midpoint. The provider
		// position jumps when it swaps the lights, and that used to pick a reflection.
		const float prefer_x = m_has_dot_track ? (m_track_ax + m_track_bx) * 0.5f : track_x;
		const float prefer_y = m_has_dot_track ? (m_track_ay + m_track_by) * 0.5f : track_y;
		float mid_x = prefer_x;
		float mid_y = prefer_y;
		float pixels = m_last_pointer_sep;
		float bar_dx = 0;
		float bar_dy = 0;
		if (forced_bar)
		{
			first = 0;
			second = 1;
			mid_x = (float(points[0].x) + float(points[1].x)) * 0.5f;
			mid_y = (float(points[0].y) + float(points[1].y)) * 0.5f;
		}
		else if (have_points)
		{
			// The sensor bar is horizontal in the room, so in the camera it is
			// perpendicular to gravity. A reflection can have the same separation
			// and sit closer to the last midpoint; that locked the sword on its side.
			float down_x = (float)status.acc.x;
			float down_y = -(float)status.acc.y;
			const float down_len = std::sqrt(down_x * down_x + down_y * down_y);
			float best = 1.0e9f;
			float best_align = 1.0e9f;
			float best_err = 1.0e9f;
			for (int i = 0; i < 4; ++i)
			{
				if (!points[i].visible)
					continue;
				for (int j = i + 1; j < 4; ++j)
				{
					if (!points[j].visible)
						continue;
					const float dx = float(points[j].x) - float(points[i].x);
					const float dy = float(points[j].y) - float(points[i].y);
					const float len = std::sqrt(dx * dx + dy * dy);
					if (len < 15.0f)
						continue;
					float align = 0.5f;
					if (down_len > 0.25f)
						align = std::abs(dx * down_x + dy * down_y) / (len * down_len);
					const float mx = (float(points[i].x) + float(points[j].x)) * 0.5f;
					const float my = (float(points[i].y) + float(points[j].y)) * 0.5f;
					const float err = (mx - prefer_x) * (mx - prefer_x) + (my - prefer_y) * (my - prefer_y);
					// Several pairs can be equally horizontal. Align noise was
					// swapping them every frame and throwing the cursor.
					const bool more_level = align < best_align - 0.12f;
					const bool as_level = std::abs(align - best_align) <= 0.12f && err < best_err;
					if (first < 0 || more_level || as_level)
					{
						best = align;
						best_align = more_level ? align : (align < best_align ? align : best_align);
						best_err = err;
						first = i;
						second = j;
						mid_x = mx;
						mid_y = my;
					}
				}
			}
		}
		if (first >= 0 && second >= 0)
		{
			// Two real camera points are enough for a complete DPD sample even
			// when the provider's historical tracker still reports PARTIAL.
			status.dpd_valid_fg = 2;
			bar_dx = float(points[second].x) - float(points[first].x);
			bar_dy = float(points[second].y) - float(points[first].y);
			pixels = std::sqrt(bar_dx * bar_dx + bar_dy * bar_dy);
			if (pixels > 1.0f)
			{
				const float ax = float(points[first].x);
				const float ay = float(points[first].y);
				const float bx = float(points[second].x);
				const float by = float(points[second].y);
				if (m_has_dot_track)
				{
					const float keep = (ax - m_track_ax) * (ax - m_track_ax) + (ay - m_track_ay) * (ay - m_track_ay)
						+ (bx - m_track_bx) * (bx - m_track_bx) + (by - m_track_by) * (by - m_track_by);
					const float swap = (ax - m_track_bx) * (ax - m_track_bx) + (ay - m_track_by) * (ay - m_track_by)
						+ (bx - m_track_ax) * (bx - m_track_ax) + (by - m_track_ay) * (by - m_track_ay);
					const float best = keep < swap ? keep : swap;
					// A dropped frame used to forget the lights. Only give up when both
					// pairings are nowhere near the lights we were following.
					if (best > 350.0f * 350.0f)
						m_has_dot_track = false;
					else if (swap < keep)
					{
						m_track_ax = bx;
						m_track_ay = by;
						m_track_bx = ax;
						m_track_by = ay;
					}
					else
					{
						m_track_ax = ax;
						m_track_ay = ay;
						m_track_bx = bx;
						m_track_by = by;
					}
				}
				if (!m_has_dot_track)
				{
					// Gravity only chooses the side when tracking starts. A roll to the
					// right makes acc.x negative; that choice matched the first half of the turn.
					float down_x = (float)status.acc.x;
					float down_y = -(float)status.acc.y;
					const float down_len = std::sqrt(down_x * down_x + down_y * down_y);
					float off_x = -(by - ay) / pixels;
					float off_y = (bx - ax) / pixels;
					bool flipped = off_y < 0.0f;
					if (down_len > 0.25f)
					{
						down_x /= down_len;
						down_y /= down_len;
						flipped = off_x * down_x + off_y * down_y < 0.0f;
					}
					if (flipped)
					{
						m_track_ax = bx;
						m_track_ay = by;
						m_track_bx = ax;
						m_track_by = ay;
					}
					else
					{
						m_track_ax = ax;
						m_track_ay = ay;
						m_track_bx = bx;
						m_track_by = by;
					}
					m_has_dot_track = true;
				}
				// Tracking keeps each light through a roll, but a fast turn can
				// finish with the ends swapped. The cursor then stays upside down
				// after the remote is picked up. Whenever the accelerometer is
				// just gravity, the bar's perpendicular has to point the same way
				// as down. A correct track already does, so this does not snap a
				// roll that is being followed. It only repairs a stuck swap.
				{
					const float ix = (float)status.acc.x;
					const float iy = (float)status.acc.y;
					const float iz = (float)status.acc.z;
					const float amag = std::sqrt(ix * ix + iy * iy + iz * iz);
					float down_x = ix;
					float down_y = -iy;
					const float down_len = std::sqrt(down_x * down_x + down_y * down_y);
					if (amag > 0.75f && amag < 1.35f && down_len > 0.25f)
					{
						const float hdx = m_track_bx - m_track_ax;
						const float hdy = m_track_by - m_track_ay;
						if ((-hdy) * down_x + hdx * down_y < 0.0f)
						{
							const float sx = m_track_ax;
							const float sy = m_track_ay;
							m_track_ax = m_track_bx;
							m_track_ay = m_track_by;
							m_track_bx = sx;
							m_track_by = sy;
						}
					}
				}
				// Virtual dots already encode a level GamePad screen. Do not
				// infer their order from a nearly vertical gravity projection.
				if (forced_bar)
				{
					m_track_ax = ax; m_track_ay = ay;
					m_track_bx = bx; m_track_by = by;
					m_has_dot_track = true;
				}
				bar_dx = m_track_bx - m_track_ax;
				bar_dy = m_track_by - m_track_ay;
				pixels = std::sqrt(bar_dx * bar_dx + bar_dy * bar_dy);
				if (pixels < 1.0f)
					pixels = m_last_pointer_sep;
				m_last_pointer_sep = pixels;
				m_last_off_x = -bar_dy / pixels;
				m_last_off_y = bar_dx / pixels;
				constexpr float kFovRad = 33.0f * 3.14159265f / 180.0f;
				const float radians = pixels / 1024.0f * kFovRad;
				const float dist = 0.20f / (2.0f * std::tan(radians * 0.5f));
				status.dist = dist;
				status.distVec = dist - m_last_pointer_dist;
				status.distSpeed = std::abs((float)status.distVec);
				m_last_pointer_dist = dist;
			}
		}
		else
		{
			// No camera lights: a phone (or other) pointer still has a position.
			// Report a full sample with a level horizon so the game accepts the aim.
			status.dpd_valid_fg = 2;
			pixels = 180.0f;
			m_last_pointer_sep = pixels;
			m_last_off_x = 0.0f;
			m_last_off_y = 0.0f;
			status.dist = 3.0f;
			status.horizon.x = 1.0f;
			status.horizon.y = 0.0f;
			m_last_horizon_x = 1.0f;
			m_last_horizon_y = 0.0f;
		}

		const auto cal = LoadPointerCal();
		const float aim_x = mid_x + m_last_off_x * pixels * cal.bar_widths;
		const float aim_y = mid_y + m_last_off_y * pixels * cal.bar_widths;
		const float nx = 1.0f - aim_x / 1023.0f;
		const float ny = aim_y / 768.0f;
		status.pos.x = cal.x_sign * cal.scale * (nx * 2.0f - 1.0f);
		status.pos.y = cal.y_sign * cal.scale * (ny * 2.0f - 1.0f);
		if (first >= 0 && second >= 0 && pixels > 1.0f)
		{
			// Screen X is mirrored. Y is not negated: negating it made a right-hand
			// roll travel to the left.
			float hx = cal.x_sign * bar_dx;
			float hy = cal.y_sign * bar_dy;
			const float hlen = std::sqrt(hx * hx + hy * hy);
			if (hlen > 1.0f)
			{
				hx /= hlen;
				hy /= hlen;
				status.horizon.x = hx;
				status.horizon.y = hy;
				const float hvx = hx - m_last_horizon_x;
				const float hvy = hy - m_last_horizon_y;
				status.horiVec.x = hvx;
				status.horiVec.y = hvy;
				status.horiSpeed = std::sqrt(hvx * hvx + hvy * hvy);
				m_last_horizon_x = hx;
				m_last_horizon_y = hy;
			}
		}
		else if (m_last_horizon_x != 0.0f || m_last_horizon_y != 0.0f)
		{
			// One light left the camera. Keep the last roll instead of snapping upright.
			status.horizon.x = m_last_horizon_x;
			status.horizon.y = m_last_horizon_y;
		}
		if (!forced_bar && first >= 0 && second >= 0 && pixels > 10.0f)
		{
			// Optical ray, before screen aim offset, scaling and mirroring.
			constexpr float fov_x = 42.0f * kTau / 360.0f;
			constexpr float fov_y = fov_x * (768.0f / 1024.0f);
			m_mpls_camera_ray = glm::normalize(glm::vec3(
				(mid_x - 511.5f) / 511.5f * std::tan(fov_x * 0.5f),
				(mid_y - 384.0f) / 384.0f * std::tan(fov_y * 0.5f), -1.0f));
			m_mpls_real_ir_valid = status.dpd_valid_fg == 2;
		}
		m_last_raw_x = mid_x;
		m_last_raw_y = mid_y;

		const float pos_x = (float)status.pos.x;
		const float pos_y = (float)status.pos.y;
		status.vec.x = pos_x - m_last_pos_x;
		status.vec.y = pos_y - m_last_pos_y;
		status.speed = std::sqrt(status.vec.x * status.vec.x + status.vec.y * status.vec.y);
		m_last_pos_x = pos_x;
		m_last_pos_y = pos_y;
	}
	else
		status.dpd_valid_fg = 0;

	if (is_mpls_attached())
	{
		if (has_motion())
		{
			glm::vec3 gyro;
			auto mpls_sample = get_motion_data();
			mpls_sample.getGyrometer(&gyro[0]);
			update_mpls(status, kpad_acc, kpad_acc_speed, gyro,
				mpls_sample.getGyroIntegral(), mpls_sample.getGyroIntegralTime());
		}
		else
		{
			status.mpls.dir.X = beVec3D_t(1.0f, 0.0f, 0.0f);
			status.mpls.dir.Y = beVec3D_t(0.0f, 1.0f, 0.0f);
			status.mpls.dir.Z = beVec3D_t(0.0f, 0.0f, 1.0f);
		}

		const auto now_mpls = std::chrono::steady_clock::now();
		if (m_last_mpls_log == std::chrono::steady_clock::time_point{} ||
			now_mpls - m_last_mpls_log >= std::chrono::seconds(1))
		{
			m_last_mpls_log = now_mpls;
			const auto stick = get_axis();
			cemuLog_log(LogType::Force,
				"KPAD mpls dev={} stick={:.2f},{:.2f} vel={:.3f},{:.3f},{:.3f} angle={:.3f},{:.3f},{:.3f} dpd={} pos={:.2f},{:.2f} dist={:.2f} horizon={:.2f},{:.2f} acc={:.2f},{:.2f},{:.2f} down={:.2f},{:.2f} dirX={:.2f},{:.2f},{:.2f} dirY={:.2f},{:.2f},{:.2f} dirZ={:.2f},{:.2f},{:.2f} bias={:.3f},{:.3f},{:.3f} still={} tilt_err={:.1f} revise={}{}",
				(int)reported_device_type(), stick.x, stick.y,
				(float)status.mpls.mpls.x, (float)status.mpls.mpls.y, (float)status.mpls.mpls.z,
				(float)status.mpls.angle.x, (float)status.mpls.angle.y, (float)status.mpls.angle.z,
				(int)status.dpd_valid_fg, (float)status.pos.x, (float)status.pos.y, (float)status.dist,
				(float)status.horizon.x, (float)status.horizon.y,
				(float)status.acc.x, (float)status.acc.y, (float)status.acc.z,
				(float)status.accVertical.x, (float)status.accVertical.y,
				(float)status.mpls.dir.X.x, (float)status.mpls.dir.X.y, (float)status.mpls.dir.X.z,
				(float)status.mpls.dir.Y.x, (float)status.mpls.dir.Y.y, (float)status.mpls.dir.Y.z,
				(float)status.mpls.dir.Z.x, (float)status.mpls.dir.Z.y, (float)status.mpls.dir.Z.z,
				m_mpls_bias.x, m_mpls_bias.y, m_mpls_bias.z, m_mpls_still_samples, m_mpls_tilt_error,
				m_mpls_dir_revise ? "dir" : "-", m_mpls_dpd_revise ? "+dpd" : "");
			m_mpls_tilt_error = 0;
		}
	}

	switch (type())
	{
	case Wiimote:
		status.ex_status.fs.stick.x = axis.x;
		status.ex_status.fs.stick.y = axis.y;
		{
			const uint32 nunchuk_buttons = uint32(*hold) & (kWPADButton_Z | kWPADButton_C);
			status.ex_status.fs.hold = nunchuk_buttons;
			status.ex_status.fs.trig = uint32(*trigger) & (kWPADButton_Z | kWPADButton_C);
			status.ex_status.fs.release = uint32(*release) & (kWPADButton_Z | kWPADButton_C);
		}

		if(has_second_motion())
		{
			auto motion_sample = get_second_motion_data();

			glm::vec3 acc;
			motion_sample.getAccelerometer(&acc[0]);
			status.ex_status.fs.acc.x = acc.x;
			status.ex_status.fs.acc.y = acc.y;
			status.ex_status.fs.acc.z = acc.z;

			status.ex_status.fs.accValue = motion_sample.getVPADAccMagnitude();
			status.ex_status.fs.accSpeed = motion_sample.getVPADAccAcceleration();
		}

		break;
	case Pro:
		status.ex_status.uc.lstick.x = axis.x;
		status.ex_status.uc.lstick.y = axis.y;

		status.ex_status.uc.rstick.x = rotation.x;
		status.ex_status.uc.rstick.y = rotation.y;

		status.ex_status.uc.charge = FALSE;
		status.ex_status.uc.cable = TRUE;

		break;
	case Classic:
		status.ex_status.cl.lstick.x = axis.x;
		status.ex_status.cl.lstick.y = axis.y;

		status.ex_status.cl.rstick.x = rotation.x;
		status.ex_status.cl.rstick.y = rotation.y;

		if (HAS_FLAG((uint32)status.ex_status.cl.hold, kCLButton_ZL))
			status.ex_status.cl.ltrigger = 1.0f;

		if (HAS_FLAG((uint32)status.ex_status.cl.hold, kCLButton_ZR))
			status.ex_status.cl.rtrigger = 1.0f;
		break;
	default:
		cemu_assert(false);
	}

	
}

void WPADController::set_mpls_direction(const glm::mat3& dir)
{
	m_mpls_dir = Orthonormalize(dir);
	m_mpls_dir_valid = true;
	// Do not apply rotation from before the game's new calibration reference.
	m_mpls_last_integral_time = 0.0;
	m_mpls_last_time = {};
	m_mpls_ir_stable_time = 0.0f;
}

void WPADController::reset_mpls()
{
	m_mpls_dir_valid = false;
	m_mpls_angle = {};
	m_mpls_last_integral_time = 0.0;
	m_mpls_last_time = {};
	m_mpls_bias = {};
	m_mpls_still_samples = 0;
	m_mpls_ir_stable_time = 0.0f;
}

void WPADController::update_mpls(KPADStatus_t& status, const glm::vec3& acc, float acc_speed, const glm::vec3& gyro_rad,
	const glm::quat& gyro_integral, double gyro_integral_time)
{
	const auto now = std::chrono::steady_clock::now();
	float dt = 0.0f;
	if (m_mpls_last_time != std::chrono::steady_clock::time_point{})
		dt = std::chrono::duration<float>(now - m_mpls_last_time).count();
	m_mpls_last_time = now;
	if (dt < 0.0f || dt > 0.1f)
		dt = 0.0f;

	// The game polls about once a frame, but a swing changes rate many times
	// within a frame. When the provider integrates every report, use the rotation
	// it accumulated since the last read and its average rate.
	glm::vec3 report_rate = gyro_rad;
	const bool native_integral = gyro_integral_time > 0.0;
	if (native_integral)
	{
		const double span = gyro_integral_time - m_mpls_last_integral_time;
		// Native rotation is accumulated at sensor cadence. Slow interpreter
		// frames must not discard it when the read interval exceeds 100 ms.
		dt = 0.0f;
		if (m_mpls_last_integral_time > 0.0 && span >= 0.0 && span < 2.0)
		{
			dt = float(span);
			if (span > 0.0)
			{
				glm::quat delta = glm::inverse(m_mpls_last_integral) * gyro_integral;
				if (delta.w < 0.0f)
					delta = -delta;
				const glm::vec3 v(delta.x, delta.y, delta.z);
				const float s = glm::length(v);
				const glm::vec3 rotation = s > 1.0e-7f ? v * (2.0f * std::atan2(s, delta.w) / s) : v * 2.0f;
				report_rate = rotation / float(span);
			}
		}
		m_mpls_last_integral = gyro_integral;
		m_mpls_last_integral_time = gyro_integral_time;
	}

	// The provider rates follow the right-hand rule in the report axes
	// (X left, Y toward the player, Z out of the buttons). KPAD axes are
	// (-X, -Z, Y) of those, a reflection, which flips the sign of a rate.
	glm::vec3 rate(report_rate.x, report_rate.z, -report_rate.y);

	// Zero drift: learn the remaining offset while the remote lies still.
	const float acc_mag = glm::length(acc);
	constexpr float kStillTolerance[3] = {0.12f, 0.06f, 0.03f};
	const float still_tolerance = kStillTolerance[std::clamp(m_mpls_zero_drift_mode, 0, 2)];
	const glm::vec3 residual = glm::abs(rate - m_mpls_bias);
	const bool still = std::abs(acc_mag - 1.0f) < 0.1f && acc_speed < 0.03f &&
		residual.x < still_tolerance && residual.y < still_tolerance && residual.z < still_tolerance;
	if (!still)
		m_mpls_still_samples = 0;
	else if (m_mpls_still_samples < 100000)
		++m_mpls_still_samples;
	// Native zero is already removed. Another bias/deadzone would erase
	// intentional slow rotations. Keep existing behavior for other APIs.
	if (native_integral)
		m_mpls_bias = {};
	else
	{
		if (m_mpls_still_samples >= 30)
			m_mpls_bias += (rate - m_mpls_bias) * 0.05f;
		rate -= m_mpls_bias;
		for (int i = 0; i < 3; ++i)
			if (std::abs(rate[i]) < 0.02f)
				rate[i] = 0.0f;
	}

	// Gravity is fixed in the KPAD frame: a level remote facing the TV is the
	// identity, so up is -Y. The revise base the game sets moves after calibration
	// and must not redefine gravity.
	const glm::vec3 world_up(0.0f, -1.0f, 0.0f);

	if (!m_mpls_dir_valid)
	{
		if (acc_mag > 0.5f)
		{
			m_mpls_dir = DirFromGravity(acc / acc_mag);
			m_mpls_dir_valid = true;
		}
	}
	else if (dt > 0.0f)
	{
		m_mpls_dir = m_mpls_dir * AxisAngle(rate, glm::length(rate) * dt);

		const float rate_mag = std::max(glm::length(rate), glm::length(gyro_rad));
		const float acc_error = std::abs(acc_mag - 1.0f);
		if (acc_mag > 0.5f)
		{
			const glm::vec3 up_now = glm::normalize(m_mpls_dir * (acc / acc_mag));
			m_mpls_tilt_error = std::max(m_mpls_tilt_error,
				std::acos(std::clamp(glm::dot(up_now, world_up), -1.0f, 1.0f)) * 360.0f / kTau);
		}
		if (acc_error < 0.15f && acc_speed < 0.06f && rate_mag < 1.5f)
		{
			const float weight = 0.02f * (1.0f - acc_error / 0.15f) * (1.0f - rate_mag / 1.5f);
			const glm::vec3 measured_up = m_mpls_dir * (acc / acc_mag);
			m_mpls_dir = TurnToward(glm::normalize(measured_up), world_up, ReviseFraction(weight, dt)) * m_mpls_dir;
		}

		// The pointer and the base only correct heading. Nintendo Land keeps the base
		// forward axis 20 degrees off level and copies the current roll into it, so
		// letting them touch pitch or roll would fight gravity.
		const glm::vec3 base_forward = -m_mpls_base[2];
		// Brief optical reacquisition should work during normal aiming motion,
		// without requiring the player to stop. Suspend it for fast swings.
		// Full confidence through 2.5 rad/s, fading to zero at 6 rad/s.
		const float optical_confidence = std::clamp((6.0f - rate_mag) / 3.5f, 0.0f, 1.0f);
		const bool stable_camera = m_mpls_real_ir_valid && optical_confidence > 0.0f &&
			acc_error < 0.25f && acc_speed < 0.3f;
		m_mpls_ir_stable_time = stable_camera ? m_mpls_ir_stable_time + dt : 0.0f;
		if (m_mpls_dpd_revise && m_mpls_ir_stable_time >= 0.025f)
		{
			// Correct the world heading of the measured bar ray, preserving the
			// actual aiming angle and camera roll. Virtual dots cannot measure it.
			const glm::vec3 bar_world = m_mpls_dir * m_mpls_camera_ray;
			// Default game weight 0.05 yields ~100 ms to remove 90% of the error
			// after reacquisition. Bound the angular step to avoid a visible snap,
			// including when a slow game frame spans many sensor reports.
			const float correction_dt = std::min(dt, 0.025f);
			const float max_step = (720.0f * kTau / 360.0f) * correction_dt;
			m_mpls_dir = TurnHeading(bar_world, base_forward, world_up,
				optical_confidence * HeadingReviseFraction(m_mpls_dpd_revise_weight, correction_dt, 450.0f),
				max_step) * m_mpls_dir;
		}
		// Without real IR there is no observed heading. A brief slowdown between
		// swings is not evidence that the remote faces the TV: keep the gyro's
		// heading until optical correction can resume. Gravity still corrects tilt.

		m_mpls_dir = Orthonormalize(m_mpls_dir);
	}

	const glm::vec3 turns = rate / kTau;
	m_mpls_angle += turns * dt;

	status.mpls.mpls = beVec3D_t(turns.x, turns.y, turns.z);
	status.mpls.angle = beVec3D_t(m_mpls_angle.x, m_mpls_angle.y, m_mpls_angle.z);
	status.mpls.dir.X = beVec3D_t(m_mpls_dir[0].x, m_mpls_dir[0].y, m_mpls_dir[0].z);
	status.mpls.dir.Y = beVec3D_t(m_mpls_dir[1].x, m_mpls_dir[1].y, m_mpls_dir[1].z);
	status.mpls.dir.Z = beVec3D_t(m_mpls_dir[2].x, m_mpls_dir[2].y, m_mpls_dir[2].z);
}
