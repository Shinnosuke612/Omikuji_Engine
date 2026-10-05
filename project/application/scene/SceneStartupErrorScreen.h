// 役割: EditorやSceneを生成せず、Scene起動失敗の詳細を専用ウィンドウへ表示する。
#pragma once

#include <Windows.h>

#include <string>

enum class SceneStartupErrorKind {
	CatalogLoad,
	CatalogValidation,
	StartScene,
	EditorStartup,
	RuntimeStartup,
	RenderResource
};

class SceneStartupErrorScreen final {
public:
	~SceneStartupErrorScreen();

	bool Show(
		HWND ownerWindow,
		SceneStartupErrorKind errorKind,
		const std::string& detail,
		const std::string& catalogFilePath
	);
	bool IsCloseRequested() const { return closeRequested_; }

private:
	static LRESULT CALLBACK WindowProc(
		HWND window,
		UINT message,
		WPARAM wParam,
		LPARAM lParam
	);
	void Paint(HWND window);
	void RequestClose();
	RECT GetCloseButtonRect(HWND window) const;

	HWND ownerWindow_ = nullptr;
	HWND window_ = nullptr;
	HFONT titleFont_ = nullptr;
	HFONT headingFont_ = nullptr;
	HFONT bodyFont_ = nullptr;
	std::wstring heading_;
	std::wstring guidance_;
	std::wstring detail_;
	std::wstring catalogFilePath_;
	bool closeRequested_ = false;
};
