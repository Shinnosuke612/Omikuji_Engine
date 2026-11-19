// 役割: 描画ComponentをObject3d/Spriteへ反映し、実行時の所有関係を管理する。
#include "SceneObjectSystem.h"
#include "../../../../engine/scene/SceneReplayPresentation.h"
#include "../replay/SceneReplayHistorySystem.h"
#include <limits>

#include "../physics/ScenePhysicsSystem.h"
#include "../../../../engine/2d/Sprite.h"
#include "../../../../engine/2d/SpriteCommon.h"
#include "../../../../engine/2d/TextureManager.h"
#include "../../../../engine/3d/Camera.h"
#include "../../../../engine/3d/ModelManager.h"
#include "../../../../engine/3d/Model.h"
#include "../../../../engine/3d/Object3d.h"
#include "../../../../engine/3d/Object3dCommon.h"
#include "../../../../engine/agent/AgentSettingsResolver.h"
#include "../../../../engine/scene/SceneDocument.h"
#include "../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../engine/scene/SceneTransformResolver.h"
#include "../../../../engine/scene/SceneUILayout.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace {
	using AgentSettingsResolver::ResolveAgentBehaviorSettings;
	using SceneEntityQuery::FindEnabledComponent;
	using SceneEntityQuery::HasComponent;
	using SceneEntityQuery::IsEntityActiveInHierarchy;
	using SceneTransformResolver::ResolveScene2DTransform;

	std::string BuildMaterialOverrideSignature(
		const std::vector<SceneMeshMaterialOverride>& overrides
	) {
		std::ostringstream stream;
		for (const SceneMeshMaterialOverride& override : overrides) {
			stream << override.materialName << '|'
				<< override.enabled << '|'
				<< override.colorOverrideEnabled << '|'
				<< override.color.x << ',' << override.color.y << ','
				<< override.color.z << ',' << override.color.w << '|'
				<< override.texturePath << ';';
		}
		return stream.str();
	}

	Object3dCommon::CullMode ToObjectCullMode(const std::string& cullMode) {
		if (cullMode == "None") {
			return Object3dCommon::CullMode::kNone;
		}
		if (cullMode == "Front") {
			return Object3dCommon::CullMode::kFront;
		}
		return Object3dCommon::CullMode::kBack;
	}
}

SceneObjectSystem::SceneObjectSystem() = default;

SceneObjectSystem::~SceneObjectSystem() {
	Finalize();
}

void SceneObjectSystem::BeginPreparedModels(Camera& camera) noexcept {
	preparedCamera_ = &camera;
	preparedModelCursor_ = 0;
}

