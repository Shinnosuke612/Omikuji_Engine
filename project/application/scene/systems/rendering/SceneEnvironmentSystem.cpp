// 役割: EnvironmentとWaterVolume Componentを実行時の環境描画へ反映する。
#include "SceneEnvironmentSystem.h"

#include "../../../../engine/2d/TextureManager.h"
#include "../../../../engine/3d/Camera.h"
#include "../../../../engine/3d/Object3d.h"
#include "../../../../engine/3d/Object3dCommon.h"
#include "../../../../engine/3d/Skybox.h"
#include "../../../../engine/effect/WaterSurfaceRenderer.h"
#include "../../../../engine/scene/SceneDocument.h"
#include "../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../engine/scene/SceneTransformResolver.h"
#include "../../../../engine/utility/EditableResourcePath.h"
#include "../../../../engine/utility/StringUtility.h"

#include <algorithm>
#include <filesystem>

namespace {
	float ResolveReflectionIntensity(
		const SceneComponent* meshRenderer,
		float environmentDefault
	) {
		if (
			meshRenderer &&
			meshRenderer->meshEnvironmentReflectionOverride
		) {
			return std::clamp(
				meshRenderer->meshEnvironmentReflectionIntensity,
				0.0f,
				1.0f
			);
		}
		return std::clamp(environmentDefault, 0.0f, 1.0f);
	}
}

SceneEnvironmentSystem::SceneEnvironmentSystem() = default;

SceneEnvironmentSystem::~SceneEnvironmentSystem() {
	Finalize();
}

void SceneEnvironmentSystem::Initialize(DirectXCommon* dxCommon) {
	Finalize();
	waterSurfaceRenderer_ = std::make_unique<WaterSurfaceRenderer>();
	waterSurfaceRenderer_->Initialize(dxCommon);
}

void SceneEnvironmentSystem::Sync(
	const SceneDocument* document,
	const std::vector<SceneRuntimeObjectBinding>& bindings
) {
	const SceneComponent* environment = nullptr;
	if (document) {
		for (const SceneEntity& entity : document->GetEntities()) {
			if (!SceneEntityQuery::IsEntityActiveInHierarchy(*document, entity)) {
				continue;
			}
			environment =
				SceneEntityQuery::FindEnabledComponent(entity, "Environment");
			if (environment) {
				break;
			}
		}
	}

	bool skyboxEnabled = false;
	std::string requestedPath;
	float skyboxIntensity = 1.0f;
	float requestedReflectionIntensity = 0.3f;
	if (environment) {
		skyboxEnabled = environment->environmentSkyboxEnabled;
		requestedPath = environment->environmentSkyboxPath.empty()
			? "resources/rostock_laage_airport_4k.dds"
			: environment->environmentSkyboxPath;
		skyboxIntensity =
			(std::max)(0.0f, environment->environmentSkyboxIntensity);
		requestedReflectionIntensity = std::clamp(
			environment->environmentReflectionIntensity,
			0.0f,
			1.0f
		);
	}

	std::string texturePath;
	if (skyboxEnabled && !requestedPath.empty()) {
		const std::filesystem::path requestedFilePath =
			StringUtility::ToPath(requestedPath);
		texturePath = requestedFilePath.is_absolute()
			? StringUtility::ToUtf8(
				EditableResourcePath::ToProjectRelative(requestedFilePath)
			)
			: requestedPath;
	}

	if (!skyboxEnabled || texturePath.empty()) {
		skybox_.reset();
		environmentMapPath_.clear();
		reflectionIntensity_ = 0.0f;
	} else if (texturePath != environmentMapPath_) {
		if (TextureManager::GetInstance()->LoadTexture(texturePath)) {
			skybox_ = std::make_unique<Skybox>();
			skybox_->Initialize(Object3dCommon::GetInstance(), texturePath);
			skybox_->SetScale({ 100.0f, 100.0f, 100.0f });
			environmentMapPath_ = texturePath;
			reflectionIntensity_ = requestedReflectionIntensity;
		}
	} else {
		reflectionIntensity_ = requestedReflectionIntensity;
	}

	if (skybox_) {
		skybox_->SetColor({
			skyboxIntensity,
			skyboxIntensity,
			skyboxIntensity,
			1.0f
		});
	}

	// ObjectはEditor操作で再生成されるため、反射設定は現在のbindingsへ毎回適用する。
	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (!binding.entity || !binding.object) {
			continue;
		}
		const SceneComponent* meshRenderer =
			SceneEntityQuery::FindEnabledComponent(
				*binding.entity,
				"MeshRenderer"
			);
		const bool monitorSurface =
			SceneEntityQuery::HasComponent(*binding.entity, "MonitorRenderer");
		binding.object->SetEnvironmentMap(
			environmentMapPath_,
			monitorSurface
				? 0.0f
				: ResolveReflectionIntensity(
					meshRenderer,
					reflectionIntensity_
				)
		);
		if (monitorSurface) {
			binding.object->SetEnableLighting(false);
		}
	}
}

