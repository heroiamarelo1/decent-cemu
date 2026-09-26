#include "gui/wxgui/input/WiimoteCalibrationFrame.h"

#include "config/ActiveSettings.h"
#include "input/InputManager.h"
#include "input/api/Controller.h"
#include "input/api/Wiimote/WiimoteMessages.h"

#include <wx/dcbuffer.h>
#include <wx/display.h>

#include <chrono>
#include <cmath>
#include <fstream>

namespace
{
constexpr float kPreviewWidths = 0.7f;
constexpr float kPreviewScale = 1.8f;
constexpr auto kHold = std::chrono::milliseconds(1000);
constexpr float kStillPixels = 40.0f;

wxString StepTitle(WiimoteCalibrationFrame::Step step)
{
	switch (step)
	{
	case WiimoteCalibrationFrame::Step::Center: return _("Point at the center");
	case WiimoteCalibrationFrame::Step::TopLeft: return _("Point at the top-left corner");
	case WiimoteCalibrationFrame::Step::TopRight: return _("Point at the top-right corner");
	case WiimoteCalibrationFrame::Step::BottomLeft: return _("Point at the bottom-left corner");
	case WiimoteCalibrationFrame::Step::BottomRight: return _("Point at the bottom-right corner");
	default: return {};
	}
}

bool IsAimStep(WiimoteCalibrationFrame::Step step)
{
	return step != WiimoteCalibrationFrame::Step::BarSide &&
		step != WiimoteCalibrationFrame::Step::Done &&
		step != WiimoteCalibrationFrame::Step::Failed;
}
}

wxBEGIN_EVENT_TABLE(WiimoteCalibrationFrame, wxFrame)
EVT_PAINT(WiimoteCalibrationFrame::OnPaint)
EVT_ERASE_BACKGROUND(WiimoteCalibrationFrame::OnErase)
EVT_CHAR_HOOK(WiimoteCalibrationFrame::OnKey)
EVT_LEFT_DOWN(WiimoteCalibrationFrame::OnClick)
wxEND_EVENT_TABLE()

WiimoteCalibrationFrame::WiimoteCalibrationFrame(wxWindow* parent)
	: wxFrame(nullptr, wxID_ANY, _("Calibrate Wii Remote"), wxDefaultPosition, wxSize(1280, 720), wxBORDER_NONE | wxFRAME_NO_TASKBAR)
	, m_timer(this)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	int display_index = parent ? wxDisplay::GetFromWindow(parent) : 0;
	if (display_index == wxNOT_FOUND)
		display_index = 0;
	SetSize(wxDisplay(display_index).GetGeometry());
	Bind(wxEVT_TIMER, &WiimoteCalibrationFrame::OnTick, this, m_timer.GetId());
	m_timer.Start(16);
	ShowFullScreen(true);
	Raise();
	SetFocus();
}

void WiimoteCalibrationFrame::OnErase(wxEraseEvent&) {}

