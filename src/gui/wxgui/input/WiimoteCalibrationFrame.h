#pragma once

#include <wx/frame.h>
#include <wx/timer.h>

#include <array>
#include <chrono>
#include <optional>

// Full-screen pointer calibration. The sensor-bar side is chosen first, then
// the remote is aimed at the center and the four corners of this screen.
class WiimoteCalibrationFrame : public wxFrame
{
public:
	explicit WiimoteCalibrationFrame(wxWindow* parent);

	enum class Step
	{
		BarSide,
		Center,
		TopLeft,
		TopRight,
		BottomLeft,
		BottomRight,
		Done,
		Failed,
	};

private:
	struct Sample
	{
		float mid_x = 0;
		float mid_y = 0;
		float pixels = 0;
		float off_x = 0;
		float off_y = 0;
		float target_x = 0;
		float target_y = 0;
	};

	struct Reading
	{
		bool connected = false;
		bool lights = false;
		bool a = false;
		bool up = false;
		bool down = false;
		bool home = false;
		float mid_x = 0;
		float mid_y = 0;
		float pixels = 0;
		float off_x = 0;
		float off_y = 0;
	};

	void OnPaint(wxPaintEvent& event);
	void OnErase(wxEraseEvent& event);
	void OnKey(wxKeyEvent& event);
	void OnClick(wxMouseEvent& event);
	void OnTick(wxTimerEvent& event);

	Reading ReadRemote() const;
	void ChooseBar(bool above);
	void AdvancePoint();
	bool SolveAndSave();
	wxPoint CursorPoint(const Reading& reading) const;

	Step m_step = Step::BarSide;
	bool m_bar_above = true;
	int m_bar_highlight = 0;
	bool m_a_held = false;
	bool m_up_held = false;
	bool m_down_held = false;
	bool m_wait_release = false;
	std::optional<std::chrono::steady_clock::time_point> m_hold_start;
	float m_hold_x = 0;
	float m_hold_y = 0;
	std::array<Sample, 5> m_samples{};
	int m_sample_count = 0;
	wxString m_detail;
	wxTimer m_timer;

	wxDECLARE_EVENT_TABLE();
};
