// 役割: Scene Event用に保存されたKey名をDirectInput key codeへ正規化する。
#pragma once

#include <dinput.h>

#include <algorithm>
#include <cctype>
#include <string>

struct SceneInputKeyDefinition {
	const char* name;
	BYTE code;
};

inline constexpr SceneInputKeyDefinition kSceneInputKeyDefinitions[] = {
	{ "1", DIK_1 }, { "2", DIK_2 }, { "3", DIK_3 }, { "4", DIK_4 },
	{ "5", DIK_5 }, { "6", DIK_6 }, { "7", DIK_7 }, { "8", DIK_8 },
	{ "9", DIK_9 }, { "0", DIK_0 },
	{ "F3", DIK_F3 }, { "F4", DIK_F4 }, { "F5", DIK_F5 },
	{ "F6", DIK_F6 }, { "F7", DIK_F7 }, { "F8", DIK_F8 },
	{ "F9", DIK_F9 }, { "F10", DIK_F10 },
	{ "ENTER", DIK_RETURN },
	{ "ESCAPE", DIK_ESCAPE },
	{ "UP", DIK_UP }, { "DOWN", DIK_DOWN },
	{ "LEFT", DIK_LEFT }, { "RIGHT", DIK_RIGHT },
	{ "A", DIK_A }, { "B", DIK_B }, { "C", DIK_C }, { "D", DIK_D },
	{ "E", DIK_E }, { "F", DIK_F }, { "G", DIK_G }, { "H", DIK_H },
	{ "I", DIK_I }, { "J", DIK_J }, { "K", DIK_K }, { "L", DIK_L },
	{ "M", DIK_M }, { "N", DIK_N }, { "O", DIK_O }, { "P", DIK_P },
	{ "Q", DIK_Q }, { "R", DIK_R }, { "S", DIK_S }, { "T", DIK_T },
	{ "U", DIK_U }, { "V", DIK_V }, { "W", DIK_W }, { "X", DIK_X },
	{ "Y", DIK_Y }, { "Z", DIK_Z }
};

inline BYTE ResolveSceneInputKey(const std::string& keyName) {
	std::string key = keyName;
	std::transform(
		key.begin(), key.end(), key.begin(),
		[](unsigned char character) {
			return static_cast<char>(std::toupper(character));
		}
	);
	for (const SceneInputKeyDefinition& definition :
		kSceneInputKeyDefinitions) {
		if (key == definition.name) {
			return definition.code;
		}
	}
	return 0;
}