WiimoteCalibrationFrame::Reading WiimoteCalibrationFrame::ReadRemote() const
{
	Reading reading;
	auto& input = InputManager::instance();
	static auto next_log = std::chrono::steady_clock::now();
	const bool log_now = std::chrono::steady_clock::now() >= next_log;
	if (log_now)
		next_log = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	for (size_t slot = 0; slot < InputManager::kMaxWPADControllers; ++slot)
	{
		const auto pad = input.get_wpad_controller(slot);
		if (log_now)
			cemuLog_log(LogType::Force, "Calibrate slot {} pad={} type={} apis={}", slot, pad != nullptr,
				pad ? (int)pad->type() : -1, pad ? pad->get_controllers().size() : 0);
		if (!pad || pad->type() != EmulatedController::Wiimote)
			continue;
		for (const auto& api : pad->get_controllers())
		{
			if (log_now && api)
				cemuLog_log(LogType::Force, "Calibrate slot {} api={} connected={}", slot, (int)api->api(), api->is_connected());
			if (!api || api->api() != InputAPI::Wiimote || !api->is_connected())
				continue;
			reading.connected = true;
			const auto state = api->raw_state();
			if (log_now)
			{
				std::string pressed;
				for (const auto id : state.buttons.GetButtonList())
					pressed += fmt::format("{} ", id);
				cemuLog_log(LogType::Force, "Calibrate slot {} buttons [{}]", slot, pressed);
			}
			reading.a = reading.a || state.buttons.GetButtonState(kWiimoteButton_A);
			reading.up = reading.up || state.buttons.GetButtonState(kWiimoteButton_Up);
			reading.down = reading.down || state.buttons.GetButtonState(kWiimoteButton_Down);
			reading.home = reading.home || state.buttons.GetButtonState(kWiimoteButton_Home);

			ControllerBase::IRPoint points[4]{};
			if (api->get_ir_points(points) <= 0)
				continue;
			float acc[3]{};
			api->get_motion_sample().getAccelerometer(acc);
			float down_x = acc[0];
			float down_y = -acc[1];
			const float down_len = std::sqrt(down_x * down_x + down_y * down_y);

			int first = -1;
			int second = -1;
			float best_align = 1.0e9f;
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
					if (first < 0 || align < best_align)
					{
						best_align = align;
						first = i;
						second = j;
					}
				}
			}
			if (first < 0)
				continue;
			float ax = float(points[first].x);
			float ay = float(points[first].y);
			float bx = float(points[second].x);
			float by = float(points[second].y);
			float dx = bx - ax;
			float dy = by - ay;
			float pixels = std::sqrt(dx * dx + dy * dy);
			if (pixels < 1.0f)
				continue;
			float off_x = -dy / pixels;
			float off_y = dx / pixels;
			bool flipped = off_y < 0.0f;
			if (down_len > 0.25f)
				flipped = off_x * down_x / down_len + off_y * down_y / down_len < 0.0f;
			if (flipped)
			{
				std::swap(ax, bx);
				std::swap(ay, by);
				off_x = -off_x;
				off_y = -off_y;
			}
			reading.lights = true;
			reading.mid_x = (ax + bx) * 0.5f;
			reading.mid_y = (ay + by) * 0.5f;
			reading.pixels = pixels;
			reading.off_x = off_x;
			reading.off_y = off_y;
			return reading;
		}
	}
	return reading;
}

wxPoint WiimoteCalibrationFrame::CursorPoint(const Reading& reading) const
{
	const float widths = m_bar_above ? kPreviewWidths : -kPreviewWidths;
	const float aim_x = reading.mid_x + reading.off_x * reading.pixels * widths;
	const float aim_y = reading.mid_y + reading.off_y * reading.pixels * widths;
	const float pos_x = kPreviewScale * ((1.0f - aim_x / 1023.0f) * 2.0f - 1.0f);
	const float pos_y = kPreviewScale * ((aim_y / 768.0f) * 2.0f - 1.0f);
	const wxSize size = GetClientSize();
	const int x = (int)std::lround((pos_x + 1.0f) * 0.5f * size.x);
	const int y = (int)std::lround((pos_y + 1.0f) * 0.5f * size.y);
	return {x, y};
}

void WiimoteCalibrationFrame::ChooseBar(bool above)
{
	m_bar_above = above;
	m_step = Step::Center;
	m_sample_count = 0;
	m_hold_start.reset();
	m_detail.clear();
	m_wait_release = true;
}

