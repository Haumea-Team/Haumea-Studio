// SPDX-License-Identifier: GPL-2.0-or-later
// The editor window's own title bar: the system's is taken away and the
// top row of the UI (logo, menus, title, window buttons) stands in for it.
//
// Windows (the GLFW and the Direct3D 9 build): the frame stays, sized to
// nothing (WM_NCCALCSIZE), and WM_NCHITTEST tells Windows which parts of our
// row are the caption and where the edges are, so moving, resizing, Aero
// Snap and the double-click are Windows' own.
// X11: the window is borderless and moved and resized here, by hand.
// Wayland: borderless too. GLFW has no call to start a move or a resize, so
// the compositor is asked directly (xdg_toplevel.move/resize, below): its
// own move, snapping and resize.
#include "haumea.h"

#ifdef LIBRW_GLFW
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#endif
#ifdef _WIN32
#include <windowsx.h>
#endif
#if defined(LIBRW_GLFW) && defined(__linux__)
#include <dlfcn.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

extern rw::EngineOpenParams engineOpenParams;

enum {
	HIT_CLIENT, HIT_CAPTION,
	HIT_LEFT, HIT_RIGHT, HIT_TOP, HIT_BOTTOM,
	HIT_TOPLEFT, HIT_TOPRIGHT, HIT_BOTTOMLEFT, HIT_BOTTOMRIGHT,
};

// This frame's row, in client pixels: its height and the empty stretch
// between the menus and the window buttons that drags the window.
static struct { bool valid; float height, captionX0, captionX1; } gLayout;

static float
edgeSize(void)
{
	return 6.0f;
}

static int
hitTest(float x, float y, float w, float h, bool maximized)
{
	if(!maximized){
		float e = edgeSize();
		bool l = x < e, r = x >= w - e, t = y < e, b = y >= h - e;
		if(t && l) return HIT_TOPLEFT;
		if(t && r) return HIT_TOPRIGHT;
		if(b && l) return HIT_BOTTOMLEFT;
		if(b && r) return HIT_BOTTOMRIGHT;
		if(l) return HIT_LEFT;
		if(r) return HIT_RIGHT;
		if(t) return HIT_TOP;
		if(b) return HIT_BOTTOM;
	}
	if(gLayout.valid && y >= 0.0f && y < gLayout.height && x >= gLayout.captionX0 && x < gLayout.captionX1)
		return HIT_CAPTION;
	return HIT_CLIENT;
}

#ifdef LIBRW_GLFW
static GLFWwindow*
glfwEditorWindow(void)
{
	return engineOpenParams.window ? *engineOpenParams.window : nil;
}

static bool
glfwWayland(void)
{
#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4)
	return glfwGetPlatform() == GLFW_PLATFORM_WAYLAND;
#else
	return false;
#endif
}
#endif

bool
TitleBarOwned(void)
{
	static int owned = -1;
	if(owned < 0){
		const char *off = getenv("HAUMEA_SYSTEM_TITLEBAR");
		owned = off && off[0] == '1' ? 0 : 1;
#if !defined(LIBRW_GLFW) && !(defined(_WIN32) && defined(RW_D3D9))
		owned = 0;
#endif
	}
	return owned == 1;
}

void
TitleBarSetLayout(float height, float captionX0, float captionX1)
{
	gLayout.valid = true;
	gLayout.height = height;
	gLayout.captionX0 = captionX0;
	gLayout.captionX1 = captionX1;
}

// ---- Windows: the frame and the hit-test ----------------------------------
#ifdef _WIN32
static HWND
win32EditorWindow(void)
{
#if defined(RW_D3D9)
	return (HWND)engineOpenParams.window;
#elif defined(LIBRW_GLFW)
	GLFWwindow *window = glfwEditorWindow();
	return window ? glfwGetWin32Window(window) : nil;
#else
	return nil;
#endif
}

