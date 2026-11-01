// 役割: EnemyBehaviorの索敵・個体値・攻撃段階・姿勢・中断集計を所有し、移動/接触の解決はPhysicsへ委譲する。
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <set>
#include <tuple>
#include <unordered_set>
#include "../combat/SceneCombatSystem.h"
#include "../../physics/ScenePhysicsSystem.h"

#include "../../../SceneRuntimeObjectBinding.h"
#include "../../../../../engine/scene/SceneDocument.h"

class SceneDocument;
class SceneHitReactionSystem;
class ScenePrefabAnimationSystem;
class SceneStateMachineSystem;
class SceneStatSystem;

// 同Frameだけ有効な値要求。ObjectやDocumentのアドレスは保持しない。
struct SceneEnemyPositioningRequest {
	uint64_t entityId = 0, targetId = 0;
	SceneEnemyPositioningSettings settings;
	SceneEnemyPositioningStatus status;
	Vector3 previousVelocity{};
	float orbitHold = 0.0f;
	bool canMove = false;
	uint64_t probeFrameToken = 0; // Agentが別Frameのsnapshotと混ぜないための照合値。
};
struct SceneEnemyPositioningResult {
	uint64_t entityId = 0;
	SceneEnemyPositioningStatus status;
	Vector3 velocity{};
	float orbitHold = 0.0f;
};

class SceneEnemySystem {
public:
	void Update(
		SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		SceneStatSystem& statSystem,
		ScenePrefabAnimationSystem& prefabAnimationSystem,
		const SceneHitReactionSystem& hitReactionSystem,
		const SceneStateMachineSystem& stateMachineSystem,
		float deltaTime,
		uint64_t runtimePlayerEntityId = 0 // 非所有の実Player対応ID。0は未出現。
	);
	// GroundXZ Agentの離隔補正後に、Hit／Dead中の移動停止を再適用する。
	void ApplyMovementStops(
		const std::vector<SceneRuntimeObjectBinding>& bindings
	) const;
	void ResetEntity(uint64_t entityId);
	void Clear();
	// Updateから次のUpdateまでの要求。Agentの結果は同Frameで取り込み、保存値には反映しない。
	const std::vector<SceneEnemyPositioningRequest>& PositioningRequests() const { return positioningRequests_; }
	void AcceptPositioningResults(const std::vector<SceneEnemyPositioningResult>& results);
	uint64_t PositioningFrameToken() const { return positioningFrameToken_; }
	// Enemy.Updateの向き更新/Object同期後に呼ぶ。時計を進めず、同Frameの位置取り目的を準備する。
	void PrepareAttackApproaches(const SceneDocument& document, const SceneMovementProbeSnapshot& snapshot, float deltaTime);
	bool TryGetPositioningStatus(uint64_t entityId, SceneEnemyPositioningStatus& out) const;
	// Input/State/Reaction後、Physics前に一回。Frame内の攻撃時計をここだけで進める。
	void PrepareChargeFrame(SceneDocument& document, const std::vector<SceneRuntimeObjectBinding>& bindings,
		SceneStatSystem& stats, const ScenePrefabAnimationSystem& animation, const SceneHitReactionSystem& reaction,
		const SceneStateMachineSystem& states, float deltaTime, bool continuation = false,
		const SceneMovementProbeSnapshot* snapshot = nullptr); // 通常の新規開始は最新snapshot必須。死後継続は不要。
	const std::vector<SceneControlledMotion>& ChargeMotions() const { return chargeMotions_; }
	const std::unordered_set<uint64_t>& ChargeHitBoxes() const { return chargeHitBoxes_; }
	// Physics結果を時計へ戻す。Combat配送が済むまで命中区間のHitBoxを閉じない。
	void AcceptChargePhysics(SceneDocument& document, const std::vector<SceneControlledMotionResult>& results);
	// Combat observer専用。HPを変更せず、段階/集計と当該HitBoxの有効状態だけを変更する。
	SceneHitReactionDecision EvaluateAcceptedHit(SceneDocument& document, const SceneCombatHitEvent& hit);
	// Combat後、Reaction/死亡演出の前に自発速度を解放し、死亡した枝の姿勢を返却する。
	// 戻り値はその場でtransformを再同期する対象。配列の追加/削除は行わない。
	std::vector<uint64_t> FinishChargeCombat(SceneDocument& document, const SceneStatSystem& stats,
		const std::vector<SceneRuntimeObjectBinding>& bindings);
	// Combat/Reaction後に判定を閉じ、Runtimeの見た目だけを変更する。返値はrefresh対象。
	std::vector<uint64_t> ApplyChargePoses(SceneDocument& document);



private:
	enum class AttackPhase {
		None,
		Windup,
		Active,
		Recovery,
		Dead
	};

