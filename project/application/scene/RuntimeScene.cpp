// 役割: SceneDocumentと各Runtime Systemを連携し、更新と描画を実行する。
#include "RuntimeScene.h"

#include "../../engine/scene/SceneManager.h"
#include "../../engine/scene/SceneExecutionContext.h"
#include "../../engine/scene/SceneDocument.h"
#include "../../engine/scene/SceneEntityQuery.h"
#include "../../engine/3d/SrvManager.h"
#include "../../engine/base/DirectXCommon.h"
#include "../../engine/utility/Logger.h"

#include "../../engine/3d/Camera.h"
#include "../../engine/3d/Object3dCommon.h"
#include "../../engine/3d/Object3d.h"
#include "../../engine/math/Math.h"
#include "../../engine/particle/ParticleManager.h"
#include "../player/Player.h"
#include <algorithm>
#include <utility>

namespace {
	bool UiEntityActive(const SceneDocument& document, const SceneEntity* entity) {
		for (size_t depth = 0; entity && entity->active && depth <= document.GetEntities().size(); ++depth) {
			if (!entity->parentId) return true;
			entity = document.FindEntity(entity->parentId);
		}
		return false;
	}

	Transform MakeRuntimeTransform(const QuaternionTransform& source) {
		Transform result{};
		result.scale = source.scale;
		result.rotate = MakeEulerFromQuaternion(source.rotate);
		result.translate = source.translate;
		result.useQuaternionRotation = true;
		result.quaternionRotate = source.rotate;
		return result;
	}

	void SynchronizeSceneTransform(
		QuaternionTransform& destination,
		const Transform& source
	) {
		destination.scale = source.scale;
		destination.rotate = source.useQuaternionRotation
			? source.quaternionRotate
			: MakeQuaternionFromEuler(source.rotate);
		destination.translate = source.translate;
	}

	Transform GetSceneTransform(
		const SceneDocument* document,
		const char* name,
		const Transform& fallback
	) {
		const SceneEntity* entity = document
			? document->FindEntityByName(name)
			: nullptr;
		return entity ? MakeRuntimeTransform(entity->transform) : fallback;
	}

	Camera* CreateOrbitCamera() {
		Camera* camera = new Camera();
		camera->SetOrbitMode(true);
		camera->SetOrbitTarget({ 0.0f, 0.0f, 0.0f });
		camera->SetOrbitDistance(10.0f);
		camera->SetOrbitAngle(0.0f, 0.0f);
		camera->Update();
		return camera;
	}
}

void RuntimeScene::ApplyRenderCamera(Camera* viewCamera) {
	objectSystem_.ApplyRenderCamera(viewCamera);
	environmentSystem_.ApplyRenderCamera(viewCamera);
	ParticleManager::GetInstance()->SetCamera(viewCamera);
}

Camera* RuntimeScene::GetSceneViewCamera() const {
	SceneExecutionContext* executionContext = sceneManager_
		? sceneManager_->GetExecutionContext()
		: nullptr;
	return cameraSystem_.SelectSceneViewCamera(
		camera_,
		debugCamera_,
		executionContext && executionContext->IsPaused() &&
			!(sceneManager_ && sceneManager_->IsTransitionHeld())
	);
}

bool RuntimeScene::TryGetRuntimePostProcessSettings(
	ScenePostProcessSettings& settings,
	uint64_t& generation
) const {
	settings = postProcessProfileSystem_.GetEffectiveSettings();
	generation = postProcessProfileSystem_.GetGeneration();
	return true;
}

void RuntimeScene::DrawSceneView(Camera* viewCamera, uint64_t skipEntityId) {
	DrawEnvironment(viewCamera);
	PrepareSceneContent(viewCamera);
	BindLighting();
	DrawPreparedSceneContentForView(viewCamera, skipEntityId);
}

void RuntimeScene::DrawPreparedSceneContentForView(
	Camera* viewCamera,
	uint64_t skipEntityId
) {
	SceneDocument* document = GetSceneDocument();
	if (document) {
		const bool hidePlayerModel =
			ShouldHidePlayerModelForCamera(viewCamera);
		objectSystem_.DrawModels(
			*document,
			skipEntityId,
			hidePlayerModel
		);
	}
	effectRenderSystem_.DrawScenePass(
		document,
		viewCamera,
		skipEntityId,
		environmentSystem_,
		objectSystem_,
		!(sceneManager_ && sceneManager_->IsTransitionHeld())
	);
	if (document) {
		DirectXCommon* dxCommon = Object3dCommon::GetInstance()->GetDxCommon();
		textRenderSystem_.DrawScene2D(
			*document,
			dxCommon->GetClientWidth(),
			dxCommon->GetClientHeight(),
			sceneManager_ && sceneManager_->IsDrawingEditorUIView()
		);
		// UI投影は制作サイズを使い、現在のRenderTarget viewportが画面/Monitorへ拡縮する。
		// 編集packetは明示されたUI編集Viewの描画中だけ使用する。
		const auto* view = sceneManager_ && sceneManager_->IsDrawingEditorUIView() && skipEntityId == 0 &&
			viewCamera == GetSceneViewCamera() ? GetEditorUIView() : nullptr;
		if (view) DrawEditorUI(*view, "Scene2D");
		else uiRenderSystem_.Draw(*document, objectSystem_, textRenderSystem_, "Scene2D", skipEntityId);
	}
}

bool RuntimeScene::ShouldHidePlayerModelForCamera(Camera* viewCamera) const {
	return
		viewCamera == camera_ &&
		cameraSystem_.IsFirstPersonMode();
}

