// 役割: Scene起動エラー用Win32ウィンドウの表示と終了操作を実装する。
#include "SceneStartupErrorScreen.h"

#include "../../engine/utility/StringUtility.h"

#include <windowsx.h>

namespace {
	constexpr wchar_t kWindowClassName[] = L"CG3SceneStartupErrorScreen";

	void UseTextFont(HDC deviceContext, HFONT font) {
		SelectObject(
			deviceContext,
			font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))
		);
	}
}

bool SceneStartupErrorScreen::Show(
	HWND ownerWindow,
	SceneStartupErrorKind errorKind,
	const std::string& detail,
	const std::string& catalogFilePath
) {
	ownerWindow_ = ownerWindow;
	closeRequested_ = false;
	detail_ = StringUtility::ConvertString(detail);
	catalogFilePath_ = StringUtility::ConvertString(catalogFilePath);

	switch (errorKind) {
	case SceneStartupErrorKind::CatalogLoad:
		heading_ = L"Scene Catalog could not be loaded";
		guidance_ =
			L"Check scenes.json syntax, version, duplicate Scene IDs, asset paths, and startScene.";
		break;
	case SceneStartupErrorKind::CatalogValidation:
		heading_ = L"Scene Catalog validation failed";
		guidance_ =
			L"Fix the reported Scene file, Entity ID, hierarchy, or transition target before startup.";
		break;
	case SceneStartupErrorKind::StartScene:
		heading_ = L"Start Scene is not available";
		guidance_ =
			L"Register the Scene in the Catalog and set startScene to its Scene ID.";
		break;
	case SceneStartupErrorKind::EditorStartup:
		heading_ = L"Editor Scene startup failed";
		guidance_ =
			L"Check the start Scene asset path and whether the Scene file can be created or read.";
		break;
	case SceneStartupErrorKind::RuntimeStartup:
		heading_ = L"Runtime Scene startup failed";
		guidance_ =
			L"Check the Release start Scene asset and its runtimeProfile before rebuilding.";
		break;
	case SceneStartupErrorKind::RenderResource:
		heading_ = L"Required render resource could not be created";
		guidance_ =
			L"Close other descriptor-heavy features, restart the application, and try again.";
		break;
	}

	const HINSTANCE instance = GetModuleHandleW(nullptr);
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(WNDCLASSEXW);
	windowClass.style = CS_HREDRAW | CS_VREDRAW;
	windowClass.lpfnWndProc = WindowProc;
	windowClass.hInstance = instance;
	windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	windowClass.hIcon = LoadIconW(nullptr, IDI_ERROR);
	windowClass.hIconSm = windowClass.hIcon;
	windowClass.lpszClassName = kWindowClassName;
	if (!RegisterClassExW(&windowClass) &&
		GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		MessageBoxW(
			ownerWindow_,
			detail_.c_str(),
			L"Scene Startup Error",
			MB_OK | MB_ICONERROR
		);
		closeRequested_ = true;
		return false;
	}

	titleFont_ = CreateFontW(
		-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"
	);
	headingFont_ = CreateFontW(
		-30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"
	);
	bodyFont_ = CreateFontW(
		-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"
	);

	constexpr int windowWidth = 760;
	constexpr int windowHeight = 480;
	RECT ownerRect{};
	GetWindowRect(ownerWindow_, &ownerRect);
	const int ownerWidth = ownerRect.right - ownerRect.left;
	const int ownerHeight = ownerRect.bottom - ownerRect.top;
	const int windowX = ownerRect.left + (ownerWidth - windowWidth) / 2;
	const int windowY = ownerRect.top + (ownerHeight - windowHeight) / 2;

	window_ = CreateWindowExW(
		WS_EX_DLGMODALFRAME,
		kWindowClassName,
		L"Scene Startup Error",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
		windowX,
		windowY,
		windowWidth,
		windowHeight,
		ownerWindow_,
		nullptr,
		instance,
		this
	);
	if (!window_) {
		MessageBoxW(
			ownerWindow_,
			detail_.c_str(),
			L"Scene Startup Error",
			MB_OK | MB_ICONERROR
		);
		closeRequested_ = true;
		return false;
	}

	EnableWindow(ownerWindow_, FALSE);
	ShowWindow(window_, SW_SHOW);
	SetForegroundWindow(window_);
	SetFocus(window_);
	return true;
}

LRESULT CALLBACK SceneStartupErrorScreen::WindowProc(
	HWND window,
	UINT message,
	WPARAM wParam,
	LPARAM lParam
) {
	SceneStartupErrorScreen* screen = reinterpret_cast<SceneStartupErrorScreen*>(
		GetWindowLongPtrW(window, GWLP_USERDATA)
	);
	if (message == WM_NCCREATE) {
		const CREATESTRUCTW* createInfo =
			reinterpret_cast<const CREATESTRUCTW*>(lParam);
		screen = static_cast<SceneStartupErrorScreen*>(createInfo->lpCreateParams);
		SetWindowLongPtrW(
			window,
			GWLP_USERDATA,
			reinterpret_cast<LONG_PTR>(screen)
		);
		if (screen) {
			screen->window_ = window;
		}
	}

	switch (message) {
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
		if (screen) {
			screen->Paint(window);
			return 0;
		}
		break;
	case WM_KEYDOWN:
		if (screen && (wParam == VK_RETURN || wParam == VK_ESCAPE)) {
			screen->RequestClose();
			return 0;
		}
		break;
	case WM_LBUTTONUP:
		if (screen) {
			POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			RECT closeButton = screen->GetCloseButtonRect(window);
			if (PtInRect(&closeButton, point)) {
				screen->RequestClose();
			}
			return 0;
		}
		break;
	case WM_CLOSE:
		if (screen) {
			screen->RequestClose();
			return 0;
		}
		break;
	case WM_NCDESTROY:
		if (screen) {
			screen->window_ = nullptr;
		}
		SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		break;
	}

	return DefWindowProcW(window, message, wParam, lParam);
}