RuntimePrepareStep SceneObjectSystem::StepPreparedModels(const SceneDocument& document,
	ScenePhysicsSystem&, const SceneRuntimePrepareBudget& budget, std::string& diagnostic) {
	using Step = RuntimePrepareStep;
	if (!preparedCamera_) return Step::Unsupported;
	if (!budget.maxItems || std::chrono::steady_clock::now() >= budget.deadline) return Step::Pending;
	if (!preparedIndexed_) {
		preparedDocumentGeneration_ = document.GetContentGeneration();
		preparedEntityCount_ = document.GetEntities().size();
		preparedModelStates_.resize(preparedEntityCount_, 0);
		preparedScanRemaining_ = preparedEntityCount_;
		preparedIndexed_ = true;
		if (budget.metrics) budget.metrics->entities = preparedEntityCount_;
		return Step::Progress;
	}
	if (document.GetContentGeneration() != preparedDocumentGeneration_ || document.GetEntities().size() != preparedEntityCount_) {
		diagnostic = "Destination document changed during Object preparation."; return Step::Failed;
	}
	if (preparedLocalReady_ == preparedEntityCount_) return Step::Ready;
	const auto waitForAsset = [&](const std::string& reason) {
		if (budget.allowRuntimeFinalize) { diagnostic = reason; return Step::Failed; }
		return Step::WaitingForAssets;
	};
	// 完了barrierへ入った時は一度最終再scanする。以後の依存欠落を無限待機にしない。
	if (budget.allowRuntimeFinalize && !preparedFinalScan_) {
		preparedFinalScan_ = true; preparedScanRemaining_ = preparedEntityCount_;
	}
	auto& textures = *TextureManager::GetInstance();
	constexpr auto environment = "resources/rostock_laage_airport_4k.dds";
	if (!textures.HasTexture(environment) || !textures.GetSrvHandleGPU(environment).ptr || !textures.GetMetaData(environment).IsCubemap()) {
		if (budget.metrics) ++budget.metrics->waitEnvironment;
		return waitForAsset("Published destination environment cube is missing or invalid.");
	}
	if (!preparedObject_) {
		if (preparedAssetRevision_ != budget.assetRevision) {
			preparedAssetRevision_ = budget.assetRevision;
			preparedScanRemaining_ = preparedEntityCount_;
		}
		if (!preparedScanRemaining_) return waitForAsset("Destination object dependencies remain unavailable after the final asset scan.");
		const size_t index = preparedModelCursor_;
		--preparedScanRemaining_;
		preparedModelCursor_ = (index + 1) % preparedEntityCount_;
		if (preparedModelStates_[index] == 2) return Step::Progress;
		const auto& entity = document.GetEntities()[index];
		const auto* mesh = FindEnabledComponent(entity, "MeshRenderer");
		const auto path = mesh ? mesh->modelPath : std::string{};
		auto* model = path.empty() ? nullptr : ModelManager::GetInstance()->FindModel(path);
		if (!path.empty() && !model) {
			if (budget.metrics) ++budget.metrics->waitModel;
			if (budget.allowRuntimeFinalize) return waitForAsset("Published destination model is missing: " + path);
			return preparedScanRemaining_ ? Step::Progress : Step::WaitingForAssets;
		}
		if (model && model->HasSkinning()) { diagnostic = "Prepared destination model is skinned: " + path; return Step::Unsupported; }
		// cache公開後の実slotだけを見る。未準備をnull Model/白画像で補わない。
		bool missing = false;
		if (model) for (const auto& slot : model->GetMaterialSlots()) {
			if (!slot.textureFilePath.empty() && (!textures.HasTexture(slot.textureFilePath) || !textures.GetSrvHandleGPU(slot.textureFilePath).ptr)) missing = true;
			if (mesh) {
				const auto found = std::find_if(mesh->meshMaterialOverrides.begin(), mesh->meshMaterialOverrides.end(), [&](const auto& item) {
					return item.enabled && !item.materialName.empty() && item.materialName == slot.name;
				});
				if (found != mesh->meshMaterialOverrides.end() && !found->texturePath.empty() &&
					(!textures.HasTexture(found->texturePath) || !textures.GetSrvHandleGPU(found->texturePath).ptr)) missing = true;
			}
		}
		if (missing) {
			if (budget.metrics) ++budget.metrics->waitTexture;
			if (budget.allowRuntimeFinalize) return waitForAsset("Published destination material texture is missing: " + path);
			return preparedScanRemaining_ ? Step::Progress : Step::WaitingForAssets;
		}
		auto object = std::make_unique<Object3d>();
		if (!object->BeginPreparedInitialize(*Object3dCommon::GetInstance(), *preparedCamera_, model, environment, diagnostic)) return Step::Failed;
		preparedObject_ = std::move(object);
		preparedModelCursor_ = index; // 作成途中は一件だけを専有し、同じ個体へ再Beginしない。
		preparedModelStates_[index] = 1;
		if (budget.metrics) {
			++budget.metrics->localStarted;
			if (std::find(preparedModelStates_.begin(), preparedModelStates_.begin() + index, uint8_t{0}) != preparedModelStates_.begin() + index) ++budget.metrics->skippedAhead;
			budget.metrics->lastEntity = entity.id; budget.metrics->lastIndex = index; budget.metrics->action = "object_begin";
		}
		return Step::Progress;
	}
	auto remaining = budget;
	const auto step = preparedObject_->StepPreparedInitialize(remaining, diagnostic);
	if (step != Step::Ready) return step;
	const size_t index = preparedModelCursor_;
	const auto& entity = document.GetEntities()[index];
	const auto* mesh = FindEnabledComponent(entity, "MeshRenderer");
	auto& transform = preparedObject_->GetTransform();
	transform.scale = entity.transform.scale;
	transform.rotate = MakeEulerFromQuaternion(entity.transform.rotate);
	transform.translate = entity.transform.translate;
	transform.useQuaternionRotation = true; transform.quaternionRotate = entity.transform.rotate;
	preparedObject_->SetCullMode(HasComponent(entity, "WaterVolume") ? Object3dCommon::CullMode::kNone : mesh ? ToObjectCullMode(mesh->meshCullMode) : Object3dCommon::CullMode::kBack);
	std::vector<Object3d::MaterialOverride> overrides;
	if (mesh) for (const auto& value : mesh->meshMaterialOverrides) overrides.push_back({value.materialName,value.enabled,value.colorOverrideEnabled,value.color,value.texturePath});
	if (!preparedObject_->TrySetPreparedMaterialOverrides(overrides)) { diagnostic = "Prepared material override is unavailable."; return Step::Failed; }
	auto [position, inserted] = models_.try_emplace(entity.id);
	if (!inserted) { diagnostic = "Duplicate destination Runtime object."; return Step::Failed; }
	auto& runtime = position->second;
	runtime.object = std::move(preparedObject_);
	runtime.modelPath = mesh ? mesh->modelPath : std::string{};
	runtime.hasRenderer = !runtime.modelPath.empty();
	runtime.materialOverrideSignature = mesh ? BuildMaterialOverrideSignature(mesh->meshMaterialOverrides) : BuildMaterialOverrideSignature({});
	preparedModelStates_[index] = 2; ++preparedLocalReady_;
	preparedModelCursor_ = (index + 1) % preparedEntityCount_;
	if (budget.metrics) {
		++budget.metrics->localReady;
		budget.metrics->lastEntity = entity.id; budget.metrics->lastIndex = index; budget.metrics->action = "object_local_ready";
	}
	return Step::Progress;
}

RuntimePrepareStep SceneObjectSystem::StepPreparedModelBindings(SceneDocument& document,
	ScenePhysicsSystem& physics, const SceneRuntimePrepareBudget& budget, std::string& diagnostic) {
	using Step = RuntimePrepareStep;
	if (!budget.allowRuntimeFinalize) return Step::WaitingForAssets;
	if (!budget.maxItems || std::chrono::steady_clock::now() >= budget.deadline) return Step::Pending;
	if (!preparedIndexed_ || preparedLocalReady_ != preparedEntityCount_ ||
		document.GetContentGeneration() != preparedDocumentGeneration_ || document.GetEntities().size() != preparedEntityCount_) {
		diagnostic = "Destination Objects or document are not ready for binding."; return Step::Failed;
	}
	const auto& entities = document.GetEntities();
	if (preparedBindingStage_ == PreparedBindingStage::Ready) return Step::Ready;
	if (preparedBindingStage_ == PreparedBindingStage::World) {
		// 再帰のenter/exitを保持し、文書root順と親優先・循環打切りを従来どおり再現する。
		if (preparedWorldStack_.empty()) {
			if (preparedBindingCursor_ == entities.size()) {
				preparedBindingStage_ = PreparedBindingStage::Ready;
				return Step::Ready;
			}
			preparedWorldStack_.push_back({entities[preparedBindingCursor_++].id,false});
			return Step::Progress;
		}
		auto& frame = preparedWorldStack_.back();
		const uint64_t id = frame.first;
		const auto* entity = document.FindEntity(id);
		auto* object = id ? FindObject(id) : nullptr;
		if (!frame.second) {
			if (!entity || !object || preparedWorldDone_.contains(id) || preparedWorldVisiting_.contains(id)) {
				preparedWorldStack_.pop_back(); return Step::Progress;
			}
			preparedWorldVisiting_.insert(id); frame.second = true;
			if (entity->parentId && FindObject(entity->parentId)) preparedWorldStack_.push_back({entity->parentId,false});
			return Step::Progress;
		}
		if (IsEntityActiveInHierarchy(document,*entity)) {
			if (preparedInitialPose_.contains(id)) object->UpdateAnimation(0.0f);
			object->Update();
		}
		preparedWorldVisiting_.erase(id); preparedWorldDone_.insert(id); preparedWorldStack_.pop_back();
		return Step::Progress;
	}
	if (preparedBindingCursor_ == entities.size()) {
		preparedBindingCursor_ = 0;
		preparedBindingStage_ = preparedBindingStage_ == PreparedBindingStage::Verify ? PreparedBindingStage::Settings :
			preparedBindingStage_ == PreparedBindingStage::Settings ? PreparedBindingStage::Parents : PreparedBindingStage::World;
		return Step::Progress;
	}
	const size_t index = preparedBindingCursor_++;
	const auto& entity = entities[index];
	auto* runtime = FindModelRuntime(entity.id);
	if (!runtime || !runtime->object) { diagnostic = "Prepared Runtime object is missing."; return Step::Failed; }
	if (budget.metrics) { budget.metrics->lastEntity = entity.id; budget.metrics->lastIndex = index; }
	if (preparedBindingStage_ == PreparedBindingStage::Verify) {
		const auto* mesh = FindEnabledComponent(entity,"MeshRenderer");
		if (runtime->modelPath != (mesh ? mesh->modelPath : std::string{}) ||
			runtime->hasRenderer != !runtime->modelPath.empty() || runtime->materialOverrideSignature !=
			(mesh ? BuildMaterialOverrideSignature(mesh->meshMaterialOverrides) : BuildMaterialOverrideSignature({}))) {
			diagnostic = "Destination model or material signature changed."; return Step::Failed;
		}
	} else if (preparedBindingStage_ == PreparedBindingStage::Settings) {
		// 身元の採番も文書順。先行構築の完了順に依存させない。
		runtime->replayIncarnation = nextReplayIncarnation_++;
		SyncModelSettings(&document,entity,*runtime,physics,true,false,false,preparedInitialPose_);
	} else runtime->object->SetParent(entity.parentId ? FindObject(entity.parentId) : nullptr);
	return Step::Progress;
}