void RuntimeScene::Initialize()
{
	// PlayerはObjectSystemのObjectを借用するため、Objectを最初に構築する。
	cameraSystem_.Reset();

	camera_ = CreateOrbitCamera();
	debugCamera_ = CreateOrbitCamera();

	Object3dCommon::GetInstance()->SetDefaultCamera(camera_);
	particleSystem_.Initialize(camera_);
	SceneDocument* initialDocument = GetSceneDocument();
	postProcessProfileSystem_.Reset(initialDocument);
	debugSystem_.LoadSettings(initialDocument);
	SceneExecutionContext* initialExecutionContext = sceneManager_
		? sceneManager_->GetExecutionContext()
		: nullptr;
	const bool initialEditing =
		initialExecutionContext && initialExecutionContext->IsEditing();
	const bool initialPlaying =
		!initialExecutionContext || initialExecutionContext->IsPlaying();
	objectSystem_.SyncModels(
		initialDocument,
		physicsSystem_,
		0.0f,
		initialPlaying,
		initialEditing
	);

	Vector3 target = GetSceneTransform(
		initialDocument,
		"Human",
		Transform{
			{ 1.0f, 1.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f },
			{ -2.0f, 0.0f, -2.0f }
		}
	).translate;
	camera_->SetOrbitTarget(target);

	player_ = new Player();
	player_->Initialize(
		objectSystem_.FindObjectByName(initialDocument, "Player")
	);
	player_->SetTransform(GetSceneTransform(
		initialDocument,
		"Player",
		Transform{
			{ 1.0f, 1.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, -4.0f }
		}
	));

	environmentSystem_.Initialize(
		Object3dCommon::GetInstance()->GetDxCommon()
	);
	if (initialDocument) {
		objectSystem_.BuildBindings(
			*initialDocument,
			runtimeObjectBindings_
		);
		environmentSystem_.Sync(
			initialDocument,
			runtimeObjectBindings_
		);
	}

	lightingSystem_.Initialize(
		Object3dCommon::GetInstance()->GetDxCommon(),
		SrvManager::GetInstance()
	);
	lightingSystem_.Sync(initialDocument);
	monitorSystem_.Initialize(
		Object3dCommon::GetInstance()->GetDxCommon(),
		SrvManager::GetInstance()
	);

	effectRenderSystem_.Initialize(
		Object3dCommon::GetInstance()->GetDxCommon()
	);
	textRenderSystem_.Initialize(
		Object3dCommon::GetInstance()->GetDxCommon(),
		GetSceneAssetId() + "_" + std::to_string(GetSceneInstanceId())
	);

}

