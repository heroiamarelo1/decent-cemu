#pragma once

#include "input/api/Controller.h"

#include <chrono>
#include "input/api/DSU/DSUControllerProvider.h"
#include "Cafe/HW/AI/AI.h"
#include "Cafe/HW/AI/AI.h"
#include "Cafe/HW/AI/AI.h"
#include "Cafe/HW/AI/AI.h"

class DSUController : public Controller<DSUControllerProvider>
{
public:
	DSUController(uint32 index);
	DSUController(uint32 index, const DSUProviderSettings& settings);
	
	std::string_view api_name() const override
	{
		static_assert(to_string(InputAPI::DSUClient) == "DSUController");
		return to_string(InputAPI::DSUClient);
	}
	InputAPI::Type api() const override { return InputAPI::DSUClient; }

	void save(pugi::xml_node& node) override;
	void load(const pugi::xml_node& node) override;

	bool connect() override;
	bool is_connected() override;
	
	bool has_motion() override { return true; }
	MotionSample get_motion_sample() override;

	bool has_position() override;
	bool has_touch() override;
	glm::vec2 get_touch_position() override;
	bool has_magnet() override;
	glm::vec3 get_magnet() override;
	glm::vec2 get_position() override;
	glm::vec2 get_prev_position() override;
	PositionVisibility GetPositionVisibility() override;

	std::string get_button_name(uint64 button) const override;

protected:
	ControllerState raw_state() override;

private:
	uint32 m_index;
	// Aim offset from screen center, in roughly screen-widths, from the phone gyro.
	glm::vec2 m_aim{};
	std::chrono::steady_clock::time_point m_aim_time{};
};