void SceneObjectSystem::SyncModels(
	SceneDocument* document,
	ScenePhysicsSystem& physicsSystem,
	float deltaTime,
	bool allowAnimatorAutoPlay,
	bool editing
) {
	SyncModelsInternal(document, physicsSystem, deltaTime, allowAnimatorAutoPlay, editing, true);
}

void SceneObjectSystem::SyncModelsForTransition(
	SceneDocument* document, ScenePhysicsSystem& physicsSystem
) {
	// 許可flagの切替はPlayAnimationを再実行するため、Runtimeのtrueを保ち時計だけ止める。
	SyncModelsInternal(document, physicsSystem, 0.0f, true, false, false);
}

void SceneObjectSystem::SyncModelSettings(SceneDocument* document, const SceneEntity& entity,
	ModelRuntime& runtime, ScenePhysicsSystem& physicsSystem, bool allowAnimatorAutoPlay,
	bool editing, bool advanceAnimation, std::unordered_set<uint64_t>& initialAnimationPoseIds) {
	const auto* meshRenderer = FindEnabledComponent(entity, "MeshRenderer");
	const SceneComponent* animator =
		FindEnabledComponent(entity, "Animator");
	// Pool再有効化は新しいclip時刻0から。停止中の通常Frameでは再生設定を切り替えない。
	for (const auto& visual : spawnFrame_.visuals) {
		uint64_t id = entity.id;
		for (size_t depth = 0; id && depth <= document->GetEntities().size(); ++depth) {
			if (id == visual.rootEntityId) {
				if (runtime.spawnActivationSerial != visual.activationSerial) runtime.animatorInitialized = false;
				runtime.spawnActivationSerial = visual.activationSerial;
				break;
			}
			const auto* parent = document->FindEntity(id);
			id = parent ? parent->parentId : 0;
		}
	}
	const bool hasAnimator = animator && runtime.object->HasAnimation();
	const size_t clipCount = runtime.object->GetAnimationClipCount();
	const int defaultClip = clipCount > 0
		? std::clamp(
			animator ? animator->animatorDefaultClip : 0,
			0,
			static_cast<int>(clipCount - 1)
		)
		: 0;
	const AnimationBlendCurve blendCurve =
		animator && animator->animatorBlendCurve == "Linear"
		? AnimationBlendCurve::Linear
		: AnimationBlendCurve::SmoothStep;
	const bool initializeAnimator = !runtime.animatorInitialized;
	if (initializeAnimator) {
		initialAnimationPoseIds.insert(entity.id);
	}

	// 継続中はAuthoring同期によるClip再開始を禁止する。
	if (!deathContinuation_) {
		if (
			!runtime.animatorInitialized ||
			(advanceAnimation && (runtime.hasAnimator != hasAnimator ||
				runtime.animatorAutoPlayAllowed != allowAnimatorAutoPlay))
		) {
			runtime.object->SetAnimationEnabled(hasAnimator);
			if (hasAnimator) {
				runtime.object->SetAnimationLoop(animator->animatorLoop);
				runtime.object->SetAnimationSpeed(animator->animatorSpeed);
				runtime.object->SetAnimationBlendCurve(blendCurve);
				runtime.object->PlayAnimation(
					static_cast<size_t>(defaultClip),
					0.0f,
					true
				);
				runtime.object->SetAnimationPlaying(
					allowAnimatorAutoPlay && animator->animatorPlayOnStart
				);
			}
			runtime.animatorInitialized = true;
			runtime.animatorAutoPlayAllowed = allowAnimatorAutoPlay;
		} else if (hasAnimator && advanceAnimation) {
			if (runtime.animatorPlayOnStart != animator->animatorPlayOnStart) {
				runtime.object->SetAnimationPlaying(
					allowAnimatorAutoPlay && animator->animatorPlayOnStart
				);
			}
			if (runtime.animatorBlendCurve != animator->animatorBlendCurve) {
				runtime.object->SetAnimationBlendCurve(blendCurve);
			}
			if (runtime.animatorLoop != animator->animatorLoop) {
				runtime.object->SetAnimationLoop(animator->animatorLoop);
			}
			if (runtime.animatorSpeed != animator->animatorSpeed) {
				runtime.object->SetAnimationSpeed(animator->animatorSpeed);
			}
			if (runtime.animatorDefaultClip != defaultClip) {
				runtime.object->PlayAnimation(
					static_cast<size_t>(defaultClip),
					(std::max)(animator->animatorTransitionDuration, 0.0f),
					true
				);
			}
		}
		if (advanceAnimation || initializeAnimator) {
			runtime.hasAnimator = hasAnimator;
		}
		if (animator && (advanceAnimation || initializeAnimator)) {
			runtime.animatorPlayOnStart = animator->animatorPlayOnStart;
			runtime.animatorLoop = animator->animatorLoop;
			runtime.animatorSpeed = animator->animatorSpeed;
			runtime.animatorDefaultClip = defaultClip;
			runtime.animatorTransitionDuration =
				animator->animatorTransitionDuration;
			runtime.animatorBlendCurve = animator->animatorBlendCurve;
		}
	} else if (!runtime.animatorInitialized) {
		runtime.object->SetAnimationPlaying(false);
		deathParticipants_.erase(entity.id);
	}

	Transform& runtimeTransform = runtime.object->GetTransform();
	runtimeTransform.scale = entity.transform.scale;
	runtimeTransform.rotate = MakeEulerFromQuaternion(entity.transform.rotate);
	runtimeTransform.translate = entity.transform.translate;
	runtimeTransform.useQuaternionRotation = true;
	runtimeTransform.quaternionRotate = entity.transform.rotate;
	const bool isWaterVolume = HasComponent(entity, "WaterVolume");
	runtime.object->SetCullMode(
		isWaterVolume
			? Object3dCommon::CullMode::kNone
			: (
				meshRenderer
					? ToObjectCullMode(meshRenderer->meshCullMode)
					: Object3dCommon::CullMode::kBack
			)
	);
	if (isWaterVolume) {
		runtime.object->SetColor({ 0.08f, 0.48f, 0.95f, 0.34f });
		runtime.object->SetEnableLighting(false);
		runtime.object->SetEnvironmentCoefficient(0.0f);
		runtime.object->SetEmissive(
			0.18f,
			{ 0.30f, 0.78f, 1.0f, 1.0f }
		);
	} else if (const SceneComponent* agentBehavior =
		FindEnabledComponent(entity, "AgentBehavior")) {
		const SceneComponent resolvedAgentBehavior =
			ResolveAgentBehaviorSettings(
				*document,
				entity,
				*agentBehavior
			);
		runtime.object->SetColor(
			resolvedAgentBehavior.agentVisualColor
		);
		runtime.object->SetEnableLighting(
			resolvedAgentBehavior.agentEnableLighting
		);
		runtime.object->SetEnvironmentCoefficient(0.05f);
		runtime.object->SetEmissive(
			resolvedAgentBehavior.agentEnableLighting ? 0.0f : 0.12f,
			resolvedAgentBehavior.agentVisualColor
		);
	} else if (const SceneComponent* agentAttractor =
		FindEnabledComponent(entity, "AgentAttractor")) {
		runtime.object->SetColor(agentAttractor->attractorVisualColor);
		runtime.object->SetEnableLighting(true);
		runtime.object->SetEnvironmentCoefficient(0.08f);
		runtime.object->SetEmissive(
			0.05f,
			agentAttractor->attractorVisualColor
		);
	} else if (HasComponent(entity, "MonitorRenderer")) {
		runtime.object->SetColor({ 1.0f, 1.0f, 1.0f, 1.0f });
		runtime.object->SetEnableLighting(false);
		runtime.object->SetEnvironmentCoefficient(0.0f);
		runtime.object->SetEmissive(0.0f);
	} else {
		runtime.object->SetColor({ 1.0f, 1.0f, 1.0f, 1.0f });
		runtime.object->SetEnableLighting(true);
		runtime.object->SetEmissive(0.0f);
	}

	const std::vector<SceneMeshMaterialOverride> emptyMaterialOverrides;
	const std::vector<SceneMeshMaterialOverride>& materialOverrides =
		meshRenderer
			? meshRenderer->meshMaterialOverrides
			: emptyMaterialOverrides;
	const std::string materialOverrideSignature =
		BuildMaterialOverrideSignature(materialOverrides);
	if (runtime.materialOverrideSignature != materialOverrideSignature) {
		std::vector<Object3d::MaterialOverride> objectOverrides;
		objectOverrides.reserve(materialOverrides.size());
		for (const SceneMeshMaterialOverride& override : materialOverrides) {
			objectOverrides.push_back({
				override.materialName,
				override.enabled,
				override.colorOverrideEnabled,
				override.color,
				override.texturePath
			});
		}
		runtime.object->SetMaterialOverrides(objectOverrides);
		runtime.materialOverrideSignature = materialOverrideSignature;
	}
	if (HasComponent(entity, "PlayerBehavior")) {
		runtime.object->SetDissolve(0.0f);
	}

	const SceneComponent* obbCollider =
		FindEnabledComponent(entity, "OBBCollider");
	runtime.hasCollider = obbCollider != nullptr;
	if (obbCollider) {
		Collider* runtimeCollider = obbCollider->colliderShape == "Sphere"
			? static_cast<Collider*>(&runtime.sphereCollider)
			: static_cast<Collider*>(&runtime.boxCollider);
		runtimeCollider->SetWorldMatrix(&runtime.object->GetWorldMatrix());
		runtimeCollider->SetOffset(obbCollider->colliderOffset);
		runtimeCollider->SetTrigger(obbCollider->colliderIsTrigger);
		runtimeCollider->SetActive(obbCollider->colliderActive);
		runtimeCollider->SetCollisionAttribute(obbCollider->colliderLayer);
		runtimeCollider->SetCollisionMask(obbCollider->colliderMask);
		if (obbCollider->colliderShape == "Sphere") {
			runtime.sphereCollider.SetRadius(
				(std::max)(obbCollider->colliderSphereRadius, 0.001f)
			);
		} else {
			runtime.boxCollider.SetHalfSize({
				(std::max)(
					std::abs(obbCollider->colliderSizeMultiplier.x),
					0.001f
				),
				(std::max)(
					std::abs(obbCollider->colliderSizeMultiplier.y),
					0.001f
				),
				(std::max)(
					std::abs(obbCollider->colliderSizeMultiplier.z),
					0.001f
				)
			});
		}
		runtime.collider = runtimeCollider;
		runtime.colliderDebugColor = obbCollider->colliderDebugColor;
		runtime.colliderDebugVisible = obbCollider->colliderDebugVisible;
		runtime.colliderDebugDrawMode =
			obbCollider->colliderDebugDrawMode;
		runtime.colliderDebugSegments =
			static_cast<uint32_t>(obbCollider->colliderDebugSegments);
	} else {
		runtime.collider = nullptr;
	}

	const SceneComponent* physicsBody =
		FindEnabledComponent(entity, "PhysicsBody");
	const bool wasPhysicsBody = runtime.hasPhysicsBody;
	runtime.hasPhysicsBody = physicsBody != nullptr;
	if (physicsBody) {
		physicsSystem.ApplyBodyComponent(
			runtime.physicsBody,
			*physicsBody,
			runtime.object.get(),
			runtime.collider,
			!wasPhysicsBody || editing
		);
	} else {
		runtime.physicsBody.syncTransform = {};
	}
}