void WiimoteCalibrationFrame::AdvancePoint()
{
	const Reading reading = ReadRemote();
	if (!reading.lights)
		return;
	Sample sample;
	sample.mid_x = reading.mid_x;
	sample.mid_y = reading.mid_y;
	sample.pixels = reading.pixels;
	sample.off_x = reading.off_x;
	sample.off_y = reading.off_y;
	switch (m_step)
	{
	case Step::Center: sample.target_x = 0; sample.target_y = 0; break;
	// KPAD pointer y grows downward, so the top corners are -1.
	case Step::TopLeft: sample.target_x = -1; sample.target_y = -1; break;
	case Step::TopRight: sample.target_x = 1; sample.target_y = -1; break;
	case Step::BottomLeft: sample.target_x = -1; sample.target_y = 1; break;
	case Step::BottomRight: sample.target_x = 1; sample.target_y = 1; break;
	default: return;
	}
	m_samples[m_sample_count++] = sample;
	m_hold_start.reset();
	m_wait_release = true;
	if (m_step == Step::Center) m_step = Step::TopLeft;
	else if (m_step == Step::TopLeft) m_step = Step::TopRight;
	else if (m_step == Step::TopRight) m_step = Step::BottomLeft;
	else if (m_step == Step::BottomLeft) m_step = Step::BottomRight;
	else if (SolveAndSave()) m_step = Step::Done;
	else m_step = Step::Failed;
	m_detail.clear();
}

bool WiimoteCalibrationFrame::SolveAndSave()
{
	float best_error = 1.0e9f;
	float best_scale = 0;
	float best_widths = 0;
	float best_x_sign = 1;
	float best_y_sign = 1;
	for (int sx = -1; sx <= 1; sx += 2)
	{
		for (int sy = -1; sy <= 1; sy += 2)
		{
			float xx = 0, xy = 0, yy = 0, xd = 0, yd = 0;
			for (int i = 0; i < m_sample_count; ++i)
			{
				const Sample& s = m_samples[i];
				const float ax = 1.0f - 2.0f * s.mid_x / 1023.0f;
				const float bx = 2.0f * s.off_x * s.pixels / 1023.0f;
				const float ay = 2.0f * s.mid_y / 768.0f - 1.0f;
				const float by = 2.0f * s.off_y * s.pixels / 768.0f;
				const float rows[2][3] = {
					{ax, -bx, s.target_x * float(sx)},
					{ay, by, s.target_y * float(sy)},
				};
				for (const auto& row : rows)
				{
					xx += row[0] * row[0];
					xy += row[0] * row[1];
					yy += row[1] * row[1];
					xd += row[0] * row[2];
					yd += row[1] * row[2];
				}
			}
			const float det = xx * yy - xy * xy;
			if (std::abs(det) < 1.0e-6f)
				continue;
			const float scale = (xd * yy - yd * xy) / det;
			const float product = (xx * yd - xy * xd) / det;
			if (scale < 0.4f || scale > 3.5f)
				continue;
			float error = 0;
			for (int i = 0; i < m_sample_count; ++i)
			{
				const Sample& s = m_samples[i];
				const float ax = 1.0f - 2.0f * s.mid_x / 1023.0f;
				const float bx = 2.0f * s.off_x * s.pixels / 1023.0f;
				const float ay = 2.0f * s.mid_y / 768.0f - 1.0f;
				const float by = 2.0f * s.off_y * s.pixels / 768.0f;
				const float got_x = float(sx) * (scale * ax - product * bx);
				const float got_y = float(sy) * (scale * ay + product * by);
				const float ex = got_x - s.target_x;
				const float ey = got_y - s.target_y;
				error += ex * ex + ey * ey;
			}
			if (error < best_error)
			{
				best_error = error;
				best_scale = scale;
				best_widths = product / scale;
				best_x_sign = float(sx);
				best_y_sign = float(sy);
			}
		}
	}
	if (best_scale == 0 || best_error / m_sample_count > 0.35f || std::abs(best_widths) > 5.0f)
		return false;

	std::ofstream out(ActiveSettings::GetUserDataPath("pointer-cal.txt"));
	if (!out)
		return false;
	// The vertical axis that already works in games stays put. Calibration
	// only changes how far the cursor travels, and which side the bar is on.
	const float saved_scale = std::abs(best_scale);
	const float saved_widths = m_bar_above ? std::abs(best_widths) : -std::abs(best_widths);
	out << "# Read every second. Written by Calibrate Wii Remote.\n";
	out << "x_sign " << (int)best_x_sign << "\n";
	out << "y_sign 1\n";
	out << "bar_widths " << saved_widths << "\n";
	out << "scale " << saved_scale << "\n";
	m_detail = wxString::Format(_("Saved. Reach %.2f, bar offset %.2f."), saved_scale, saved_widths);
	return true;
}

