// 役割: 被弾・命中・死亡確定と、死亡後の世界時計/演出状態を独立した型として定義する。
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../../SceneRuntimeObjectBinding.h"
#include "../../../../../engine/math/Vector3.h"
#include "../../../../../engine/scene/BaseScene.h"
#include "../../../../../engine/scene/SceneRunUpgradeRuntime.h"

class SceneDocument;
class SceneStatSystem;

// Healthの受付状態だけを所有する。HPはStatSystem、死亡確定は後続DeathResponseの責務。
// RuntimeSceneが1Frameに一度Advanceし、Combat/Eventは同じインスタンスへ同期要求する。
class SceneHealthSystem {
public:
	// 非active/無効な参照の状態を破棄し、blocked以外の無敵時計だけを進める。Pause/Hold中は呼ばない。
	void Advance(const SceneDocument& document, float gameplayDeltaTime,
		const std::unordered_set<uint64_t>& blockedEntityIds);
	// HP参照を持つHealthを検索。曖昧/無効な設定もlegacyへ落とさず、ApplyDamageで拒否する。
	uint64_t FindReceiver(const SceneDocument& document, uint64_t statsEntityId, const std::string& statId) const;
	// 正の有限量かつHPが実際に減少した場合だけ成功。失敗はHP/通知/無敵を変更しない。
	bool ApplyDamage(const SceneDocument& document, SceneStatSystem& stats, uint64_t receiverId,
		float amount, const std::unordered_set<uint64_t>& blockedEntityIds);
	// 0は未被弾。Eventは評価間のrevision差を一回の通知へ集約する。
	uint64_t DamageRevision(uint64_t receiverId) const;
	// Pool復元では所有Entity/参照先の両方を破棄。Scene終了では全状態とserialを破棄する。
	void ResetEntity(uint64_t entityId);
	void Clear();
private:
	struct Runtime {
		uint64_t statsEntityId = 0;
		std::string statId;
		float remaining = 0.0f;
		uint64_t revision = 0;
	};
	std::unordered_map<uint64_t, Runtime> runtimes_;
	uint64_t nextRevision_ = 0;
};

// 論理的な死亡とは独立した世界の時計。RuntimeSceneがScene寿命内で所有する。
enum class SceneDeathWorldPhase { Normal, LiveSlow, Frozen };

struct SceneDeathWorldStep {
	float worldDelta = 0.0f;
	float elapsedRealDelta = 0.0f;
	// 最終stepにも正のdeltaがある。全consumerが適用してから凍結を扱う。
	bool freezeAfterStep = false;
};

class SceneDeathWorldClock {
public:
	// 非0の識別子、有限の0<scale<=1/duration>0のみ受理。
	// Normalから一度だけ開始し、拒否時は状態も残時間も変更しない。
	bool Begin(uint64_t instance, uint64_t generation, uint64_t owner, uint64_t serial,
		float scale, float duration);
	// 実時間秒を一Frame一回渡す。不正deltaは無変更でzero step。
	// 終端ではPhaseがFrozenでも返した最終stepを消費し、次Frameへ持ち越さない。
	SceneDeathWorldStep Advance(float realDelta);
	void Freeze();
	// Scene終了/内容差替えで呼ぶ。死亡フラグやゲーム処理には触れない。
	void Clear();
	SceneDeathWorldPhase Phase() const { return phase_; }
private:
	SceneDeathWorldPhase phase_ = SceneDeathWorldPhase::Normal;
	uint64_t instance_ = 0, generation_ = 0, owner_ = 0, serial_ = 0;
	float scale_ = 0.0f;
	float remaining_ = 0.0f;
};