void SceneObjectSystem::SyncModelsInternal(
	SceneDocument* document,
	ScenePhysicsSystem& physicsSystem,
	float deltaTime,
	bool allowAnimatorAutoPlay,
	bool editing,
	bool advanceAnimation
) {
	// Entity IDを安定キーにし、Model変更時だけRuntime一式を作り直す。
	if (!document) {
		ClearModels();
		return;
	}

	std::unordered_set<uint64_t> requiredIds;
	if (spawnFrame_.documentGeneration != document->GetContentGeneration()) spawnFrame_ = {};
	modelDocument_ = spawnFrame_.documentGeneration != 0 ? document : nullptr;
	std::unordered_set<uint64_t> initialAnimationPoseIds;
	for (const SceneEntity& entity : document->GetEntities()) {
		const SceneComponent* meshRenderer =
			FindEnabledComponent(entity, "MeshRenderer");
		const std::string modelPath = meshRenderer
			? meshRenderer->modelPath
			: std::string{};
		const bool hasRenderer = !modelPath.empty();
		requiredIds.insert(entity.id);
		auto found = models_.find(entity.id);
		if (
			found != models_.end() &&
			(
				found->second.modelPath != modelPath ||
				found->second.hasRenderer != hasRenderer
			)
		) {
			models_.erase(found);
			found = models_.end();
		}

		if (found == models_.end()) {
			ModelRuntime runtime{};
			runtime.object = std::make_unique<Object3d>();
			runtime.object->Initialize(Object3dCommon::GetInstance());
			if (hasRenderer) {
				ModelManager::GetInstance()->LoadModel(modelPath);
				runtime.object->SetModel(modelPath);
			}
			runtime.modelPath = modelPath;
			runtime.hasRenderer = hasRenderer;
			runtime.replayIncarnation = nextReplayIncarnation_++;
			found = models_.emplace(entity.id, std::move(runtime)).first;
		}

		SyncModelSettings(document, entity, found->second, physicsSystem,
			allowAnimatorAutoPlay, editing, advanceAnimation, initialAnimationPoseIds);
	}

	for (auto iterator = models_.begin(); iterator != models_.end();) {
		if (!requiredIds.contains(iterator->first)) {
			iterator = models_.erase(iterator);
		} else {
			++iterator;
		}
	}

	auto resolveObject = [this](uint64_t entityId) -> Object3d* {
		return entityId == 0 ? nullptr : FindObject(entityId);
	};
	for (const SceneEntity& entity : document->GetEntities()) {
		if (Object3d* object = resolveObject(entity.id)) {
			object->SetParent(resolveObject(entity.parentId));
		}
	}

	std::unordered_set<uint64_t> updatedIds;
	std::unordered_set<uint64_t> updatingIds;
	std::function<void(uint64_t)> updateEntity;
	updateEntity = [&](uint64_t entityId) {
		if (updatedIds.contains(entityId) || updatingIds.contains(entityId)) {
			return;
		}
		const SceneEntity* entity = document->FindEntity(entityId);
		Object3d* object = resolveObject(entityId);
		if (!entity || !object) {
			return;
		}
		updatingIds.insert(entityId);
		if (resolveObject(entity->parentId)) {
			updateEntity(entity->parentId);
		}
		if (IsEntityActiveInHierarchy(*document, *entity)) {
			if ((advanceAnimation && !spawnFrame_.blockedEntityIds.contains(entityId)) || initialAnimationPoseIds.contains(entityId)) {
				if (!deathContinuation_ || deathParticipants_.contains(entityId)) {
					object->UpdateAnimation(spawnFrame_.blockedEntityIds.contains(entityId) ? 0.0f : deltaTime);
				}
			}
			object->Update();
		}
		updatingIds.erase(entityId);
		updatedIds.insert(entityId);
	};
	for (const SceneEntity& entity : document->GetEntities()) {
		updateEntity(entity.id);
	}
	ApplySpawnPresentation();
}