void RuntimeScene::ClearUiRuntime() {
	menuSystem_.Clear(); eventSystem_.ClearMenuInput(); pendingMenuRequests_.clear(); deferredMenuPulses_.clear();
	uiTransitionSource_ = {}; uiLoadReservation_ = kInvalidSceneInstanceId; uiControl_ = {};
	uiTerminalAttempted_ = false;
}
void RuntimeScene::ReportMenuFailure(const std::string& detail) {
	const auto* document = GetSceneDocument();
	preparationDiagnostic_ = {ScenePreparationDiagnosticCode::InvalidReference, document ? document->GetContentGeneration() : 0,
		menuSystem_.GetFrame().panelId, ++preparationDiagnosticSerial_, detail};
	Logger::Log("[MenuRuntime] " + detail + "\n");
}
void RuntimeScene::ApplyUiRequests(const SceneDocument& document, const std::vector<SceneUiActionRequest>& requests) {
	for (const auto& request : requests) {
		if (uiTransitionSource_.requestSerial || uiTerminalAttempted_) break;
		if (!menuSystem_.IsSourceValid(document, request.source)) continue;
		const auto* owner = document.FindEntity(request.ownerEntityId);
		if (!owner) continue;
		if (request.origin == SceneUiActionRequest::Origin::PanelStandard) {
			const auto* panel = SceneEntityQuery::FindEnabledComponent(*owner, "MenuPanel");
			const auto& action = request.action;
			if (!panel || panel->menuInputMode != "Standard" || owner->id != menuSystem_.GetFrame().panelId ||
				request.source.panelId != owner->id || request.source.openSerial != menuSystem_.GetFrame().openSerial ||
				action.targetEntityId != owner->id || (action.type != "SelectPreviousMenuItem" && action.type != "SelectNextMenuItem" &&
					action.type != "ConfirmMenuItem" && action.type != "CancelMenu")) continue;
		} else if (!SceneEntityQuery::FindEnabledComponent(*owner, "EventTrigger")) continue;
		if (!UiEntityActive(document, owner)) continue;
		uiControl_.blockGameplayInput = true;
		const auto& action = request.action;
		const auto before = menuSystem_.GetSource();
		SceneMenuCommandType type{};
		bool menuCommand = true;
		if (action.type == "OpenMenu") type = SceneMenuCommandType::Open;
		else if (action.type == "CloseMenu") type = SceneMenuCommandType::Close;
		else if (action.type == "SelectPreviousMenuItem") type = SceneMenuCommandType::Previous;
		else if (action.type == "SelectNextMenuItem") type = SceneMenuCommandType::Next;
		else if (action.type == "ConfirmMenuItem") type = SceneMenuCommandType::Confirm;
		else if (action.type == "CancelMenu") type = SceneMenuCommandType::Cancel;
		else menuCommand = false;
		if (menuCommand) {
			if (!menuSystem_.Apply(document, SceneMenuCommand{request.source, type, action.targetEntityId})) {
				const auto issue = menuSystem_.ConsumeDiagnostic();
				ReportMenuFailure(issue.empty() ? "MenuRuntime:InvalidPanel" : issue);
			}
		} else if (action.type == "SceneTransition") {
			uiTerminalAttempted_ = true;
			auto source = request.source; source.requestSerial = nextUiTransitionSerial_++;
			bool accepted = false;
			if (!action.sceneId.empty() && action.transitionAssetPath.empty()) {
				uiLoadReservation_ = sceneManager_->RequestUiSceneTransition(source, action.sceneId);
				accepted = uiLoadReservation_ != kInvalidSceneInstanceId;
			} else if (!action.sceneId.empty()) {
				accepted = sceneManager_->RequestPresentedTransition({GetSceneInstanceId(), action.sceneId, action.transitionAssetPath, source});
			}
			if (accepted) { uiTransitionSource_ = source; eventSystem_.RearmMenuKeys(); }
			else { eventSystem_.RearmMenuKeys(); ReportMenuFailure("MenuRuntime:TransitionFailed"); break; }
		} else {
			const auto* target = document.FindEntity(action.targetEntityId);
			if (action.type == "PlayAudio" || action.type == "StopAudio" || action.type == "PauseAudio" || action.type == "ResumeAudio") {
				if (!UiEntityActive(document, target) || !SceneEntityQuery::FindEnabledComponent(*target, "AudioSource")) { ReportMenuFailure("MenuRuntime:InvalidTarget"); continue; }
				const auto kind = action.type == "PlayAudio" ? SceneAudioRequestType::Play : action.type == "StopAudio" ? SceneAudioRequestType::Stop :
					action.type == "PauseAudio" ? SceneAudioRequestType::Pause : SceneAudioRequestType::Resume;
				audioSystem_.ApplyRequests(document, {{kind, target->id}});
			} else if (action.type == "PlayTextMotion" || action.type == "StopTextMotion" || action.type == "ResetTextMotion") {
				if (!UiEntityActive(document, target) || !SceneEntityQuery::FindEnabledComponent(*target, "TextMotion")) { ReportMenuFailure("MenuRuntime:InvalidTarget"); continue; }
				if (action.type == "PlayTextMotion") {
					if (!textMotionSystem_.Play(document, target->id, action.textMotionClipId)) ReportMenuFailure("MenuRuntime:InvalidTarget");
				} else if (action.type == "StopTextMotion") textMotionSystem_.Stop(target->id);
				else textMotionSystem_.Reset(target->id);
			} else ReportMenuFailure("MenuRuntime:UnsupportedAction " + action.type);
		}
		const auto after = menuSystem_.GetSource();
		if (before.panelId != after.panelId || before.openSerial != after.openSerial) eventSystem_.RearmMenuKeys();
	}
}
void RuntimeScene::ProcessMenuPulses(const SceneDocument& document, const std::vector<SceneMenuPulse>& pulses) {
	// pulse順を固定し、Cancel通知が閉じたPanelの後続Confirmedを失効させる。
	for (const auto& pulse : pulses) {
		if (uiTransitionSource_.requestSerial || uiTerminalAttempted_ || !menuSystem_.IsSourceValid(document, pulse.source)) continue;
		ApplyUiRequests(document, eventSystem_.EvaluateUiNotifications(document, {pulse}, 0.0f));
		const auto before = menuSystem_.GetSource();
		if (!uiTransitionSource_.requestSerial && !uiTerminalAttempted_) menuSystem_.CompleteCancel(document, pulse);
		const auto after = menuSystem_.GetSource();
		if (before.panelId != after.panelId || before.openSerial != after.openSerial) eventSystem_.RearmMenuKeys();
	}
	// 通知Actionが作った通知は次Frame。一つの出所/種類/Itemは一回に畳み、再帰を作らない。
	for (const auto& pulse : menuSystem_.ConsumePulses()) {
		const bool duplicate = std::any_of(deferredMenuPulses_.begin(), deferredMenuPulses_.end(), [&](const auto& old) {
			return old.type == pulse.type && old.itemId == pulse.itemId && old.source.panelId == pulse.source.panelId &&
				old.source.openSerial == pulse.source.openSerial && old.source.generation == pulse.source.generation;
		});
		if (!duplicate) deferredMenuPulses_.push_back(pulse);
	}
}
void RuntimeScene::PrepareMenuInput(float deltaTime, bool allowed) {
	uiTerminalAttempted_ = false;
	const auto* context = sceneManager_ ? sceneManager_->GetExecutionContext() : nullptr;
	auto* document = GetSceneDocument();
	const bool active = sceneManager_ && GetSceneInstanceId() == sceneManager_->GetActiveSceneInstanceId();
	if (!document || !active || (context && context->IsEditing())) {
		ClearUiRuntime(); textRenderSystem_.SetMenuColorOverrides({}); return;
	}
	if (menuSystem_.GetFrame().instanceId != GetSceneInstanceId() || menuSystem_.GetFrame().generation != document->GetContentGeneration()) ClearUiRuntime();
	const auto oldTop = menuSystem_.GetSource();
	menuSystem_.Sync(*document, GetSceneInstanceId());
	uiControl_ = {GetSceneInstanceId(), document->GetContentGeneration(), oldTop.panelId != 0 || menuSystem_.GetFrame().panelId != 0 || uiTransitionSource_.requestSerial != 0};
	const auto top = menuSystem_.GetSource();
	if (oldTop.panelId != top.panelId || oldTop.openSerial != top.openSerial) eventSystem_.RearmMenuKeys();
	if ((context && context->IsPaused()) || sceneManager_->IsTransitionHeld()) { eventSystem_.RearmMenuKeys(); return; }
	if (uiTransitionSource_.requestSerial) {
		bool failed = false; std::string diagnostic;
		if (uiLoadReservation_ != kInvalidSceneInstanceId) {
			SceneLoadOutcome outcome;
			if (sceneManager_->ConsumeLoadOutcome(uiLoadReservation_, outcome)) { failed = outcome.status == SceneLoadStatus::Failed; diagnostic = outcome.diagnostic; }
		} else {
			SceneUiTransitionOutcome outcome;
			if (sceneManager_->ConsumeUiTransitionOutcome(uiTransitionSource_, outcome)) { failed = outcome.status == SceneUiTransitionStatus::Failed; diagnostic = outcome.diagnostic; }
		}
		if (failed) {
			uiTerminalAttempted_ = true;
			uiTransitionSource_ = {}; uiLoadReservation_ = kInvalidSceneInstanceId; eventSystem_.RearmMenuKeys();
			ReportMenuFailure("MenuRuntime:TransitionFailed " + diagnostic);
		}
	}
	// 通知clockは一Frame一回。入力非許可中はUIのonce/cooldownを進めない。
	eventSystem_.EvaluateUiNotifications(*document, {}, allowed ? deltaTime : 0.0f);
	auto pending = std::exchange(pendingMenuRequests_, {});
	ApplyUiRequests(*document, pending);
	auto deferred = std::exchange(deferredMenuPulses_, {});
	// Gameplay要求が生成した通知も、このprepassの一回評価へ含める。
	auto pendingPulses = menuSystem_.ConsumePulses();
	deferred.insert(deferred.end(), pendingPulses.begin(), pendingPulses.end());
	ProcessMenuPulses(*document, deferred);
	const auto input = eventSystem_.EvaluateUiInput(*document, menuSystem_.GetFrame(), deltaTime, allowed && !uiTransitionSource_.requestSerial && !uiTerminalAttempted_);
	ApplyUiRequests(*document, input);
	ProcessMenuPulses(*document, menuSystem_.ConsumePulses());
	uiControl_.blockGameplayInput |= menuSystem_.GetFrame().panelId != 0 || uiTransitionSource_.requestSerial != 0;
	if (const auto issue = menuSystem_.ConsumeDiagnostic(); !issue.empty()) ReportMenuFailure(issue);
}
void RuntimeScene::SyncMenuPresentation() {
	const auto* context = sceneManager_ ? sceneManager_->GetExecutionContext() : nullptr;
	auto* document = GetSceneDocument();
	if (!document || !sceneManager_ || GetSceneInstanceId() != sceneManager_->GetActiveSceneInstanceId() || (context && context->IsEditing())) {
		ClearUiRuntime(); textRenderSystem_.SetMenuColorOverrides({}); return;
	}
	// 通常Eventがactive/参照を変更した後、文字cache同期前に再検証する。キーは再評価しない。
	menuSystem_.Sync(*document, GetSceneInstanceId());
	textRenderSystem_.SetMenuColorOverrides(menuSystem_.GetFrame().textColors);
	if (const auto issue = menuSystem_.ConsumeDiagnostic(); !issue.empty()) ReportMenuFailure(issue);
}