void SceneEnvironmentSystem::Update(float deltaTime) {
	if (waterSurfaceRenderer_) {
		waterSurfaceRenderer_->Update(deltaTime);
	}
}

void SceneEnvironmentSystem::ApplyRenderCamera(Camera* camera) {
	if (skybox_) {
		skybox_->SetCamera(camera);
		skybox_->Update();
	}
}

void SceneEnvironmentSystem::DrawSkybox() {
	if (skybox_) {
		skybox_->Draw();
	}
}

void SceneEnvironmentSystem::DrawWaterSurfaces(
	const SceneDocument& document,
	Camera* camera,
	uint64_t skipEntityId
) {
	if (!waterSurfaceRenderer_ || !camera) {
		return;
	}

	for (const SceneEntity& entity : document.GetEntities()) {
		if (
			entity.id == skipEntityId ||
			!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)
		) {
			continue;
		}
		const SceneComponent* waterVolume =
			SceneEntityQuery::FindEnabledComponent(entity, "WaterVolume");
		if (!waterVolume) {
			continue;
		}

		const Transform transform =
			SceneTransformResolver::ResolveScene3DTransform(document, entity);
		const Vector3 center{
			transform.translate.x + waterVolume->waterOffset.x,
			transform.translate.y + waterVolume->waterOffset.y,
			transform.translate.z + waterVolume->waterOffset.z
		};
		const Vector3 halfSize{
			(std::max)(waterVolume->waterHalfSize.x, 0.05f),
			(std::max)(waterVolume->waterHalfSize.y, 0.05f),
			(std::max)(waterVolume->waterHalfSize.z, 0.05f)
		};

		WaterSurfaceRenderer::Settings settings{};
		settings.enabled = waterVolume->waterSurfaceEnabled;
		settings.baseColor = waterVolume->waterSurfaceBaseColor;
		settings.highlightColor = waterVolume->waterSurfaceHighlightColor;
		settings.alpha = waterVolume->waterSurfaceAlpha;
		settings.waveScale = waterVolume->waterSurfaceWaveScale;
		settings.normalStrength = waterVolume->waterSurfaceNormalStrength;
		settings.fresnelPower = waterVolume->waterSurfaceFresnelPower;
		waterSurfaceRenderer_->Draw(camera, center, halfSize, settings);
	}
}

void SceneEnvironmentSystem::DrawEditor(SceneDocument& document) {
	const auto generatedSkybox = starFieldGenerator_.DrawImGui("Environment");
	if (
		!generatedSkybox ||
		!TextureManager::GetInstance()->ReloadTexture(*generatedSkybox)
	) {
		return;
	}

	SceneComponent* targetEnvironment = nullptr;
	for (SceneEntity& entity : document.GetEntities()) {
		SceneComponent* environment =
			SceneEntityQuery::FindComponent(entity, "Environment");
		if (environment) {
			targetEnvironment = environment;
			break;
		}
	}
	if (!targetEnvironment) {
		SceneEntity& entity = document.CreateEntity("Environment");
		document.AddComponent(entity.id, "Environment");
		targetEnvironment = &entity.components.back();
	}
	targetEnvironment->environmentSkyboxEnabled = true;
	targetEnvironment->environmentSkyboxPath =
		StringUtility::ToUtf8(EditableResourcePath::ToProjectRelative(
			StringUtility::ToPath(*generatedSkybox)
		));
	document.MarkDirty();
}

void SceneEnvironmentSystem::Finalize() {
	if (waterSurfaceRenderer_) {
		waterSurfaceRenderer_->Finalize();
		waterSurfaceRenderer_.reset();
	}
	skybox_.reset();
	environmentMapPath_.clear();
	reflectionIntensity_ = 0.3f;
}
