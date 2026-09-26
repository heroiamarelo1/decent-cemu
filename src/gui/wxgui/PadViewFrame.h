#pragma once

#include <wx/frame.h>

#define WM_CREATE_PAD	(WM_USER+1)
#define WM_DESTROY_PAD	(WM_USER+2)

wxDECLARE_EVENT(EVT_PAD_CLOSE, wxCommandEvent);
wxDECLARE_EVENT(EVT_SET_WINDOW_TITLE, wxCommandEvent);

class PadViewFrame : public wxFrame
{
public:
	PadViewFrame(wxFrame* parent);
	~PadViewFrame();

	bool Initialize();
	// Move the GamePad window onto the second display and maximize it.
	void PlaceOnSecondMonitor();
	// A normal GamePad window on the primary screen. Does not overwrite the Android placement.
	void PlaceAsNormalWindow();
	void InitializeRenderCanvas();
	void DestroyCanvas();

	void OnKeyUp(wxKeyEvent& event);
	void OnChar(wxKeyEvent& event);
	
	void AsyncSetTitle(std::string_view windowTitle);

private:

	void OnMouseMove(wxMouseEvent& event);
	void OnMouseLeft(wxMouseEvent& event);
	void OnMouseRight(wxMouseEvent& event);
	void OnSizeEvent(wxSizeEvent& event);
	void OnDPIChangedEvent(wxDPIChangedEvent& event);
	void OnMoveEvent(wxMoveEvent& event);
	void OnGesturePan(wxPanGestureEvent& event);
	void OnSetWindowTitle(wxCommandEvent& event);

	wxWindow* m_render_canvas = nullptr;
	// While set, moving this window does not replace the saved Android placement.
	bool m_preservePlacement = false;
};