// 死亡通知はScene/内容世代を持つ値型。Scene終了とPool復元で破棄する。
struct SceneDeathStarted {
	uint64_t instance = 0, generation = 0, owner = 0, stats = 0, serial = 0;
	bool wholeScene = false;
};
// 一死亡試行の演出状態だけを所有。Clock/HP/Eventを操作せずRuntimeが順序を保証する。
class SceneDeathReplaySystem {
public:
	// Clearまで最初の試行だけ受理。同一試行の失敗だけがLiveSlow/Frozenを更新できる。
	void AcceptStart(const SceneDeathStarted& pulse, float duration);
	void FailStart(const SceneDeathStarted& pulse, const std::string& diagnostic);
	// LiveSlowの残秒だけはWorld Clockから反映。記録後のReplay時計とは分離する。
	void AdvancePresentation(const SceneDeathWorldStep& step);
	// 全world consumerが最後のstepを適用してから呼ぶ。リプレイ完了通知ではない。
	void FinishLiveSlow();
    // Sealed履歴と準備済み描画資源だけを受理。1点clipは各cutを実秒1秒保持する。
    bool BeginReplay(double recordedSeconds, uint64_t sampleCount, float speed);
    // Gameの通常Updateから一度。非表示/停止中はallowed=falseで時計もcutも進めない。
    void AdvanceReplay(float realDelta, bool allowed);
    // Game描画のtoken発行と最終GPU利用完了。別cut/古いpose/重複receiptを拒否する。
    SceneReplayPresentationToken PresentationToken(uint64_t drawSerial) const;
    void AcceptPresentationReceipt(const SceneReplayPresentationToken& receipt);
    bool IsReplaying() const;
    double ReplaySampleSeconds() const;
    bool ConsumeReplayCompleted(); // 自然終了だけ一度。Event保存条件には接続しない。
	void Clear();
	bool TryGetStatus(uint64_t owner, SceneDeathReplayStatus& out) const;
private:
	SceneDeathStarted started_{};
	SceneDeathReplayStatus status_{};
    float replaySpeed_ = 1.0f;
    uint64_t poseSerial_ = 0, acceptedDrawSerial_ = 0;
    bool endpointPresented_ = false, completionPending_ = false;
};

// Health/Combatと同じ翻訳単位に置く独立System。RuntimeSceneが所有し描画を所有しない。
class SceneDeathResponseSystem {
public:
	// Stat同期/Pool復元後だけ登録。Observeは各Hit/Action境界で呼び停止を即時反映する。
	void Sync(const SceneDocument& document, SceneStatSystem& stats, uint64_t instance);
	void Observe(const SceneDocument& document, const SceneStatSystem& stats, bool refreshHierarchy = false);
	bool IsBlocked(uint64_t id) const { return defeated_ || blocked_.contains(id); }
	bool IsDefeated() const { return defeated_; }
	const std::unordered_set<uint64_t>& Blocked() const { return blocked_; }
	// Scene敗北で膨らまない、対象のみ死亡の枝。World継続の除外に使う。
	const std::unordered_set<uint64_t>& TargetBlocked() const { return targetBlocked_; }
	std::vector<SceneDeathStarted> ConsumeStarted();
	// 0=未死亡、1=演出なし、2=開始待ち、3=受理、4=失敗。完了ではない。
	int Status(uint64_t id) const;
	void SetStatus(uint64_t id, int status);
	void ResetEntity(uint64_t id);
	void Clear();
private:
	struct Binding { uint64_t stats = 0, observed = 0; std::string hp, flag; bool whole = false; int status = 0; };
	std::unordered_map<uint64_t, Binding> bindings_;
	std::unordered_set<uint64_t> blocked_, targetBlocked_;
	std::vector<SceneDeathStarted> started_;
	uint64_t instance_ = 0, generation_ = 0, serial_ = 0, observedStatRevision_ = 0;
	bool defeated_ = false;
};

