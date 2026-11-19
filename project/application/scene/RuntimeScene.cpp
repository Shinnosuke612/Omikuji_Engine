// 役割: SceneDocumentと各Runtime Systemを連携し、更新と描画を実行する。
#include <new>
#include <stdexcept>
#include "RuntimeScene.h"

#include "../../engine/scene/SceneManager.h"
#include "../../engine/scene/SceneExecutionContext.h"
#include "../../engine/scene/SceneDocument.h"
#include "../../engine/scene/SceneEntityQuery.h"
#include "../../engine/3d/SrvManager.h"
#include "../../engine/base/DirectXCommon.h"
#include "../../engine/utility/Logger.h"

#include "../../engine/3d/Camera.h"
#include "../../engine/3d/Model.h"
#include "../../engine/3d/ModelManager.h"
#include "../../engine/3d/Object3dCommon.h"
#include "../../engine/3d/Object3d.h"
#include "../../engine/math/Math.h"
#include "../../engine/particle/ParticleManager.h"
#include "../player/Player.h"
#include <algorithm>
#include <utility>
#include <cmath>
#include <limits>

namespace {
	float BuildingVisibility(
		const Vector3& camera, const Vector3& focus,
		const Matrix4x4& world, const Vector3& minimum, const Vector3& maximum
	) {
		for (const auto& row : world.m) {
			for (const float value : row) {
				if (!std::isfinite(value)) return 1.0f;
			}
		}
		if (world.m[0][3] != 0.0f || world.m[1][3] != 0.0f ||
			world.m[2][3] != 0.0f || world.m[3][3] != 1.0f) {
			return 1.0f;
		}
		const float determinant = Determinant(world);
		// Inverseはこの閾値未満でゼロ行列を返す。
		if (!std::isfinite(determinant) || std::abs(determinant) < 1e-6f) {
			return 1.0f;
		}
		const Matrix4x4 inverse = Inverse(world);
		const auto localCoordinate = [&](const Vector3& point, int axis) {
			return static_cast<double>(point.x) * inverse.m[0][axis] +
				static_cast<double>(point.y) * inverse.m[1][axis] +
				static_cast<double>(point.z) * inverse.m[2][axis] +
				inverse.m[3][axis];
		};
		const float low[] = {minimum.x, minimum.y, minimum.z};
		const float high[] = {maximum.x, maximum.y, maximum.z};
		double start[3], delta[3], expansionScale[3];
		for (int axis = 0; axis < 3; ++axis) {
			start[axis] = localCoordinate(camera, axis);
			const double end = localCoordinate(focus, axis);
			// 逆変換の列長でワールド距離をローカルの各面の幅に換算する。
			expansionScale[axis] = std::hypot(
				static_cast<double>(inverse.m[0][axis]),
				static_cast<double>(inverse.m[1][axis]),
				static_cast<double>(inverse.m[2][axis]));
			if (!std::isfinite(start[axis]) || !std::isfinite(end) ||
				!std::isfinite(expansionScale[axis]) ||
				!std::isfinite(low[axis]) || !std::isfinite(high[axis]) ||
				low[axis] > high[axis]) {
				return 1.0f;
			}
			delta[axis] = end - start[axis];
		}
		const auto intersects = [&](double expansion) {
			// 注視点より奥を隠さないよう、無限のRayではなく閉区間を比較する。
			double enter = 0.0;
			double exit = 1.0;
			for (int axis = 0; axis < 3; ++axis) {
				const double padding = expansion * expansionScale[axis];
				const double minimum = low[axis] - padding;
				const double maximum = high[axis] + padding;
				if (delta[axis] == 0.0) {
					if (start[axis] < minimum || start[axis] > maximum) return false;
					continue;
				}
				const double a = (minimum - start[axis]) / delta[axis];
				const double b = (maximum - start[axis]) / delta[axis];
				enter = (std::max)(enter, (std::min)(a, b));
				exit = (std::min)(exit, (std::max)(a, b));
				if (enter > exit) return false;
			}
			return true;
		};

		// 完全非表示の境界は、フェード帯を追加する前と同じ元の箱。
		if (intersects(0.0)) return 0.0f;
		constexpr double fadeWidth = 2.0; // ALLEY PREVIEWでは1m相当。
		if (!intersects(fadeWidth)) return 1.0f;
		double outside = 0.0;
		double inside = fadeWidth;
		for (int i = 0; i < 12; ++i) {
			const double middle = (outside + inside) * 0.5;
			if (intersects(middle)) inside = middle;
			else outside = middle;
		}
		const double t = inside / fadeWidth;
		return static_cast<float>(t * t * (3.0 - 2.0 * t));
	}

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

bool RuntimeScene::IsPreparedDestinationDraw() const noexcept {
	return sceneManager_ && GetSceneInstanceId() &&
		sceneManager_->GetPreparedDestinationRenderInstance() == GetSceneInstanceId();
}

void RuntimeScene::ApplyRenderCamera(Camera* viewCamera) {
	objectSystem_.ApplyRenderCamera(viewCamera);
	environmentSystem_.ApplyRenderCamera(viewCamera);
	if (!IsPreparedDestinationDraw()) ParticleManager::GetInstance()->SetCamera(viewCamera);
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

bool RuntimeScene::TryGetViewEffectClock(SceneViewEffectClock& out) const {
	const auto* document = GetSceneDocument();
	if (!document || !deathSystem_.IsDefeated() ||
		deathDocumentGeneration_ != document->GetContentGeneration() ||
		(deathWorldInstance_ != 0 && (deathWorldInstance_ != GetSceneInstanceId() ||
			deathWorldGeneration_ != document->GetContentGeneration()))) return false;
	out = { document->GetContentGeneration(), deathWorldElapsedSeconds_ };
	return true;
}

bool RuntimeScene::TryGetReplayHistoryStatus(uint64_t owner, SceneReplayHistoryStatus& out) const {
	const auto* document = GetSceneDocument();
	const auto* entity = document ? document->FindEntity(owner) : nullptr;
	if (!entity || deathDocumentGeneration_ != document->GetContentGeneration() ||
		!SceneEntityQuery::FindComponent(*entity, "DeathReplay")) return false;
	const auto& identity = replayHistory_.Identity();
	if (identity.owner && (identity.owner != owner || identity.instance != GetSceneInstanceId() ||
		identity.generation != document->GetContentGeneration())) return false;
	out = replayHistory_.Status(); out.owner = owner; return true;
}
void RuntimeScene::CaptureReplayBeforeDraw(const SceneReplayCaptureRequest& request) {
	if (replayHistory_.Status().phase != SceneReplayHistoryPhase::Recording &&
        replayHistory_.Status().phase != SceneReplayHistoryPhase::Sealed) return;
	const auto* document = GetSceneDocument();
	const auto& identity = replayHistory_.Identity();
	if (!document || !sceneManager_ || sceneManager_->GetActiveSceneInstanceId() != GetSceneInstanceId() ||
		identity.instance != GetSceneInstanceId() || identity.generation != document->GetContentGeneration() ||
		identity.owner != deathReplayStarted_.owner || identity.deathSerial != deathReplayStarted_.serial) {
		FailDeathReplay(deathReplayStarted_, "DeathResponse:HistoryIdentity"); return;
	}
	const auto* context = sceneManager_->GetExecutionContext();
	if (context && context->IsEditing()) { replayHistory_.Clear(); replayBeginPending_ = replayFinalPending_ = false; return; }
    // Sealした描画はliveのまま。次frameの最初の影より前に専用資源を準備するだけ。
    // 3視点の適用はこの採取処理の後、PrepareReplayGameFrameで行う。
    if (replayHistory_.Status().phase == SceneReplayHistoryPhase::Sealed) {
        if (replayRender_.IsPrepared()) return;
        try {
            const auto environment = environmentSystem_.CaptureReplayEnvironment(*document);
            if (!replayRender_.Prepare(replayHistory_, Object3dCommon::GetInstance()->GetDxCommon(),
                SrvManager::GetInstance(), lightingSystem_, environment))
                FailDeathReplay(deathReplayStarted_, "DeathResponse:RenderPreparation");
        } catch (const std::bad_alloc&) { FailDeathReplay(deathReplayStarted_, "DeathResponse:RenderPreparation"); }
          catch (const std::length_error&) { FailDeathReplay(deathReplayStarted_, "DeathResponse:RenderPreparation"); }
        return;
    }
	if (!request.frameSerial || request.frameSerial == replayCapturedDraw_ || replayUpdateSerial_ == replayCapturedUpdate_ ||
		(!replayBeginPending_ && !replayFinalPending_ && replayRealSeconds_ < replayNextDue_)) return;
	try {
		uint64_t estimated = 0;
		if (!objectSystem_.EstimateReplayFrameBytes(*document, estimated, replayBeginPending_ ? identity.owner : 0)) {
			FailDeathReplay(deathReplayStarted_, "DeathResponse:HistoryPose"); return;
		}
		if (!replayHistory_.PrepareCapture(estimated)) {
			FailDeathReplay(deathReplayStarted_, replayHistory_.Status().diagnostic); return;
		}
		// 出現補正とCPU骨格を確定。delta加算やGPU dispatchはしない。
		objectSystem_.ApplyRenderCamera(camera_);
		SceneReplayFrame frame;
		frame.identity = identity; frame.realSeconds = replayRealSeconds_;
		frame.worldElapsedSeconds = deathWorldElapsedSeconds_;
		frame.gameViewEffectTime = request.gameViewEffectTime;
		frame.effectivePostProcess = postProcessProfileSystem_.GetEffectiveSettings();
		float waterTime = 0;
		if (environmentSystem_.TryGetWaterTime(*document, waterTime)) frame.waterTime = waterTime;
		if (!objectSystem_.CaptureReplaySubjects(*document, replayHistory_, frame)) {
			FailDeathReplay(deathReplayStarted_, "DeathResponse:HistoryPose"); return;
		}
		if (replayBeginPending_) {
			const auto* ownerObject = objectSystem_.FindObject(identity.owner);
			if (!ownerObject || !camera_) { FailDeathReplay(deathReplayStarted_, "DeathResponse:HistoryPose"); return; }
			replayOwnerBasis_ = {ownerObject->GetWorldMatrix(), camera_->GetWorldMatrix(), camera_->GetProjectionMatrix()};
		}
		frame.ownerBasis = replayOwnerBasis_;
		const bool accepted = replayFinalPending_ ? replayHistory_.Seal(std::move(frame)) : replayHistory_.Append(std::move(frame));
		if (!accepted) { FailDeathReplay(deathReplayStarted_, replayHistory_.Status().diagnostic); return; }
		replayCapturedDraw_ = request.frameSerial; replayCapturedUpdate_ = replayUpdateSerial_;
		replayBeginPending_ = replayFinalPending_ = false;
		replayNextDue_ = replayRealSeconds_ + replayInterval_;
	} catch (const std::bad_alloc&) {
		FailDeathReplay(deathReplayStarted_, "DeathResponse:HistoryAllocation");
	} catch (const std::length_error&) {
		FailDeathReplay(deathReplayStarted_, "DeathResponse:HistoryBudget");
	}
}

bool RuntimeScene::TryGetDeathReplayStatus(uint64_t owner, SceneDeathReplayStatus& out) const {
	const auto* document = GetSceneDocument();
	const auto* entity = document ? document->FindEntity(owner) : nullptr;
	if (!entity || deathDocumentGeneration_ != document->GetContentGeneration() ||
		!SceneEntityQuery::FindComponent(*entity, "DeathReplay")) return false;
	if (deathReplaySystem_.TryGetStatus(owner, out)) {
        out.gpuBytes=replayRender_.EstimatedGpuBytes(); out.descriptors=replayRender_.EstimatedDescriptors();
        if (out.phase==SceneDeathReplayPhase::Frozen && replayHistory_.Status().phase==SceneReplayHistoryPhase::Sealed)
            out.phase=SceneDeathReplayPhase::Preparing;
        return true;
    }
	out = { SceneDeathReplayPhase::Waiting, owner, 0.0f, {} }; return true;
}

void RuntimeScene::DrawSceneView(Camera* viewCamera, uint64_t skipEntityId, bool mainView) {
	DrawEnvironment(viewCamera);
	PrepareSceneContent(viewCamera);
	BindLighting();
	DrawPreparedSceneContentForView(viewCamera, skipEntityId, mainView);
}

std::unordered_map<uint64_t, float> RuntimeScene::FindMainViewBuildingVisibility(
	const SceneDocument& document, const Camera* viewCamera
) const {
	std::unordered_map<uint64_t, float> visibility;
	const auto* context = sceneManager_ ? sceneManager_->GetExecutionContext() : nullptr;
	if (!viewCamera || viewCamera != camera_ || viewCamera != GetSceneViewCamera() ||
		!sceneManager_ || sceneManager_->GetActiveSceneInstanceId() != GetSceneInstanceId() ||
		(context && (!context->IsPlaying() || context->IsEditing() || context->IsPaused())) ||
		sceneManager_->IsTransitionHeld() || deathSystem_.IsDefeated()) {
		return visibility;
	}
	Vector3 focus{};
	if (!cameraSystem_.TryGetPlayerFollowFocus(document, focus)) {
		return visibility;
	}

	struct Bounds {
		Vector3 minimum{};
		Vector3 maximum{};
		bool valid = false;
	};
	// 建物の複製は同じModelを参照する。頂点走査はビューにつき各Model一回とする。
	std::unordered_map<const Model*, Bounds> boundsByModel;
	for (const auto& entity : document.GetEntities()) {
		const auto* renderer = SceneEntityQuery::FindEnabledComponent(entity, "MeshRenderer");
		const auto& modelPath = renderer ? renderer->modelPath : entity.modelPath;
		if (!SceneCameraSystem::IsAlleyCameraOccluder(modelPath) ||
			!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		const auto* object = objectSystem_.FindObject(entity.id);
		const auto* model = ModelManager::GetInstance()->FindModel(modelPath);
		if (!object || !model) continue;
		auto [found, inserted] = boundsByModel.try_emplace(model);
		auto& bounds = found->second;
		if (inserted) {
			bounds.valid = model->GetLocalBounds(bounds.minimum, bounds.maximum);
		}
		if (!bounds.valid) continue;
		const float coverage = BuildingVisibility(
			viewCamera->GetTranslate(), focus, object->GetWorldMatrix(),
			bounds.minimum, bounds.maximum);
		if (coverage < 1.0f) {
			visibility.emplace(entity.id, coverage);
		}
	}
	return visibility;
}

void RuntimeScene::DrawPreparedSceneContentForView(
	Camera* viewCamera,
	uint64_t skipEntityId,
	bool mainView
) {
	SceneDocument* document = GetSceneDocument();
	if (document) {
		const bool hidePlayerModel =
			ShouldHidePlayerModelForCamera(viewCamera);
		// 監視ビューと共有影に、メインの非表示を伝播させない。
		const auto visibility = mainView
			? FindMainViewBuildingVisibility(*document, viewCamera)
			: std::unordered_map<uint64_t, float>{};
		objectSystem_.DrawModels(
			*document,
			skipEntityId,
			hidePlayerModel,
			visibility
		);
	}
	effectRenderSystem_.DrawScenePass(
		document,
		viewCamera,
		skipEntityId,
		environmentSystem_,
		objectSystem_,
		!(sceneManager_ && sceneManager_->IsTransitionHeld()), !IsPreparedDestinationDraw()
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

bool RuntimeScene::BeginPreparedRuntime() {
	if (!GetSceneDocument() || camera_ || debugCamera_ ||
		initialPreparationStage_ != InitialPreparationStage::Idle) return false;
	initialPreparationStage_ = InitialPreparationStage::Cameras;
	return true;
}

RuntimePrepareStep RuntimeScene::StepPreparedRuntime(
	const SceneRuntimePrepareBudget& budget, std::string& diagnostic) {
	if (initialPreparationStage_ == InitialPreparationStage::Idle) return RuntimePrepareStep::Unsupported;
	if (!budget.maxItems || !budget.maxResourceCreates || !budget.maxCopyBytes) return RuntimePrepareStep::Pending;
	if (std::chrono::steady_clock::now() >= budget.deadline) return RuntimePrepareStep::Pending;
	// 一単位だけ進める。Gameの同一deadline内でProgressを再Pumpし、フレーム待ちにしない。
	if (initialPreparationStage_ == InitialPreparationStage::Cameras) {
		// 候補のcameraは局所所有。共有Object3d/Particleの既定cameraは変更しない。
		auto camera = std::unique_ptr<Camera>(CreateOrbitCamera());
		auto debugCamera = std::unique_ptr<Camera>(CreateOrbitCamera());
		if (!camera || !debugCamera) {
			diagnostic = "Destination camera preparation failed.";
			return RuntimePrepareStep::Failed;
		}
		cameraSystem_.Reset();
		postProcessProfileSystem_.Reset(GetSceneDocument());
		debugSystem_.LoadSettings(GetSceneDocument());
		camera_ = camera.release();
		debugCamera_ = debugCamera.release();
		objectSystem_.BeginPreparedModels(*camera_);
		initialPreparationStage_ = InitialPreparationStage::Objects;
		if (budget.metrics) budget.metrics->action = "camera_ready";
		return RuntimePrepareStep::Progress;
	}
	if (initialPreparationStage_ == InitialPreparationStage::Objects) {
		const auto step = objectSystem_.StepPreparedModels(*GetSceneDocument(), physicsSystem_, budget, diagnostic);
		if (step != RuntimePrepareStep::Ready) return step;
		if (budget.metrics) budget.metrics->action = "objects_local_ready";
		if (!budget.allowRuntimeFinalize) return RuntimePrepareStep::WaitingForAssets;
		initialPreparationStage_ = InitialPreparationStage::ModelBindings;
		return RuntimePrepareStep::Progress;
	}
	if (initialPreparationStage_ == InitialPreparationStage::ModelBindings) {
		const auto step = objectSystem_.StepPreparedModelBindings(*GetSceneDocument(), physicsSystem_, budget, diagnostic);
		if (step != RuntimePrepareStep::Ready) return step;
		initialPreparationStage_ = InitialPreparationStage::Player;
		return RuntimePrepareStep::Progress;
	}
	if (initialPreparationStage_ == InitialPreparationStage::Player) {
		InitializeScenePlayer(GetSceneDocument());
		runtimeObjectBindings_.clear();
		runtimeObjectBindings_.reserve(GetSceneDocument()->GetEntities().size());
		initialBindingCursor_ = 0;
		initialPreparationStage_ = InitialPreparationStage::BindingReferences;
		return RuntimePrepareStep::Progress;
	}
	if (initialPreparationStage_ == InitialPreparationStage::BindingReferences) {
		auto& entities = GetSceneDocument()->GetEntities();
		if (initialBindingCursor_ < entities.size()) {
			auto& entity = entities[initialBindingCursor_++];
			auto* runtime = objectSystem_.FindModelRuntime(entity.id);
			if (!runtime || !runtime->object) { diagnostic = "Prepared binding object is missing."; return RuntimePrepareStep::Failed; }
			runtimeObjectBindings_.push_back({entity.id,&entity,runtime->object.get(),runtime->hasCollider ? runtime->collider : nullptr,
				runtime->hasPhysicsBody ? &runtime->physicsBody : nullptr});
			if (budget.metrics) { ++budget.metrics->bound; budget.metrics->action = "binding";
				budget.metrics->lastEntity = entity.id; budget.metrics->lastIndex = initialBindingCursor_-1; }
		} else initialPreparationStage_ = InitialPreparationStage::InitialPhysics;
		return RuntimePrepareStep::Progress;
	}
	if (initialPreparationStage_ == InitialPreparationStage::InitialPhysics) {
		auto* document = GetSceneDocument();
		ApplyInitialBindingPhysics(*document, true);
		attachmentSystem_.Update(*document, objectSystem_, runtimeObjectBindings_);
		cameraSystem_.UpdateTransitionHold(*document, camera_, player_, runtimeObjectBindings_);
		initialPreparationStage_ = InitialPreparationStage::Environment;
		if (budget.metrics) budget.metrics->action = "ordered_bindings_done";
		return RuntimePrepareStep::Progress;
	}
	if (initialPreparationStage_ == InitialPreparationStage::Environment) {
		const auto step = environmentSystem_.StepPreparedSkybox(*GetSceneDocument(), *camera_,
			runtimeObjectBindings_, budget, diagnostic);
		if (step != RuntimePrepareStep::Ready) return step;
		initialPreparationStage_ = InitialPreparationStage::Sprites;
		return RuntimePrepareStep::Progress;
	}
	if (initialPreparationStage_ == InitialPreparationStage::Sprites) {
		const auto step = objectSystem_.StepPreparedSprites(*GetSceneDocument(), budget, diagnostic);
		if (step != RuntimePrepareStep::Ready) return step;
		textRenderSystem_.Initialize(Object3dCommon::GetInstance()->GetDxCommon(),
			GetSceneAssetId() + "_" + std::to_string(GetSceneInstanceId()));
		PrepareGameFlowTextOverrides(false);
		SyncMenuPresentationForOwnDocument();
		initialPreparationStage_ = InitialPreparationStage::Text;
		return RuntimePrepareStep::Progress;
	}
	if (initialPreparationStage_ == InitialPreparationStage::Text) {
		const auto step = textRenderSystem_.StepPreparedText(*GetSceneDocument(), budget, diagnostic);
		if (step != RuntimePrepareStep::Ready) return step;
		initialPreparationStage_ = InitialPreparationStage::Lighting;
		return RuntimePrepareStep::Progress;
	}
	if (initialPreparationStage_ == InitialPreparationStage::Lighting) {
		const auto step = lightingSystem_.StepPrepared(*GetSceneDocument(), budget, diagnostic);
		if (step != RuntimePrepareStep::Ready) return step;
		initialPreparationStage_ = InitialPreparationStage::Boundary;
		return RuntimePrepareStep::Progress;
	}
	if (initialPreparationStage_ == InitialPreparationStage::Boundary) {
		auto* dx = Object3dCommon::GetInstance()->GetDxCommon();
		const auto step = effectRenderSystem_.StepPreparedBoundary(*dx, budget, diagnostic);
		if (step != RuntimePrepareStep::Ready) return step;
		effectRenderSystem_.SetMovementBoundary(physicsSystem_.MovementBoundsFrame(*GetSceneDocument(), false));
		monitorSystem_.Initialize(dx, SrvManager::GetInstance());
		initialPreparationStage_ = InitialPreparationStage::GroundCracks;
		return RuntimePrepareStep::Progress;
	}
	if (initialPreparationStage_ == InitialPreparationStage::GroundCracks) {
		auto* dx = Object3dCommon::GetInstance()->GetDxCommon();
		const auto step = effectRenderSystem_.StepPreparedGroundCracks(*dx, budget, diagnostic);
		if (step != RuntimePrepareStep::Ready) return step;
		initialPreparationStage_ = InitialPreparationStage::Verify;
		return RuntimePrepareStep::Progress;
	}
	if (initialPreparationStage_ == InitialPreparationStage::Verify) {
		const auto& document = *GetSceneDocument();
		for (const auto& entity : document.GetEntities()) {
			if (!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)) continue;
			if (const auto* water = SceneEntityQuery::FindEnabledComponent(entity, "WaterVolume");
				water && water->waterSurfaceEnabled) {
				diagnostic = "Destination water renderer preparation is unsupported.";
				return RuntimePrepareStep::Unsupported;
			}
			if (SceneEntityQuery::FindEnabledComponent(entity, "MonitorRenderer")) {
				diagnostic = "Destination monitor preparation is unsupported.";
				return RuntimePrepareStep::Unsupported;
			}
		}
		initialPreparationStage_ = InitialPreparationStage::Ready;
		if (budget.metrics) budget.metrics->action = "runtime_ready";
	}
	return RuntimePrepareStep::Ready;
}

void RuntimeScene::CancelPreparedRuntime() noexcept {
	// この段階では共有Runtime systemを開始していないため、通常Finalizeは行わない。
	environmentSystem_.Finalize();
	textRenderSystem_.Finalize();
	lightingSystem_.Finalize();
	effectRenderSystem_.Finalize();
	monitorSystem_.Finalize();
	physicsSystem_.Clear();
	runtimeObjectBindings_.clear();
	if (player_) {
		player_->Finalize();
		delete std::exchange(player_, nullptr);
	}
	objectSystem_.Finalize();
	delete std::exchange(debugCamera_, nullptr);
	delete std::exchange(camera_, nullptr);
	initialPreparationStage_ = InitialPreparationStage::Idle;
	initialBindingCursor_ = 0;
}

bool RuntimeScene::TryTakePreparedTextImage(PreparedSceneTextImage& out) {
	return textRenderSystem_.TryTakePreparedImage(out);
}

bool RuntimeScene::AcceptPreparedTextImage(uint64_t id, const std::string& signature, std::string& diagnostic) {
	return textRenderSystem_.AcceptPublishedImage(id, signature, diagnostic);
}

void RuntimeScene::ActivatePreparedRuntime() noexcept {
	Object3dCommon::GetInstance()->SetDefaultCamera(camera_);
	ParticleManager::GetInstance()->SetCamera(camera_);
}

void RuntimeScene::InitializeScenePlayer(SceneDocument* initialDocument) {
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
}

void RuntimeScene::InitializeSceneBindings(SceneDocument& document, bool playing) {
	objectSystem_.BuildBindings(
		document,
		runtimeObjectBindings_
	);
	ApplyInitialBindingPhysics(document, playing);
}

void RuntimeScene::ApplyInitialBindingPhysics(SceneDocument& document, bool playing) {
	auto* owner = document.FindEntityByName("Player");
	if (playing && owner && SceneEntityQuery::FindEnabledComponent(*owner, "PlayerMovementBounds")) {
		// 初回描画と開始案内より前に、Runtimeコピーだけを範囲内へ置く。
		physicsSystem_.SyncSceneSettings(document, player_, runtimeObjectBindings_, false);
		physicsSystem_.Step(player_, runtimeObjectBindings_, 0.0f, true, &document);
		player_->PostPhysicsUpdate();
		if (player_->GetObject()) {
			SynchronizeSceneTransform(owner->transform, player_->GetObject()->GetTransform());
			objectSystem_.RefreshTransforms(document, {owner->id});
		}
	}
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

	InitializeScenePlayer(initialDocument);

	environmentSystem_.Initialize(
		Object3dCommon::GetInstance()->GetDxCommon()
	);
	if (initialDocument) {
		InitializeSceneBindings(*initialDocument, initialPlaying);
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
	if (initialDocument) {
		effectRenderSystem_.SetMovementBoundary(physicsSystem_.MovementBoundsFrame(*initialDocument, initialEditing));
	}
	textRenderSystem_.Initialize(
		Object3dCommon::GetInstance()->GetDxCommon(),
		GetSceneAssetId() + "_" + std::to_string(GetSceneInstanceId())
	);
	PrepareGameFlowTextOverrides(false);

}

void RuntimeScene::ClearUiRuntime() {
	gamePauseSystem_.Clear();
	pendingUiAudioRequests_.clear();
	menuSystem_.Clear(); eventSystem_.ClearMenuInput(); pendingMenuRequests_.clear(); deferredMenuPulses_.clear();
	uiTransitionSource_ = {}; uiLoadReservation_ = kInvalidSceneInstanceId; uiControl_ = {};
	uiTerminalAttempted_ = false;
    replayCompletion_ = {}; replayMenuSource_ = {}; replayMenuDrawToken_ = {};
    replayMenuPresented_ = false;
}
void RuntimeScene::ReportMenuFailure(const std::string& detail) {
	const auto* document = GetSceneDocument();
	preparationDiagnostic_ = {ScenePreparationDiagnosticCode::InvalidReference, document ? document->GetContentGeneration() : 0,
		menuSystem_.GetFrame().panelId, ++preparationDiagnosticSerial_, detail};
	Logger::Log("[MenuRuntime] " + detail + "\n");
}
void RuntimeScene::SyncGamePause(const SceneDocument& document) {
	std::string error;
	const auto invalidRoot = gamePauseSystem_.Sync(document, menuSystem_, sceneManager_ && sceneManager_->CanPauseGame(), error);
	if (invalidRoot) {
		menuSystem_.RemoveBranch(document, invalidRoot);
		gamePauseSystem_.Clear(); eventSystem_.RearmMenuKeys();
		ReportMenuFailure(error);
	}
	uiControl_.gamePaused = gamePauseSystem_.IsPaused();
	uiControl_.pauseBgm = gamePauseSystem_.PausesBgm();
}
void RuntimeScene::ApplyPendingUiAudio() {
	const auto requests = std::exchange(pendingUiAudioRequests_, {});
	if (const auto* document = GetSceneDocument()) audioSystem_.ApplyRequests(*document, requests);
}
void RuntimeScene::ApplyUiRequests(const SceneDocument& document, const std::vector<SceneUiActionRequest>& requests) {
	for (const auto& request : requests) {
		if (uiTransitionSource_.requestSerial || uiTerminalAttempted_) break;
		if (!menuSystem_.IsSourceValid(document, request.source)) continue;
        if (request.origin == SceneUiActionRequest::Origin::ReplayCompletion) {
            // 通常Eventの出所検証を緩めず、自然完了一回から来たOpenだけ追加で許可する。
            SceneDeathReplayStatus status;
            if (!replayCompletion_.deathSerial || request.replayCompletion != replayCompletion_ ||
                !deathReplaySystem_.TryGetStatus(replayCompletion_.owner,status) || status.phase!=SceneDeathReplayPhase::FinishedHold ||
                replayMenuSource_.panelId || request.action.type!="OpenMenu") continue;
            const auto* target=document.FindEntity(request.action.targetEntityId);
            const auto* panel=target && UiEntityActive(document,target) ? SceneEntityQuery::FindEnabledComponent(*target,"MenuPanel") : nullptr;
            std::string issue;
            const auto* pause=SceneGamePauseSystem::FindSettings(document,request.action.targetEntityId,issue);
            const auto* canvas=target ? SceneEntityQuery::FindEnabledComponent(*target,"UICanvas") : nullptr;
            bool selectable=false;
            if (panel) for (uint64_t id : panel->menuItemEntityIds) {
                const auto* itemOwner=document.FindEntity(id);
                const auto* item=itemOwner && UiEntityActive(document,itemOwner) ? SceneEntityQuery::FindEnabledComponent(*itemOwner,"MenuItem") : nullptr;
                selectable |= item && item->menuInteractable;
            }
            if (!panel || panel->menuInitiallyOpen || pause || !issue.empty() || !canvas || canvas->uiRenderSpace!="ScreenOverlay" || !selectable) {
                ReportMenuFailure("MenuRuntime:ReplayPanel"); continue;
            }
        } else if (deathReplayStarted_.serial && replayHistory_.Status().phase!=SceneReplayHistoryPhase::Idle) {
            SceneDeathReplayStatus status;
            if (deathReplaySystem_.TryGetStatus(deathReplayStarted_.owner,status) &&
                status.phase!=SceneDeathReplayPhase::Waiting && status.phase!=SceneDeathReplayPhase::Failed &&
                (!replayMenuPresented_ || request.source.panelId!=replayMenuSource_.panelId ||
                 request.source.openSerial!=replayMenuSource_.openSerial)) continue;
        }
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
		uint64_t targetPanelId = action.targetEntityId;
		if (action.type == "PauseGame" || action.type == "ResumeGame") {
			const auto* target = document.FindEntity(action.targetEntityId);
			const auto* pause = target && UiEntityActive(document, target) ? SceneEntityQuery::FindEnabledComponent(*target, "GamePause") : nullptr;
			if (!pause) { ReportMenuFailure("MenuRuntime:PauseInvalidPanel"); continue; }
			targetPanelId = pause->gamePausePanelEntityId;
		}
		if (deathSystem_.IsDefeated() && (action.type == "PauseGame" || action.type == "OpenMenu")) {
			const auto* panel = document.FindEntity(targetPanelId);
			if (action.type == "PauseGame" || (panel && SceneEntityQuery::FindEnabledComponent(*panel, "GamePause"))) {
				ReportMenuFailure("DeathResponse:PauseBlocked"); continue;
			}
		}
		SceneMenuCommandType type{};
		bool menuCommand = true;
		if (action.type == "OpenMenu") type = SceneMenuCommandType::Open;
		else if (action.type == "CloseMenu") type = SceneMenuCommandType::Close;
		else if (action.type == "SelectPreviousMenuItem") type = SceneMenuCommandType::Previous;
		else if (action.type == "SelectNextMenuItem") type = SceneMenuCommandType::Next;
		else if (action.type == "ConfirmMenuItem") type = SceneMenuCommandType::Confirm;
		else if (action.type == "CancelMenu") type = SceneMenuCommandType::Cancel;
		else menuCommand = false;
		if (action.type == "PauseGame") { type = SceneMenuCommandType::Open; menuCommand = true; }
		if (action.type == "ResumeGame") {
			// 互換のResumeはその理由を所有するrootだけを対象にし、上の確認も閉じる。
			if (menuSystem_.GetOpenSource(targetPanelId).panelId) menuSystem_.RemoveBranch(document, targetPanelId);
			SyncGamePause(document); eventSystem_.RearmMenuKeys(); continue;
		}
		if (menuCommand) {
			if (type == SceneMenuCommandType::Open) {
				std::string error;
				const auto* setting = SceneGamePauseSystem::FindSettings(document, targetPanelId, error);
				if (setting && !sceneManager_->CanPauseGame()) error = "MenuRuntime:PauseUnsupportedScene";
				if (!error.empty()) { ReportMenuFailure(error); continue; }
			}
			if (!menuSystem_.Apply(document, SceneMenuCommand{request.source, type, targetPanelId})) {
				const auto issue = menuSystem_.ConsumeDiagnostic();
				ReportMenuFailure(issue.empty() ? "MenuRuntime:InvalidPanel" : issue);
			}
            if (request.origin==SceneUiActionRequest::Origin::ReplayCompletion) {
                const auto opened=menuSystem_.GetSource();
                if (opened.panelId==targetPanelId) {
                    replayMenuSource_=opened; replayMenuPresented_=false;
                    eventSystem_.RearmMenuKeys();
                }
            }
			SyncGamePause(document);
		} else if (action.type == "ExitGame") {
			uiTerminalAttempted_ = true;
			eventSystem_.RearmMenuKeys();
			if (!sceneManager_->RequestGameExit(GetSceneInstanceId(), document.GetContentGeneration()))
				ReportMenuFailure("MenuRuntime:ExitRejected");
			break;
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
				pendingUiAudioRequests_.push_back({kind, target->id});
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
		SyncGamePause(document);
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
	SyncGamePause(*document);
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
    // 完了は世界停止中もUI側で消費する。非表示/Pause中はSequenceに保留したままにする。
    if (allowed && replayPresentationAllowed_ && !uiTransitionSource_.requestSerial && deathReplaySystem_.ConsumeReplayCompleted()) {
        replayCompletion_={deathReplayStarted_.instance,deathReplayStarted_.generation,deathReplayStarted_.owner,deathReplayStarted_.serial};
        const auto* replayOwner=document->FindEntity(replayCompletion_.owner);
        if (replayCompletion_.instance==GetSceneInstanceId() && replayCompletion_.generation==document->GetContentGeneration() &&
            replayOwner && UiEntityActive(*document,replayOwner) && SceneEntityQuery::FindEnabledComponent(*replayOwner,"DeathReplay")) {
            std::string diagnostic;
            const auto requests=eventSystem_.EvaluateReplayCompletion(*document,menuSystem_.GetSource(),replayCompletion_,diagnostic);
            if (!diagnostic.empty()) ReportMenuFailure(diagnostic);
            else ApplyUiRequests(*document,requests);
        }
    }
    if (deathReplayStarted_.serial && replayHistory_.Status().phase!=SceneReplayHistoryPhase::Idle) {
        SceneDeathReplayStatus replayStatus;
        if (deathReplaySystem_.TryGetStatus(deathReplayStarted_.owner,replayStatus) &&
            replayStatus.phase!=SceneDeathReplayPhase::Waiting && replayStatus.phase!=SceneDeathReplayPhase::Failed) {
            const auto current=menuSystem_.GetSource();
            allowed = allowed && replayPresentationAllowed_ && replayMenuPresented_ && current.panelId==replayMenuSource_.panelId &&
                current.openSerial==replayMenuSource_.openSerial && current.generation==replayMenuSource_.generation;
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
	SyncGamePause(*document);
	if (const auto issue = menuSystem_.ConsumeDiagnostic(); !issue.empty()) ReportMenuFailure(issue);
}
void RuntimeScene::SyncMenuPresentation() {
	const auto* context = sceneManager_ ? sceneManager_->GetExecutionContext() : nullptr;
	auto* document = GetSceneDocument();
	if (!document || !sceneManager_ || GetSceneInstanceId() != sceneManager_->GetActiveSceneInstanceId() || (context && context->IsEditing())) {
		ClearUiRuntime(); textRenderSystem_.SetMenuColorOverrides({}); return;
	}
	SyncMenuPresentationForOwnDocument();
}

void RuntimeScene::SyncMenuPresentationForOwnDocument() {
	const auto* document = GetSceneDocument();
	// 通常Eventがactive/参照を変更した後、文字cache同期前に再検証する。キーは再評価しない。
	menuSystem_.Sync(*document, GetSceneInstanceId());
	textRenderSystem_.SetMenuColorOverrides(menuSystem_.GetFrame().textColors);
	if (const auto issue = menuSystem_.ConsumeDiagnostic(); !issue.empty()) ReportMenuFailure(issue);
}

void RuntimeScene::PrepareGameFlowTextOverrides(bool reset) {
	const SceneDocument* document = GetSceneDocument();
	const auto* context = sceneManager_ ? sceneManager_->GetExecutionContext() : nullptr;
	if (!document || (context && context->IsEditing())) {
		if (gameFlowTextDocumentGeneration_ != 0) textRenderSystem_.ClearTextOverrides();
		gameFlowTextDocumentGeneration_ = 0;
		return;
	}
	const uint64_t generation = document->GetContentGeneration();
	if (!reset && gameFlowTextDocumentGeneration_ == generation) return;
	// 保存文字や有効状態を書き換えず、旧Documentの同IDにも表示を持ち越さない。
	if (gameFlowTextDocumentGeneration_ != generation) textRenderSystem_.ClearTextOverrides();
	for (const SceneEntity& entity : document->GetEntities()) {
		for (const SceneComponent& component : entity.components) {
			if (!component.enabled || component.type != "GameFlowDirector") continue;
			for (uint64_t id : {component.gameFlowIntroTextEntityId, component.gameFlowCountdownTextEntityId,
				component.gameFlowPhaseTextEntityId, component.gameFlowTimerTextEntityId,
				component.gameFlowRemainingTextEntityId, component.gameFlowResultTimeTextEntityId}) {
				textRenderSystem_.SetTextOverride(id, {});
			}
		}
	}
	gameFlowTextDocumentGeneration_ = generation;
}

void RuntimeScene::ReportPreparationDiagnostic(const ScenePreparationDiagnostic& diagnostic, uint64_t& sourceSerial) {
	if (diagnostic.code == ScenePreparationDiagnosticCode::None || !diagnostic.serial || diagnostic.serial == sourceSerial) return;
	sourceSerial = diagnostic.serial;
	preparationDiagnostic_ = diagnostic;
	preparationDiagnostic_.serial = ++preparationDiagnosticSerial_;
	Logger::Log("[ScenePreparation] " + diagnostic.detail + "\n");
}

void RuntimeScene::ReportSceneRestartFailure(const std::string& detail) {
	const auto* document = GetSceneDocument();
	preparationDiagnostic_ = {ScenePreparationDiagnosticCode::InvalidReference,
		document ? document->GetContentGeneration() : 0, 0, ++preparationDiagnosticSerial_, detail};
	Logger::Log("[SceneRestart] " + detail + "\n");
}

void RuntimeScene::Update(float deltaTime)
{
	if (sceneManager_ && sceneManager_->IsGameExitPending()) return;
	if (initialPreparationStage_ == InitialPreparationStage::Ready) {
		// 設定変更時にログを確保するsetterは無割当のCommitから外す。
		ParticleManager::GetInstance()->SetGpuParticleEnabled(true);
		initialPreparationStage_ = InitialPreparationStage::Idle;
	}
	++replayUpdateSerial_;
	ApplyPendingUiAudio();
	// 成功時はこのRuntime自体が置換される。失敗時だけ生存側が結果を一回消費する。
	if (sceneManager_ && pendingRestartSceneInstanceId_ != kInvalidSceneInstanceId) {
		SceneLoadOutcome outcome;
		if (sceneManager_->ConsumeLoadOutcome(pendingRestartSceneInstanceId_, outcome)) {
			pendingRestartSceneInstanceId_ = kInvalidSceneInstanceId;
			if (outcome.status == SceneLoadStatus::Failed)
				ReportSceneRestartFailure("SceneRestart:LoadFailed " + outcome.diagnostic);
		}
	}
	SceneExecutionContext* executionContext = sceneManager_
		? sceneManager_->GetExecutionContext()
		: nullptr;
	const bool editing = executionContext && executionContext->IsEditing();
	const bool playing = !executionContext || executionContext->IsPlaying();
	SceneDocument* activeDocument = GetSceneDocument();
	if (activeDocument && deathDocumentGeneration_ != activeDocument->GetContentGeneration()) {
		// 内容差替え後の同じIDへ旧死亡flag/要求を持ち越さない。
		statSystem_.Clear(); healthSystem_.Clear(); deathSystem_.Clear(); ClearDeathWorldContinuation();
		pendingDeathActions_.clear(); pendingDeathGeneration_ = 0;
		deathTextEntities_.clear(); deathTransformEntities_.clear(); heldDeathFlow_ = {};
		deathDocumentGeneration_ = activeDocument->GetContentGeneration();
	}
	if (!activeDocument || (spawnFrame_.documentGeneration != 0 &&
		spawnFrame_.documentGeneration != activeDocument->GetContentGeneration())) {
		enemySpawnerSystem_.Clear();
		spawnFrame_ = {};
		preparationDiagnostic_ = {};
		flowDiagnosticSerial_ = spawnerDiagnosticSerial_ = 0;
		objectSystem_.SetSpawnPresentationFrame({});
	}
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
	if (activeDocument && playing) deathSystem_.Observe(*activeDocument, statSystem_);
	const bool defeatedAtFrameStart = deathSystem_.IsDefeated();
	if (deathWorldInstance_ != 0 && (!activeDocument || deathWorldInstance_ != GetSceneInstanceId() ||
		deathWorldGeneration_ != activeDocument->GetContentGeneration())) ClearDeathWorldContinuation();
	if (deathReplayStarted_.serial) {
		const auto* owner = activeDocument ? activeDocument->FindEntity(deathReplayStarted_.owner) : nullptr;
		const auto* response = owner ? SceneEntityQuery::FindEnabledComponent(*owner, "DeathResponse") : nullptr;
		SceneDeathReplayStatus status;
		if (deathReplaySystem_.TryGetStatus(deathReplayStarted_.owner, status) && status.phase != SceneDeathReplayPhase::Failed &&
			(!owner || !SceneEntityQuery::IsEntityActiveInHierarchy(*activeDocument, *owner) ||
			!SceneEntityQuery::FindEnabledComponent(*owner, "DeathReplay") || !response || response->deathResponseScope != "Scene" ||
			!SceneEntityQuery::FindEnabledComponent(*owner, "Health")))
			FailDeathReplay(deathReplayStarted_, "DeathResponse:ReplayOwnerLost");
	}
	// t=0未採取のままWorldを進めると開始姿勢を復元できないため、その前に停止する。
	if (replayBeginPending_ && playing && std::isfinite(deltaTime) && deltaTime > 0)
		FailDeathReplay(deathReplayStarted_, "DeathResponse:HistoryMissingStart");
	if (replayHistory_.Status().phase == SceneReplayHistoryPhase::Recording &&
		(!sceneManager_ || sceneManager_->GetActiveSceneInstanceId() != GetSceneInstanceId()))
		FailDeathReplay(deathReplayStarted_, "DeathResponse:HistoryIdentity");
	// Phaseは最終stepを返す時点でFrozenになる。正のstepを先に全consumerへ渡す。
	const SceneDeathWorldStep deathWorldStep = playing && deathWorldInstance_ != 0
		? deathWorldClock_.Advance(deltaTime) : SceneDeathWorldStep{};
	deathReplaySystem_.AdvancePresentation(deathWorldStep);
	if (replayHistory_.Status().phase == SceneReplayHistoryPhase::Recording) {
		replayRealSeconds_ += deathWorldStep.elapsedRealDelta;
		replayFinalPending_ = replayFinalPending_ || deathWorldStep.freezeAfterStep;
	}
	const bool deathWorldMoving = deathWorldStep.worldDelta > 0.0f;
	// 最終stepは既にFrozenでも正の秒を持つ。query/Drawでは加算しない。
	if (deathWorldMoving) deathWorldElapsedSeconds_ += static_cast<double>(deathWorldStep.worldDelta);
	std::vector<SceneRuntimeObjectBinding> deathWorldBindings;
	std::vector<uint64_t> deathAttackEnded, deathReactionEnded;
	std::unordered_set<uint64_t> deathReactionOwned;

	std::vector<uint64_t> spawnerResetEntityIds;
	const float gameplayDeltaTime = playing && deathSystem_.IsDefeated() ? 0.0f : playing
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
			PrepareGameFlowTextOverrides(false);
			textRenderSystem_.Sync(activeDocument, editing ? nullptr : &statSystem_);
			sceneManager_->ChangeScene(targetSceneId);
			return;
		}
	}
	SceneGameFlowResult gameFlowResult{};
	if (activeDocument && playing && !deathSystem_.IsDefeated()) {
		gameFlowResult = gameFlowSystem_.Update(
			*activeDocument,
			enemySpawnerSystem_,
			deltaTime
		);
		ReportPreparationDiagnostic(gameFlowResult.diagnostic, flowDiagnosticSerial_);
		for (const SceneGameFlowEntityRequest& request : gameFlowResult.entityRequests) {
			if (SceneEntity* entity = activeDocument->FindEntity(request.entityId)) {
				entity->active = request.active;
			}
		}
		for (const SceneGameFlowWaveRequest& request : gameFlowResult.waveRequests) {
			enemySpawnerSystem_.BeginFiniteWave(
				request.spawnerEntityId,
				request.generation,
				request.count,
				request.holdUntilGameplay
			);
		}
		for (const SceneGameFlowMotionRequest& request : gameFlowResult.motionRequests) {
			if (request.type == SceneGameFlowMotionRequestType::Reset) textMotionSystem_.Reset(request.entityId);
			else textMotionSystem_.Play(*activeDocument, request.entityId, request.clipId);
		}
	} else if (!playing) {
		gameFlowSystem_.Clear();
	} else { gameFlowResult = heldDeathFlow_; gameFlowResult.gameplayAllowed = false; }
	// pulseをheldDeathFlow/finalizeへ持ち越さない。Scene交換時もローカル値と共に破棄する。
	const uint64_t startedDirectorId = std::exchange(gameFlowResult.startedDirectorEntityId, uint64_t{0});
	const uint64_t startedGeneration = std::exchange(gameFlowResult.startedDocumentGeneration, uint64_t{0});
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

	if (!playing || !deathSystem_.IsDefeated()) particleSystem_.Update(
		runtimeSceneId,
		editing
	);
	effectRenderSystem_.Update(deathSystem_.IsDefeated() ? 0.0f : deltaTime);
	// 最後の正stepも水面へ渡す。Frozenの描画同期は後段のSyncで継続する。
	environmentSystem_.Update(playing && deathSystem_.IsDefeated() ? deathWorldStep.worldDelta : deltaTime);

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
		// Statのinactive除去後にPoolを復元し、同じFrameの全consumerへ参加制限を渡す。
		if (!deathSystem_.IsDefeated()) enemySpawnerSystem_.Update(*activeDocument, SceneSpawnUpdateContext{
			gameplayDeltaTime, deltaTime, gameFlowResult.gameplayAllowed, gameFlowResult.preparationSpawnerIds, deathSystem_.Blocked()
		});
		spawnerResetEntityIds = enemySpawnerSystem_.ConsumeResetEntityIds();
		const std::unordered_set<uint64_t> resetIds(spawnerResetEntityIds.begin(), spawnerResetEntityIds.end());
		prefabAnimationSystem_.DiscardPresentationsForEntities(resetIds);
		for (uint64_t entityId : spawnerResetEntityIds) {
			attackRunnerSystem_.ResetEntity(*activeDocument, entityId);
			stateMachineSystem_.ResetEntity(entityId);
			prefabAnimationSystem_.ResetEntity(entityId);
			enemySystem_.ResetEntity(entityId);
			combatSystem_.ResetEntity(entityId);
			hitReactionSystem_.ResetEntity(entityId);
			eventSystem_.ResetEntity(entityId);
			healthSystem_.ResetEntity(entityId);
			statSystem_.ResetEntity(entityId); deathSystem_.ResetEntity(entityId);
			deathWorldParticipants_.erase(entityId); deathWorldBaseVelocities_.erase(entityId);
			objectSystem_.ResetDeathContinuationEntity(entityId);
			objectSystem_.ResetReplayEntity(entityId);
			deathTextEntities_.erase(entityId); deathTransformEntities_.erase(entityId);
			// Poolの新しい実体へ旧要求を持ち越さない。発生元が残る場合は失敗を保持する。
			std::erase_if(pendingDeathActions_, [&](const auto& a) {
				if (a.owner == entityId) return true;
				if (a.action.targetEntityId != entityId) return false;
				if (!resetIds.contains(a.owner)) {
					deathSystem_.SetStatus(a.owner, 4);
					preparationDiagnostic_ = {ScenePreparationDiagnosticCode::InvalidReference,
						activeDocument->GetContentGeneration(), a.owner, ++preparationDiagnosticSerial_,
						"DeathResponse:StartFailed " + a.action.type + " " + std::to_string(entityId)};
				}
				return true;
			});
		}
		if (!spawnerResetEntityIds.empty()) statSystem_.Update(*activeDocument);
		deathSystem_.Sync(*activeDocument, statSystem_, GetSceneInstanceId());
		// Event等で確定した死亡も、演出が姿勢を取得する前に攻撃所有分を返却する。
		objectSystem_.RefreshTransforms(*activeDocument,enemySystem_.FinishChargeCombat(*activeDocument,statSystem_,{}));
		ApplyDeathActions(*activeDocument, true);
		spawnFrame_ = enemySpawnerSystem_.BuildPresentationFrame(*activeDocument, gameFlowResult.gameplayAllowed);
		spawnFrame_.blockedEntityIds.insert(deathSystem_.Blocked().begin(), deathSystem_.Blocked().end());
		hitReactionSystem_.DiscardBlocked(deathSystem_.IsDefeated()
			? deathSystem_.TargetBlocked() : deathSystem_.Blocked());
		healthSystem_.Advance(*activeDocument, gameFlowResult.gameplayAllowed ? gameplayDeltaTime : 0.0f, spawnFrame_.blockedEntityIds);
		ReportPreparationDiagnostic(enemySpawnerSystem_.GetPreparationDiagnostic(), spawnerDiagnosticSerial_);
		// 前Frame Eventの順を保ち、active化と時刻0 PoseをObject同期より前にまとめる。
		// 内容差替え後の同IDへ古い要求を適用しない。Persistent detachは同世代を維持する。
		if (pendingPresentationDocumentGeneration_ != 0 &&
			pendingPresentationDocumentGeneration_ == activeDocument->GetContentGeneration() && !deathSystem_.IsDefeated()) {
			for (const ScenePresentationRequest& request : pendingPresentationRequests_) {
				if (request.type == ScenePresentationRequestType::Play) {
					prefabAnimationSystem_.PlayPresentation(*activeDocument, request.entityId, request.clipName, spawnFrame_.blockedEntityIds);
				} else if (request.type == ScenePresentationRequestType::Stop) {
					prefabAnimationSystem_.StopPresentation(request.entityId);
				} else {
					prefabAnimationSystem_.ResetPresentation(*activeDocument, request.entityId, spawnFrame_.blockedEntityIds);
				}
			}
		}
		pendingPresentationRequests_.clear();
		pendingPresentationDocumentGeneration_ = 0;
		// 演出だけ通常deltaTimeで更新し、GameFlow permission/HitStopから独立させる。
		prefabAnimationSystem_.UpdatePresentations(*activeDocument, deltaTime,
			enemySpawnerSystem_.BuildPresentationFrame(*activeDocument, gameFlowResult.gameplayAllowed).blockedEntityIds,
			deathSystem_.Blocked(), deathTransformEntities_);
		if (gameFlowResult.gameplayAllowed) {
			hitReactionSystem_.AdvanceRecoveries(statSystem_, gameplayDeltaTime);
			attackRunnerSystem_.Advance(
				*activeDocument,
				prefabAnimationSystem_,
				gameplayDeltaTime,
				spawnFrame_.blockedEntityIds
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
			prefabAnimationSystem_.Update(*activeDocument, gameplayDeltaTime, spawnFrame_.blockedEntityIds);
		}
	} else {
		audioSystem_.Clear();
		statSystem_.Clear();
		deathSystem_.Clear(); ClearDeathWorldContinuation(); pendingDeathActions_.clear(); pendingDeathGeneration_ = 0; deathTextEntities_.clear(); deathTransformEntities_.clear(); heldDeathFlow_ = {};
		healthSystem_.Clear();
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
		spawnFrame_ = {};
		preparationDiagnostic_ = {};
		flowDiagnosticSerial_ = spawnerDiagnosticSerial_ = 0;
		attachmentSystem_.Clear(&objectSystem_);
	}

	SceneSpawnFrame worldFrame;
	if (activeDocument && deathWorldMoving) {
		worldFrame = spawnFrame_;
		worldFrame.blockedEntityIds.clear();
		for (const auto& entity : activeDocument->GetEntities()) {
			if (!deathWorldParticipants_.contains(entity.id) || deathSystem_.TargetBlocked().contains(entity.id) ||
				!SceneEntityQuery::IsEntityActiveInHierarchy(*activeDocument, entity)) worldFrame.blockedEntityIds.insert(entity.id);
		}
		attackRunnerSystem_.AdvanceDeathContinuation(*activeDocument, deathWorldStep.worldDelta);
		// Particle時計の接続前は表示要求を発生させず、このFrameで破棄する。
		attackRunnerSystem_.ConsumeDeathEffectRequests();
		prefabAnimationSystem_.UpdateDeathContinuation(*activeDocument, deathWorldStep.worldDelta, worldFrame.blockedEntityIds);
	}
	// Objectが実体を所有し、以降のSystemは再構築したbindingsだけを借用する。
	objectSystem_.SetSpawnPresentationFrame(deathWorldMoving ? worldFrame : spawnFrame_);
	objectSystem_.SyncModels(
		activeDocument,
		physicsSystem_,
		deathWorldMoving ? deathWorldStep.worldDelta : gameplayDeltaTime,
		playing,
		editing
	);
	// 値形状の寿命は呼出元の同期点まで。Agent用と攻撃直前用を別採取し、古い通路を許可に使わない。
	auto captureAttackProbe = [&]() {
		if (!activeDocument) return SceneMovementProbeSnapshot{};
		std::vector<uint64_t> roots;
		for (const auto& request : enemySystem_.PositioningRequests()) if (request.canMove && request.targetId) {
			roots.push_back(request.entityId); roots.push_back(request.targetId);
		}
		if (roots.empty()) return SceneMovementProbeSnapshot{};
		std::sort(roots.begin(), roots.end()); roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
		// Playerは入力更新で実Objectの姿勢を先に変える。採取用の再反映で古いEntity姿勢へ戻さない。
		SceneEntity* playerEntity = player_ ? activeDocument->FindEntityByName("Player") : nullptr;
		Object3d* playerObject = player_ ? player_->GetObject() : nullptr;
		if (playerEntity && playerObject && objectSystem_.FindObject(playerEntity->id) == playerObject) {
			SynchronizeSceneTransform(playerEntity->transform, playerObject->GetTransform());
		}
		objectSystem_.RefreshTransforms(*activeDocument, roots);
		return physicsSystem_.CaptureMovementProbeSnapshot(*activeDocument, gameplayObjectBindings_, player_, enemySystem_.PositioningFrameToken());
	};
	if (activeDocument) {
		objectSystem_.BuildBindings(
			*activeDocument,
			runtimeObjectBindings_
		);
		gameplayObjectBindings_.clear();
		for (const auto& binding : runtimeObjectBindings_) {
			if (!spawnFrame_.blockedEntityIds.contains(binding.entityId)) gameplayObjectBindings_.push_back(binding);
		}
		if (deathWorldMoving) {
			for (auto binding : runtimeObjectBindings_) {
				if (worldFrame.blockedEntityIds.contains(binding.entityId)) continue;
				if (binding.entity && SceneEntityQuery::HasComponent(*binding.entity, "PlayerBehavior") && player_ && binding.entity == activeDocument->FindEntityByName("Player"))
					binding.body = &player_->GetPhysicsBody();
				deathWorldBindings.push_back(binding);
			}
		}
		physicsSystem_.SyncSceneSettings(
			*activeDocument,
			player_,
			deathWorldMoving ? deathWorldBindings : gameplayObjectBindings_,
			editing
		);
		physicsSystem_.ResetBodies(
			runtimeObjectBindings_,
			spawnerResetEntityIds
		);
		if (playing && gameFlowResult.gameplayAllowed && gameplayDeltaTime > 0.0f) {
			// Physicsが使用する実Playerと同じScene対応を渡す。別のPlayerBehaviorを敵だけで選ばない。
			const auto* enemyPlayer = player_ ? activeDocument->FindEntityByName("Player") : nullptr;
			const uint64_t enemyPlayerId = enemyPlayer && SceneEntityQuery::FindEnabledComponent(*enemyPlayer, "PlayerBehavior") ? enemyPlayer->id : 0;
			enemySystem_.Update(
				*activeDocument,
				gameplayObjectBindings_,
				statSystem_,
				prefabAnimationSystem_,
				hitReactionSystem_,
				stateMachineSystem_,
				gameplayDeltaTime,
				enemyPlayerId
			);
			const auto attackProbe = captureAttackProbe();
			enemySystem_.PrepareAttackApproaches(*activeDocument, attackProbe, gameplayDeltaTime);
			// 新方式は値要求から位置取り速度を決める。従来GroundXZは既存の離隔加算を維持する。
			agentSystem_.Update(
				*activeDocument,
				gameplayObjectBindings_,
				gameplayDeltaTime,
				enemySystem_.PositioningRequests(), &attackProbe
			);
			enemySystem_.AcceptPositioningResults(agentSystem_.PositioningResults());
			enemySystem_.ApplyMovementStops(gameplayObjectBindings_);
			projectileSystem_.Update(
				*activeDocument,
				gameplayObjectBindings_,
				gameplayDeltaTime
			);
		}
	} else {
		audioSystem_.Clear();
		runtimeObjectBindings_.clear();
		gameplayObjectBindings_.clear();
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
		deathSystem_.Clear(); ClearDeathWorldContinuation(); pendingDeathActions_.clear(); pendingDeathGeneration_ = 0; deathTextEntities_.clear(); deathTransformEntities_.clear(); heldDeathFlow_ = {};
		healthSystem_.Clear();
		physicsSystem_.Clear();
		cameraSystem_.Reset();
	}

	const auto* playerEntityForDeath = activeDocument ? activeDocument->FindEntityByName("Player") : nullptr;
	const bool playerDead = deathSystem_.IsDefeated() || (playerEntityForDeath && deathSystem_.IsBlocked(playerEntityForDeath->id));
	// Camera入力、Player移動、Physics、追従Cameraの順序は相互依存を持つ。
	if (activeDocument) {
		if (deathWorldMoving) cameraSystem_.UpdateDeathBeforeSimulation(
			*activeDocument, camera_, player_, runtimeObjectBindings_, deathWorldStep.worldDelta);
		else if (!deathSystem_.IsDefeated()) cameraSystem_.UpdateBeforeSimulation(
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
    // 見た目ObjectのBodyではなく、Physicsが進める実Player BodyをReactionだけへ渡す。
	std::vector<SceneRuntimeObjectBinding> reactionBindings = gameplayObjectBindings_;
	for (auto& binding : reactionBindings) if (player_ && playerEntityForDeath && binding.entityId == playerEntityForDeath->id &&
		SceneEntityQuery::FindEnabledComponent(*playerEntityForDeath, "PlayerBehavior")) binding.body = &player_->GetPhysicsBody();
	std::vector<SceneControlledMotionResult> chargeResults;
	std::vector<SceneEnemyContactRequest> chargeContacts;
	std::vector<SceneHitReactionDecision> reactionDecisions;
	Vector3 playerAttackInputDirection{};
	if (player_ && playing && !playerDead) {
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
			gameplayObjectBindings_,
			player_,
			attackRunnerSystem_,
			prefabAnimationSystem_,
			gameplayDeltaTime,
			spawnFrame_.blockedEntityIds,
			!uiGameplayInputBlocked_
		);
		attackRunnerSystem_.ApplyMotion(
			*activeDocument,
			gameplayObjectBindings_,
			player_,
			playerAttackInputDirection,
			gameplayDeltaTime
		);
		// Combat後に予約した被弾速度を、AI/Agent/Stateの書込み後に上書きする。
		hitReactionSystem_.ApplyMotionOverrides(
			*activeDocument,
			reactionBindings,
			gameplayDeltaTime
		);
	}
	if (activeDocument && playing && gameFlowResult.gameplayAllowed && gameplayDeltaTime > 0.0f) {
		const auto attackProbe = captureAttackProbe();
		enemySystem_.PrepareChargeFrame(*activeDocument, gameplayObjectBindings_, statSystem_, prefabAnimationSystem_,
			hitReactionSystem_, stateMachineSystem_, gameplayDeltaTime, false, &attackProbe);
	}
	if (activeDocument && deathWorldMoving) {
		for (const auto& binding : deathWorldBindings) {
			const auto base = deathWorldBaseVelocities_.find(binding.entityId);
			if (binding.body && base != deathWorldBaseVelocities_.end()) {
				binding.body->velocity.x = base->second.x; binding.body->velocity.z = base->second.z;
			}
			if (hitReactionSystem_.IsKnockbackActive(binding.entityId)) deathReactionOwned.insert(binding.entityId);
		}
		deathAttackEnded = attackRunnerSystem_.ApplyDeathContinuationMotion(*activeDocument, deathWorldBindings, deathWorldStep.worldDelta);
		deathReactionEnded = hitReactionSystem_.ApplyDeathContinuationMotion(*activeDocument, deathWorldBindings, deathWorldStep.worldDelta);
		projectileSystem_.UpdateDeathContinuation(*activeDocument, deathWorldBindings, deathWorldStep.worldDelta);
		// 全Projectileを除外し、未初期化弾もPhysics経由で飛び始めさせない。
		std::vector<SceneRuntimeObjectBinding> physical;
		for (const auto& binding : deathWorldBindings) {
			if (binding.entity && SceneEntityQuery::IsEntityActiveInHierarchy(*activeDocument, *binding.entity) &&
				!SceneEntityQuery::FindEnabledComponent(*binding.entity, "Projectile")) physical.push_back(binding);
		}
		const bool movePlayer = player_ && std::any_of(physical.begin(), physical.end(), [&](const auto& b) { return b.entity == playerEntityForDeath; });
		enemySystem_.PrepareChargeFrame(*activeDocument, physical, statSystem_, prefabAnimationSystem_,
			hitReactionSystem_, stateMachineSystem_, deathWorldStep.worldDelta, true);
		std::vector<uint64_t> chargeRoots;
		for(const auto& motion:enemySystem_.ChargeMotions())chargeRoots.push_back(motion.ownerId);
		objectSystem_.RefreshTransforms(*activeDocument,chargeRoots);
		physicsSystem_.Step(movePlayer ? player_ : nullptr, physical, deathWorldStep.worldDelta, true,
			activeDocument, enemySystem_.ChargeMotions(), &chargeResults);
		enemySystem_.AcceptChargePhysics(*activeDocument, chargeResults);
	}
	if (activeDocument && (!playing || gameplayDeltaTime > 0.0f)) {
		if(activeDocument && gameFlowResult.gameplayAllowed) {
			std::vector<uint64_t> chargeRoots;
			for(const auto& motion:enemySystem_.ChargeMotions())chargeRoots.push_back(motion.ownerId);
			objectSystem_.RefreshTransforms(*activeDocument,chargeRoots);
		}
		physicsSystem_.Step(
			playerDead ? nullptr : player_,
			gameplayObjectBindings_,
			gameplayDeltaTime,
			playing, activeDocument,
			gameFlowResult.gameplayAllowed ? enemySystem_.ChargeMotions() : std::vector<SceneControlledMotion>{}, &chargeResults
		);
		if (playing && gameFlowResult.gameplayAllowed) {
			enemySystem_.AcceptChargePhysics(*activeDocument, chargeResults);
			for (const auto& result : chargeResults) for (const auto& contact : result.contacts)
				chargeContacts.push_back({activeDocument->GetContentGeneration(), contact.ownerId, contact.serial,
					contact.hitBoxId, contact.hurtBoxId, contact.fraction, contact.position, contact.normal});
		}
	}
	if (player_ && playing && ((!playerDead && gameplayDeltaTime > 0.0f) ||
		(deathWorldMoving && std::any_of(deathWorldBindings.begin(), deathWorldBindings.end(), [&](const auto& b) { return b.entity == playerEntityForDeath; })))) {
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
	if (activeDocument && playing) {
		// 初期準備でも時刻0の骨へ追従する。参加制限はCombat側に残す。
		attachmentSystem_.Update(
			*activeDocument,
			objectSystem_,
			runtimeObjectBindings_
		);
	}
	if (deathWorldMoving) {
		std::unordered_set<uint64_t> ended(deathReactionEnded.begin(), deathReactionEnded.end());
		for (uint64_t id : deathAttackEnded) {
			deathWorldBaseVelocities_[id].x = deathWorldBaseVelocities_[id].z = 0.0f;
			if (!deathReactionOwned.contains(id)) ended.insert(id);
		}
		for (uint64_t id : ended) {
			deathWorldBaseVelocities_[id].x = deathWorldBaseVelocities_[id].z = 0.0f;
			for (const auto& binding : deathWorldBindings) if (binding.entityId == id && binding.body)
				binding.body->velocity.x = binding.body->velocity.z = 0.0f;
		}
	}
	if (activeDocument && playing && gameFlowResult.gameplayAllowed && gameplayDeltaTime > 0.0f) {
		combatSystem_.Update(
			*activeDocument,
			gameplayObjectBindings_,
			statSystem_,
			healthSystem_,
			deathSystem_,
			gameplayDeltaTime,
			spawnFrame_.blockedEntityIds, chargeContacts, enemySystem_.ChargeHitBoxes(),
			[&](const SceneCombatHitEvent& hit) {
				reactionDecisions.push_back(enemySystem_.EvaluateAcceptedHit(*activeDocument, hit));
			}
		);
		objectSystem_.RefreshTransforms(*activeDocument,enemySystem_.FinishChargeCombat(*activeDocument,statSystem_,reactionBindings));
		std::vector<SceneCombatHitEvent> hitEvents = combatSystem_.ConsumeHitEvents();
		for (const SceneCombatHitEvent& hitEvent : hitEvents) {
			hitStopSystem_.Request(hitEvent.hitStopDuration);
		}
		if (!deathSystem_.IsDefeated()) runtimeEffectSystem_.SpawnHitEffects(hitEvents);
		std::erase_if(hitEvents, [&](const auto& hit) { return deathSystem_.IsDefeated() || statSystem_.IsManagedDead(hit.targetEntityId, hit.healthStatId); });
		hitReactionSystem_.DiscardBlocked(deathSystem_.IsDefeated()
			? deathSystem_.TargetBlocked() : deathSystem_.Blocked());
		if (!deathSystem_.IsDefeated()) hitReactionSystem_.Update(
			*activeDocument,
			reactionBindings,
			statSystem_,
			stateMachineSystem_,
			hitEvents,
			gameplayDeltaTime, reactionDecisions
		);
		runtimeEffectSystem_.SpawnDeathEffects(
			*activeDocument,
			hitReactionSystem_.ConsumeDeathEffectRequests()
		);
	}
	if (activeDocument && playing) {
		objectSystem_.RefreshTransforms(*activeDocument, enemySystem_.ApplyChargePoses(*activeDocument));
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
		if (deathWorldMoving) cameraSystem_.UpdateDeathAfterSimulation(
			*activeDocument, camera_, player_, runtimeObjectBindings_, deathWorldStep.worldDelta);
		else if (deathSystem_.IsDefeated()) cameraSystem_.HoldDeathCamera(camera_);
		else cameraSystem_.UpdateAfterSimulation(
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
	RefreshMovementBoundary(editing);
	// Transform確定後に環境設定とDebug形状を登録し、描画時の状態を揃える。
	environmentSystem_.Sync(activeDocument, runtimeObjectBindings_);
	if (activeDocument && playing) {
		// TextMotionの自然終了もTransform演出の完了も、この後のEvent段階で受け取る。
		std::vector<uint64_t> allowedText;
		for (const auto& e : activeDocument->GetEntities())
			if (!deathSystem_.IsBlocked(e.id) || deathTextEntities_.contains(e.id)) allowedText.push_back(e.id);
		textMotionSystem_.Update(*activeDocument, deltaTime, &allowedText, false);
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
	const auto* startedDirector = activeDocument && startedDirectorId &&
		startedGeneration == activeDocument->GetContentGeneration() ? activeDocument->FindEntity(startedDirectorId) : nullptr;
	const bool hasGameFlowStart = startedDirector && SceneEntityQuery::IsEntityActiveInHierarchy(*activeDocument, *startedDirector) &&
		SceneEntityQuery::FindEnabledComponent(*startedDirector, "GameFlowDirector");
	if (activeDocument && playing && !deathSystem_.IsDefeated() && (gameplayDeltaTime > 0.0f || hasGameFlowStart)) {
		// Prefab生成はEntity配列を再配置し得るため、bindingを使い終えた最後に行う。
		// HitStop中は開始通知だけ評価し、通常の演出完了pulseとbinding時計は保持する。
		const bool evaluateGameplay = gameplayDeltaTime > 0.0f;
		SceneEventRuntimeSignals eventSignals{};
		if (evaluateGameplay) {
			eventSignals.completedCameraPathEntityId = cameraSystem_.ConsumeCompletedCameraPathEntityId();
			eventSignals.finishedAudioEntityIds = audioSystem_.ConsumeFinishedEntityIds(*activeDocument);
			eventSignals.textMotionCompletions = textMotionSystem_.ConsumeCompletions();
			eventSignals.presentationCompletions = prefabAnimationSystem_.ConsumePresentationCompletions();
		}
		if (hasGameFlowStart) {
			eventSignals.startedDirectorEntityId = startedDirectorId;
			eventSignals.startedDocumentGeneration = startedGeneration;
		}
		const SceneEventResult eventResult = eventSystem_.Update(
			*activeDocument,
			statSystem_,
			healthSystem_,
			deathSystem_,
			stateMachineSystem_,
			gameplayDeltaTime,
			eventSignals,
			spawnFrame_.blockedEntityIds,
			!uiGameplayInputBlocked_,
			evaluateGameplay
		);
		if (!eventResult.deathDiagnostic.empty()) preparationDiagnostic_ = {ScenePreparationDiagnosticCode::InvalidReference, activeDocument->GetContentGeneration(), eventResult.deathDiagnosticEntityId, ++preparationDiagnosticSerial_, eventResult.deathDiagnostic};
		if (eventResult.exitGameRequested) {
			if (sceneManager_->RequestGameExit(GetSceneInstanceId(), activeDocument->GetContentGeneration())) return;
			ReportMenuFailure("MenuRuntime:ExitRejected");
		}
		if (eventResult.restartSceneRequested) {
			pendingRestartSceneInstanceId_ = sceneManager_->RequestSceneRestart(GetSceneInstanceId());
			if (pendingRestartSceneInstanceId_ != kInvalidSceneInstanceId) return;
			ReportSceneRestartFailure("SceneRestart:Rejected");
		}
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
			PrepareGameFlowTextOverrides(false);
			textRenderSystem_.Sync(activeDocument, editing ? nullptr : &statSystem_);
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
	if (activeDocument && playing) {
		deathSystem_.Observe(*activeDocument, statSystem_);
		if (!deathSystem_.IsDefeated() && gameFlowSystem_.NeedsFinalize()) {
			gameFlowResult = gameFlowSystem_.Update(*activeDocument, enemySpawnerSystem_, 0.0f, true);
			ReportPreparationDiagnostic(gameFlowResult.diagnostic, flowDiagnosticSerial_);
			for (const auto& r : gameFlowResult.entityRequests) if (auto* e = activeDocument->FindEntity(r.entityId)) e->active = r.active;
			for (const auto& r : gameFlowResult.waveRequests) enemySpawnerSystem_.BeginFiniteWave(r.spawnerEntityId, r.generation, r.count, r.holdUntilGameplay);
			for (const auto& r : gameFlowResult.motionRequests) {
				if (r.type == SceneGameFlowMotionRequestType::Reset) textMotionSystem_.Reset(r.entityId);
				else textMotionSystem_.Play(*activeDocument, r.entityId, r.clipId);
			}
		}
		if (!defeatedAtFrameStart) heldDeathFlow_ = gameFlowResult;
		DispatchDeaths(*activeDocument);
		// スローを受理した対象は既存Reactionを継続する。即時停止の敗北だけ従来通り破棄。
		if (deathSystem_.IsDefeated() && deathWorldInstance_ == 0) hitReactionSystem_.DiscardBlocked(deathSystem_.Blocked());
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
	postProcessProfileSystem_.Update(playing
		? (deathSystem_.IsDefeated() ? deathWorldStep.worldDelta : gameplayDeltaTime) : 0.0f);
	textRenderSystem_.ClearTextOverrides();
	textRenderSystem_.ClearPresentationOverrides();
	// Flowがまだ内容を要求しない文字も空表示にし、この後の要求だけを表示する。
	PrepareGameFlowTextOverrides(true);
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
	textRenderSystem_.Sync(resourceDocument, editing ? nullptr : &statSystem_);
	// Eventによる消失/階層変更をIDで再確認。Simulation中のsnapshotは変えない。
	if (activeDocument && playing) objectSystem_.SetSpawnPresentationFrame(
		enemySpawnerSystem_.BuildPresentationFrame(*activeDocument, gameFlowResult.gameplayAllowed));
	if (activeDocument) objectSystem_.ObserveReplaySubjects(*activeDocument);
	// 最終Pose/Camera/環境の更新を終えてから中間停止状態を公開する。
	if (deathWorldStep.freezeAfterStep) deathReplaySystem_.FinishLiveSlow();
}

void RuntimeScene::UpdateGamePaused(float uiDeltaTime)
{
	RefreshMovementBoundary(false);
	ApplyPendingUiAudio();
	auto* document = GetSceneDocument();
	if (!document) return;
	std::vector<uint64_t> movingText;
	const auto top = menuSystem_.GetFrame().panelId;
	if (top) {
		for (const auto& entity : document->GetEntities()) {
			const SceneEntity* node = &entity;
			for (size_t depth = 0; node && depth <= document->GetEntities().size(); ++depth) {
				if (SceneEntityQuery::FindComponent(*node, "MenuPanel")) {
					if (node->id == top) movingText.push_back(entity.id);
					break;
				}
				node = document->FindEntity(node->parentId);
			}
		}
	}
	textMotionSystem_.Update(*document, uiDeltaTime, &movingText);
	// Flow文字とworld Poseは保持し、UIで変更された文字演出/色/cacheだけ同期する。
	textRenderSystem_.ClearPresentationOverrides();
	for (const auto& [id, p] : textMotionSystem_.GetPresentationOverrides())
		textRenderSystem_.SetPresentationOverride(id, p.positionOffset, p.rotationOffset, p.scaleMultiplier, p.opacityMultiplier);
	SyncMenuPresentation();
	objectSystem_.SyncSprites(document);
	textRenderSystem_.Sync(document, &statSystem_);
}

void RuntimeScene::UpdateTransitionHold()
{
	RefreshMovementBoundary(false);
	SceneDocument* document = GetSceneDocument();
	if (!document) {
		return;
	}
	// 次Sceneは開始Eventより前に描く。旧Sceneでは同じ同期で再生時計を保つ。
	objectSystem_.SyncModelsForTransition(document, physicsSystem_);
	objectSystem_.BuildBindings(*document, runtimeObjectBindings_);
	attachmentSystem_.Update(*document, objectSystem_, runtimeObjectBindings_);
	objectSystem_.SyncSprites(document);
	if (deathSystem_.IsDefeated()) cameraSystem_.HoldDeathCamera(camera_);
	else cameraSystem_.UpdateTransitionHold(*document, camera_, player_, runtimeObjectBindings_);
	lightingSystem_.Sync(document);
	environmentSystem_.Sync(document, runtimeObjectBindings_);
	// 新Sceneの管理文字は初回から隠し、旧Sceneの進行済み表示/演出時計は保持する。
	PrepareGameFlowTextOverrides(false);
	SyncMenuPresentation();
	textRenderSystem_.Sync(document, &statSystem_);
}

void RuntimeScene::UpdatePaused()
{
	const auto* source = GetSceneDocument();
	if (!source || (spawnFrame_.documentGeneration != 0 && spawnFrame_.documentGeneration != source->GetContentGeneration())) {
		enemySpawnerSystem_.Clear();
		spawnFrame_ = {};
		preparationDiagnostic_ = {};
		flowDiagnosticSerial_ = spawnerDiagnosticSerial_ = 0;
		objectSystem_.SetSpawnPresentationFrame({});
	}
	cameraSystem_.UpdatePaused(camera_, debugCamera_);
	// UI時計はprepassで凍結済み。参照と文字cacheだけ同期し、内容置換後の旧色を残さない。
	SyncMenuPresentation();
	PrepareGameFlowTextOverrides(false);
	const auto* context = sceneManager_ ? sceneManager_->GetExecutionContext() : nullptr;
	textRenderSystem_.Sync(GetSceneDocument(), context && context->IsEditing() ? nullptr : &statSystem_);
	RefreshMovementBoundary(context && context->IsEditing());
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

void RuntimeScene::RefreshMovementBoundary(bool editing) {
	const auto* document = GetSceneDocument();
	effectRenderSystem_.SetMovementBoundary(document ? physicsSystem_.MovementBoundsFrame(*document, editing) : SceneMovementBoundsFrame{});
	const char* issue = document && !editing ? physicsSystem_.MovementBoundsIssue(*document) : nullptr;
	if (!issue) {
		movementBoundsReportedGeneration_ = 0;
		return;
	}
	const auto generation = document->GetContentGeneration();
	if (movementBoundsReportedGeneration_ == generation) return;
	movementBoundsReportedGeneration_ = generation;
	const auto* owner = document->FindEntityByName("Player");
	preparationDiagnostic_ = {ScenePreparationDiagnosticCode::InvalidReference, generation,
		owner ? owner->id : 0, ++preparationDiagnosticSerial_, issue};
}

void RuntimeScene::BeginRenderFrame(uint64_t serial) {
	effectRenderSystem_.BeginRenderFrame(serial);
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
	DrawSceneView(viewCamera ? viewCamera : GetSceneViewCamera(), 0, true);
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
	DrawPreparedSceneContentForView(viewCamera, 0, true);
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
		0,
		true
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
		!(sceneManager_ && sceneManager_->IsTransitionHeld()), !IsPreparedDestinationDraw()
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
			DrawSceneView(monitorCamera, skipEntityId, false);
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
	// 最初の描画がShadowでも、Attachment後の出現倍率を定数へ反映する。
	ApplyRenderCamera(GetSceneViewCamera());
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
	PrepareReplayForFinalize();
	pendingRestartSceneInstanceId_ = kInvalidSceneInstanceId;
	gameFlowTextDocumentGeneration_ = 0;
	spawnFrame_ = {};
	preparationDiagnostic_ = {};
	flowDiagnosticSerial_ = spawnerDiagnosticSerial_ = 0;
	gameplayObjectBindings_.clear();
	objectSystem_.SetSpawnPresentationFrame({});
	enemySpawnerSystem_.Clear();
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
	deathSystem_.Clear(); ClearDeathWorldContinuation(); pendingDeathActions_.clear(); pendingDeathGeneration_ = 0; deathTextEntities_.clear(); deathTransformEntities_.clear(); heldDeathFlow_ = {};
	healthSystem_.Clear();

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

void RuntimeScene::FailDeathReplay(const SceneDeathStarted& pulse, const std::string& issue) {
    replayGameFrameReady_ = false;
    replayRender_.Cancel(); // Draw中でも即解放しない。次の安全境界で返却。
	SceneDeathReplayStatus status;
	if (deathReplaySystem_.TryGetStatus(pulse.owner, status) && status.phase == SceneDeathReplayPhase::Failed) return;
	if (replayHistory_.Status().phase == SceneReplayHistoryPhase::Recording ||
		replayHistory_.Status().phase == SceneReplayHistoryPhase::Sealed) replayHistory_.Fail(issue.c_str());
	replayBeginPending_ = replayFinalPending_ = false;
	deathReplayStarted_ = pulse;
	deathReplaySystem_.FailStart(pulse, issue);
	deathWorldClock_.Freeze();
	preparationDiagnostic_ = { ScenePreparationDiagnosticCode::InvalidReference, pulse.generation,
		pulse.owner, ++preparationDiagnosticSerial_, issue };
	Logger::Log(issue + "\n");
}

void RuntimeScene::DispatchDeaths(SceneDocument& document) {
	const auto pulses = deathSystem_.ConsumeStarted();
	// 同時死亡は最初の全体敗北だけを採用。別ownerの設定を流用しない。
	const SceneDeathStarted* first = nullptr;
	for (const auto& pulse : pulses) if (pulse.wholeScene && (!first || pulse.serial < first->serial)) first = &pulse;
	if (first && deathWorldClock_.Phase() == SceneDeathWorldPhase::Normal) {
		const auto* owner = document.FindEntity(first->owner);
		const auto* replay = owner ? SceneEntityQuery::FindEnabledComponent(*owner, "DeathReplay") : nullptr;
		if (replay) {
			const auto* response = SceneEntityQuery::FindEnabledComponent(*owner, "DeathResponse");
			const auto* health = SceneEntityQuery::FindEnabledComponent(*owner, "Health");
			const SceneEntity* statsOwner = nullptr;
			if (health) {
				if (health->healthStatsEntityId) statsOwner = document.FindEntity(health->healthStatsEntityId);
				else if (health->healthStatsEntityName.empty()) statsOwner = owner;
				else for (const auto& e : document.GetEntities()) if (e.name == health->healthStatsEntityName) {
					if (statsOwner) { statsOwner = nullptr; break; }
					statsOwner = &e;
				}
			}
			const auto* stats = statsOwner ? SceneEntityQuery::FindEnabledComponent(*statsOwner, "StatSet") : nullptr;
			const bool sourceValid = response && health && stats && statsOwner->id == first->stats &&
				std::any_of(stats->stats.begin(), stats->stats.end(), [&](const auto& s) {
					return s.id == health->healthStatId && s.minValue == 0 &&
						std::isfinite(s.maxValue) && s.maxValue > 0 && std::isfinite(s.initialValue) && s.initialValue > 0;
				}) &&
				std::any_of(stats->flags.begin(), stats->flags.end(), [&](const auto& f) { return f.id == response->deathResponseFlagId && !f.initialValue; });
			size_t activeCount = 0, ownCount = 0;
			for (const auto& e : document.GetEntities()) for (const auto& c : e.components) if (c.type == "DeathReplay") {
				if (e.id == owner->id) ++ownCount;
				if (c.enabled && SceneEntityQuery::IsEntityActiveInHierarchy(document, e)) ++activeCount;
			}
			if (!sceneManager_ || sceneManager_->GetActiveSceneInstanceId() != GetSceneInstanceId()) {
				FailDeathReplay(*first, "DeathResponse:ReplayInactiveScene");
			} else if (ownCount != 1 || activeCount != 1 || !SceneEntityQuery::IsEntityActiveInHierarchy(document, *owner) ||
				!sourceValid || response->deathResponseScope != "Scene" ||
				!std::isfinite(replay->deathReplaySlowScale) || replay->deathReplaySlowScale <= 0 || replay->deathReplaySlowScale > 1 ||
				!std::isfinite(replay->deathReplaySlowDuration) || replay->deathReplaySlowDuration <= 0) {
				FailDeathReplay(*first, "DeathResponse:ReplayInvalidSettings");
			} else if (BeginDeathWorldContinuation(*first, replay->deathReplaySlowScale, replay->deathReplaySlowDuration)) {
				deathReplayStarted_ = *first;
				deathReplaySystem_.AcceptStart(*first, replay->deathReplaySlowDuration);
				if (replay->deathReplayEnabled) {
					if (replayHistory_.Start({first->instance, first->generation, first->owner, first->serial},
						replay->deathReplayCaptureHz, replay->deathReplayHistoryBudgetMiB)) {
						replayBeginPending_ = true;
						replayInterval_ = 1.0 / replay->deathReplayCaptureHz;
					} else FailDeathReplay(*first, replayHistory_.Status().diagnostic);
				}
			} else {
				SceneDeathReplayStatus status;
				if (!deathReplaySystem_.TryGetStatus(first->owner, status)) FailDeathReplay(*first, "DeathResponse:ReplayStartFailed");
			}
		}
		if (deathWorldClock_.Phase() == SceneDeathWorldPhase::Normal) deathWorldClock_.Freeze();
	}
	for (const auto& p : pulses) deathSystem_.SetStatus(p.owner, 1);
	const auto actions = eventSystem_.EvaluateDeaths(document, pulses);
	for (const auto& a : actions) { deathSystem_.SetStatus(a.owner, 2); pendingDeathActions_.push_back(a); }
	if (!actions.empty()) pendingDeathGeneration_ = document.GetContentGeneration();
	ApplyDeathActions(document, false);
}
void RuntimeScene::ApplyDeathActions(SceneDocument& document, bool transformOnly) {
	if (pendingDeathGeneration_ != document.GetContentGeneration()) {
		pendingDeathActions_.clear(); pendingDeathGeneration_ = 0; return;
	}
	for (auto it = pendingDeathActions_.begin(); it != pendingDeathActions_.end();) {
		const auto& action = it->action;
		if ((action.type == "PlayPresentationClip") != transformOnly) { ++it; continue; }
		const auto* source = document.FindEntity(it->owner);
		const SceneEntity* target = action.targetEntityId ? document.FindEntity(action.targetEntityId) : nullptr;
		if (!action.targetEntityId && !action.targetEntityName.empty()) {
			for (const auto& e : document.GetEntities()) if (e.name == action.targetEntityName) {
				if (target) { target = nullptr; break; } target = &e;
			}
		}
		bool accepted = false;
		if (source && SceneEntityQuery::FindEnabledComponent(*source, "DeathResponse") && target) {
			if (action.type == "PlayAudio") accepted = audioSystem_.ApplyRequests(document, {{SceneAudioRequestType::Play, target->id}});
			else if (action.type == "PlayTextMotion") { accepted = textMotionSystem_.Play(document, target->id, action.textMotionClipId); if (accepted) deathTextEntities_.insert(target->id); }
			else if (action.type == "PlayPresentationClip") {
				auto blocked = enemySpawnerSystem_.BuildPresentationFrame(document, !deathSystem_.IsDefeated()).blockedEntityIds;
				// Frozen後もWorld姿勢の所有を維持し、別の演出による書込みを拒否する。
				if (deathWorldInstance_) blocked.insert(deathWorldParticipants_.begin(), deathWorldParticipants_.end());
				accepted = prefabAnimationSystem_.PlayPresentation(document, target->id, action.presentationClipName, blocked);
			}
			if (accepted && action.type == "PlayPresentationClip") deathTransformEntities_.insert(target->id);
		}
		if (!accepted) {
			deathSystem_.SetStatus(it->owner, 4);
			preparationDiagnostic_ = {ScenePreparationDiagnosticCode::InvalidReference, document.GetContentGeneration(),
				it->owner, ++preparationDiagnosticSerial_, "DeathResponse:StartFailed " + action.type + " " + std::to_string(action.targetEntityId)};
		} else if (deathSystem_.Status(it->owner) != 4) deathSystem_.SetStatus(it->owner, 3);
		it = pendingDeathActions_.erase(it);
	}
	for (const auto& a : pendingDeathActions_) if (deathSystem_.Status(a.owner) != 4) deathSystem_.SetStatus(a.owner, 2);
}

void RuntimeScene::ClearDeathWorldContinuation() {
    replayCompletion_={}; replayMenuSource_={}; replayMenuDrawToken_={};
    replayMenuPresented_=false; replayPresentationAllowed_=false;
    replayGameFrameReady_ = false; replayGameFrame_ = {}; replayViews_.clear();
    replayRender_.Cancel();
    replayRender_.ReleaseAtSafeBoundary(); // 使用中なら完了通知まで資源だけ保持。
	deathReplaySystem_.Clear(); deathReplayStarted_ = {};
	replayHistory_.Clear(); replayOwnerBasis_ = {};
	replayRealSeconds_ = replayNextDue_ = replayInterval_ = 0;
	replayCapturedUpdate_ = replayCapturedDraw_ = 0;
	replayBeginPending_ = replayFinalPending_ = false;
	cameraSystem_.ClearDeathContinuation();
	deathWorldClock_.Clear();
	deathWorldElapsedSeconds_ = 0.0;
	deathWorldInstance_ = deathWorldGeneration_ = 0;
	deathWorldParticipants_.clear(); deathWorldBaseVelocities_.clear();
	attackRunnerSystem_.ClearDeathContinuation();
	hitReactionSystem_.ClearDeathContinuation();
	prefabAnimationSystem_.ClearDeathContinuation();
	objectSystem_.ClearDeathContinuation();
}

bool RuntimeScene::BeginDeathWorldContinuation(const SceneDeathStarted& started, float scale, float duration) {
	auto* document = GetSceneDocument();
	if (!document || !started.wholeScene || !deathSystem_.IsDefeated() ||
		started.instance != GetSceneInstanceId() || started.generation != document->GetContentGeneration() ||
		!document->FindEntity(started.owner) || deathWorldClock_.Phase() != SceneDeathWorldPhase::Normal ||
		!std::isfinite(scale) || scale <= 0.0f || scale > 1.0f || !std::isfinite(duration) || duration <= 0.0f) return false;
	const auto preparation = enemySpawnerSystem_.BuildPresentationFrame(*document, heldDeathFlow_.gameplayAllowed);
	std::unordered_set<uint64_t> participants;
	std::unordered_map<uint64_t, Vector3> velocities;
	for (const auto& entity : document->GetEntities()) {
		if (!SceneEntityQuery::IsEntityActiveInHierarchy(*document, entity) || preparation.blockedEntityIds.contains(entity.id) ||
			deathSystem_.TargetBlocked().contains(entity.id)) continue;
		if (SceneEntityQuery::HasComponent(entity, "PlayerBehavior") && SceneEntityQuery::FindEnabledComponent(entity, "Projectile")) return false;
		// 非有限の姿勢/移動設定を、状態を変更する前に拒否する。
		if (attackRunnerSystem_.IsRunning(entity.id) &&
			(!std::isfinite(attackRunnerSystem_.GetDuration(entity.id)) || !std::isfinite(attackRunnerSystem_.GetTime(entity.id)))) return false;
		if (const auto* attacks = SceneEntityQuery::FindEnabledComponent(entity, "AttackSet")) {
			for (const auto& definition : attacks->attackDefinitions) {
				if (!std::isfinite(definition.windup) || !std::isfinite(definition.activeTime) || !std::isfinite(definition.recovery) ||
					!std::isfinite(definition.forwardDistance) || !std::isfinite(definition.sideDistance) || !std::isfinite(definition.facingRotateAngle) ||
					!std::isfinite(definition.windup + definition.activeTime + definition.recovery)) return false;
				// 対応easingの最大傾きは3。XZ速度がfloatで表現可能な範囲を確認する。
				const double maxSpeed = 3.0 * (std::abs(static_cast<double>(definition.forwardDistance)) +
					std::abs(static_cast<double>(definition.sideDistance))) / (std::max)(static_cast<double>(definition.activeTime), 0.0001);
				if (maxSpeed > (std::numeric_limits<float>::max)()) return false;
			}
		}
		participants.insert(entity.id);
	}
	if (prefabAnimationSystem_.HasPresentationTargetsIn(participants)) {
		FailDeathReplay(started, "DeathResponse:ReplayTransformConflict");
		return false;
	}
	std::vector<SceneRuntimeObjectBinding> bindings;
	objectSystem_.BuildBindings(*document, bindings);
	for (auto& binding : bindings) {
		if (!participants.contains(binding.entityId)) continue;
		if (player_ && binding.entity == document->FindEntityByName("Player")) binding.body = &player_->GetPhysicsBody();
		if (binding.body) {
			const auto& v = binding.body->velocity;
			if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return false;
			velocities.emplace(binding.entityId, v);
		}
	}
	if (!deathWorldClock_.Begin(started.instance, started.generation, started.owner, started.serial, scale, duration)) return false;
	deathWorldElapsedSeconds_ = 0.0;
	deathWorldInstance_ = started.instance; deathWorldGeneration_ = started.generation;
	deathWorldParticipants_ = std::move(participants); deathWorldBaseVelocities_ = std::move(velocities);
	attackRunnerSystem_.BeginDeathContinuation(*document, bindings, deathWorldParticipants_);
	hitReactionSystem_.BeginDeathContinuation(deathWorldParticipants_);
	prefabAnimationSystem_.BeginDeathContinuation(*document, deathWorldParticipants_, attackRunnerSystem_.DeathAnimationTargets());
	objectSystem_.BeginDeathContinuation(deathWorldParticipants_, attackRunnerSystem_.DeathAnimationTargets());
	cameraSystem_.BeginDeathContinuation(*document);
	return true;
}

void RuntimeScene::NotifyReplayGpuCompleted() { replayRender_.NotifyGpuCompleted(); }
void RuntimeScene::PrepareReplayForFinalize() {
    replayRender_.Cancel();
    if (replayRender_.HasPendingGpuUse()) {
        // 通常はGame末尾で完了済み。例外的な破棄だけ、元Asset解放より先にdrainする。
        Object3dCommon::GetInstance()->GetDxCommon()->ExecuteCommandListAndWait();
        replayRender_.NotifyGpuCompleted();
    }
    replayRender_.ReleaseAtSafeBoundary();
}

void RuntimeScene::AdvanceReplayPresentation(float delta, bool allowed) {
    replayPresentationAllowed_ = allowed;
    deathReplaySystem_.AdvanceReplay(delta, allowed);
}
bool RuntimeScene::PrepareReplayGameFrame(uint64_t drawSerial, float aspect, SceneReplayGameFrame& out) {
    replayGameFrameReady_ = false;
    if (!replayRender_.IsPrepared() || replayHistory_.Status().phase != SceneReplayHistoryPhase::Sealed) return false;
    const auto* document = GetSceneDocument();
    const auto& identity = replayHistory_.Identity();
    if (!document || identity.instance != GetSceneInstanceId() || identity.generation != document->GetContentGeneration()) {
        FailDeathReplay(deathReplayStarted_, "DeathResponse:HistoryIdentity"); return false;
    }
    try {
        if (!deathReplaySystem_.IsReplaying()) {
            const auto* owner = document->FindEntity(identity.owner);
            const auto* settings = owner ? SceneEntityQuery::FindEnabledComponent(*owner,"DeathReplay") : nullptr;
            if (!settings || settings->deathReplayViews.size()!=3 ||
                !std::all_of(settings->deathReplayViews.begin(),settings->deathReplayViews.end(),IsValidReplayView)) {
                FailDeathReplay(deathReplayStarted_, "DeathResponse:ReplayViewSettings"); return false;
            }
            replayViews_ = settings->deathReplayViews;
            // 全cutの構図を開始前に検証し、B/C到達時の設定エラーを先送りしない。
            for (size_t cut=0;cut<replayViews_.size();++cut) {
                const auto& view=replayViews_[cut];
                Camera camera;
                const auto& basis=replayRender_.OriginBasis();
                const auto& origin=replayRender_.OriginBounds(); const auto& clip=replayRender_.ClipBounds();
                if (!origin.valid || !clip.valid || !ResolveReplayCamera(view,basis.world,basis.cameraWorld,
                    origin.min,origin.max,clip.min,clip.max,aspect,camera)) {
                    FailDeathReplay(deathReplayStarted_, "DeathResponse:ReplayCamera"); return false;
                }
                replayCameras_[cut]=camera;
            }
            const auto& history = replayHistory_.Status();
            if (!deathReplaySystem_.BeginReplay(history.realSeconds,history.sampleCount,settings->deathReplaySpeed)) {
                FailDeathReplay(deathReplayStarted_, "DeathResponse:ReplayViewSettings"); return false;
            }
        }
        SceneReplayGameFrame game;
        game.token = deathReplaySystem_.PresentationToken(drawSerial);
        if (!game.token.drawSerial || game.token.cut>=replayViews_.size()) return false;
        if (!std::isfinite(aspect) || aspect<=0) return false;
        game.camera=replayCameras_[game.token.cut];
        game.camera.SetAspectRatio(aspect); game.camera.UpdatePreviewMatrices();
        SceneDeathReplayStatus status;
        deathReplaySystem_.TryGetStatus(identity.owner,status);
        game.finishedHold=status.phase==SceneDeathReplayPhase::FinishedHold;
        SceneReplayFrame evaluated;
        if (!replayHistory_.Evaluate(deathReplaySystem_.ReplaySampleSeconds(),evaluated) ||
            !replayRender_.ApplyEvaluatedFrame(evaluated,game.camera)) {
            FailDeathReplay(deathReplayStarted_, "DeathResponse:RenderPreparation"); return false;
        }
        game.postProcess=evaluated.effectivePostProcess; game.effectTime=evaluated.gameViewEffectTime;
        replayGameFrame_=game; replayGameFrameReady_=true; out=game;
        return true;
    } catch (const std::bad_alloc&) { FailDeathReplay(deathReplayStarted_, "DeathResponse:RenderPreparation"); }
      catch (const std::length_error&) { FailDeathReplay(deathReplayStarted_, "DeathResponse:RenderPreparation"); }
    return false;
}
bool RuntimeScene::DrawReplayGameShadow(const SceneReplayPresentationToken& token) {
    if (!replayGameFrameReady_ || token!=replayGameFrame_.token) return false;
    if (replayRender_.DrawShadow()) return true;
    RejectReplayGameFrame(token); return false;
}
bool RuntimeScene::DrawReplayGameWorld(const SceneReplayPresentationToken& token) {
    if (!replayGameFrameReady_ || token!=replayGameFrame_.token) return false;
    if (replayRender_.DrawWorld()) return true;
    RejectReplayGameFrame(token); return false;
}
bool RuntimeScene::DrawReplayMenu(const SceneReplayPresentationToken& token, SceneMenuSource& out) {
    const auto* document=GetSceneDocument();
    const auto source=menuSystem_.GetSource();
    if (!document || !replayGameFrameReady_ || token!=replayGameFrame_.token || !replayGameFrame_.finishedHold ||
        !replayMenuSource_.panelId || source.panelId!=replayMenuSource_.panelId || source.openSerial!=replayMenuSource_.openSerial ||
        source.instanceId!=token.instance || source.generation!=token.generation || token.deathSerial!=replayCompletion_.deathSerial ||
        !menuSystem_.IsSourceValid(*document,source)) return false;
    // 下層Menuと通常HUDを含めず、この完了通知が開いた一枚だけを描く。
    auto frame=menuSystem_.GetFrame(); frame.visiblePanelIds={source.panelId};
    if (!uiRenderSystem_.HasMenu(*document,frame)) return false;
    uiRenderSystem_.DrawMenu(*document,objectSystem_,textRenderSystem_,frame);
    replayMenuDrawToken_=token; out=source; return true;
}
void RuntimeScene::AcceptReplayMenuReceipt(const SceneReplayPresentationToken& token, const SceneMenuSource& source) {
    const auto* document=GetSceneDocument();
    if (!document || !replayGameFrameReady_ || !replayGameFrame_.finishedHold || token!=replayGameFrame_.token ||
        token!=replayMenuDrawToken_ || replayRender_.HasPendingGpuUse() || !source.panelId ||
        source.panelId!=replayMenuSource_.panelId || source.openSerial!=replayMenuSource_.openSerial ||
        source.instanceId!=token.instance || source.generation!=token.generation || !menuSystem_.IsSourceValid(*document,source)) return;
    if (!replayMenuPresented_) { replayMenuPresented_=true; eventSystem_.RearmMenuKeys(); }
}
void RuntimeScene::AcceptReplayPresentationReceipt(const SceneReplayPresentationToken& token) {
    if (replayGameFrameReady_ && token==replayGameFrame_.token && !replayRender_.HasPendingGpuUse())
        deathReplaySystem_.AcceptPresentationReceipt(token);
}

bool RuntimeScene::TryGetReplayFraming(uint64_t owner, SceneReplayFramingBasis& out) const {
    const auto* document=GetSceneDocument();
    if (!document) return false;
    try { return objectSystem_.TryGetReplayFraming(*document,owner,out); }
    catch (const std::bad_alloc&) { return false; } catch (const std::length_error&) { return false; }
}

void RuntimeScene::RejectReplayGameFrame(const SceneReplayPresentationToken& token) {
    if (replayGameFrameReady_ && token==replayGameFrame_.token)
        FailDeathReplay(deathReplayStarted_,"DeathResponse:RenderPreparation");
}