RuntimePrepareStep SceneObjectSystem::StepPreparedSprites(const SceneDocument& document,
	const SceneRuntimePrepareBudget& budget, std::string& diagnostic) {
	if (preparedSpriteCursor_ == document.GetEntities().size()) return RuntimePrepareStep::Ready;
	if (!budget.maxItems) return RuntimePrepareStep::Pending;
	while (preparedSpriteCursor_ < document.GetEntities().size()) {
		const auto& entity = document.GetEntities()[preparedSpriteCursor_];
		const auto* renderer = FindEnabledComponent(entity, "SpriteRenderer");
		if (!renderer || renderer->texturePath.empty()) {
			++preparedSpriteCursor_;
			continue;
		}
		if (!preparedSprite_) {
			auto sprite = std::make_unique<Sprite>();
			if (!sprite->BeginPreparedInitialize(*SpriteCommon::GetInstance(), renderer->texturePath, diagnostic)) {
				return RuntimePrepareStep::Failed;
			}
			preparedSprite_ = std::move(sprite);
		}
		for (;;) {
			auto remaining = budget;
			const auto step = preparedSprite_->StepPreparedInitialize(remaining, diagnostic);
			if (step == RuntimePrepareStep::Ready) break;
			if (step != RuntimePrepareStep::Pending) return step;
			// 同期資源作成/copyの進捗がある間だけ続行。未消費のPendingは待機点として戻す。
			if (remaining.maxItems == budget.maxItems &&
				remaining.maxResourceCreates == budget.maxResourceCreates &&
				remaining.maxCopyBytes == budget.maxCopyBytes) return step;
		}
		const Transform transform = ResolveScene2DTransform(document, entity);
		preparedSprite_->SetPosition({ transform.translate.x, transform.translate.y });
		preparedSprite_->SetRotation(transform.rotate.z);
		preparedSprite_->SetSize({ renderer->spriteSize.x * transform.scale.x, renderer->spriteSize.y * transform.scale.y });
		preparedSprite_->SetAnchorPoint(renderer->spriteAnchor);
		preparedSprite_->SetColor(renderer->spriteColor);
		preparedSprite_->SetIsFlipX(renderer->spriteFlipX);
		preparedSprite_->SetIsFlipY(renderer->spriteFlipY);
		if (!SceneUILayout::IsUIEntity(document, entity) && IsEntityActiveInHierarchy(document, entity)) {
			preparedSprite_->Update();
		}
		auto [position, inserted] = sprites_.try_emplace(entity.id);
		if (!inserted) {
			diagnostic = "Destination entity has a duplicate Runtime sprite.";
			return RuntimePrepareStep::Failed;
		}
		position->second.texturePath = renderer->texturePath;
		position->second.sprite = std::move(preparedSprite_);
		++preparedSpriteCursor_;
	}
	return RuntimePrepareStep::Ready;
}