void RuntimeScene::Update(float deltaTime)
{
	SceneExecutionContext* executionContext = sceneManager_
		? sceneManager_->GetExecutionContext()
		: nullptr;
	const bool editing = executionContext && executionContext->IsEditing();
	const bool playing = !executionContext || executionContext->IsPlaying();
	SceneDocument* activeDocument = GetSceneDocument();
	if (activeDocument) {
		postProcessProfileSystem_.Sync(*activeDocument);
		// 2Dの先読みはTransform確定を待たないため、Eventより前に完了させる。
		audioSystem_.Sync(
			*activeDocument,
			playing,
			GetSceneInstanceId(),
			sceneManager_ && sceneManager_->GetActiveSceneInstanceId() == GetSceneInstanceId()
		);
	} else {
		postProcessProfileSystem_.Reset();
	}
	std::vector<uint64_t> spawnerResetEntityIds;
	const float gameplayDeltaTime = playing
		? hitStopSystem_.Advance(deltaTime)
		: deltaTime;

	// 遷移が成立したフレームは旧Sceneの状態をこれ以上変更しない。
	if (playing && gameplayDeltaTime > 0.0f && activeDocument) {
		const std::string targetSceneId =
			transitionSystem_.Update(*activeDocument, !uiGameplayInputBlocked_);
		if (!targetSceneId.empty()) {
			pendingPresentationRequests_.clear();
			pendingPresentationDocumentGeneration_ = 0;
			SyncMenuPresentation();
			objectSystem_.SyncSprites(activeDocument);
			textRenderSystem_.Sync(activeDocument);
			sceneManager_->ChangeScene(targetSceneId);
			return;
		}
	}
	SceneGameFlowResult gameFlowResult{};
	if (activeDocument && playing) {
		gameFlowResult = gameFlowSystem_.Update(
			*activeDocument,
			enemySpawnerSystem_,
			deltaTime
		);
		for (const SceneGameFlowEntityRequest& request : gameFlowResult.entityRequests) {
			if (SceneEntity* entity = activeDocument->FindEntity(request.entityId)) {
				entity->active = request.active;
			}
		}
		for (const SceneGameFlowWaveRequest& request : gameFlowResult.waveRequests) {
			enemySpawnerSystem_.BeginFiniteWave(
				request.spawnerEntityId,
				request.generation,
				request.count
			);
		}
		for (const SceneGameFlowMotionRequest& request : gameFlowResult.motionRequests) {
			textMotionSystem_.Play(*activeDocument, request.entityId, request.clipId);
		}
	} else {
		gameFlowSystem_.Clear();
	}
	const std::string runtimeSceneId = GetSceneAssetId().empty()
		? "runtime"
		: GetSceneAssetId();
	if (editing && player_) {
		const SceneEntity* playerEntity = activeDocument
			? activeDocument->FindEntityByName("Player")
			: nullptr;
		if (playerEntity) {
			player_->SetTransform(MakeRuntimeTransform(playerEntity->transform));
		}
	}

	particleSystem_.Update(
		runtimeSceneId,
		editing
	);
	effectRenderSystem_.Update(deltaTime);
	environmentSystem_.Update(deltaTime);

#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editing) {
		// Editor操作を先に受け取り、変更されたDocumentを直後の同期へ反映する。
		debugSystem_.DrawEditor(
			activeDocument,
			objectSystem_,
			false
		);

		monitorSystem_.DrawEditor(
			activeDocument,
			GetSceneViewCamera()
		);

		if (activeDocument) {
			environmentSystem_.DrawEditor(*activeDocument);
		}

		particleSystem_.DrawEditor(runtimeSceneId);
	}
