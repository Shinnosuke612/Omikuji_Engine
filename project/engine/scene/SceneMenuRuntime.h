// 役割: Menuの操作要求・通知・表示snapshotをengine/application間で共有する値型を定義する。
#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <string>
#include "../math/Vector4.h"

// 同一Instance/内容/Revisionでのみ使用する。Entity pointerやGPU資源は所有しない。
struct SceneMenuFrame {
	uint64_t instanceId = 0;
	uint64_t generation = 0;
	uint64_t revision = 0;
	uint64_t panelId = 0;
	uint64_t selectedItemId = 0;
	std::vector<uint64_t> hiddenMarkerRoots;
	std::unordered_map<uint64_t, Vector4> textColors;
	std::vector<uint64_t> visiblePanelIds;
	uint64_t openSerial = 0;
};

// 保存しない出所。閉鎖後に同じPanelを開き直しても古い通知を適用しない。
struct SceneMenuSource {
	uint64_t instanceId = 0, generation = 0, revision = 0;
	uint64_t panelId = 0, openSerial = 0;
	uint64_t requestSerial = 0; // 遷移結果待ちだけに使い、同じPanelでの再試行を区別する。
};
enum class SceneMenuCommandType { Open, Close, Previous, Next, Confirm, Cancel };
struct SceneMenuCommand {
	SceneMenuSource source;
	SceneMenuCommandType type = SceneMenuCommandType::Open;
	uint64_t panelId = 0;
};
enum class SceneMenuPulseType { ItemConfirmed, Cancelled };
struct SceneMenuPulse {
	SceneMenuSource source;
	SceneMenuPulseType type = SceneMenuPulseType::ItemConfirmed;
	uint64_t itemId = 0;
	bool closeAfterNotification = false;
};
// engineはMenu Componentを解釈せず、入力許可とゲーム内停止の値だけを各Instanceへ配る。
struct SceneUiControlFrame {
	uint64_t instanceId = 0, generation = 0;
	bool blockGameplayInput = false;
	// Menu rootが生存する間だけ有効。Editor Pauseとは独立した非保存の理由。
	bool gamePaused = false;
	bool pauseBgm = false;
};
struct SceneMenuMove {
	uint64_t instanceId = 0;
	uint64_t generation = 0;
	uint64_t revision = 0;
	uint64_t panelId = 0;
	bool previous = false;
	bool next = false;
};