void SceneObjectSystem::SyncSprites(const SceneDocument* document) {
	if (!document) {
		ClearSprites();
		return;
	}

	std::unordered_set<uint64_t> requiredIds;
	for (const SceneEntity& entity : document->GetEntities()) {
		const SceneComponent* spriteRenderer =
			FindEnabledComponent(entity, "SpriteRenderer");
		if (!spriteRenderer || spriteRenderer->texturePath.empty()) {
			continue;
		}

		requiredIds.insert(entity.id);
		auto found = sprites_.find(entity.id);
		if (
			found != sprites_.end() &&
			found->second.texturePath != spriteRenderer->texturePath
		) {
			sprites_.erase(found);
			found = sprites_.end();
		}

		if (found == sprites_.end()) {
			TextureManager::GetInstance()->LoadTexture(
				spriteRenderer->texturePath
			);
			SpriteRuntime runtime{};
			runtime.sprite = std::make_unique<Sprite>();
			runtime.sprite->Initialize(
				SpriteCommon::GetInstance(),
				spriteRenderer->texturePath
			);
			runtime.texturePath = spriteRenderer->texturePath;
			found = sprites_.emplace(entity.id, std::move(runtime)).first;
		}

		Sprite* sprite = found->second.sprite.get();
		const Transform transform = ResolveScene2DTransform(*document, entity);
		sprite->SetPosition({ transform.translate.x, transform.translate.y });
		sprite->SetRotation(transform.rotate.z);
		sprite->SetSize({
			spriteRenderer->spriteSize.x * transform.scale.x,
			spriteRenderer->spriteSize.y * transform.scale.y
		});
		sprite->SetAnchorPoint(spriteRenderer->spriteAnchor);
		sprite->SetColor(spriteRenderer->spriteColor);
		sprite->SetIsFlipX(spriteRenderer->spriteFlipX);
		sprite->SetIsFlipY(spriteRenderer->spriteFlipY);
		if (!SceneUILayout::IsUIEntity(*document, entity) && IsEntityActiveInHierarchy(*document, entity)) {
			sprite->Update();
		}
	}

	for (auto iterator = sprites_.begin(); iterator != sprites_.end();) {
		if (!requiredIds.contains(iterator->first)) {
			iterator = sprites_.erase(iterator);
		} else {
			++iterator;
		}
	}
}

void SceneObjectSystem::BuildBindings(
	SceneDocument& document,
	std::vector<SceneRuntimeObjectBinding>& bindings
) {
	// 外部Systemへ渡す参照なので、所有コンテナの再配置後に必ず作り直す。
	bindings.clear();
	bindings.reserve(document.GetEntities().size());
	for (SceneEntity& entity : document.GetEntities()) {
		ModelRuntime* runtime = FindModelRuntime(entity.id);
		if (!runtime) {
			continue;
		}
		bindings.push_back(SceneRuntimeObjectBinding{
			entity.id,
			&entity,
			runtime->object.get(),
			runtime->hasCollider ? runtime->collider : nullptr,
		runtime->hasPhysicsBody
				? &runtime->physicsBody
				: nullptr
		});
	}
}

void SceneObjectSystem::ObserveReplaySubjects(const SceneDocument& document) {
	for (auto& [id, runtime] : models_) {
		const auto* entity = document.FindEntity(id);
		const bool active = entity && SceneEntityQuery::IsEntityActiveInHierarchy(document, *entity);
		if (active && !runtime.replayActive) runtime.replayIncarnation = nextReplayIncarnation_++;
		runtime.replayActive = active;
	}
}
void SceneObjectSystem::ResetReplayEntity(uint64_t id) {
	if (auto* runtime = FindModelRuntime(id)) runtime->replayIncarnation = nextReplayIncarnation_++;
}
bool SceneObjectSystem::EstimateReplayFrameBytes(const SceneDocument& document, uint64_t& out, uint64_t originOwner) const {
	uint64_t bytes = sizeof(SceneReplayFrame) * 4 + 256; // move中のframe値/末尾nodeの同時存在も含む。
	const auto limit = (std::numeric_limits<uint64_t>::max)();
	for (const auto& entity : document.GetEntities()) {
		const auto* runtime = FindModelRuntime(entity.id);
		if (!runtime || !runtime->hasRenderer || !IsEntityActiveInHierarchy(document, entity) || HasComponent(entity, "WaterVolume")) continue;
		uint64_t poseBytes = 0;
		if (!runtime->object || !runtime->object->EstimateReplayPoseBytes(runtime->modelPath, poseBytes)) return false;
		const uint64_t metadata = sizeof(SceneReplaySubject) * 4 + 128;
		if (poseBytes > limit - metadata || bytes > limit - poseBytes - metadata) return false;
		bytes += poseBytes + metadata;
        // posed boundsはt=0の死亡対象groupだけ。全対象・全sampleへ余分な予約を課さない。
        if (!originOwner || (entity.id != originOwner && !document.IsDescendantOf(entity.id,originOwner))) continue;
        const auto* cached = ModelManager::GetInstance()->FindModel(runtime->modelPath);
        if (!cached) return false;
        const uint64_t vertexScratch = uint64_t(cached->GetVertexCount()) * sizeof(SkinCluster::VertexInfluence);
        if (poseBytes > (limit-vertexScratch)/2) return false;
        const uint64_t boundsScratch = vertexScratch + poseBytes * 2;
        if (bytes > limit - boundsScratch) return false;
        bytes += boundsScratch;
	}
	out = bytes; return true;
}
bool SceneObjectSystem::CaptureReplaySubjects(const SceneDocument& document, SceneReplayHistorySystem& history, SceneReplayFrame& frame) const {
	size_t count = 0;
	for (const auto& entity : document.GetEntities()) {
		const auto* runtime = FindModelRuntime(entity.id);
		if (runtime && runtime->hasRenderer && IsEntityActiveInHierarchy(document, entity) && !HasComponent(entity, "WaterVolume")) ++count;
	}
	frame.subjects.reserve(count);
	for (const auto& entity : document.GetEntities()) {
		const auto* runtime = FindModelRuntime(entity.id);
		if (!runtime || !runtime->hasRenderer || !IsEntityActiveInHierarchy(document, entity) || HasComponent(entity, "WaterVolume")) continue;
		Object3d::ReplayPose pose;
		if (!runtime->object || !runtime->object->CaptureReplayPose(runtime->modelPath, pose)) return false;
		// 一人称のCamera用非表示は採取対象から除かない。影の対象条件も通常modelと共通。
		frame.subjects.push_back({entity.id, runtime->replayIncarnation, true, true,
			history.SharePose(entity.id, runtime->replayIncarnation, std::move(pose))});
        if (history.Status().sampleCount == 0 && (entity.id == frame.identity.owner ||
            document.IsDescendantOf(entity.id, frame.identity.owner))) {
            SceneReplayOriginSubject origin; origin.entityId=entity.id;
            if (!Object3d::ComputeReplayBounds(*frame.subjects.back().pose, origin.bounds)) return false;
            frame.originSubjects.push_back(origin);
        }
	}
	return true;
}

