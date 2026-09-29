#include "interface/WindowSystem.h"
#include "wxgui/wxgui.h"
#include "wxgui/PadViewFrame.h"
#include "wxgui/GamePadViewStream.h"

#include <wx/display.h>

#include "config/ActiveSettings.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"
#ifdef ENABLE_OPENGL
#include "wxgui/canvas/OpenGLCanvas.h"
#endif
#ifdef ENABLE_VULKAN
#include "wxgui/canvas/VulkanCanvas.h"
#endif
#ifdef ENABLE_METAL
#include "wxgui/canvas/MetalCanvas.h"
#endif
#include "config/CemuConfig.h"
#include "wxgui/MainWindow.h"
#include "wxgui/helpers/wxHelpers.h"
#include "input/InputManager.h"

#if BOOST_OS_WINDOWS
#include <windows.h>
#endif

#if BOOST_OS_LINUX || BOOST_OS_MACOS || BOOST_OS_BSD
#include "resource/embedded/resources.h"
#endif
#include "wxHelper.h"

extern WindowSystem::WindowInfo g_window_info;

#define PAD_MIN_WIDTH  320
#define PAD_MIN_HEIGHT 180

PadViewFrame::PadViewFrame(wxFrame* parent)
	: wxFrame(nullptr, wxID_ANY, _("GamePad View"), wxDefaultPosition, wxDefaultSize, wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxSYSTEM_MENU | wxCAPTION | wxCLIP_CHILDREN | wxRESIZE_BORDER | wxCLOSE_BOX | wxWANTS_CHARS)
{
	g_window_info.window_pad = initHandleContextFromWxWidgetsWindow(this);
	GamePadViewStream_Start(reinterpret_cast<uintptr_t>(parent->GetHandle()));

	SetIcon(wxICON(M_WND_ICON128));
	wxWindow::EnableTouchEvents(wxTOUCH_PAN_GESTURES);

	SetMinClientSize({ PAD_MIN_WIDTH, PAD_MIN_HEIGHT });

	SetPosition({ g_window_info.restored_pad_x, g_window_info.restored_pad_y });
	if (g_window_info.restored_pad_width >= PAD_MIN_WIDTH && g_window_info.restored_pad_height >= PAD_MIN_HEIGHT)
		SetClientSize({ g_window_info.restored_pad_width, g_window_info.restored_pad_height });
	else
		SetClientSize(wxSize(854, 480));

	// When the second-monitor option is on, PlaceOnSecondMonitor() owns maximize.
	if (g_window_info.pad_maximized && !GetConfig().pad_second_monitor.GetValue())
		Maximize();

	Bind(wxEVT_SIZE, &PadViewFrame::OnSizeEvent, this);
	Bind(wxEVT_DPI_CHANGED, &PadViewFrame::OnDPIChangedEvent, this);
	Bind(wxEVT_MOVE, &PadViewFrame::OnMoveEvent, this);
	Bind(wxEVT_MOTION, &PadViewFrame::OnMouseMove, this);

	Bind(wxEVT_SET_WINDOW_TITLE, &PadViewFrame::OnSetWindowTitle, this);

	g_window_info.pad_open = true;
}

PadViewFrame::~PadViewFrame()
{
	g_window_info.pad_open = false;
}

void PadViewFrame::PlaceOnSecondMonitor()
{
	// Same size a freshly opened view uses, so turning the option on does not
	// leave the normal 854x480 window in place. Keep the saved rect intact.
	m_preservePlacement = true;
	if (g_window_info.restored_pad_width >= PAD_MIN_WIDTH && g_window_info.restored_pad_height >= PAD_MIN_HEIGHT)
	{
		if (IsMaximized())
			Maximize(false);
		SetPosition({ (int)g_window_info.restored_pad_x, (int)g_window_info.restored_pad_y });
		SetClientSize({ (int)g_window_info.restored_pad_width, (int)g_window_info.restored_pad_height });
	}
	m_preservePlacement = false;
	if (!GetConfig().pad_second_monitor.GetValue())
	{
		Layout();
		SendSizeEvent();
		Show(true);
		return;
	}

	const unsigned count = wxDisplay::GetCount();
	if (count < 2)
		return;

	// Prefer the first non-primary display (index 1 is not always the second screen).
	int secondId = -1;
	for (unsigned i = 0; i < count; ++i)
	{
		wxDisplay display(i);
		if (!display.IsPrimary())
		{
			secondId = static_cast<int>(i);
			break;
		}
	}
	if (secondId < 0)
		return;

	wxDisplay second(secondId);
	// Full geometry so Windows associates the window with that monitor before maximize.
	const wxRect geo = second.GetGeometry();

	if (IsFullScreen())
		ShowFullScreen(false);

#if BOOST_OS_WINDOWS
	HWND hwnd = GetHWND();
	if (!hwnd)
		return;

	// Hide first. On a window that is already open, wx keeps the old client size
	// unless the placement is applied while the window is hidden.
	const bool wasShown = IsShown();
	if (wasShown)
		Show(false);

	if (IsMaximized() || IsIconized())
		::ShowWindow(hwnd, SW_RESTORE);

	// SetWindowPlacement is reliable on Windows: maximize lands on the monitor
	// that contains rcNormalPosition. wx Maximize() after SetPosition often stays
	// on the primary display because the move is not committed yet.
	WINDOWPLACEMENT placement{};
	placement.length = sizeof(placement);
	::GetWindowPlacement(hwnd, &placement);
	placement.showCmd = SW_MAXIMIZE;
	placement.flags = 0;
	placement.rcNormalPosition.left = geo.GetLeft() + 40;
	placement.rcNormalPosition.top = geo.GetTop() + 40;
	placement.rcNormalPosition.right = geo.GetRight() - 40;
	placement.rcNormalPosition.bottom = geo.GetBottom() - 40;
	::SetWindowPlacement(hwnd, &placement);
	if (wasShown)
		Show(true);
#else
	Maximize(false);
	SetPosition(geo.GetTopLeft());
	SetSize(geo.GetSize());
	Maximize(true);
#endif
	Layout();
	SendSizeEvent();
}

