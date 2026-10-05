// 役割: 水面屈折の前後関係を保ちながらParticle、Sprite、Lightningを描画する。
#include "SceneEffectRenderSystem.h"

#include "../rendering/SceneEnvironmentSystem.h"
#include "../rendering/SceneObjectSystem.h"
#include "../../../../engine/effect/GroundCrackRenderer.h"
#include "../../../../engine/3d/Camera.h"
#include "../../../../engine/effect/LightningRenderer.h"
#include "../../../../engine/particle/ParticleManager.h"
#include "../../../../engine/scene/SceneDocument.h"
#include "../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../engine/scene/SceneTransformResolver.h"

#include <algorithm>
#include <cmath>

namespace {
	using SceneEntityQuery::FindEnabledComponent;
	using SceneEntityQuery::IsEntityActiveInHierarchy;

	bool TryBuildWaterParticleDrawFilter(
		const SceneDocument& document,
		Camera* camera,
		ParticleManager::WaterDrawFilter& filter
	) {
		if (
			!camera ||
			!document.GetPostProcessSettings().waterRefractionEnabled
		) {
			return false;
		}

		const Vector3 cameraPosition = camera->GetTranslate();
		for (const SceneEntity& entity : document.GetEntities()) {
			if (!IsEntityActiveInHierarchy(document, entity)) {
				continue;
			}
			const SceneComponent* waterVolume =
				FindEnabledComponent(entity, "WaterVolume");
			if (!waterVolume) {
				continue;
			}

			const Transform transform =
				SceneTransformResolver::ResolveScene3DTransform(
					document,
					entity
				);
			const Vector3 center{
				transform.translate.x + waterVolume->waterOffset.x,
				transform.translate.y + waterVolume->waterOffset.y,
				transform.translate.z + waterVolume->waterOffset.z
			};
			const Vector3 halfSize{
				(std::max)(waterVolume->waterHalfSize.x, 0.001f),
				(std::max)(waterVolume->waterHalfSize.y, 0.001f),
				(std::max)(waterVolume->waterHalfSize.z, 0.001f)
			};
			if (
				std::abs(cameraPosition.x - center.x) <= halfSize.x &&
				std::abs(cameraPosition.y - center.y) <= halfSize.y &&
				std::abs(cameraPosition.z - center.z) <= halfSize.z
			) {
				return false;
			}

			filter.cameraPosition = cameraPosition;
			filter.waterCenter = center;
			filter.waterHalfSize = halfSize;
			return true;
		}

		return false;
	}
}

SceneEffectRenderSystem::SceneEffectRenderSystem() = default;

SceneEffectRenderSystem::~SceneEffectRenderSystem() {
	Finalize();
}

void SceneEffectRenderSystem::Initialize(DirectXCommon* dxCommon) {
	Finalize();
	if (!dxCommon) {
		return;
	}
	lightningRenderer_ = std::make_unique<LightningRenderer>();
	lightningRenderer_->Initialize(dxCommon);
	groundCrackRenderer_ = std::make_unique<GroundCrackRenderer>();
	groundCrackRenderer_->Initialize(dxCommon);
}

void SceneEffectRenderSystem::Update(float deltaTime) {
	if (!lightningRenderer_) {
		return;
	}

	ParticleManager::LightningEvent event{};
	while (ParticleManager::GetInstance()->ConsumeLightningEvent(event)) {
		LightningRenderer::Settings settings =
			lightningRenderer_->GetSettings();
		settings.start = event.start;
		settings.end = event.end;
		settings.coreColor = event.desc.coreColor;
		settings.branchColor = event.desc.branchColor;
		settings.jitter = event.desc.jitter;
		settings.branchLength = event.desc.branchLength;
		settings.branchProbability = event.desc.branchProbability;
		settings.thickness = event.desc.thickness;
		settings.duration = event.desc.duration;
		settings.segmentCount = event.desc.segmentCount;
		settings.seed = event.seed;
		lightningRenderer_->Trigger(settings);
	}
	lightningRenderer_->Update(deltaTime);
	if (groundCrackRenderer_) { groundCrackRenderer_->Update(deltaTime); }
}

void SceneEffectRenderSystem::SpawnGroundCracks(
	const std::vector<SceneGroundCrackSpawnRequest>& requests
) {
	if (!groundCrackRenderer_) { return; }
	for (const SceneGroundCrackSpawnRequest& request : requests) {
		groundCrackRenderer_->Spawn({
			request.position, request.normal, request.radius,
			request.primaryBranchCount, request.segmentsPerBranch,
			request.branchProbability, request.width, request.lifetime,
			request.surfaceOffset, request.seed
		});
	}
}

void SceneEffectRenderSystem::DrawScenePass(
	SceneDocument* document,
	Camera* camera,
	uint64_t skipEntityId,
	SceneEnvironmentSystem& environmentSystem,
	SceneObjectSystem& objectSystem
) {
	// 通常構成では水面を先に描き、前景分離時は合成Passへ描画を遅延する。
	if (!deferForegroundEffects_ && document) {
		environmentSystem.DrawWaterSurfaces(
			*document,
			camera,
			skipEntityId
		);
	}

	if (lightningRenderer_) {
		lightningRenderer_->Draw(camera);
	}
	if (groundCrackRenderer_) { groundCrackRenderer_->Draw(camera); }

	if (deferForegroundEffects_) {
		DrawParticles(document, camera, false);
	} else {
		DrawParticles(document, camera, true);
		if (camera && document) {
			objectSystem.DrawSprites(*document, skipEntityId);
		}
	}
}

void SceneEffectRenderSystem::DrawForegroundPass(
	SceneDocument* document,
	Camera* camera,
	uint64_t skipEntityId,
	SceneEnvironmentSystem& environmentSystem,
	SceneObjectSystem& objectSystem
) {
	if (!camera) {
		return;
	}

	if (deferForegroundEffects_ && document) {
		environmentSystem.DrawWaterSurfaces(
			*document,
			camera,
			skipEntityId
		);
	}
	DrawParticles(document, camera, true);
	if (document) {
		objectSystem.DrawSprites(*document, skipEntityId);
	}
}

void SceneEffectRenderSystem::DrawParticles(
	const SceneDocument* document,
	Camera* camera,
	bool foreground
) const {
	if (!camera) {
		return;
	}

	ParticleManager* particleManager = ParticleManager::GetInstance();
	ParticleManager::WaterDrawFilter filter{};
	if (
		deferForegroundEffects_ &&
		document &&
		TryBuildWaterParticleDrawFilter(*document, camera, filter)
	) {
		filter.mode = foreground
			? ParticleManager::WaterDrawMode::kForeground
			: ParticleManager::WaterDrawMode::kRefracted;
		particleManager->RefreshCpuParticleInstancesForCamera(camera, filter);
		particleManager->Draw(!foreground);
		return;
	}

	if (foreground) {
		particleManager->RefreshCpuParticleInstancesForCamera(camera);
		particleManager->Draw();
	}
}

void SceneEffectRenderSystem::Finalize() {
	if (groundCrackRenderer_) {
		groundCrackRenderer_->Finalize();
		groundCrackRenderer_.reset();
	}
	if (lightningRenderer_) {
		lightningRenderer_->Finalize();
		lightningRenderer_.reset();
	}
	deferForegroundEffects_ = false;
}