void SceneStartupErrorScreen::Paint(HWND window) {
	PAINTSTRUCT paint{};
	HDC deviceContext = BeginPaint(window, &paint);
	RECT clientRect{};
	GetClientRect(window, &clientRect);

	HBRUSH background = CreateSolidBrush(RGB(24, 27, 32));
	FillRect(deviceContext, &clientRect, background);
	DeleteObject(background);
	SetBkMode(deviceContext, TRANSPARENT);

	RECT accentRect{ 0, 0, 10, clientRect.bottom };
	HBRUSH accent = CreateSolidBrush(RGB(216, 73, 82));
	FillRect(deviceContext, &accentRect, accent);
	DeleteObject(accent);

	SetTextColor(deviceContext, RGB(238, 112, 119));
	UseTextFont(deviceContext, titleFont_);
	RECT titleRect{ 48, 38, clientRect.right - 48, 72 };
	DrawTextW(
		deviceContext,
		L"SCENE STARTUP ERROR",
		-1,
		&titleRect,
		DT_LEFT | DT_SINGLELINE | DT_VCENTER
	);

	SetTextColor(deviceContext, RGB(242, 244, 247));
	UseTextFont(deviceContext, headingFont_);
	RECT headingRect{ 48, 82, clientRect.right - 48, 132 };
	DrawTextW(
		deviceContext,
		heading_.c_str(),
		-1,
		&headingRect,
		DT_LEFT | DT_WORDBREAK
	);

	UseTextFont(deviceContext, bodyFont_);
	SetTextColor(deviceContext, RGB(205, 210, 218));
	RECT detailLabelRect{ 48, 152, clientRect.right - 48, 178 };
	DrawTextW(
		deviceContext,
		L"Details",
		-1,
		&detailLabelRect,
		DT_LEFT | DT_SINGLELINE
	);
	SetTextColor(deviceContext, RGB(238, 238, 240));
	RECT detailRect{ 48, 182, clientRect.right - 48, 252 };
	DrawTextW(
		deviceContext,
		detail_.c_str(),
		-1,
		&detailRect,
		DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL
	);

	SetTextColor(deviceContext, RGB(155, 164, 176));
	RECT pathLabelRect{ 48, 270, clientRect.right - 48, 294 };
	DrawTextW(
		deviceContext,
		L"Scene Catalog",
		-1,
		&pathLabelRect,
		DT_LEFT | DT_SINGLELINE
	);
	RECT pathRect{ 48, 296, clientRect.right - 48, 334 };
	DrawTextW(
		deviceContext,
		catalogFilePath_.c_str(),
		-1,
		&pathRect,
		DT_LEFT | DT_WORDBREAK | DT_PATH_ELLIPSIS
	);

	SetTextColor(deviceContext, RGB(184, 191, 201));
	RECT guidanceRect{ 48, 344, clientRect.right - 190, clientRect.bottom - 34 };
	DrawTextW(
		deviceContext,
		guidance_.c_str(),
		-1,
		&guidanceRect,
		DT_LEFT | DT_WORDBREAK
	);

	const RECT closeButton = GetCloseButtonRect(window);
	HBRUSH buttonBrush = CreateSolidBrush(RGB(185, 59, 68));
	FillRect(deviceContext, &closeButton, buttonBrush);
	DeleteObject(buttonBrush);
	SetTextColor(deviceContext, RGB(255, 255, 255));
	RECT buttonTextRect = closeButton;
	DrawTextW(
		deviceContext,
		L"Close",
		-1,
		&buttonTextRect,
		DT_CENTER | DT_SINGLELINE | DT_VCENTER
	);

	EndPaint(window, &paint);
}

RECT SceneStartupErrorScreen::GetCloseButtonRect(HWND window) const {
	RECT clientRect{};
	GetClientRect(window, &clientRect);
	return RECT{
		clientRect.right - 156,
		clientRect.bottom - 72,
		clientRect.right - 48,
		clientRect.bottom - 32
	};
}

void SceneStartupErrorScreen::RequestClose() {
	closeRequested_ = true;
	if (window_) {
		DestroyWindow(window_);
	}
}

SceneStartupErrorScreen::~SceneStartupErrorScreen() {
	if (window_) {
		DestroyWindow(window_);
		window_ = nullptr;
	}
	if (ownerWindow_) {
		EnableWindow(ownerWindow_, TRUE);
	}
	DeleteObject(titleFont_);
	titleFont_ = nullptr;
	DeleteObject(headingFont_);
	headingFont_ = nullptr;
	DeleteObject(bodyFont_);
	bodyFont_ = nullptr;
}
