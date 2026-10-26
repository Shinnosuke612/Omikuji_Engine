// 役割: Gameplay条件の適用と、UI入力・通知からの値型操作要求の生成を担う。
#pragma once

#include "../../../../engine/scene/SceneMenuRuntime.h"
#include "../../../../engine/scene/SceneDocument.h"
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../text/SceneTextMotionSystem.h"
#include "../animation/ScenePrefabAnimationSystem.h"

class SceneDocument;
class SceneStatSystem;
class SceneStateMachineSystem;

enum class ScenePostProcessRequestType {
	None,
	SetProfile,
	NextProfile,
	ResetToSceneDefault
};

struct ScenePostProcessRequest {
	ScenePostProcessRequestType type = ScenePostProcessRequestType::None;
	uint64_t managerEntityId = 0;
	std::string managerEntityName;
	std::string profileId;
};

enum class SceneCameraRequestType {
	PlayPath,
	StopPath,
	SelectCamera
};

struct SceneCameraRequest {
	SceneCameraRequestType type = SceneCameraRequestType::PlayPath;
	uint64_t entityId = 0;
	std::string entityName;
};

enum class SceneAudioRequestType {
	Play,
	Stop,
	Pause,
	Resume
};

struct SceneAudioRequest {
	SceneAudioRequestType type = SceneAudioRequestType::Play;
	uint64_t entityId = 0;
};

enum class SceneTextMotionRequestType {
	Play,
	Stop,
	Reset
};

struct SceneTextMotionRequest {
	SceneTextMotionRequestType type = SceneTextMotionRequestType::Play;
	uint64_t entityId = 0;
	std::string clipId;
};

enum class ScenePresentationRequestType { Play, Stop, Reset };

// Event順を保持し、RuntimeSceneが次FrameのObject同期前に再解決して適用する。
struct ScenePresentationRequest {
	ScenePresentationRequestType type = ScenePresentationRequestType::Play;
	uint64_t entityId = 0;
	std::string clipName;
};

struct SceneEventRuntimeSignals {
	uint64_t completedCameraPathEntityId = 0;
	std::vector<uint64_t> finishedAudioEntityIds;
	std::vector<SceneTextMotionCompletion> textMotionCompletions;
	std::vector<ScenePresentationCompletion> presentationCompletions;
};

// UI評価中にDocumentを変更しない。適用側は出所とownerの生存を再確認する。
struct SceneUiActionRequest {
	SceneMenuSource source;
	uint64_t ownerEntityId = 0;
	SceneEventAction action;
	// 標準操作だけはEventTrigger不要。適用側はPanel所有と4操作を再検証する。
	enum class Origin { EventBinding, PanelStandard };
	Origin origin = Origin::EventBinding;
};

struct SceneEventResult {
	std::string sceneTransitionId;
	std::string transitionAssetPath;
	ScenePostProcessRequest postProcessRequest;
	std::vector<SceneCameraRequest> cameraRequests;
	std::vector<SceneAudioRequest> audioRequests;
	std::vector<SceneTextMotionRequest> textMotionRequests;
	std::vector<ScenePresentationRequest> presentationRequests;
	// Scene破棄は行わず、callerが現在Instanceの再読込を予約する。
	bool restartSceneRequested = false;
	std::vector<SceneUiActionRequest> menuRequests;
};

class SceneEventSystem {
public:
	// blocked ownerの状態/時計は保持し、外部Actionからの対象操作は取消以外を不適用にする。
	SceneEventResult Update(
		SceneDocument& document,
		SceneStatSystem& statSystem,
		SceneStateMachineSystem& stateMachineSystem,
		float deltaTime,
		const SceneEventRuntimeSignals& signals,
		const std::unordered_set<uint64_t>& blockedEntityIds = {},
		bool acceptGameplayInput = true
	);
	// Pool再利用時だけGameplayのonce/cooldownを破棄する。UI/Menu入力は変更しない。
	void ResetEntity(uint64_t entityId);
	void Clear();
	// 既存caller向けの移動結果adapter。完全なUI操作はEvaluateUiInputを使う。
	SceneMenuMove EvaluateMenuInput(const SceneDocument& document, const SceneMenuFrame& frame, float deltaTime, bool allowed);
	// 入力/通知を別段階で評価。戻り値は記載順、通知からの再帰実行はcallerが次Frameへ送る。
	std::vector<SceneUiActionRequest> EvaluateUiInput(const SceneDocument& document, const SceneMenuFrame& frame, float deltaTime, bool allowed);
	std::vector<SceneUiActionRequest> EvaluateUiNotifications(const SceneDocument& document, const std::vector<SceneMenuPulse>& pulses, float deltaTime);
	void RearmMenuKeys() { menuKeyArmed_.fill(false); }
	void ClearMenuInput();

private:
	struct BindingRuntime {
		float cooldown = 0.0f;
		bool initialized = false;
		bool wasConditionTrue = false;
		bool fired = false;
	};
	BindingRuntime& UiBindingState(uint64_t ownerId, size_t index, const SceneComponent& events);

	std::unordered_map<uint64_t, std::vector<BindingRuntime>> runtimes_;
	std::unordered_map<uint64_t, std::vector<BindingRuntime>> menuRuntimes_;
	std::unordered_map<uint64_t, std::vector<std::string>> menuBindingSignatures_;
	std::array<bool, 256> menuKeyArmed_{};
	uint64_t menuGeneration_ = 0, menuInstance_ = 0;
	std::string menuInputSignature_;
};