#endif
	lightingSystem_.Sync(activeDocument);
	if (activeDocument && playing) {
		// 前フレームで寿命切れ/HitしたRuntime Entityをbinding再構築前に破棄する。
		combatSystem_.FlushRemovals(*activeDocument);
		projectileSystem_.FlushRemovals(*activeDocument);
		// 保存値を実行時状態へ展開し、Transform AnimationをObject同期前に反映する。
		statSystem_.Update(*activeDocument);
		// 前Frame Eventの順を保ち、active化と時刻0 PoseをObject同期より前にまとめる。
		// 内容差替え後の同IDへ古い要求を適用しない。Persistent detachは同世代を維持する。
		if (pendingPresentationDocumentGeneration_ != 0 &&
			pendingPresentationDocumentGeneration_ == activeDocument->GetContentGeneration()) {
			for (const ScenePresentationRequest& request : pendingPresentationRequests_) {
				if (request.type == ScenePresentationRequestType::Play) {
					prefabAnimationSystem_.PlayPresentation(*activeDocument, request.entityId, request.clipName);
				} else if (request.type == ScenePresentationRequestType::Stop) {
					prefabAnimationSystem_.StopPresentation(request.entityId);
				} else {
					prefabAnimationSystem_.ResetPresentation(*activeDocument, request.entityId);
				}
			}
		}
		pendingPresentationRequests_.clear();
		pendingPresentationDocumentGeneration_ = 0;
		// 演出だけ通常deltaTimeで更新し、GameFlow permission/HitStopから独立させる。
		prefabAnimationSystem_.UpdatePresentations(*activeDocument, deltaTime);
		if (gameFlowResult.gameplayAllowed) {
			enemySpawnerSystem_.Update(*activeDocument, gameplayDeltaTime);
		}
		spawnerResetEntityIds = enemySpawnerSystem_.ConsumeResetEntityIds();
		for (uint64_t entityId : spawnerResetEntityIds) {
			attackRunnerSystem_.ResetEntity(*activeDocument, entityId);
			stateMachineSystem_.ResetEntity(entityId);
			prefabAnimationSystem_.ResetEntity(entityId);
			enemySystem_.ResetEntity(entityId);
			hitReactionSystem_.ResetEntity(entityId);
		}
		if (gameFlowResult.gameplayAllowed) {
			hitReactionSystem_.AdvanceRecoveries(statSystem_, gameplayDeltaTime);
			attackRunnerSystem_.Advance(
				*activeDocument,
				prefabAnimationSystem_,
				gameplayDeltaTime
			);
			runtimeEffectSystem_.Spawn(
				*activeDocument,
				physicsSystem_,
				attackRunnerSystem_.ConsumeEffectRequests()
			);
			effectRenderSystem_.SpawnGroundCracks(
				runtimeEffectSystem_.ConsumeGroundCrackRequests()
			);
			runtimeEffectSystem_.Advance(*activeDocument, deltaTime);
			prefabAnimationSystem_.Update(*activeDocument, gameplayDeltaTime);
		}
	} else {
		audioSystem_.Clear();
		statSystem_.Clear();
		attackRunnerSystem_.Clear(activeDocument);
		runtimeEffectSystem_.Clear(activeDocument);
		prefabAnimationSystem_.Clear(activeDocument);
		pendingPresentationRequests_.clear();
		pendingPresentationDocumentGeneration_ = 0;
		eventSystem_.Clear();
		textMotionSystem_.Clear();
		gameFlowSystem_.Clear();
		postProcessProfileSystem_.Reset(activeDocument);
		stateMachineSystem_.Clear();
		combatSystem_.Clear();
		hitReactionSystem_.Clear();
		hitStopSystem_.Clear();
		enemySystem_.Clear();
		enemySpawnerSystem_.Clear();
		projectileSystem_.Clear();
		attachmentSystem_.Clear(&objectSystem_);
	}

	// Objectが実体を所有し、以降のSystemは再構築したbindingsだけを借用する。
	objectSystem_.SyncModels(
		activeDocument,
		physicsSystem_,
		gameplayDeltaTime,
		playing,
		editing
	);
	if (activeDocument) {
		objectSystem_.BuildBindings(
			*activeDocument,
			runtimeObjectBindings_
		);
		physicsSystem_.SyncSceneSettings(
			*activeDocument,
			player_,
			runtimeObjectBindings_,
			editing
		);
		physicsSystem_.ResetBodies(
			runtimeObjectBindings_,
			spawnerResetEntityIds
		);
		if (playing && gameFlowResult.gameplayAllowed && gameplayDeltaTime > 0.0f) {
			enemySystem_.Update(
				*activeDocument,
				runtimeObjectBindings_,
				statSystem_,
				prefabAnimationSystem_,
				hitReactionSystem_,
				stateMachineSystem_,
				gameplayDeltaTime
			);
			// GroundXZ Agentは敵AIが決めた速度へ離隔補正だけを加える。
			agentSystem_.Update(
				*activeDocument,
				runtimeObjectBindings_,
				gameplayDeltaTime
			);
			enemySystem_.ApplyMovementStops(runtimeObjectBindings_);
			projectileSystem_.Update(
				*activeDocument,
				runtimeObjectBindings_,
				gameplayDeltaTime
			);
		}
	} else {
		audioSystem_.Clear();
		runtimeObjectBindings_.clear();
		agentSystem_.Clear();
		attachmentSystem_.Clear(&objectSystem_);
		combatSystem_.Clear();
		hitReactionSystem_.Clear();
		hitStopSystem_.Clear();
		enemySystem_.Clear();
		eventSystem_.Clear();
		gameFlowSystem_.Clear();
		postProcessProfileSystem_.Reset();
		stateMachineSystem_.Clear();
		attackRunnerSystem_.Clear();
		prefabAnimationSystem_.Clear();
		pendingPresentationRequests_.clear();
		pendingPresentationDocumentGeneration_ = 0;
		projectileSystem_.Clear();
		statSystem_.Clear();
		physicsSystem_.Clear();
		cameraSystem_.Reset();
	}

	// Camera入力、Player移動、Physics、追従Cameraの順序は相互依存を持つ。
	if (activeDocument) {
		cameraSystem_.UpdateBeforeSimulation(
			*activeDocument,
			camera_,
			player_,
			runtimeObjectBindings_,
			deltaTime,
			playing,
			playing,
			!uiGameplayInputBlocked_
		);
	}
	Vector3 playerAttackInputDirection{};
	if (player_ && playing) {
		player_->Update(camera_, gameFlowResult.gameplayAllowed && !uiGameplayInputBlocked_);
		const Vector3& playerVelocity = player_->GetPhysicsBody().velocity;
		playerAttackInputDirection = { playerVelocity.x, 0.0f, playerVelocity.z };
		if (Math::Length(playerAttackInputDirection) > 0.0001f) {
			playerAttackInputDirection = Math::Normalize(playerAttackInputDirection);
		}
	}
	if (activeDocument && playing && gameFlowResult.gameplayAllowed && gameplayDeltaTime > 0.0f) {
		// State行動は入力取得後、Physics確定前に速度・攻撃判定を更新する。
		stateMachineSystem_.Update(
			*activeDocument,
			runtimeObjectBindings_,
			player_,
			attackRunnerSystem_,
			prefabAnimationSystem_,
			gameplayDeltaTime,
			{},
			!uiGameplayInputBlocked_
		);
		attackRunnerSystem_.ApplyMotion(
			*activeDocument,
			runtimeObjectBindings_,
			player_,
			playerAttackInputDirection,
			gameplayDeltaTime
		);
		// Combat後に予約した被弾速度を、AI/Agent/Stateの書込み後に上書きする。
		hitReactionSystem_.ApplyMotionOverrides(
			*activeDocument,
			runtimeObjectBindings_,
			gameplayDeltaTime
		);
	}
	if (activeDocument && (!playing || gameplayDeltaTime > 0.0f)) {
		physicsSystem_.Step(
			player_,
			runtimeObjectBindings_,
			gameplayDeltaTime,
			playing
		);
	}
	if (player_ && playing && gameplayDeltaTime > 0.0f) {
		player_->PostPhysicsUpdate();
		SceneEntity* playerEntity = activeDocument
			? activeDocument->FindEntityByName("Player")
			: nullptr;
		if (playerEntity && player_->GetObject()) {
			SynchronizeSceneTransform(
				playerEntity->transform,
				player_->GetObject()->GetTransform()
			);
		}
	}
	if (activeDocument && playing && gameFlowResult.gameplayAllowed && gameplayDeltaTime > 0.0f) {
		// Bone追従はAnimation/Physics後、当たり判定とEventは最終Transform後に評価する。
		attachmentSystem_.Update(
			*activeDocument,
			objectSystem_,
			runtimeObjectBindings_
		);
		combatSystem_.Update(
			*activeDocument,
			runtimeObjectBindings_,
			statSystem_,
			gameplayDeltaTime
		);
		std::vector<SceneCombatHitEvent> hitEvents = combatSystem_.ConsumeHitEvents();
		for (const SceneCombatHitEvent& hitEvent : hitEvents) {
			hitStopSystem_.Request(hitEvent.hitStopDuration);
		}
		runtimeEffectSystem_.SpawnHitEffects(hitEvents);
		hitReactionSystem_.Update(
			*activeDocument,
			runtimeObjectBindings_,
			statSystem_,
			stateMachineSystem_,
			hitEvents,
			gameplayDeltaTime
		);
		runtimeEffectSystem_.SpawnDeathEffects(
			*activeDocument,
			hitReactionSystem_.ConsumeDeathEffectRequests()
		);
	}
	// 有効化は資源同期用の複製だけに行う。保存DocumentやRuntime overrideは変更しない。
	SceneDocument editorResourceDocument;
	SceneDocument* resourceDocument = activeDocument;
	if (const auto* view = GetEditorUIView(); view && activeDocument) {
		editorResourceDocument = *activeDocument;
		view->PrepareDisplay(editorResourceDocument);
		resourceDocument = &editorResourceDocument;
	}
	if (activeDocument) {
		cameraSystem_.UpdateAfterSimulation(
			*activeDocument,
			camera_,
			player_,
			runtimeObjectBindings_,
			deltaTime,
			playing,
			playing,
			!uiGameplayInputBlocked_
		);
	} else if (camera_) {
		camera_->Update();
	}
	// Transform確定後に環境設定とDebug形状を登録し、描画時の状態を揃える。
	environmentSystem_.Sync(activeDocument, runtimeObjectBindings_);
	if (activeDocument && playing) {
		// TextMotionの自然終了もTransform演出の完了も、この後のEvent段階で受け取る。
		textMotionSystem_.Update(*activeDocument, deltaTime);
	} else {
		textMotionSystem_.Clear();
	}