void PadViewFrame::PlaceAsNormalWindow()
{
	m_preservePlacement = true;

	int primaryId = 0;
	const unsigned count = wxDisplay::GetCount();
	for (unsigned i = 0; i < count; ++i)
	{
		if (wxDisplay(i).IsPrimary())
		{
			primaryId = static_cast<int>(i);
			break;
		}
	}
	const wxRect work = wxDisplay(primaryId).GetClientArea();

	if (IsFullScreen())
		ShowFullScreen(false);

#if BOOST_OS_WINDOWS
	HWND hwnd = GetHWND();
	if (hwnd && (IsMaximized() || IsIconized()))
		::ShowWindow(hwnd, SW_RESTORE);
#else
	if (IsMaximized())
		Maximize(false);
#endif

	SetClientSize(854, 480);
	const wxSize frame = GetSize();
	SetPosition(wxPoint(
		work.GetLeft() + (work.GetWidth() - frame.GetWidth()) / 2,
		work.GetTop() + (work.GetHeight() - frame.GetHeight()) / 2));
	Show(true);
	Raise();
}

bool PadViewFrame::Initialize()
{
	const wxSize client_size = GetClientSize();
	g_window_info.pad_width = client_size.GetWidth();
	g_window_info.pad_height = client_size.GetHeight();
	g_window_info.phys_pad_width = ToPhys(client_size.GetWidth());
	g_window_info.phys_pad_height = ToPhys(client_size.GetHeight());

	return true;
}

void PadViewFrame::InitializeRenderCanvas()
{
	auto sizer = new wxBoxSizer(wxVERTICAL);
	{
		#ifdef ENABLE_VULKAN
		if (ActiveSettings::GetGraphicsAPI() == kVulkan)
			m_render_canvas = new VulkanCanvas(this, wxSize(854, 480), false);
		#endif
		#ifdef ENABLE_OPENGL
		if (ActiveSettings::GetGraphicsAPI() == kOpenGL)
			m_render_canvas = GLCanvas_Create(this, wxSize(854, 480), false);
		#endif
		#ifdef ENABLE_METAL
		if (ActiveSettings::GetGraphicsAPI() == kMetal)
			m_render_canvas = new MetalCanvas(this, wxSize(854, 480), false);
		#endif
		sizer->Add(m_render_canvas, 1, wxEXPAND, 0, nullptr);
	}
	cemu_assert(m_render_canvas != nullptr);
	SetSizer(sizer);
	Layout();

	m_render_canvas->Bind(wxEVT_KEY_UP, &PadViewFrame::OnKeyUp, this);
	m_render_canvas->Bind(wxEVT_CHAR, &PadViewFrame::OnChar, this);

	m_render_canvas->Bind(wxEVT_MOTION, &PadViewFrame::OnMouseMove, this);
	m_render_canvas->Bind(wxEVT_LEFT_DOWN, &PadViewFrame::OnMouseLeft, this);
	m_render_canvas->Bind(wxEVT_LEFT_UP, &PadViewFrame::OnMouseLeft, this);
	m_render_canvas->Bind(wxEVT_RIGHT_DOWN, &PadViewFrame::OnMouseRight, this);
	m_render_canvas->Bind(wxEVT_RIGHT_UP, &PadViewFrame::OnMouseRight, this);

	m_render_canvas->Bind(wxEVT_GESTURE_PAN, &PadViewFrame::OnGesturePan, this);

	m_render_canvas->SetFocus();
	SendSizeEvent();
}

