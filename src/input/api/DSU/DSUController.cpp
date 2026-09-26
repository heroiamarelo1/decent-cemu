#include "input/api/DSU/DSUController.h"

#include <algorithm>
#include <boost/program_options/value_semantic.hpp>

DSUController::DSUController(uint32 index)
	: base_type(fmt::format("{}", index), fmt::format("Controller {}", index + 1)), m_index(index)
{
	if (index >= DSUControllerProvider::kMaxClients)
		throw std::runtime_error(fmt::format("max {} dsu controllers are supported! given index: {}",
		                                     DSUControllerProvider::kMaxClients, index));
}

DSUController::DSUController(uint32 index, const DSUProviderSettings& settings)
	: base_type(fmt::format("{}", index), fmt::format("Controller {}", index + 1), settings), m_index(index)
{
	if (index >= DSUControllerProvider::kMaxClients)
		throw std::runtime_error(fmt::format("max {} dsu controllers are supported! given index: {}",
			DSUControllerProvider::kMaxClients, index));
}

void DSUController::save(pugi::xml_node& node)
{
	base_type::save(node);

	node.append_child("ip").append_child(pugi::node_pcdata).set_value(
		fmt::format("{}", m_provider->get_settings().ip).c_str());
	node.append_child("port").append_child(pugi::node_pcdata).set_value(
		fmt::format("{}", m_provider->get_settings().port).c_str());
}

void DSUController::load(const pugi::xml_node& node)
{
	base_type::load(node);

	DSUProviderSettings settings;
	if (const auto value = node.child("ip"))
		settings.ip = value.child_value();
	if (const auto value = node.child("port"))
		settings.port = ConvertString<uint16>(value.child_value());

	const auto provider = InputManager::instance().get_api_provider(api(), settings);
	update_provider(std::dynamic_pointer_cast<DSUControllerProvider>(provider));
	set_use_motion(true);
	connect();
}

bool DSUController::connect()
{
	if (is_connected())
		return true;

	m_provider->request_pad_data(m_index);
	return is_connected();
}

bool DSUController::is_connected()
{
	return m_provider->is_connected(m_index);
}

MotionSample DSUController::get_motion_sample()
{
	return m_provider->get_motion_sample(m_index);
}

bool DSUController::has_touch()
{
	const auto state = m_provider->get_state(m_index);
	return state.data.tpad1.active || state.data.tpad2.active;
}

glm::vec2 DSUController::get_touch_position()
{
	const auto state = m_provider->get_state(m_index);
	if (state.data.tpad1.active)
		return glm::vec2{(float)state.data.tpad1.x / 1920.0f, (float)state.data.tpad1.y / 942.0f};
	if (state.data.tpad2.active)
		return glm::vec2{(float)state.data.tpad2.x / 1920.0f, (float)state.data.tpad2.y / 942.0f};
	return {};
}

bool DSUController::has_magnet()
{
	return m_provider->get_state(m_index).has_magnet;
}

glm::vec3 DSUController::get_magnet()
{
	const auto state = m_provider->get_state(m_index);
	return {state.magnet[0], state.magnet[1], state.magnet[2]};
}

bool DSUController::has_position()
{
	const auto state = m_provider->get_state(m_index);
	if (state.data.tpad1.active || state.data.tpad2.active)
		return true;
	return state.info.state == DsState::Connected;
}

glm::vec2 DSUController::get_position()
{
	const auto state = m_provider->get_state(m_index);
	if (state.data.tpad1.active)
		return glm::vec2{(float)state.data.tpad1.x / 1920.0f, (float)state.data.tpad1.y / 942.0f};
	if (state.data.tpad2.active)
		return glm::vec2{(float)state.data.tpad2.x / 1920.0f, (float)state.data.tpad2.y / 942.0f};

	// Hold the phone like a remote. Yaw moves the cursor sideways, pitch moves it up
	// and down. WiiMoteDSU's recenter control is the touch button.
	if (state.data.touch || HAS_BIT(state.data.state1, 1))
		m_aim = {};
	float gyro[3]{};
	m_provider->get_motion_sample(m_index).getGyrometer(gyro);
	const auto now = std::chrono::steady_clock::now();
	float dt = 0.0f;
	if (m_aim_time != std::chrono::steady_clock::time_point{})
		dt = std::chrono::duration<float>(now - m_aim_time).count();
	m_aim_time = now;
	if (dt > 0.0f && dt < 0.1f)
	{
		constexpr float kGain = 0.55f;
		m_aim.x = std::clamp(m_aim.x + gyro[1] * dt * kGain, -0.85f, 0.85f);
		m_aim.y = std::clamp(m_aim.y - gyro[0] * dt * kGain, -0.85f, 0.85f);
	}
	return glm::vec2{0.5f + m_aim.x, 0.5f + m_aim.y};
}

