// 役割: Scene Event用に保存されたKey名をDirectInput key codeへ正規化する。
#pragma once

#include <dinput.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <vector>

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
	{ "SPACE", DIK_SPACE },
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
	// 既存Sceneの短縮名を読み取り互換として受け付ける。保存値や候補一覧は変えない。
	if (key == "ESC") key = "ESCAPE";
	for (const SceneInputKeyDefinition& definition :
		kSceneInputKeyDefinitions) {
		if (key == definition.name) {
			return definition.code;
		}
	}
	return 0;
}

// 有効コードだけを保存順で一度ずつ返す。未知名の診断は呼出し側が元の文字列で行う。
// Document型を参照せず、旧単一キーも空の追加配列で同じ規則を使う。
inline std::vector<BYTE> ResolveSceneInputKeys(const std::string& first,
	const std::vector<std::string>& additional) {
	std::vector<BYTE> result;
	const auto append = [&](const std::string& name) {
		const BYTE code = ResolveSceneInputKey(name);
		if (code && std::find(result.begin(), result.end(), code) == result.end()) result.push_back(code);
	};
	append(first);
	for (const auto& name : additional) append(name);
	return result;
}

// Documentに依存せず、軸に対応するPrevious/Next/Confirm/Cancelを同じ規則で扱う。
inline std::array<size_t, 4> MenuActiveKeyGroups(const std::string& axis) {
	return {axis == "Horizontal" ? 2u : 0u, axis == "Horizontal" ? 3u : 1u, 4u, 5u};
}

template<class Groups>
inline std::vector<BYTE> ResolveMenuStandardKeys(const Groups& groups, const std::string& axis) {
	std::vector<std::string> names;
	for (size_t index : MenuActiveKeyGroups(axis)) names.insert(names.end(), groups[index]->begin(), groups[index]->end());
	return ResolveSceneInputKeys("", names);
}

// 重複は保存互換の警告として別扱い。構造不正と使用中操作間の衝突だけを拒否する。
template<class Groups>
inline const char* ValidateMenuStandardKeys(const Groups& groups, const std::string& axis) {
	for (const auto* group : groups) {
		if (group->empty()) return "MenuRuntime:StandardKeysEmpty";
		for (const auto& name : *group) if (!ResolveSceneInputKey(name)) return "MenuRuntime:StandardKeysInvalid";
	}
	std::vector<BYTE> used;
	for (size_t index : MenuActiveKeyGroups(axis)) {
		const auto keys = ResolveSceneInputKeys("", *groups[index]);
		for (BYTE key : keys) {
			if (std::find(used.begin(), used.end(), key) != used.end()) return "MenuRuntime:StandardKeysConflict";
			used.push_back(key);
		}
	}
	return "";
}