void PadViewFrame::DestroyCanvas()
{
	if(!m_render_canvas)
		return;
	m_render_canvas->Destroy();
	m_render_canvas = nullptr;
}

void PadViewFrame::OnSizeEvent(wxSizeEvent& event)
{
	if (!m_preservePlacement && !IsMaximized() && !IsFullScreen())
	{
		g_window_info.restored_pad_width = GetSize().x;
		g_window_info.restored_pad_height = GetSize().y;
	}
	g_window_info.pad_maximized = IsMaximized() && !IsFullScreen();

	const wxSize client_size = GetClientSize();
	g_window_info.pad_width = client_size.GetWidth();
	g_window_info.pad_height = client_size.GetHeight();
	g_window_info.phys_pad_width = ToPhys(client_size.GetWidth());
	g_window_info.phys_pad_height = ToPhys(client_size.GetHeight());
	g_window_info.pad_dpi_scale = GetDPIScaleFactor();

	event.Skip();
}

void PadViewFrame::OnDPIChangedEvent(wxDPIChangedEvent& event)
{
	event.Skip();
	const wxSize client_size = GetClientSize();
	g_window_info.pad_width = client_size.GetWidth();
	g_window_info.pad_height = client_size.GetHeight();
	g_window_info.phys_pad_width = ToPhys(client_size.GetWidth());
	g_window_info.phys_pad_height = ToPhys(client_size.GetHeight());
	g_window_info.pad_dpi_scale = GetDPIScaleFactor();
}

void PadViewFrame::OnMoveEvent(wxMoveEvent& event)
{
	if (!m_preservePlacement && !IsMaximized() && !IsFullScreen())
	{
		g_window_info.restored_pad_x = GetPosition().x;
		g_window_info.restored_pad_y = GetPosition().y;
	}
}

void PadViewFrame::OnKeyUp(wxKeyEvent& event)
{
	event.Skip();

	if (swkbd_hasKeyboardInputHook())
		return;

	const auto code = event.GetKeyCode();
	if (code == WXK_ESCAPE)
		ShowFullScreen(false);
	else if (code == WXK_RETURN && event.AltDown() || code == WXK_F11)
		ShowFullScreen(!IsFullScreen());
}

void PadViewFrame::OnGesturePan(wxPanGestureEvent& event)
{
	auto& instance = InputManager::instance();

	std::scoped_lock lock(instance.m_pad_touch.m_mutex);
	auto physPos = ToPhys(event.GetPosition());
	instance.m_pad_touch.position = { physPos.x, physPos.y };
	instance.m_pad_touch.left_down = event.IsGestureStart() || !event.IsGestureEnd();
	if (event.IsGestureStart() || !event.IsGestureEnd())
		instance.m_pad_touch.left_down_toggle = true;
}

void PadViewFrame::OnChar(wxKeyEvent& event)
{
	if (swkbd_hasKeyboardInputHook())
		swkbd_keyInput(event.GetUnicodeKey());

	event.Skip();
}

void PadViewFrame::OnMouseMove(wxMouseEvent& event)
{
	auto& instance = InputManager::instance();

	std::scoped_lock lock(instance.m_pad_touch.m_mutex);
	auto physPos = ToPhys(event.GetPosition());
	instance.m_pad_mouse.position = { physPos.x, physPos.y };

	event.Skip();
}

void PadViewFrame::OnMouseLeft(wxMouseEvent& event)
{
	auto& instance = InputManager::instance();

	std::scoped_lock lock(instance.m_pad_mouse.m_mutex);
	instance.m_pad_mouse.left_down = event.ButtonDown(wxMOUSE_BTN_LEFT);
	auto physPos = ToPhys(event.GetPosition());
	instance.m_pad_mouse.position = { physPos.x, physPos.y };
	if (event.ButtonDown(wxMOUSE_BTN_LEFT))
		instance.m_pad_mouse.left_down_toggle = true;

}

void PadViewFrame::OnMouseRight(wxMouseEvent& event)
{
	auto& instance = InputManager::instance();

	std::scoped_lock lock(instance.m_pad_mouse.m_mutex);
	instance.m_pad_mouse.right_down = event.ButtonDown(wxMOUSE_BTN_LEFT);
	auto physPos = ToPhys(event.GetPosition());
	instance.m_pad_mouse.position = { physPos.x, physPos.y };
	if (event.ButtonDown(wxMOUSE_BTN_RIGHT))
		instance.m_pad_mouse.right_down_toggle = true;
}

void PadViewFrame::OnSetWindowTitle(wxCommandEvent& event)
{
	this->SetTitle(event.GetString());
}

void PadViewFrame::AsyncSetTitle(std::string_view windowTitle)
{
	wxCommandEvent set_title_event(wxEVT_SET_WINDOW_TITLE);
	set_title_event.SetString(wxString::FromUTF8(windowTitle));
	QueueEvent(set_title_event.Clone());
}
