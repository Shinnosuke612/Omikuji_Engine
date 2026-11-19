// 役割: AttackSetの実行時刻、HitBox Payload、攻撃移動をEntity単位で所有する。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../../../../engine/math/Vector3.h"
#include "../../../../../engine/scene/SceneDocument.h"
#include "../../../../../engine/scene/SceneRunUpgradeRuntime.h"

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
	// 後隙補正は開始時にコピーする。以後の倍率変更/プレイ終了で実行中の時計を再計算しない。
	bool Start(
		SceneDocument& document,
		uint64_t ownerEntityId,
		uint64_t attackSetEntityId,
		const std::string& attackName,
		const SceneRunUpgradeModifiers& upgrades = {}
	);
	// 出現中ownerの時刻/HitWindow/演出開始を保留する。状態は破棄しない。
	void Advance(
		SceneDocument& document,
		ScenePrefabAnimationSystem& prefabAnimationSystem,
		float deltaTime,
		const std::unordered_set<uint64_t>& blockedEntityIds = {}
	);
	void ApplyMotion(
		SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		Player* player,
		const Vector3& playerInputDirection,
		float deltaTime
	);
	// 既存実行だけを継続。新規Start/HitWindow/Loopは停止し、方向は入口で固定する。
	void BeginDeathContinuation(SceneDocument& document, const std::vector<SceneRuntimeObjectBinding>& bindings,
		const std::unordered_set<uint64_t>& participants);
	void AdvanceDeathContinuation(SceneDocument& document, float worldDelta);
	// 終了ownerのXZ解除は呼出側がPhysics後に行う。Yには触れない。
	std::vector<uint64_t> ApplyDeathContinuationMotion(SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings, float worldDelta);
	const std::unordered_set<uint64_t>& DeathAnimationTargets() const { return deathAnimationTargets_; }
	std::vector<SceneAttackEffectRequest> ConsumeDeathEffectRequests();
	void ClearDeathContinuation();
	bool IsRunning(uint64_t ownerEntityId) const;
	// 同じ世代/owner/AttackSet/定義の通常実行だけを読取照合。終了/死亡継続は含めない。
	bool IsRunning(uint64_t ownerEntityId, uint64_t attackSetEntityId,
		const std::string& attackName, uint64_t documentGeneration) const;
	bool IsFinished(uint64_t ownerEntityId) const;
	float GetTime(uint64_t ownerEntityId) const;
	float GetDuration(uint64_t ownerEntityId) const;
	// 開始時snapshotで明示指定のコンボ時刻だけを変換する。未実行なら入力値を返す。
	float MapRecoveryTime(uint64_t ownerEntityId, float authoredTime) const;
	// legacy Stateも同じHit/Effect保護終端を使う。保存値は変更しない。
	static SceneAttackRecoveryTiming RecoveryTiming(const SceneAttackDefinition& definition, float multiplier);
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

	// この実行が借りたColliderだけを、同じDocument世代/localIdへ復元する。
	struct GeometrySnapshot {
		bool valid = false;
		uint64_t generation = 0, colliderId = 0, executionId = 0;
		std::string shape;
		Vector3 offset{}, halfSize{};
		float radius = 0;
	};
	struct ActiveWindow {
		GeometrySnapshot geometry;
		size_t windowIndex = 0;
		uint64_t entityId = 0;
		bool usesLegacyPayload = false;
		HitBoxSnapshot legacySnapshot{};
	};

	struct Runtime {
		uint64_t documentGeneration = 0;
		std::unordered_set<uint64_t> invalidGeometryTargets;
		uint64_t attackSetEntityId = 0;
		uint64_t animationTargetEntityId = 0;
		uint64_t facingTargetEntityId = 0;
		SceneAttackDefinition definition{};
		SceneAttackRecoveryTiming recoveryTiming{};
		float time = 0.0f;
		float previousTime = 0.0f;
		float appliedMotionProgress = 0.0f;
		uint64_t attackExecutionId = 0;
		bool started = false;
		bool animationStarted = false;
		bool axesCaptured = false;
		bool finished = false;
		bool deathContinuing = false;
		bool deathEndPending = false;
		bool loopRequested = false;
		int loopCount = 0;
		float elapsedTime = 0.0f;
		Vector3 forward = { 0.0f, 0.0f, 1.0f };
		Vector3 right = { 1.0f, 0.0f, 0.0f };
		Vector3 startForward = { 0.0f, 0.0f, 1.0f };
		std::vector<ActiveWindow> activeWindows;
	};

	static float GetDuration(const SceneAttackDefinition& definition);
	static float MotionProgress(const Runtime& runtime);
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
	// キーなしだけの既存攻撃では全Runtimeの排他走査を増やさない。
	size_t keyedActiveCount_ = 0;
	std::vector<SceneAttackEffectRequest> effectRequests_;
	uint64_t nextAttackExecutionId_ = 1;
	bool deathContinuation_ = false;
	std::unordered_set<uint64_t> deathAnimationTargets_;
	std::vector<SceneAttackEffectRequest> deathEffectRequests_;
};
