// 役割: Agentの群れ移動と、Enemyから受けた地上位置取り要求の速度計算を行う。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../../SceneRuntimeObjectBinding.h"
#include "SceneEnemySystem.h"
#include "../../../../../engine/math/Vector3.h"

class SceneDocument;

// TeamRuntimeを群れの共通基準、AgentRuntimeを個体差として保持する。
// SceneDocumentとObjectは所有せず、bindingsを通して同じフレームのTransformを更新する。
class SceneAgentSystem {
public:
	// 位置取り要求がある個体は従来Agent処理から除く。結果のBody速度反映はXZだけ。
	// requestsは同Frameの借用値。追加要求なしの呼出しは従来経路を維持する。
	void Update(
		SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		float deltaTime,
		const std::vector<SceneEnemyPositioningRequest>& positioningRequests = {},
		const SceneMovementProbeSnapshot* snapshot = nullptr // 同Frameだけ借用。仮候補は値形状へ適用。
	);
	// 同Frameの結果だけを返す。要求元Enemyが個体状態を所有する。
	const std::vector<SceneEnemyPositioningResult>& PositioningResults() const { return positioningResults_; }
	void Clear();

private:
	struct AgentRuntime {
		Vector3 velocity{};
		Vector3 rotation{};
		Vector3 wanderDirection = { 0.0f, 0.0f, 1.0f };
		Vector3 jitterOffset{};
		Vector3 jitterTargetLocal{};
		Vector3 cachedSchoolingSteering{};
		Vector3 cachedSeparationSteering{};
		float phase = 0.0f;
		float wanderTimer = 0.0f;
		float jitterTimer = 0.0f;
		float schoolingTimer = 0.0f;
		float separationTimer = 0.0f;
		uint64_t flockSeedId = 0;
		uint64_t wanderSeedId = 0;
		uint32_t wanderStep = 0;
		uint32_t jitterStep = 0;
		bool initialized = false;
		bool flockInitialized = false;
		bool schoolingCacheValid = false;
		bool separationCacheValid = false;
	};

	struct TeamRuntime {
		Vector3 center{};
		Vector3 velocity{};
		Vector3 heading = { 0.0f, 0.0f, 1.0f };
		Vector3 rotation{};
		Vector3 wanderDirection = { 0.0f, 0.0f, 1.0f };
		Vector3 desiredDirection = { 0.0f, 0.0f, 1.0f };
		std::string forwardAxis = "+Z";
		float phase = 0.0f;
		float wanderTimer = 0.0f;
		float decisionTimer = 0.0f;
		float desiredSpeed = 0.0f;
		uint64_t seedId = 0;
		uint32_t wanderStep = 0;
		bool initialized = false;
		bool decisionValid = false;
	};

	std::unordered_map<uint64_t, AgentRuntime> agentRuntimes_;
	std::unordered_map<std::string, TeamRuntime> teamRuntimes_;
	std::vector<SceneEnemyPositioningResult> positioningResults_;
};