void SceneObjectSystem::ApplyRenderCamera(Camera* camera) {
	ApplySpawnPresentation();
	Object3dCommon::GetInstance()->SetDefaultCamera(camera);
	for (auto& [entityId, runtime] : models_) {
		(void)entityId;
		if (runtime.object) {
			runtime.object->UpdateForCamera(camera);
		}
	}
}

void SceneObjectSystem::SetSpawnPresentationFrame(SceneSpawnFrame frame) {
	spawnFrame_ = std::move(frame);
}

void SceneObjectSystem::ApplySpawnPresentation() {
	// 前Frameの対象も必ず解除する。Modelの作り直しや世代差替えへ倍率を持ち越さない。
	for (auto& [id, runtime] : models_) {
		(void)id;
		if (runtime.object) runtime.object->SetRenderWorldDelta(MakeIdentity4x4());
	}
	if (spawnFrame_.documentGeneration == 0 || !modelDocument_ || spawnFrame_.documentGeneration != modelDocument_->GetContentGeneration()) return;
	for (const auto& visual : spawnFrame_.visuals) {
		const auto* root = modelDocument_->FindEntity(visual.rootEntityId);
		const auto* rootObject = FindObject(visual.rootEntityId);
		if (!root || !root->active || !rootObject || !std::isfinite(visual.scale) || visual.scale < 0.0f || visual.scale > 1.0f) continue;
		const auto& world = rootObject->GetWorldMatrix();
		const Vector3 p{world.m[3][0], world.m[3][1], world.m[3][2]};
		const Matrix4x4 delta = Multiply(Multiply(MakeTranslateMatrix({-p.x, -p.y, -p.z}),
			MakeScaleMatrix({visual.scale, visual.scale, visual.scale})), MakeTranslateMatrix(p));
		for (const auto& entity : modelDocument_->GetEntities()) {
			uint64_t id = entity.id;
			for (size_t depth = 0; id && depth <= modelDocument_->GetEntities().size(); ++depth) {
				if (id == root->id) {
					if (auto* object = FindObject(entity.id)) object->SetRenderWorldDelta(delta);
					break;
				}
				const auto* ancestor = modelDocument_->FindEntity(id);
				id = ancestor ? ancestor->parentId : 0;
			}
		}
	}
}

void SceneObjectSystem::PrepareModelDraw() const {
	Object3dCommon::GetInstance()->SetCommonRenderState();
}

void SceneObjectSystem::DrawModels(
	const SceneDocument& document,
	uint64_t skipEntityId,
	bool hidePlayerModel,
	const std::unordered_map<uint64_t, float>& modelVisibility
) const {
	for (const SceneEntity& entity : document.GetEntities()) {
		if (
			entity.id == skipEntityId ||
			!IsEntityActiveInHierarchy(document, entity) ||
			HasComponent(entity, "WaterVolume") ||
			(hidePlayerModel && HasComponent(entity, "PlayerBehavior"))
		) {
			continue;
		}
		if (Object3d* object = FindObject(entity.id)) {
			const auto found = modelVisibility.find(entity.id);
			if (found == modelVisibility.end()) {
				object->Draw();
			} else if (found->second > 0.0f) {
				object->DrawWithCoverage(found->second);
			}
		}
	}
}

void SceneObjectSystem::DrawSprites(
	const SceneDocument& document,
	uint64_t skipEntityId
) const {
	if (sprites_.empty()) {
		return;
	}

	SpriteCommon::GetInstance()->SetCommonRenderState();
	for (const SceneEntity& entity : document.GetEntities()) {
		const auto found = sprites_.find(entity.id);
		if (
			entity.id != skipEntityId &&
			!SceneUILayout::IsUIEntity(document, entity) &&
			found != sprites_.end() &&
			IsEntityActiveInHierarchy(document, entity) &&
			found->second.sprite
		) {
			found->second.sprite->Draw();
		}
	}
}

void SceneObjectSystem::DrawUISprite(uint64_t entityId, const Matrix4x4& unitQuadToClip,
	const Vector4& color, SpriteCommon::OutputTarget target, bool editorView) const {
	const auto found = sprites_.find(entityId);
	if (found == sprites_.end() || !found->second.sprite) return;
	SpriteCommon::GetInstance()->SetCommonRenderState(target, true);
	found->second.sprite->UpdateUI(unitQuadToClip, color);
	found->second.sprite->Draw(editorView);
}