bool
TitleBarWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result)
{
	if(!TitleBarOwned())
		return false;
	switch(message){
	case WM_NCCALCSIZE:
		if(!wparam)
			return false;
		// No frame: the client is the whole window. Maximized, Windows hangs
		// the frame over the screen's edges; keep the client on the screen.
		if(IsZoomed(hwnd)){
			NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS*)lparam;
			int fx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
			int fy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
			p->rgrc[0].left += fx;
			p->rgrc[0].right -= fx;
			p->rgrc[0].top += fy;
			p->rgrc[0].bottom -= fy;
		}
		*result = 0;
		return true;
	case WM_NCHITTEST: {
		POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
		ScreenToClient(hwnd, &pt);
		RECT rc;
		GetClientRect(hwnd, &rc);
		static const LRESULT codes[] = {
			HTCLIENT, HTCAPTION, HTLEFT, HTRIGHT, HTTOP, HTBOTTOM,
			HTTOPLEFT, HTTOPRIGHT, HTBOTTOMLEFT, HTBOTTOMRIGHT,
		};
		*result = codes[hitTest((float)pt.x, (float)pt.y, (float)rc.right, (float)rc.bottom, IsZoomed(hwnd) != 0)];
		return true;
	}
	case WM_NCACTIVATE:
		// Without a frame to repaint; -1 keeps Windows from drawing one.
		*result = DefWindowProcW(hwnd, message, wparam, -1);
		return true;
	}
	return false;
}

#if defined(LIBRW_GLFW)
static WNDPROC gGlfwWindowProc;

static LRESULT CALLBACK
titleBarGlfwProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	LRESULT result;
	if(TitleBarWindowProc(hwnd, message, wparam, lparam, &result))
		return result;
	return CallWindowProcW(gGlfwWindowProc, hwnd, message, wparam, lparam);
}
#endif
#endif	// _WIN32

void
TitleBarInstall(void)
{
	static bool installed;
	if(installed || !TitleBarOwned())
		return;
	installed = true;
#ifdef _WIN32
	HWND hwnd = win32EditorWindow();
	if(hwnd == nil)
		return;
#if defined(LIBRW_GLFW) && !defined(RW_D3D9)
	// GLFW's borderless window is a popup: give it the caption and the
	// sizing frame back (for snapping and resizing), then hide them.
	gGlfwWindowProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)titleBarGlfwProc);
	LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
#endif
	// Windows 11 rounds the window's corners when asked (DWMWA_WINDOW_CORNER_PREFERENCE,
	// DWMWCP_ROUND); older Windows ignores it. dwmapi is looked up, not linked.
	if(HMODULE dwm = LoadLibraryW(L"dwmapi.dll")){
		typedef HRESULT (WINAPI *SetAttribute)(HWND, DWORD, LPCVOID, DWORD);
		if(SetAttribute set = (SetAttribute)GetProcAddress(dwm, "DwmSetWindowAttribute")){
			DWORD round = 2;
			set(hwnd, 33, &round, sizeof(round));
		}
	}
	// The Direct3D 9 window's procedure (imgui_windows.cpp) asks
	// TitleBarWindowProc first; recalculate its frame now.
	SetWindowPos(hwnd, nil, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#elif defined(LIBRW_GLFW)
	if(GLFWwindow *window = glfwEditorWindow())
		glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
#endif
}

bool
TitleBarMaximized(void)
{
#if defined(_WIN32) && defined(RW_D3D9)
	HWND hwnd = win32EditorWindow();
	return hwnd && IsZoomed(hwnd);
#elif defined(LIBRW_GLFW)
	GLFWwindow *window = glfwEditorWindow();
	return window && glfwGetWindowAttrib(window, GLFW_MAXIMIZED);
#else
	return false;
#endif
}

void
TitleBarMinimize(void)
{
#if defined(_WIN32) && defined(RW_D3D9)
	if(HWND hwnd = win32EditorWindow())
		ShowWindow(hwnd, SW_MINIMIZE);
#elif defined(LIBRW_GLFW)
	if(GLFWwindow *window = glfwEditorWindow())
		glfwIconifyWindow(window);
#endif
}

