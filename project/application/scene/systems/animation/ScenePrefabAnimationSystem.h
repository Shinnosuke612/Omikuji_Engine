// 役割: PrefabAnimatorのGameplay再生/Cross Fadeと、演出用のPose退避/自然終了を別状態で所有する。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../../../engine/math/Transform.h"
#include "../../../../engine/scene/SceneDocument.h"

class SceneDocument;

// 自然終了だけを通知する値型。Entity/ClipへのpointerをFrame越しに保持しない。
struct ScenePresentationCompletion {
	uint64_t entityId = 0;
	std::string clipName;
	uint64_t generation = 0;
};

class ScenePrefabAnimationSystem {
public:
	// owner/解決targetがblockedならsampleと時計を保留し、Runtimeを保持する。
	void Update(SceneDocument& document, float deltaTime, const std::unordered_set<uint64_t>& blockedEntityIds = {});
	bool Play(
		const SceneDocument& document,
		uint64_t entityId,
		const std::string& clipName,
		bool restart = true,
		float transitionDuration = 0.0f
	);
	void ResetEntity(uint64_t entityId);
	// 入口の既存ClipをID解決して保持。新規Play/Active trackを開始しない。
	void BeginDeathContinuation(const SceneDocument& document, const std::unordered_set<uint64_t>& participants,
		const std::unordered_set<uint64_t>& attackTargets);
	void UpdateDeathContinuation(SceneDocument& document, float worldDelta,
		const std::unordered_set<uint64_t>& blockedEntityIds);
	void ClearDeathContinuation();
	// 保持中の退避姿勢も書込み所有に含め、World継続開始前に競合を調べる。
	// 保存ClipのTransform書込先と演出の退避先を調べ、攻撃Poseの同時所有を拒否する。
	bool HasTransformTargetsIn(const SceneDocument& document, const std::unordered_set<uint64_t>& ids) const;
	bool HasPresentationTargetsIn(const std::unordered_set<uint64_t>& ids) const;
	// 演出入口は非LoopのTransform Clip専用。失敗時はPose/activeを変更しない。
	// 成功時はownerをactive化し、親座標系の時刻0 Poseを直ちに適用する。
	bool PlayPresentation(SceneDocument& document, uint64_t entityId,
		const std::string& clipName, const std::unordered_set<uint64_t>& blockedEntityIds = {});
	// Stopは現在Poseを保持し、Resetは初回Play前の所有propertyだけを復元する。
	// 両操作はactiveを変えず、完了通知を破棄する。
	void StopPresentation(uint64_t entityId);
	// blocked targetへの復元は拒否しfalseを返す。
	bool ResetPresentation(SceneDocument& document, uint64_t entityId, const std::unordered_set<uint64_t>& blockedEntityIds = {});
	// Object同期前に通常deltaTime（秒）で一回呼ぶ。開始Frameは時刻0を保持する。
	// pausedEntityIdsは死亡による参加停止。deathOwnersから開始した独立演出だけ迂回する。
	void UpdatePresentations(SceneDocument& document, float deltaTime, const std::unordered_set<uint64_t>& blockedEntityIds = {},
		const std::unordered_set<uint64_t>& pausedEntityIds = {}, const std::unordered_set<uint64_t>& deathOwners = {});
	// Pool baseline復元後だけ使う。古いPose/activeを復元せず、対象を持つ演出と通知を破棄する。
	void DiscardPresentationsForEntities(const std::unordered_set<uint64_t>& entityIds);
	// 未消費pulseを取り出して空にする。Event gateが開くまでUpdateをまたいで保持する。
	std::vector<ScenePresentationCompletion> ConsumePresentationCompletions();
	// 同じ内容世代のdocumentだけPose/activeを復元する。差替え済みなら無適用破棄。
	void Clear(SceneDocument* document = nullptr);

private:
	struct TransitionPose {
		uint64_t entityId = 0;
		QuaternionTransform transform{};
		uint8_t propertyMask = 0;
	};

	struct AnimationRuntime {
		size_t clipIndex = 0;
		float time = 0.0f;
		float transitionTime = 0.0f;
		float transitionDuration = 0.0f;
		bool initialized = false;
		bool playing = false;
		std::vector<TransitionPose> transitionPoses;
	};
	struct PresentationRuntime {
		std::string clipName;
		float time = 0.0f;
		uint64_t generation = 0;
		bool playing = false;
		bool justStarted = false;
		std::vector<TransitionPose> clipTargets;
		std::vector<TransitionPose> originalPoses;
	};
	bool HasGameplayOwnership(const SceneDocument& document,
		const std::vector<TransitionPose>& targets, uint64_t activatingOwnerId = 0) const;
	void RemovePresentationCompletions(uint64_t entityId);
	bool BindPresentationDocument(const SceneDocument& document);
	void DiscardPresentations();
	void RestorePresentation(SceneDocument& document,
		const PresentationRuntime& runtime);

	std::unordered_map<uint64_t, AnimationRuntime> runtimes_;
	std::unordered_map<uint64_t, ScenePrefabAnimationClip> deathClips_;
	std::unordered_set<uint64_t> deathParticipants_;
	// Reset後もPlay前activeの退避はScene終了まで残し、Stop/Resetのactiveを変えない。
	std::unordered_map<uint64_t, bool> presentationOriginalActive_;
	std::unordered_map<uint64_t, PresentationRuntime> presentationRuntimes_;
	std::vector<ScenePresentationCompletion> presentationCompletions_;
	uint64_t presentationGeneration_ = 0;
	uint64_t presentationDocumentGeneration_ = 0;
};