void SceneObjectSystem::CollectShadowCasters(
	const SceneDocument& document,
	bool hidePlayerModel,
	std::vector<Object3d*>& shadowCasters
) const {
	shadowCasters.reserve(shadowCasters.size() + models_.size());
	for (const SceneEntity& entity : document.GetEntities()) {
		if (
			!IsEntityActiveInHierarchy(document, entity) ||
			HasComponent(entity, "WaterVolume") ||
			(hidePlayerModel && HasComponent(entity, "PlayerBehavior"))
		) {
			continue;
		}
		if (Object3d* object = FindObject(entity.id)) {
			shadowCasters.push_back(object);
		}
	}
}

Object3d* SceneObjectSystem::FindObject(uint64_t entityId) const {
	const ModelRuntime* runtime = FindModelRuntime(entityId);
	return runtime ? runtime->object.get() : nullptr;
}

Object3d* SceneObjectSystem::FindObjectByName(
	const SceneDocument* document,
	const char* name
) const {
	if (!document || !name) {
		return nullptr;
	}
	const SceneEntity* entity = document->FindEntityByName(name);
	return entity ? FindObject(entity->id) : nullptr;
}

SceneObjectSystem::ModelRuntime* SceneObjectSystem::FindModelRuntime(
	uint64_t entityId
) {
	const auto found = models_.find(entityId);
	return found == models_.end() ? nullptr : &found->second;
}

const SceneObjectSystem::ModelRuntime* SceneObjectSystem::FindModelRuntime(
	uint64_t entityId
) const {
	const auto found = models_.find(entityId);
	return found == models_.end() ? nullptr : &found->second;
}

void SceneObjectSystem::ClearModels() {
	preparedObject_.reset();
	preparedCamera_ = nullptr;
	preparedModelCursor_ = 0;
	preparedModelStates_.clear(); preparedDocumentGeneration_ = preparedAssetRevision_ = 0;
	preparedEntityCount_ = preparedLocalReady_ = preparedScanRemaining_ = preparedBindingCursor_ = 0;
	preparedIndexed_ = preparedFinalScan_ = false; preparedBindingStage_ = PreparedBindingStage::Verify;
	preparedWorldStack_.clear(); preparedWorldDone_.clear(); preparedWorldVisiting_.clear(); preparedInitialPose_.clear();
	ClearDeathContinuation();
	spawnFrame_ = {};
	modelDocument_ = nullptr;
	models_.clear();
}

void SceneObjectSystem::ClearSprites() {
	preparedSprite_.reset();
	preparedSpriteCursor_ = 0;
	sprites_.clear();
}

void SceneObjectSystem::Finalize() {
	ClearSprites();
	ClearModels();
}

void SceneObjectSystem::BeginDeathContinuation(const std::unordered_set<uint64_t>& participants,
	const std::unordered_set<uint64_t>& attackTargets) {
	ClearDeathContinuation(); deathContinuation_ = true;
	for (uint64_t id : participants) {
		auto* runtime = FindModelRuntime(id);
		if (!runtime || !runtime->object || !runtime->animatorInitialized) continue;
		deathParticipants_.insert(id);
		if (attackTargets.contains(id)) {
			deathAttackTargets_.insert(id); runtime->object->SetAnimationLoop(false);
		}
	}
}
void SceneObjectSystem::ResetDeathContinuationEntity(uint64_t id) {
	if (deathAttackTargets_.erase(id)) {
		if (auto* runtime = FindModelRuntime(id); runtime && runtime->object) runtime->object->SetAnimationLoop(runtime->animatorLoop);
	}
	deathParticipants_.erase(id);
}
void SceneObjectSystem::ClearDeathContinuation() {
	for (uint64_t id : deathAttackTargets_) {
		if (auto* runtime = FindModelRuntime(id); runtime && runtime->object) runtime->object->SetAnimationLoop(runtime->animatorLoop);
	}
	deathParticipants_.clear(); deathAttackTargets_.clear(); deathContinuation_ = false;
}

bool SceneObjectSystem::TryGetReplayFraming(const SceneDocument& document, uint64_t owner, SceneReplayFramingBasis& out) const {
    const auto* object=FindObject(owner);
    if (!object) return false;
    SceneReplayFramingBasis basis; basis.world=object->GetWorldMatrix();
    for (const auto& entity : document.GetEntities()) {
        if ((entity.id!=owner && !document.IsDescendantOf(entity.id,owner)) || !IsEntityActiveInHierarchy(document,entity) ||
            HasComponent(entity,"WaterVolume")) continue;
        const auto* runtime=FindModelRuntime(entity.id);
        if (!runtime || !runtime->hasRenderer || !runtime->object) continue;
        Object3d::ReplayPose pose; Object3d::ReplayBounds bounds;
        if (!runtime->object->CaptureReplayPose(runtime->modelPath,pose) || !Object3d::ComputeReplayBounds(pose,bounds) || !bounds.valid) return false;
        if (!basis.valid) { basis.min=bounds.min; basis.max=bounds.max; basis.valid=true; }
        else {
            basis.min={(std::min)(basis.min.x,bounds.min.x),(std::min)(basis.min.y,bounds.min.y),(std::min)(basis.min.z,bounds.min.z)};
            basis.max={(std::max)(basis.max.x,bounds.max.x),(std::max)(basis.max.y,bounds.max.y),(std::max)(basis.max.z,bounds.max.z)};
        }
    }
    if (!basis.valid) return false;
    out=basis; return true;
}

void SceneObjectSystem::RefreshTransforms(const SceneDocument& document, const std::vector<uint64_t>& changedTargets) {
	std::unordered_set<uint64_t> updated, visiting;
	std::function<void(uint64_t)> refresh = [&](uint64_t id) {
		if (updated.contains(id) || !visiting.insert(id).second) return;
		const auto* entity = document.FindEntity(id);
		auto* runtime = FindModelRuntime(id);
		if (entity && runtime && runtime->object) {
			if (entity->parentId) refresh(entity->parentId);
			auto& transform = runtime->object->GetTransform();
			transform.translate = entity->transform.translate; transform.scale = entity->transform.scale;
			transform.useQuaternionRotation = true; transform.quaternionRotate = entity->transform.rotate;
			runtime->object->Update();
		}
		visiting.erase(id); updated.insert(id);
	};
	for (const auto& entity : document.GetEntities()) {
		for (uint64_t id : changedTargets) if (entity.id == id || document.IsDescendantOf(entity.id, id)) { refresh(entity.id); break; }
	}
}