#if defined(_DEBUG) || defined(DEVELOPMENT)
	debugSystem_.AddDebugDraw(
		activeDocument,
		objectSystem_,
		cameraSystem_,
		camera_,
		playing,
		playing,
		false
	);
#endif
	if (activeDocument && playing && gameplayDeltaTime > 0.0f) {
		// Prefab生成はEntity配列を再配置し得るため、bindingを使い終えた最後に行う。
		// HitStop中は既存Event gateを維持し、演出完了pulseは次の評価まで保持する。
		const SceneEventRuntimeSignals eventSignals{
			cameraSystem_.ConsumeCompletedCameraPathEntityId(),
			audioSystem_.ConsumeFinishedEntityIds(*activeDocument),
			textMotionSystem_.ConsumeCompletions(),
			prefabAnimationSystem_.ConsumePresentationCompletions()
		};
		const SceneEventResult eventResult = eventSystem_.Update(
			*activeDocument,
			statSystem_,
			stateMachineSystem_,
			gameplayDeltaTime,
			eventSignals,
			{},
			!uiGameplayInputBlocked_
		);
		if (!eventResult.sceneTransitionId.empty()) {
			// Capture前は現在のPose/Profileを保持する。拒否でも即時遷移へ落とさない。
			if (!eventResult.transitionAssetPath.empty()) {
				sceneManager_->RequestPresentedTransition({GetSceneInstanceId(),
					eventResult.sceneTransitionId, eventResult.transitionAssetPath});
				return;
			}
			pendingPresentationRequests_.clear();
			pendingPresentationDocumentGeneration_ = 0;
			postProcessProfileSystem_.Reset(activeDocument);
			SyncMenuPresentation();
			objectSystem_.SyncSprites(activeDocument);
			textRenderSystem_.Sync(activeDocument);
			sceneManager_->ChangeScene(eventResult.sceneTransitionId);
			return;
		}
		pendingPresentationRequests_ = eventResult.presentationRequests;
		if (sceneManager_ && GetSceneInstanceId() == sceneManager_->GetActiveSceneInstanceId()) {
			for (auto request : eventResult.menuRequests) {
				request.source = {GetSceneInstanceId(), activeDocument->GetContentGeneration(), activeDocument->GetRevision()};
				pendingMenuRequests_.push_back(std::move(request));
			}
		}
		pendingPresentationDocumentGeneration_ = activeDocument->GetContentGeneration();
		for (const SceneTextMotionRequest& request : eventResult.textMotionRequests) {
			if (request.type == SceneTextMotionRequestType::Play) {
				textMotionSystem_.Play(*activeDocument, request.entityId, request.clipId);
			} else if (request.type == SceneTextMotionRequestType::Stop) {
				textMotionSystem_.Stop(request.entityId);
			} else {
				textMotionSystem_.Reset(request.entityId);
			}
		}
		cameraSystem_.ApplyEventRequests(
			*activeDocument,
			camera_,
			player_,
			eventResult.cameraRequests
		);
		audioSystem_.ApplyRequests(*activeDocument, eventResult.audioRequests);
		postProcessProfileSystem_.ApplyEventResult(*activeDocument, eventResult);
	}
	if (activeDocument) {
		audioSystem_.Sync(
			*activeDocument,
			playing,
			GetSceneInstanceId(),
			sceneManager_ && sceneManager_->GetActiveSceneInstanceId() == GetSceneInstanceId()
		);
		if (playing) {
			// Event Play直後のVoiceにも、三人称Cameraを含む最終姿勢を同Frameで適用する。
			audioSystem_.UpdateSpatial(*activeDocument, camera_);
		}
	}
	postProcessProfileSystem_.Update(playing ? gameplayDeltaTime : 0.0f);
	textRenderSystem_.ClearTextOverrides();
	textRenderSystem_.ClearPresentationOverrides();
	if (activeDocument) {
		for (const SceneGameFlowTextRequest& request : gameFlowResult.textRequests) {
			textRenderSystem_.SetTextOverride(request.entityId, request.text);
		}
		SceneEntity* statusText = postProcessProfileSystem_.GetStatusTextEntityId() != 0
			? activeDocument->FindEntity(
				postProcessProfileSystem_.GetStatusTextEntityId()
			)
			: nullptr;
		if (!statusText &&
			!postProcessProfileSystem_.GetStatusTextEntityName().empty()) {
			statusText = activeDocument->FindEntityByName(
				postProcessProfileSystem_.GetStatusTextEntityName()
			);
		}
		if (statusText) {
			textRenderSystem_.SetTextOverride(
				statusText->id,
				postProcessProfileSystem_.GetStatusTextPrefix() +
					postProcessProfileSystem_.GetActiveProfileLabel()
			);
		}
	}
	for (const auto& [entityId, presentation] :
		textMotionSystem_.GetPresentationOverrides()) {
		textRenderSystem_.SetPresentationOverride(
			entityId,
			presentation.positionOffset,
			presentation.rotationOffset,
			presentation.scaleMultiplier,
			presentation.opacityMultiplier
		);
	}
	SyncMenuPresentation();
	objectSystem_.SyncSprites(resourceDocument);
	textRenderSystem_.Sync(resourceDocument);
}

