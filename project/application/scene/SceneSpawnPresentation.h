// 役割: 敵出現の表示倍率と参加制限を、Scene内の各Systemへ値で渡す。
#pragma once
#include <cstdint>
#include <unordered_set>
#include <vector>

struct SceneSpawnVisual {
	uint64_t rootEntityId = 0;
	uint64_t activationSerial = 0;
	float scale = 1.0f;
};

// RuntimeSceneが所有するFrame snapshot。pointerを保持せず、内容世代の一致時だけ使う。
struct SceneSpawnFrame {
	uint64_t documentGeneration = 0;
	std::vector<SceneSpawnVisual> visuals;
	std::unordered_set<uint64_t> blockedEntityIds;
};

struct SceneSpawnUpdateContext {
	float gameplayDeltaTime = 0.0f;
	float presentationDeltaTime = 0.0f;
	bool gameplayAllowed = true;
	std::vector<uint64_t> preparationSpawnerIds;
};