glm::vec2 DSUController::get_prev_position()
{
	const auto state = m_provider->get_prev_state(m_index);
	if (state.data.tpad1.active)
		return glm::vec2{(float)state.data.tpad1.x / 1920.0f, (float)state.data.tpad1.y / 942.0f};

	if (state.data.tpad2.active)
		return glm::vec2{(float)state.data.tpad2.x / 1920.0f, (float)state.data.tpad2.y / 942.0f};

	return {};
}

PositionVisibility DSUController::GetPositionVisibility()
{
	const auto state = m_provider->get_prev_state(m_index);

	if (state.data.tpad1.active || state.data.tpad2.active || state.info.state == DsState::Connected)
		return PositionVisibility::FULL;
	return PositionVisibility::NONE;
}

std::string DSUController::get_button_name(uint64 button) const
{
	switch (button)
	{
	case kButton0: return "Share";
	case kButton1: return "Stick L";
	case kButton2: return "Stick R";
	case kButton3: return "Options";
	case kButton4: return "Up";
	case kButton5: return "Right";
	case kButton6: return "Down";
	case kButton7: return "Left";

	case kButton8: return "ZL";
	case kButton9: return "ZR";
	case kButton10: return "L";
	case kButton11: return "R";
	case kButton12: return "Triangle";
	case kButton13: return "Circle";
	case kButton14: return "Cross";
	case kButton15: return "Square";

	case kButton16: return "Touch";
	case kButton17: return "PS";
	case kButton18: return "Mic";
	case kButton19: return "TV";
	}
	return base_type::get_button_name(button);
}

ControllerState DSUController::raw_state()
{
	ControllerState result{};

	if (!is_connected())
		return result;

	const auto state = m_provider->get_state(m_index);
	// didn't read any data from the controller yet
	if (state.info.state != DsState::Connected)
		return result;

	int bitindex = 0;
	for (int i = 0; i < 8; ++i, ++bitindex)
	{
		if (HAS_BIT(state.data.state1, i))
		{
			result.buttons.SetButtonState(bitindex, true);
		}
	}

	for (int i = 0; i < 8; ++i, ++bitindex)
	{
		if (HAS_BIT(state.data.state2, i))
		{
			result.buttons.SetButtonState(bitindex, true);
		}
	}

	if (state.data.touch)
		result.buttons.SetButtonState(kButton16, true);
	if (state.data.ps)
		result.buttons.SetButtonState(kButton17, true);
	if (state.mic_down)
		result.buttons.SetButtonState(kButton18, true);
	if (state.screen_down)
		result.buttons.SetButtonState(kButton19, true);

	result.axis.x = (float)state.data.lx / std::numeric_limits<uint8>::max();
	result.axis.x = (result.axis.x * 2.0f) - 1.0f;

	result.axis.y = (float)state.data.ly / std::numeric_limits<uint8>::max();
	result.axis.y = (result.axis.y * 2.0f) - 1.0f;

	result.rotation.x = (float)state.data.rx / std::numeric_limits<uint8>::max();
	result.rotation.x = (result.rotation.x * 2.0f) - 1.0f;

	result.rotation.y = (float)state.data.ry / std::numeric_limits<uint8>::max();
	result.rotation.y = (result.rotation.y * 2.0f) - 1.0f;

    result.trigger.x = (float)state.data.l2 / std::numeric_limits<uint8>::max();
    result.trigger.y = (float)state.data.r2 / std::numeric_limits<uint8>::max();

    return result;
}