void
TitleBarToggleMaximize(void)
{
#if defined(_WIN32) && defined(RW_D3D9)
	if(HWND hwnd = win32EditorWindow())
		ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
#elif defined(LIBRW_GLFW)
	if(GLFWwindow *window = glfwEditorWindow()){
		if(glfwGetWindowAttrib(window, GLFW_MAXIMIZED))
			glfwRestoreWindow(window);
		else
			glfwMaximizeWindow(window);
	}
#endif
}

// ---- Wayland: asking the compositor ----------------------------------------
// libwayland-client is loaded, not linked (GLFW loads it the same way): the
// few calls below, and our own seat and pointer on a queue of our own, whose
// button events give the serial a move or a resize has to quote.
#if defined(LIBRW_GLFW) && defined(__linux__)
namespace wl {
struct proxy;
struct queue;
struct iface { const char *name; };
struct registryListener {
	void (*global)(void *data, proxy *registry, uint32_t name, const char *interface, uint32_t version);
	void (*globalRemove)(void *data, proxy *registry, uint32_t name);
};
// wl_pointer version 1: enter, leave, motion, button, axis.
struct pointerListener {
	void (*enter)(void*, proxy*, uint32_t, proxy*, int32_t, int32_t);
	void (*leave)(void*, proxy*, uint32_t, proxy*);
	void (*motion)(void*, proxy*, uint32_t, int32_t, int32_t);
	void (*button)(void*, proxy*, uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
	void (*axis)(void*, proxy*, uint32_t, uint32_t, int32_t);
};

static struct {
	bool tried, ready;
	queue *(*createQueue)(void *display);
	proxy *(*createWrapper)(void *proxy);
	void (*setQueue)(proxy *p, queue *q);
	proxy *(*marshalFlags)(proxy *p, uint32_t opcode, const iface *interface, uint32_t version, uint32_t flags, ...);
	int (*addListener)(proxy *p, void (**implementation)(void), void *data);
	uint32_t (*getVersion)(proxy *p);
	int (*roundtripQueue)(void *display, queue *q);
	int (*dispatchQueuePending)(void *display, queue *q);
	const iface *registryInterface, *seatInterface, *pointerInterface;
	void *display;
	queue *events;
	proxy *seat, *pointer;
	uint32_t pressSerial;
	proxy *toplevel;
} w;

static void
onGlobal(void*, proxy *registry, uint32_t name, const char *interface, uint32_t)
{
	if(w.seat || strcmp(interface, "wl_seat") != 0)
		return;
	// wl_registry.bind (opcode 0): name, then the new id as (interface, version).
	w.seat = w.marshalFlags(registry, 0, w.seatInterface, 1, 0, name, w.seatInterface->name, 1u, nil);
}
static void onGlobalRemove(void*, proxy*, uint32_t) {}
static void onEnter(void*, proxy*, uint32_t, proxy*, int32_t, int32_t) {}
static void onLeave(void*, proxy*, uint32_t, proxy*) {}
static void onMotion(void*, proxy*, uint32_t, int32_t, int32_t) {}
static void
onButton(void*, proxy*, uint32_t serial, uint32_t, uint32_t, uint32_t state)
{
	if(state == 1)	// pressed
		w.pressSerial = serial;
}
static void onAxis(void*, proxy*, uint32_t, uint32_t, int32_t) {}
static const registryListener registryEvents = { onGlobal, onGlobalRemove };
static const pointerListener pointerEvents = { onEnter, onLeave, onMotion, onButton, onAxis };

// Whether these bytes can be read, without touching them.
static bool
readable(const void *at, size_t size)
{
	char buf[64];
	struct iovec local = { buf, size }, remote = { (void*)at, size };
	return size <= sizeof(buf) && process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)size;
}

// GLFW keeps its xdg_toplevel to itself. Look through its window for a word
// that is a live proxy of that interface on this display: a wl_proxy starts
// with its interface (whose first field is the name) and has the display
// three words in. Read-checked, so a wrong guess can't crash.
static proxy*
findToplevel(GLFWwindow *window)
{
	const uintptr_t *words = (const uintptr_t*)window;
	for(int i = 0; i < 8192; i++){
		if(!readable(&words[i], sizeof(uintptr_t)))
			break;
		uintptr_t candidate = words[i];
		if(candidate < 0x10000 || (candidate & 7))
			continue;
		const uintptr_t *p = (const uintptr_t*)candidate;
		if(!readable(p, 4*sizeof(uintptr_t)))
			continue;
		const char *const *iface = (const char *const*)p[0];
		if(iface == nil || ((uintptr_t)iface & 7) || !readable(iface, sizeof(char*)))
			continue;
		const char *name = iface[0];
		char text[16] = {};
		if(name == nil || !readable(name, 13))
			continue;
		memcpy(text, name, 13);
		if(strcmp(text, "xdg_toplevel") == 0 && (void*)p[3] == w.display)
			return (proxy*)candidate;
	}
	return nil;
}

static bool
ready(GLFWwindow *window)
{
	if(w.tried)
		return w.ready;
	w.tried = true;
	void *lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
	if(lib == nil)
		return false;
	#define LOAD(field, symbol) *(void**)&w.field = dlsym(lib, symbol); if(w.field == nil) return false;
	LOAD(createQueue, "wl_display_create_queue")
	LOAD(createWrapper, "wl_proxy_create_wrapper")
	LOAD(setQueue, "wl_proxy_set_queue")
	LOAD(marshalFlags, "wl_proxy_marshal_flags")
	LOAD(addListener, "wl_proxy_add_listener")
	LOAD(getVersion, "wl_proxy_get_version")
	LOAD(roundtripQueue, "wl_display_roundtrip_queue")
	LOAD(dispatchQueuePending, "wl_display_dispatch_queue_pending")
	LOAD(registryInterface, "wl_registry_interface")
	LOAD(seatInterface, "wl_seat_interface")
	LOAD(pointerInterface, "wl_pointer_interface")
	#undef LOAD
	// Not in GLFW builds without Wayland: looked up, not linked.
	void *(*getDisplay)(void) = (void*(*)(void))dlsym(RTLD_DEFAULT, "glfwGetWaylandDisplay");
	w.display = getDisplay ? getDisplay() : nil;
	if(w.display == nil)
		return false;
	w.toplevel = findToplevel(window);
	if(w.toplevel == nil){
		log("Title bar: no xdg_toplevel found; the window can't be dragged\n");
		return false;
	}
	// Our own registry and seat, on our own queue, so GLFW's events stay its own.
	w.events = w.createQueue(w.display);
	proxy *display = w.createWrapper((proxy*)w.display);
	w.setQueue(display, w.events);
	// wl_display.get_registry (opcode 1).
	proxy *registry = w.marshalFlags(display, 1, w.registryInterface, w.getVersion(display), 0, nil);
	w.addListener(registry, (void(**)(void))&registryEvents, nil);
	w.roundtripQueue(w.display, w.events);
	if(w.seat == nil)
		return false;
	// wl_seat.get_pointer (opcode 0).
	w.pointer = w.marshalFlags(w.seat, 0, w.pointerInterface, 1, 0, nil);
	w.addListener(w.pointer, (void(**)(void))&pointerEvents, nil);
	w.roundtripQueue(w.display, w.events);
	w.ready = true;
	return true;
}

// xdg_toplevel.move (opcode 5) or .resize (6) with the edges, from the press
// just made.
static bool
begin(GLFWwindow *window, int hit)
{
	if(!ready(window))
		return false;
	w.dispatchQueuePending(w.display, w.events);
	if(w.pressSerial == 0)
		return false;
	uint32_t version = w.getVersion(w.toplevel);
	if(hit == HIT_CAPTION){
		w.marshalFlags(w.toplevel, 5, nil, version, 0, w.seat, w.pressSerial);
		return true;
	}
	uint32_t edges = 0;
	switch(hit){
	case HIT_TOP: edges = 1; break;
	case HIT_BOTTOM: edges = 2; break;
	case HIT_LEFT: edges = 4; break;
	case HIT_TOPLEFT: edges = 5; break;
	case HIT_BOTTOMLEFT: edges = 6; break;
	case HIT_RIGHT: edges = 8; break;
	case HIT_TOPRIGHT: edges = 9; break;
	case HIT_BOTTOMRIGHT: edges = 10; break;
	default: return false;
	}
	w.marshalFlags(w.toplevel, 6, nil, version, 0, w.seat, w.pressSerial, edges);
	return true;
}
}	// namespace wl
#endif

