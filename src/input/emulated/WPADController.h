#pragma once

#include "input/emulated/EmulatedController.h"
#include "Cafe/OS/libs/padscore/padscore.h"
#include "Cafe/OS/libs/vpad/vpad.h"

constexpr uint32 kWPADButtonRepeat = 0x80000000;

enum WPADDeviceType
{
	kWAPDevCore = 0,
	kWAPDevFreestyle = 1,
	kWAPDevClassic = 2,
	kWAPDevMPLS = 5,
	kWAPDevMPLSFreeStyle = 6,
	kWAPDevMPLSClassic = 7,
	kWAPDevURCC = 31,
	kWAPDevNotFound = 253,
	kWAPDevUnknown = 255,
};

// core, balanceboard
enum WPADCoreButtons
{
	kWPADButton_Left = 0x1,
	kWPADButton_Right = 0x2,
	kWPADButton_Down = 0x4,
	kWPADButton_Up = 0x8,
	kWPADButton_Plus = 0x10,
	kWPADButton_2 = 0x100,
	kWPADButton_1 = 0x200,
	kWPADButton_B = 0x400,
	kWPADButton_A = 0x800,
	kWPADButton_Minus = 0x1000,
	kWPADButton_Home = 0x8000,
};

// Nunchuck aka Freestyle
enum WPADNunchuckButtons
{
	kWPADButton_Z = 0x2000,
	kWPADButton_C = 0x4000,
};

// Classic Controller
enum WPADClassicButtons
{
	kCLButton_Up = 0x1,
	kCLButton_Left = 0x2,
	kCLButton_ZR = 0x4,
	kCLButton_X = 0x8,
	kCLButton_A = 0x10,
	kCLButton_Y = 0x20,
	kCLButton_B = 0x40,
	kCLButton_ZL = 0x80,
	kCLButton_R = 0x200,
	kCLButton_Plus = 0x400,
	kCLButton_Home = 0x800,
	kCLButton_Minus = 0x1000,
	kCLButton_L = 0x2000,
	kCLButton_Down = 0x4000,
	kCLButton_Right = 0x8000
};

// Pro Controller aka URCC
enum WPADProButtons
{
	kProButton_Up = 0x1,
	kProButton_Left = 0x2,
	kProButton_ZR = 0x4,
	kProButton_X = 0x8,
	kProButton_A = 0x10,
	kProButton_Y = 0x20,
	kProButton_B = 0x40,
	kProButton_ZL = 0x80,
	kProButton_R = 0x200,
	kProButton_Plus = 0x400,
	kProButton_Home = 0x800,
	kProButton_Minus = 0x1000,
	kProButton_L = 0x2000,
	kProButton_Down = 0x4000,
	kProButton_Right = 0x8000,
	kProButton_StickR = 0x10000,
	kProButton_StickL = 0x20000
};

enum WPADDataFormat {
	 kDataFormat_CORE = 0,
	 kDataFormat_CORE_ACC = 1,
	 kDataFormat_CORE_ACC_DPD = 2,
	 kDataFormat_FREESTYLE = 3,
	 kDataFormat_FREESTYLE_ACC = 4,
	 kDataFormat_FREESTYLE_ACC_DPD = 5,
	 kDataFormat_CLASSIC = 6,
	 kDataFormat_CLASSIC_ACC = 7,
	 kDataFormat_CLASSIC_ACC_DPD = 8,
	 kDataFormat_CORE_ACC_DPD_FULL = 9, // buttons, motion, pointing
	 kDataFormat_TRAIN = 10,
	 kDataFormat_GUITAR = 11,
	 kDataFormat_BALANCE_CHECKER = 12,
	 kDataFormat_DRUM = 15,
	 kDataFormat_MPLS = 16, // buttons, motion, pointing, motion plus
	 kDataFormat_TAIKO = 17,
	 kDataFormat_URCC = 22, // buttons, URCC aka pro
};

class WPADController : public EmulatedController
{
	using base_type = EmulatedController;
public:
	WPADController(size_t player_index, WPADDataFormat data_format);

	uint32 get_emulated_button_flag(WPADDataFormat format, uint32 id) const;

	virtual WPADDeviceType get_device_type() const = 0;
	// What the game's probe and KPAD sample should say. MotionPlus stays available
	// even when this reports a plain Nunchuk.
	virtual WPADDeviceType reported_device_type() const { return get_device_type(); }

	WPADDataFormat get_data_format() const { return m_data_format; }
	void set_data_format(WPADDataFormat data_format)
	{
		m_data_format = data_format;
		m_format_from_game = true;
	}

