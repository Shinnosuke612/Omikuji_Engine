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

		const SceneComponent* animator =
			FindEnabledComponent(entity, "Animator");
		ModelRuntime& runtime = found->second;
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
	bool hidePlayerModel
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
			object->Draw();
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
	ClearDeathContinuation();
	spawnFrame_ = {};
	modelDocument_ = nullptr;
	models_.clear();
}

void SceneObjectSystem::ClearSprites() {
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