	struct ChargeRuntime {
		SceneEnemyChargePhase phase = SceneEnemyChargePhase::Idle;
		SceneEnemyChargeEnd end = SceneEnemyChargeEnd::None;
		uint64_t lastAcceptedHitSerial = 0;
		uint64_t serial = 0, targetId = 0, poseId = 0, hitBoxId = 0, contentGeneration = 0;
		bool configurationValid = false, firstWait = true;
		std::unordered_set<uint64_t> poseBranch;
		bool initialized = false, ownsPose = false, restorePose = false, ready = false, continuing = false;
		bool interruptedThisFrame = false, hitState = false, reacting = false;
		float aim = 0, locked = 0, cooldownDuration = 0, cooldown = 0;
		float elapsed = 0, travelled = 0, frameDelta = 0, cursor = 0, pendingSeconds = 0;
		// Physicsで無衝突が確定したとき、生成済みの時間端点へ進める。距離epsilonで終わらせない。
		float pendingElapsed = 0;
		bool pendingReachesEnd = false;
		float reactionRemaining = 0, idleSinceHit = 0;
		Quaternion rotation{0, 0, 0, 1}, returnRotation{0, 0, 0, 1};
		Vector3 direction{0,0,1}, offset{}, returnOffset{};
		QuaternionTransform base;
		float damage = 0;
		uint32_t hitCount = 0;
		uint8_t achieved = 0;
		int ruleScope = -2;
		// 接触時刻で耐性の段階を選ぶ。物理確定後の最終段階だけで判定しない。
		struct PhaseInterval { SceneEnemyChargePhase phase; float end; };
		std::vector<PhaseInterval> framePhases;
		float ledgerTime = 0;
		size_t ledgerInterval = 0;
		using CountKey = std::tuple<uint64_t, uint64_t, bool, uint64_t, uint64_t, uint64_t, uint64_t>;
		std::set<CountKey> counted;
	};
	struct EnemyRuntime {
		AttackPhase phase = AttackPhase::None;
		float phaseTimer = 0.0f;
		float cooldown = 0.0f;
		bool hasTarget = false;
		bool hitBoxSuppressedByReaction = false;
		bool movementSuppressed = false;
		bool initialized = false;
		bool positioning = false, individualInitialized = false;
		SceneEnemyPositioningStatus individual;
		Vector3 positioningVelocity{};
		float orbitHold = 0.0f;
		uint64_t individualSeed = 0;
		// Agentの個体値コピーと独立して保持し、Inspectorへ取得時に合成する。
		SceneEnemyTargetResolution target;
		std::string targetName, configurationIssue;
		bool targetInRange = false, targetRestricted = false;
		uint64_t contentGeneration = 0;
		ChargeRuntime charge;
		// 表示用の最終照会。開始許可には再利用せず、PrepareChargeFrameで再照会する。
		SceneMovementProbeResult approach;
		bool approachValid = false, canStartAttack = false;
		float startDistanceLimit = 0.0f;
		std::string blockerName;
	};

	void RefreshAttackApproach(const SceneDocument& document, uint64_t ownerId, EnemyRuntime& runtime,
		const SceneMovementProbeSnapshot* snapshot);
	void AdvanceChargeClock(SceneDocument& document, uint64_t ownerId, EnemyRuntime& runtime, float seconds);
	void EndCharge(ChargeRuntime& charge, SceneEnemyChargeEnd reason, bool restore);
	void SelectInterruptRule(const SceneEnemyInterruptSettings& settings, ChargeRuntime& charge, SceneEnemyChargePhase phase);
	void AdvanceInterruptLedger(const SceneEnemyInterruptSettings& settings, ChargeRuntime& charge, float time);
	std::vector<SceneControlledMotion> chargeMotions_;
	std::unordered_set<uint64_t> chargeHitBoxes_;
	uint64_t nextChargeSerial_ = 1;
	std::unordered_map<uint64_t, EnemyRuntime> runtimes_;
	std::unordered_map<uint64_t, uint64_t> generations_;
	std::vector<SceneEnemyPositioningRequest> positioningRequests_;
	uint64_t playSeed_ = 0, positioningFrameToken_ = 0;
};