	void WPADRead(WPADStatus_t* status);

	void KPADRead(KPADStatus_t& status, const BtnRepeat& repeat);
	virtual bool is_mpls_attached() { return false; }
	// 14-bit MotionPlus counts: pitch, yaw, roll. Rest is near 8192.
	virtual bool get_motion_plus_raw(uint16& pitch, uint16& yaw, uint16& roll) const { return false; }

	enum class ConnectCallbackStatus
	{
		None, // do nothing
		ReportDisconnect, // call disconnect
		ReportConnect, // call connect
	};
	ConnectCallbackStatus m_status = ConnectCallbackStatus::ReportConnect;
	ConnectCallbackStatus m_extension_status = ConnectCallbackStatus::ReportConnect;
	int m_last_reported_extension = -1;
	bool m_format_from_game = false;

	WPADDataFormat get_default_data_format() const;

protected:
	WPADDataFormat m_data_format;

private:
	uint32be m_last_holdvalue = 0;

	std::chrono::steady_clock::time_point m_last_hold_change{}, m_last_pulse{};
	std::chrono::steady_clock::time_point m_last_mpls_log{};
	float m_last_pointer_dist = 0;
	float m_last_pointer_sep = 0;
	float m_last_raw_x = 0;
	float m_last_raw_y = 0;
	// Unit offset from the bar toward the screen, in camera pixels. +Y is down in the image.
	float m_last_off_x = 0;
	float m_last_off_y = 1;
	float m_last_pos_x = 0;
	float m_last_pos_y = 0;
	float m_last_horizon_x = 0;
	float m_last_horizon_y = 0;
	// The two lights are identical. Once gravity picks which end is which,
	// follow each light so a roll does not flip the aim to the other side.
	bool m_has_dot_track = false;
	// Session-local gate for this player; the global option remains the master switch.
	bool m_gamepad_bar_enabled = true;
	bool m_gamepad_bar_combo_latched = false;
	bool update_gamepad_bar_shortcut();
	bool m_inverted_bar_latched = false;
	bool m_last_bar_was_virtual = false;
	// Physical ray is independent of cursor scaling and virtual GamePad dots.
	bool m_mpls_real_ir_valid = false;
	glm::vec3 m_mpls_camera_ray{0.0f, 0.0f, -1.0f};
	float m_mpls_ir_stable_time = 0.0f;
	// Option on, MotionPlus attached, camera toward the floor, and the camera
	// does not already see two lights. Fills two synthetic sensor-bar dots.
	bool fill_inverted_sensor_bar(uint16 xs[4], uint16 ys[4], bool lit[4]);
	float m_track_ax = 0;
	float m_track_ay = 0;
	float m_track_bx = 0;
	float m_track_by = 0;

	void update_mpls(KPADStatus_t& status, const glm::vec3& acc, float acc_speed, const glm::vec3& gyro_rad,
		const glm::quat& gyro_integral, double gyro_integral_time);

	// KPAD MotionPlus state. Columns of m_mpls_dir are the remote's X, Y and Z
	// axes in the frame the game set with KPADSetMplsDirection.
	glm::mat3 m_mpls_dir{1.0f};
	glm::mat3 m_mpls_base{1.0f};
	glm::vec3 m_mpls_angle{};
	glm::vec3 m_mpls_bias{};
	bool m_mpls_dir_valid = false;
	uint32 m_mpls_still_samples = 0;
	// Largest angle, in degrees, between gravity and the integrated attitude since the last log line.
	float m_mpls_tilt_error = 0;
	std::chrono::steady_clock::time_point m_mpls_last_time{};
	glm::quat m_mpls_last_integral{1.0f, 0.0f, 0.0f, 0.0f};
	double m_mpls_last_integral_time = 0.0;

public:
	void set_mpls_direction(const glm::mat3& dir);
	void set_mpls_dir_revise_base(const glm::mat3& base) { m_mpls_base = base; }
	void reset_mpls();

	// Stays false until the game calls KPADEnableMpls. Games that never do, such as
	// Super Mario 3D World, then see a Nunchuk instead of a sideways remote.
	bool m_mpls_enabled = false;
	bool m_mpls_dir_revise = false;
	float m_mpls_dir_revise_weight = 0.03f;
	bool m_mpls_dpd_revise = false;
	float m_mpls_dpd_revise_weight = 0.05f;
	sint32 m_mpls_zero_drift_mode = 1;

	
};