// ---- X11 and Wayland: moving and resizing by hand --------------------------
// Each frame, from the bar. Windows does all of this itself.
void
TitleBarUpdate(void)
{
#if defined(LIBRW_GLFW) && !defined(_WIN32)
	if(!TitleBarOwned() || UiScriptActive())
		return;
	GLFWwindow *window = glfwEditorWindow();
	if(window == nil)
		return;
	ImGuiIO &io = ImGui::GetIO();
	ImVec2 origin = ImGui::GetMainViewport()->Pos;
	float mx = io.MousePos.x - origin.x, my = io.MousePos.y - origin.y;
	bool maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
	bool wayland = glfwWayland();
	int ww, wh;
	glfwGetWindowSize(window, &ww, &wh);

	static int op = HIT_CLIENT;	// the move or resize in progress
	static double startX, startY;	// the pointer, on the screen, when it began
	static int startWX, startWY, startW, startH;
	static float grabFraction, grabY;	// where on the bar a move took hold

	auto pointer = [&](double *x, double *y){
		int wx, wy;
		double cx, cy;
		glfwGetWindowPos(window, &wx, &wy);
		glfwGetCursorPos(window, &cx, &cy);
		*x = wx + cx;
		*y = wy + cy;
	};

	if(op == HIT_CLIENT){
		bool overItem = ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
		int hit = io.MousePos.x == -FLT_MAX ? HIT_CLIENT : hitTest(mx, my, (float)ww, (float)wh, maximized);
		if(hit == HIT_CAPTION && overItem)
			hit = HIT_CLIENT;
		switch(hit){
		case HIT_LEFT: case HIT_RIGHT: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); break;
		case HIT_TOP: case HIT_BOTTOM: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS); break;
		case HIT_TOPLEFT: case HIT_BOTTOMRIGHT: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE); break;
		case HIT_TOPRIGHT: case HIT_BOTTOMLEFT: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW); break;
		}
		if(hit == HIT_CAPTION && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)){
			TitleBarToggleMaximize();
			return;
		}
		// Wayland: the compositor moves or resizes it, holding the pointer
		// until the button comes up, which then never reaches us.
		if(hit != HIT_CLIENT && wayland && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
#ifdef __linux__
			if(wl::begin(window, hit))
				io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
#endif
			return;
		}
		if(hit != HIT_CLIENT && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
			op = hit;
			pointer(&startX, &startY);
			glfwGetWindowPos(window, &startWX, &startWY);
			startW = ww;
			startH = wh;
			grabFraction = ww > 0 ? mx / ww : 0.5f;
			grabY = my;
		}
		return;
	}
	if(!ImGui::IsMouseDown(ImGuiMouseButton_Left)){
		op = HIT_CLIENT;
		return;
	}
	double px, py;
	pointer(&px, &py);
	int dx = (int)(px - startX), dy = (int)(py - startY);
	if(op == HIT_CAPTION){
		if(dx == 0 && dy == 0)
			return;
		// A maximized window comes off the top at its old size, still
		// under the pointer at the same place along the bar.
		if(maximized){
			glfwRestoreWindow(window);
			return;
		}
		glfwSetWindowPos(window, (int)(px - grabFraction*ww), (int)(py - grabY));
		return;
	}
	int x = startWX, y = startWY, w = startW, h = startH;
	const int minW = 640, minH = 400;
	if(op == HIT_LEFT || op == HIT_TOPLEFT || op == HIT_BOTTOMLEFT){
		w = std::max(minW, startW - dx);
		x = startWX + startW - w;
	}
	if(op == HIT_RIGHT || op == HIT_TOPRIGHT || op == HIT_BOTTOMRIGHT)
		w = std::max(minW, startW + dx);
	if(op == HIT_TOP || op == HIT_TOPLEFT || op == HIT_TOPRIGHT){
		h = std::max(minH, startH - dy);
		y = startWY + startH - h;
	}
	if(op == HIT_BOTTOM || op == HIT_BOTTOMLEFT || op == HIT_BOTTOMRIGHT)
		h = std::max(minH, startH + dy);
	if(x != startWX || y != startWY)
		glfwSetWindowPos(window, x, y);
	glfwSetWindowSize(window, w, h);
	switch(op){
	case HIT_LEFT: case HIT_RIGHT: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); break;
	case HIT_TOP: case HIT_BOTTOM: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS); break;
	case HIT_TOPLEFT: case HIT_BOTTOMRIGHT: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE); break;
	case HIT_TOPRIGHT: case HIT_BOTTOMLEFT: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW); break;
	}