void RuntimeScene::UpdateTransitionHold()
{
	SceneDocument* document = GetSceneDocument();
	if (!document) {
		return;
	}
	// 次Sceneは開始Eventより前に描く。旧Sceneでは同じ同期で再生時計を保つ。
	objectSystem_.SyncModelsForTransition(document, physicsSystem_);
	objectSystem_.BuildBindings(*document, runtimeObjectBindings_);
	attachmentSystem_.Update(*document, objectSystem_, runtimeObjectBindings_);
	objectSystem_.SyncSprites(document);
	cameraSystem_.UpdateTransitionHold(*document, camera_, player_, runtimeObjectBindings_);
	lightingSystem_.Sync(document);
	environmentSystem_.Sync(document, runtimeObjectBindings_);
	// TextMotion/GameFlowの既存overrideは消さず、新Sceneだけauthoring表示を準備する。
	SyncMenuPresentation();
	textRenderSystem_.Sync(document);
}

void RuntimeScene::UpdatePaused()
{
	const auto* source = GetSceneDocument();
	cameraSystem_.UpdatePaused(camera_, debugCamera_);
	// UI時計はprepassで凍結済み。参照と文字cacheだけ同期し、内容置換後の旧色を残さない。
	SyncMenuPresentation();
	textRenderSystem_.Sync(GetSceneDocument());
	SceneDocument* document = GetSceneDocument();
	lightingSystem_.Sync(document);
#if defined(_DEBUG) || defined(DEVELOPMENT)
	debugSystem_.DrawEditor(document, objectSystem_, true);
	monitorSystem_.DrawEditor(
		document,
		GetSceneViewCamera()
	);
	debugSystem_.AddDebugDraw(
		document,
		objectSystem_,
		cameraSystem_,
		camera_,
		true,
		false,
		true
	);
#endif
}

void RuntimeScene::Draw()
{
	DrawWithCamera(GetSceneViewCamera());
}

Camera* RuntimeScene::GetRenderCamera() const
{
	return GetSceneViewCamera();
}

void RuntimeScene::DrawWithCamera(Camera* viewCamera)
{
	DrawSceneView(viewCamera ? viewCamera : GetSceneViewCamera());
}

void RuntimeScene::DrawEnvironment(Camera* viewCamera)
{
	ApplyRenderCamera(viewCamera ? viewCamera : GetSceneViewCamera());
	environmentSystem_.DrawSkybox();
}

void RuntimeScene::BindLighting()
{
	lightingSystem_.Bind();
}

void RuntimeScene::DrawSceneContent(Camera* viewCamera)
{
	viewCamera = viewCamera ? viewCamera : GetSceneViewCamera();
	PrepareSceneContent(viewCamera);
	BindLighting();
	DrawPreparedSceneContentForView(viewCamera, 0);
}

void RuntimeScene::PrepareSceneContent(Camera* viewCamera)
{
	ApplyRenderCamera(viewCamera ? viewCamera : GetSceneViewCamera());
	objectSystem_.PrepareModelDraw();
}

void RuntimeScene::DrawPreparedSceneContent(Camera* viewCamera)
{
	DrawPreparedSceneContentForView(
		viewCamera ? viewCamera : GetSceneViewCamera(),
		0
	);
}

void RuntimeScene::DrawForegroundEffects()
{
	DrawForegroundEffectsWithCamera(GetSceneViewCamera());
}

void RuntimeScene::DrawForegroundEffectsWithCamera(Camera* viewCamera)
{
	viewCamera = viewCamera ? viewCamera : GetSceneViewCamera();
	ApplyRenderCamera(viewCamera);
	effectRenderSystem_.DrawForegroundPass(
		GetSceneDocument(),
		viewCamera,
		0,
		environmentSystem_,
		objectSystem_,
		!(sceneManager_ && sceneManager_->IsTransitionHeld())
	);
}