struct SceneCombatHitEvent {
	uint64_t attackerEntityId = 0;
	uint64_t targetEntityId = 0;
	float damage = 0.0f;
	float poiseDamage = 0.0f;
	float knockback = 0.0f;
	float verticalKnockback = 0.0f;
	Vector3 knockbackDirection{};
	float hitStopDuration = 0.0f;
	Vector3 hitPosition{};
	Vector3 hitNormal{};
	std::string healthStatId = "hp";
	std::string reactionTag;
	uint64_t attackExecutionId = 0;
	uint32_t reactionPriority = 0xffffffffu;
	// 条件判定用の受付事実。既存damageは要求値のまま維持する。
	uint64_t acceptedHitSerial = 0, contentGeneration = 0, hitBoxId = 0, activationId = 0;
	uint64_t windowSerial = 0, attackerGeneration = 0;
	float actualHpDamage = 0.0f, candidateTime = 1.0f;
	std::string poiseStatId;
	float poiseBefore = 0.0f, poiseAfter = 0.0f, poiseMinimum = 0.0f;
	bool poiseValid = false, controlledAttack = false;

};

// Physicsの同Frame接触から生成する非保存要求。通常の重なり候補と同じ受付を通す。
struct SceneEnemyContactRequest {
	uint64_t contentGeneration = 0, ownerId = 0, serial = 0, hitBoxId = 0, hurtBoxId = 0;
	float fraction = 1.0f;
	Vector3 position{}, normal{};
};
enum class SceneHitReactionPolicy { Default, Suppress, Interrupt };
struct SceneHitReactionDecision {
	uint64_t acceptedHitSerial = 0;
	SceneHitReactionPolicy policy = SceneHitReactionPolicy::Default;
	bool managePoiseRecovery = false;
};

class SceneCombatSystem {
public:
	// bindings外のowner/Stats参照もblockedならDamage/命中通知を生成しない。
	// observerは受付後に同期呼出しし、保持しない。Entity/Component配列変更・再帰Combat・HP変更は禁止。
	// swept候補は時刻順、従来候補は時刻1の安定順。引数省略時は従来の受付経路を使用する。
	// upgradesは呼出中だけ借用し、対象ownerのDamage/水平Knockback/判定寸法に一回適用する。
	void Update(
		SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		SceneStatSystem& statSystem,
		SceneHealthSystem& healthSystem,
		SceneDeathResponseSystem& deathSystem,
		float deltaTime,
		const std::unordered_set<uint64_t>& blockedEntityIds = {},
		const std::vector<SceneEnemyContactRequest>& contacts = {},
		const std::unordered_set<uint64_t>& controlledHitBoxes = {},
		const std::function<void(const SceneCombatHitEvent&)>& onAcceptedHit = {},
		const SceneRunUpgradeModifiers& upgrades = {}
	);
	// Poolで復元したEntityの受付/有効化世代を破棄する。旧攻撃を新しい出現へ持ち越さない。
	void ResetEntity(uint64_t entityId);
	void FlushRemovals(SceneDocument& document);
	std::vector<SceneCombatHitEvent> ConsumeHitEvents();
	void Clear();

private:
	struct TargetCooldownContact {
		uint64_t hitBoxWindowKey = 0;
		float nextHitTime = 0.0f;
	};
	std::unordered_map<uint64_t, uint64_t> consumedActivationContacts_;
	std::unordered_map<uint64_t, TargetCooldownContact> targetCooldownContacts_;
	std::unordered_set<uint64_t> activeHitBoxWindows_;
	std::unordered_set<uint64_t> pendingRemovals_;
	std::vector<SceneCombatHitEvent> hitEvents_;
	float elapsedTime_ = 0.0f;
	uint64_t acceptedSerial_ = 0, activationSerial_ = 0, contentGeneration_ = 0;
	std::map<std::pair<uint64_t, uint64_t>, uint64_t> activations_;
	std::unordered_map<uint64_t, uint64_t> controlledConsumed_;
	std::unordered_map<uint64_t, uint64_t> spawnGenerations_;
	// 不正な拡大形状のログを同じHitBoxで毎Frame繰り返さない。
	std::unordered_set<uint64_t> invalidRangeHitBoxes_;

};