#endif
}

// ---- Rounded corners --------------------------------------------------------
// Linux: the window has a transparent framebuffer (main.cpp). Before each
// frame is shown, every pixel is made opaque (translucent things drawn in the
// frame would otherwise let the desktop through), then each corner outside a
// quarter circle is cleared to nothing, a row at a time. Windows 11 rounds the
// window itself (TitleBarInstall). Maximized, the corners stay square.
void
TitleBarRoundCorners(void)
{
#if defined(LIBRW_GLFW) && !defined(_WIN32)
	if(!TitleBarOwned())
		return;
	GLFWwindow *window = glfwEditorWindow();
	if(window == nil || !glfwGetWindowAttrib(window, GLFW_TRANSPARENT_FRAMEBUFFER))
		return;
	int fw, fh, ww, wh;
	glfwGetFramebufferSize(window, &fw, &fh);
	glfwGetWindowSize(window, &ww, &wh);
	if(fw <= 0 || fh <= 0)
		return;
	bool square = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;

	GLboolean mask[4], scissor = glIsEnabled(GL_SCISSOR_TEST);
	GLfloat clear[4];
	GLint box[4];
	glGetBooleanv(GL_COLOR_WRITEMASK, mask);
	glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
	glGetIntegerv(GL_SCISSOR_BOX, box);

	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	if(!square){
		// Nothing (premultiplied: black, alpha 0) outside the rounding.
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glEnable(GL_SCISSOR_TEST);
		float scale = ww > 0 ? (float)fw / ww : 1.0f;
		int r = (int)(10.0f*scale + 0.5f);
		for(int y = 0; y < r; y++){
			float dy = r - y - 0.5f;
			int cut = (int)(r - sqrtf(std::max(0.0f, (float)r*r - dy*dy)) + 0.5f);
			if(cut <= 0)
				continue;
			// GL counts rows from the bottom.
			int top = fh - 1 - y, bottom = y;
			glScissor(0, top, cut, 1); glClear(GL_COLOR_BUFFER_BIT);
			glScissor(fw - cut, top, cut, 1); glClear(GL_COLOR_BUFFER_BIT);
			glScissor(0, bottom, cut, 1); glClear(GL_COLOR_BUFFER_BIT);
			glScissor(fw - cut, bottom, cut, 1); glClear(GL_COLOR_BUFFER_BIT);
		}
	}
	glColorMask(mask[0], mask[1], mask[2], mask[3]);
	glClearColor(clear[0], clear[1], clear[2], clear[3]);
	glScissor(box[0], box[1], box[2], box[3]);
	if(scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
#endif
}