const SceneUIEditorView* RuntimeScene::GetEditorUIView() const {
	// 同じSceneアセットの別Instanceへ編集表示を渡さない。未所属の無効IDも拒否する。
	const SceneInstanceId instanceId = GetSceneInstanceId();
	return sceneManager_ && instanceId != kInvalidSceneInstanceId &&
		instanceId == sceneManager_->GetActiveSceneInstanceId() ? sceneManager_->GetEditorUIView() : nullptr;
}

void RuntimeScene::DrawEditorUI(const SceneUIEditorView& view, const char* renderSpace) {
	const auto* source = GetSceneDocument();
	if (!source || !GetEditorUIView() || !view.Matches(*source)) return;
	SceneDocument display = *source;
	view.PrepareDisplay(display);
	uiRenderSystem_.Draw(display, objectSystem_, textRenderSystem_, renderSpace, 0, &view);
}

bool RuntimeScene::HasMenuOverlay() const {
	// 公開済みInstance IDでActiveを照合する。Editorの一時表示経路とは混在させない。
	if (!sceneManager_ || GetSceneInstanceId() == kInvalidSceneInstanceId ||
		GetSceneInstanceId() != sceneManager_->GetActiveSceneInstanceId()) return false;
	const auto* context = sceneManager_->GetExecutionContext();
	if (context && context->IsEditing()) return false;
	const auto* document = GetSceneDocument();
	return document && menuSystem_.Matches(*document, GetSceneInstanceId()) && uiRenderSystem_.HasMenu(*document, menuSystem_.GetFrame());
}

void RuntimeScene::DrawMenuOverlay() {
	if (!HasMenuOverlay()) return;
	uiRenderSystem_.DrawMenu(*GetSceneDocument(), objectSystem_, textRenderSystem_, menuSystem_.GetFrame());
}

bool RuntimeScene::HasScreenOverlay() const
{
	const SceneDocument* document = GetSceneDocument();
	return document && (textRenderSystem_.HasScreenOverlay(*document) || uiRenderSystem_.HasScreenOverlay(*document));
}

void RuntimeScene::DrawScreenOverlay(uint32_t width, uint32_t height)
{
	SceneDocument* document = GetSceneDocument();
	if (document) {
		textRenderSystem_.DrawScreenOverlay(*document, width, height, sceneManager_ && sceneManager_->IsDrawingEditorUIView());
		if (const auto* view = sceneManager_ && sceneManager_->IsDrawingEditorUIView() ? GetEditorUIView() : nullptr) DrawEditorUI(*view, "ScreenOverlay");
		else uiRenderSystem_.Draw(*document, objectSystem_, textRenderSystem_, "ScreenOverlay");
	}
}

void RuntimeScene::DrawOffscreenViews()
{
	SceneDocument* document = GetSceneDocument();
	monitorSystem_.DrawOffscreen(
		document,
		runtimeObjectBindings_,
		cameraSystem_,
		[this](Camera* monitorCamera, uint64_t skipEntityId) {
			DrawSceneView(monitorCamera, skipEntityId);
		}
	);
	if (document) {
		// Monitor描画が差し替えたCameraを、通常Scene View用へ戻す。
		ApplyRenderCamera(GetSceneViewCamera());
	}
}

void RuntimeScene::DrawShadow()
{
	std::vector<Object3d*> shadowCasters;
	CollectShadowCasters(shadowCasters);
	RenderShadowCasters(shadowCasters);
}

void RuntimeScene::CollectShadowCasters(
	std::vector<Object3d*>& shadowCasters
) {
	const bool hidePlayerModel = ShouldHidePlayerModelForCamera(camera_);
	SceneDocument* document = GetSceneDocument();
	if (document) {
		objectSystem_.CollectShadowCasters(
			*document,
			hidePlayerModel,
			shadowCasters
		);
	}
}

void RuntimeScene::RenderShadowCasters(
	const std::vector<Object3d*>& shadowCasters
) {
	lightingSystem_.RenderShadows(shadowCasters);
}

void RuntimeScene::Finalize()
{
	// 非所有参照を持つSystemから解除し、最後にObjectとCameraを破棄する。
	monitorSystem_.Finalize(&runtimeObjectBindings_);
	agentSystem_.Clear();
	attachmentSystem_.Clear(&objectSystem_);
	combatSystem_.Clear();
	hitReactionSystem_.Clear();
	hitStopSystem_.Clear();
	enemySystem_.Clear();
	eventSystem_.Clear();
	textMotionSystem_.Clear();
	gameFlowSystem_.Clear();
	audioSystem_.Clear();
	postProcessProfileSystem_.Reset();
	stateMachineSystem_.Clear();
	attackRunnerSystem_.Clear();
	physicsSystem_.Clear();
	prefabAnimationSystem_.Clear(GetSceneDocument());
	pendingPresentationRequests_.clear();
	pendingPresentationDocumentGeneration_ = 0;
	projectileSystem_.Clear();
	statSystem_.Clear();

	if (player_) {
		player_->Finalize();
		delete player_;
		player_ = nullptr;
	}

	runtimeObjectBindings_.clear();
	ClearUiRuntime();
	uiGameplayInputBlocked_ = false;
	textRenderSystem_.Finalize();
	objectSystem_.Finalize();

	particleSystem_.Finalize();
	lightingSystem_.Finalize();
	effectRenderSystem_.Finalize();
	environmentSystem_.Finalize();
	cameraSystem_.Reset();

	delete camera_;
	camera_ = nullptr;

	delete debugCamera_;
	debugCamera_ = nullptr;
}

void RuntimeScene::PrepareForSceneTransition()
{
	prefabAnimationSystem_.Clear(GetSceneDocument());
	pendingPresentationRequests_.clear();
	pendingPresentationDocumentGeneration_ = 0;
	textMotionSystem_.Clear();
	gameFlowSystem_.Clear();
	SceneExecutionContext* executionContext = sceneManager_
		? sceneManager_->GetExecutionContext()
		: nullptr;
	if (!executionContext || executionContext->IsPlaying()) {
		audioSystem_.PrepareForSceneTransition();
	}
}