void WiimoteCalibrationFrame::OnTick(wxTimerEvent&)
{
	const Reading reading = ReadRemote();
	if (reading.home)
	{
		Close();
		return;
	}

	if (m_step == Step::BarSide)
	{
		if (reading.up && !m_up_held)
			m_bar_highlight = 0;
		if (reading.down && !m_down_held)
			m_bar_highlight = 1;
		if (reading.a && !m_a_held)
			ChooseBar(m_bar_highlight == 0);
	}
	else if (m_step == Step::Done || m_step == Step::Failed)
	{
		if (reading.a && !m_a_held)
			Close();
	}
	else if (IsAimStep(m_step))
	{
		const auto now = std::chrono::steady_clock::now();
		if (!reading.connected)
		{
			m_hold_start.reset();
			m_detail = _("Connect a Wii Remote and point it at this screen.");
		}
		else if (!reading.lights)
		{
			m_hold_start.reset();
			m_detail = _("The sensor bar left the camera. Step closer and point at the marker.");
		}
		else if (!reading.a)
		{
			m_hold_start.reset();
			m_wait_release = false;
			m_detail.clear();
		}
		else if (m_wait_release)
		{
			m_hold_start.reset();
		}
		else
		{
			if (!m_hold_start)
			{
				m_hold_start = now;
				m_hold_x = reading.mid_x;
				m_hold_y = reading.mid_y;
			}
			const float moved = std::hypot(reading.mid_x - m_hold_x, reading.mid_y - m_hold_y);
			if (moved > kStillPixels)
			{
				m_hold_start = now;
				m_hold_x = reading.mid_x;
				m_hold_y = reading.mid_y;
				m_detail = _("Hold still on the marker.");
			}
			else if (now - *m_hold_start >= kHold)
				AdvancePoint();
		}
	}

	m_a_held = reading.a;
	m_up_held = reading.up;
	m_down_held = reading.down;
	Refresh(false);
}

void WiimoteCalibrationFrame::OnKey(wxKeyEvent& event)
{
	if (event.GetKeyCode() == WXK_ESCAPE)
	{
		Close();
		return;
	}
	if (m_step == Step::BarSide)
	{
		if (event.GetKeyCode() == WXK_UP)
			m_bar_highlight = 0;
		else if (event.GetKeyCode() == WXK_DOWN)
			m_bar_highlight = 1;
		else if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER)
			ChooseBar(m_bar_highlight == 0);
		Refresh(false);
		return;
	}
	if ((m_step == Step::Done || m_step == Step::Failed) &&
		(event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER))
		Close();
	event.Skip();
}

void WiimoteCalibrationFrame::OnClick(wxMouseEvent& event)
{
	if (m_step != Step::BarSide)
		return;
	const int mid = GetClientSize().y / 2;
	ChooseBar(event.GetY() < mid);
}

