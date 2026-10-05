// 役割: AttackSetの実行時刻、HitBox Payload、攻撃移動をEntity単位で所有する。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../../../../engine/math/Vector3.h"
#include "../../../../../engine/scene/SceneDocument.h"

class ScenePrefabAnimationSystem;
class Player;
struct SceneRuntimeObjectBinding;

struct SceneAttackEffectRequest {
	uint64_t ownerEntityId = 0;
	uint64_t spawnEntityId = 0;
	std::string particleEffectPath;
	Vector3 localOffset{};
	std::string groundPrefabPath;
	float groundProbeDistance = 0.0f;
	float groundPrefabLifetime = 0.0f;
	std::string groundEffectType;
	float groundCrackRadius = 0.0f;
	uint32_t groundCrackPrimaryBranchCount = 0;
	uint32_t groundCrackSegmentsPerBranch = 0;
	float groundCrackBranchProbability = 0.0f;
	float groundCrackWidth = 0.0f;
	float groundCrackLifetime = 0.0f;
	float groundCrackSurfaceOffset = 0.0f;
};

class SceneAttackRunnerSystem {
public:
	bool Start(
		SceneDocument& document,
		uint64_t ownerEntityId,
		uint64_t attackSetEntityId,
		const std::string& attackName
	);
	void Advance(
		SceneDocument& document,
		ScenePrefabAnimationSystem& prefabAnimationSystem,
		float deltaTime
	);
	void ApplyMotion(
		SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		Player* player,
		const Vector3& playerInputDirection,
		float deltaTime
	);
	bool IsRunning(uint64_t ownerEntityId) const;
	bool IsFinished(uint64_t ownerEntityId) const;
	float GetTime(uint64_t ownerEntityId) const;
	float GetDuration(uint64_t ownerEntityId) const;
	void SetLoopRequested(uint64_t ownerEntityId, bool requested);
	std::vector<SceneAttackEffectRequest> ConsumeEffectRequests();
	void Stop(SceneDocument& document, uint64_t ownerEntityId);
	void ResetEntity(SceneDocument& document, uint64_t entityId);
	void Clear(SceneDocument* document = nullptr);

private:
	struct HitBoxSnapshot {
		uint64_t entityId = 0;
		bool active = false;
		float damage = 0.0f;
		float poiseDamage = 0.0f;
		float knockback = 0.0f;
		float verticalKnockback = 0.0f;
		float hitStopDuration = 0.0f;
		std::string reactionTag;
		std::string knockbackDirectionMode;
		Vector3 knockbackLocalDirection{};
		std::string hitPolicy;
		float targetCooldown = 0.0f;
		bool hasColliderHalfSizeSnapshot = false;
		Vector3 colliderHalfSize{};
	};

	struct ActiveWindow {
		size_t windowIndex = 0;
		uint64_t entityId = 0;
		bool usesLegacyPayload = false;
		HitBoxSnapshot legacySnapshot{};
	};

	struct Runtime {
		uint64_t attackSetEntityId = 0;
		uint64_t animationTargetEntityId = 0;
		uint64_t facingTargetEntityId = 0;
		SceneAttackDefinition definition{};
		float time = 0.0f;
		float previousTime = 0.0f;
		float appliedMotionProgress = 0.0f;
		uint64_t attackExecutionId = 0;
		bool started = false;
		bool animationStarted = false;
		bool axesCaptured = false;
		bool finished = false;
		bool loopRequested = false;
		int loopCount = 0;
		float elapsedTime = 0.0f;
		Vector3 forward = { 0.0f, 0.0f, 1.0f };
		Vector3 right = { 1.0f, 0.0f, 0.0f };
		Vector3 startForward = { 0.0f, 0.0f, 1.0f };
		std::vector<ActiveWindow> activeWindows;
	};

	static float GetDuration(const SceneAttackDefinition& definition);
	static SceneEntity* ResolveEntity(
		SceneDocument& document,
		uint64_t entityId,
		const std::string& entityName
	);
	static void ResolvePlanarAxes(
		const SceneRuntimeObjectBinding* binding,
		Vector3& forward,
		Vector3& right
	);
	void DeactivateWindow(SceneDocument& document, ActiveWindow& activeWindow);
	void DeactivateActiveWindows(SceneDocument& document, Runtime& runtime);
	void UpdateHitWindows(SceneDocument& document, Runtime& runtime);

	std::unordered_map<uint64_t, Runtime> runtimes_;
	std::vector<SceneAttackEffectRequest> effectRequests_;
	uint64_t nextAttackExecutionId_ = 1;
};