void WiimoteCalibrationFrame::OnPaint(wxPaintEvent&)
{
	wxBufferedPaintDC dc(this);
	const wxSize size = GetClientSize();
	dc.SetBackground(wxBrush(wxColour(12, 16, 28)));
	dc.Clear();
	dc.SetTextForeground(wxColour(240, 244, 248));
	wxFont title = GetFont();
	title.SetPointSize(28);
	title.SetWeight(wxFONTWEIGHT_BOLD);
	wxFont body = GetFont();
	body.SetPointSize(16);

	if (m_step == Step::BarSide)
	{
		dc.SetFont(title);
		dc.DrawLabel(_("Where is the sensor bar?"), wxRect(0, size.y / 2 - 220, size.x, 60), wxALIGN_CENTER);
		dc.SetFont(body);
		dc.DrawLabel(_("Up and Down on the remote, then A. This screen is the one Cemu is on."),
			wxRect(80, size.y / 2 - 150, size.x - 160, 50), wxALIGN_CENTER);
		const wxRect above(size.x / 2 - 280, size.y / 2 - 60, 560, 80);
		const wxRect below(size.x / 2 - 280, size.y / 2 + 40, 560, 80);
		dc.SetBrush(wxBrush(m_bar_highlight == 0 ? wxColour(36, 110, 196) : wxColour(32, 40, 58)));
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.DrawRoundedRectangle(above, 8);
		dc.SetBrush(wxBrush(m_bar_highlight == 1 ? wxColour(36, 110, 196) : wxColour(32, 40, 58)));
		dc.DrawRoundedRectangle(below, 8);
		dc.DrawLabel(_("Above the TV"), above, wxALIGN_CENTER);
		dc.DrawLabel(_("Below the TV"), below, wxALIGN_CENTER);
		return;
	}

	if (m_step == Step::Done || m_step == Step::Failed)
	{
		dc.SetFont(title);
		dc.DrawLabel(m_step == Step::Done ? _("Calibration saved") : _("Calibration was not accepted"),
			wxRect(0, size.y / 2 - 80, size.x, 60), wxALIGN_CENTER);
		dc.SetFont(body);
		const wxString why = m_step == Step::Failed && m_detail.empty()
			? _("The points did not agree. Stay at the distance you play from, and try again.")
			: m_detail;
		dc.DrawLabel(why, wxRect(80, size.y / 2, size.x - 160, 80), wxALIGN_CENTER);
		dc.DrawLabel(_("A or Enter closes this screen."), wxRect(0, size.y / 2 + 90, size.x, 40), wxALIGN_CENTER);
		return;
	}

	wxPoint marker(size.x / 2, size.y / 2);
	if (m_step == Step::TopLeft) marker = {48, 48};
	else if (m_step == Step::TopRight) marker = {size.x - 48, 48};
	else if (m_step == Step::BottomLeft) marker = {48, size.y - 48};
	else if (m_step == Step::BottomRight) marker = {size.x - 48, size.y - 48};

	dc.SetPen(wxPen(wxColour(255, 196, 48), 6));
	dc.SetBrush(*wxTRANSPARENT_BRUSH);
	dc.DrawCircle(marker, 36);
	dc.DrawLine(marker.x - 58, marker.y, marker.x - 42, marker.y);
	dc.DrawLine(marker.x + 42, marker.y, marker.x + 58, marker.y);
	dc.DrawLine(marker.x, marker.y - 58, marker.x, marker.y - 42);
	dc.DrawLine(marker.x, marker.y + 42, marker.x, marker.y + 58);

	const Reading reading = ReadRemote();
	if (reading.lights)
	{
		const wxPoint cursor = CursorPoint(reading);
		dc.SetPen(wxPen(wxColour(8, 10, 16), 3));
		dc.SetBrush(wxBrush(wxColour(120, 220, 255)));
		dc.DrawCircle(cursor, 14);
	}

	dc.SetFont(title);
	dc.DrawLabel(StepTitle(m_step), wxRect(0, 36, size.x, 50), wxALIGN_CENTER);
	dc.SetFont(body);
	wxString hint = _("Aim the remote at the marker and hold A. The dot may not sit on it yet.");
	if (!m_detail.empty())
		hint = m_detail;
	dc.DrawLabel(hint, wxRect(40, 96, size.x - 80, 70), wxALIGN_CENTER);

	if (m_hold_start && reading.lights && reading.a)
	{
		const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - *m_hold_start).count();
		const int radius = (int)(36 + 28 * std::min(t, 1.0f));
		dc.SetPen(wxPen(wxColour(120, 220, 255), 4));
		dc.DrawCircle(marker, radius);
	}
}
