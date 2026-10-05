// 役割: Prefab DocumentをPreview専用Object3dへ同期し、全階層を描画する。
#include "PrefabPreviewRenderer.h"

#include "../engine/3d/Camera.h"
#include "../engine/3d/Model.h"
#include "../engine/3d/ModelManager.h"
#include "../engine/3d/Object3d.h"
#include "../engine/3d/Object3dCommon.h"
#include "../engine/3d/SrvManager.h"
#include "../engine/base/RenderFormats.h"
#include "../engine/base/SceneRenderTarget.h"
#include "../engine/collision/OBBCollider.h"
#include "../engine/collision/SphereCollider.h"
#include "../engine/debug/DebugRenderer.h"
#include "../engine/debug/EditorGridRenderer.h"
#include "../engine/math/Matrix4x4.h"
#include "../engine/math/Math.h"
#include "../engine/math/Quaternion.h"
#include "../engine/scene/SceneDocument.h"
#include "../engine/scene/SceneEntityQuery.h"
#include "../engine/scene/SceneTransformResolver.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
	using SceneEntityQuery::FindEnabledComponent;
	using SceneEntityQuery::IsEntityActiveInHierarchy;
	using SceneTransformResolver::ResolveSceneWorldMatrix;

	Object3dCommon::CullMode ResolveCullMode(const std::string& cullMode) {
		if (cullMode == "None") {
			return Object3dCommon::CullMode::kNone;
		}
		if (cullMode == "Front") {
			return Object3dCommon::CullMode::kFront;
		}
		return Object3dCommon::CullMode::kBack;
	}

	Vector3 TransformPoint(const Vector3& point, const Matrix4x4& matrix) {
		return {
			point.x * matrix.m[0][0] + point.y * matrix.m[1][0] +
				point.z * matrix.m[2][0] + matrix.m[3][0],
			point.x * matrix.m[0][1] + point.y * matrix.m[1][1] +
				point.z * matrix.m[2][1] + matrix.m[3][1],
			point.x * matrix.m[0][2] + point.y * matrix.m[1][2] +
				point.z * matrix.m[2][2] + matrix.m[3][2]
		};
	}

	std::vector<Object3d::MaterialOverride> BuildMaterialOverrides(
		const SceneComponent& meshRenderer
	) {
		std::vector<Object3d::MaterialOverride> result;
		result.reserve(meshRenderer.meshMaterialOverrides.size());
		for (const SceneMeshMaterialOverride& override :
			meshRenderer.meshMaterialOverrides) {
			result.push_back({
				override.materialName,
				override.enabled,
				override.colorOverrideEnabled,
				override.color,
				override.texturePath
			});
		}
		return result;
	}
}

PrefabPreviewRenderer::~PrefabPreviewRenderer() {
	Finalize();
}

bool PrefabPreviewRenderer::Initialize(
	DirectXCommon* dxCommon,
	SrvManager* srvManager
) {
	Finalize();
	dxCommon_ = dxCommon;
	srvManager_ = srvManager;
	if (!dxCommon_ || !srvManager_) {
		return false;
	}

	renderTarget_ = new SceneRenderTarget();
	SceneRenderTarget::Desc desc{};
	desc.width = 768;
	desc.height = 432;
	desc.format = RenderFormats::kSceneHdrFormat;
	desc.createDepth = true;
	desc.clearColor[0] = 0.035f;
	desc.clearColor[1] = 0.04f;
	desc.clearColor[2] = 0.05f;
	desc.clearColor[3] = 1.0f;
	if (!renderTarget_->Initialize(dxCommon_, srvManager_, desc)) {
		delete renderTarget_;
		renderTarget_ = nullptr;
		dxCommon_ = nullptr;
		srvManager_ = nullptr;
		return false;
	}

	camera_ = new Camera();
	camera_->SetOrbitMode(true);
	camera_->SetFovY(0.7f);
	camera_->SetAspectRatio(
		static_cast<float>(desc.width) / static_cast<float>(desc.height)
	);
	camera_->SetNearClip(0.01f);
	camera_->SetFarClip(10000.0f);
	return true;
}

void PrefabPreviewRenderer::Render(
	const std::string& assetPath,
	const SceneDocument& document,
	const SceneDocument* ghostDocument,
	uint32_t width,
	uint32_t height,
	float yaw,
	float pitch,
	float zoom,
	uint64_t framingSerial,
	const OverlayOptions& overlayOptions
) {
	if (!renderTarget_ || !camera_ || !srvManager_) {
		return;
	}

	width = std::clamp(width, 320u, 1600u);
	height = std::clamp(height, 180u, 900u);
	renderTarget_->Resize(width, height);
	SyncModels(document);
	SyncGhostModels(ghostDocument);
	const bool framingRequested =
		!framingInitialized_ ||
		framedAssetPath_ != assetPath ||
		appliedFramingSerial_ != framingSerial;
	if (framingRequested) {
		// Transform編集中にCameraまで追従すると、次FrameのGizmo操作量が
		// 再計算されて移動が増幅するため、明示要求時だけFramingを更新する。
		UpdateFraming(document, overlayOptions);
		framedAssetPath_ = assetPath;
		appliedFramingSerial_ = framingSerial;
		framingInitialized_ = true;
	}

	camera_->SetAspectRatio(
		static_cast<float>(renderTarget_->GetWidth()) /
		static_cast<float>(renderTarget_->GetHeight())
	);
	camera_->SetOrbitTarget(orbitTarget_);
	camera_->SetOrbitAngle(yaw, pitch);
	camera_->SetOrbitDistance(
		(std::max)(fitDistance_ * (std::max)(zoom, 0.02f), 0.02f)
	);
	camera_->UpdatePreviewMatrices();
	viewMatrix_ = camera_->GetViewMatrix();
	projectionMatrix_ = camera_->GetProjectionMatrix();

	for (auto& [entityId, runtime] : models_) {
		(void)entityId;
		runtime.object->UpdateForCamera(camera_);
	}
	for (auto& [entityId, runtime] : ghostModels_) {
		(void)entityId;
		runtime.object->UpdateForCamera(camera_);
	}

	renderTarget_->Begin();
	srvManager_->PreDraw();
	Object3dCommon::GetInstance()->SetCommonRenderState();
	for (const auto& [entityId, runtime] : ghostModels_) {
		(void)entityId;
		runtime.object->Draw();
	}
	for (const auto& [entityId, runtime] : models_) {
		(void)entityId;
		runtime.object->Draw();
	}
	DrawEditorOverlays(document, ghostDocument, overlayOptions);
	renderTarget_->End();
}

void PrefabPreviewRenderer::Finalize() {
	models_.clear();
	ghostModels_.clear();
	delete camera_;
	camera_ = nullptr;
	delete renderTarget_;
	renderTarget_ = nullptr;
	dxCommon_ = nullptr;
	srvManager_ = nullptr;
	framedAssetPath_.clear();
	appliedFramingSerial_ = 0;
	framingInitialized_ = false;
	orbitTarget_ = {};
	fitDistance_ = 5.0f;
	viewMatrix_ = MakeIdentity4x4();
	projectionMatrix_ = MakeIdentity4x4();
}

D3D12_GPU_DESCRIPTOR_HANDLE PrefabPreviewRenderer::GetTexture() const {
	return renderTarget_
		? renderTarget_->GetSrvGpuHandle()
		: D3D12_GPU_DESCRIPTOR_HANDLE{};
}

uint32_t PrefabPreviewRenderer::GetWidth() const {
	return renderTarget_ ? renderTarget_->GetWidth() : 1u;
}

uint32_t PrefabPreviewRenderer::GetHeight() const {
	return renderTarget_ ? renderTarget_->GetHeight() : 1u;
}

void PrefabPreviewRenderer::SyncModels(const SceneDocument& document) {
	std::unordered_set<uint64_t> requiredIds;
	for (const SceneEntity& entity : document.GetEntities()) {
		const SceneComponent* meshRenderer =
			FindEnabledComponent(entity, "MeshRenderer");
		if (
			!meshRenderer ||
			meshRenderer->modelPath.empty() ||
			!IsEntityActiveInHierarchy(document, entity)
		) {
			continue;
		}

		requiredIds.insert(entity.id);
		auto found = models_.find(entity.id);
		if (
			found != models_.end() &&
			found->second.modelPath != meshRenderer->modelPath
		) {
			models_.erase(found);
			found = models_.end();
		}
		if (found == models_.end()) {
			ModelManager* modelManager = ModelManager::GetInstance();
			modelManager->LoadModel(meshRenderer->modelPath);
			Model* model = modelManager->FindModel(meshRenderer->modelPath);
			if (!model) {
				continue;
			}

			ModelRuntime runtime{};
			runtime.object = std::make_unique<Object3d>();
			runtime.object->Initialize(Object3dCommon::GetInstance());
			runtime.object->SetModel(model);
			runtime.object->SetCamera(camera_);
			// Previewは通常SceneのLight Bindingへ依存させない。
			runtime.object->SetEnableLighting(false);
			runtime.object->SetEnvironmentCoefficient(0.0f);
			runtime.modelPath = meshRenderer->modelPath;
			found = models_.emplace(entity.id, std::move(runtime)).first;
		}

		Object3d& object = *found->second.object;
		object.SetScale({ 1.0f, 1.0f, 1.0f });
		object.SetRotateQuaternion(MakeIdentityQuaternion());
		object.SetTranslate({ 0.0f, 0.0f, 0.0f });
		object.SetParentMatrixOverride(
			ResolveSceneWorldMatrix(document, entity)
		);
		object.SetCullMode(ResolveCullMode(meshRenderer->meshCullMode));
		object.SetMaterialOverrides(BuildMaterialOverrides(*meshRenderer));
	}

	for (auto it = models_.begin(); it != models_.end();) {
		if (!requiredIds.contains(it->first)) {
			it = models_.erase(it);
		} else {
			++it;
		}
	}
}

void PrefabPreviewRenderer::SyncGhostModels(const SceneDocument* document) {
	if (!document) {
		ghostModels_.clear();
		return;
	}

	std::unordered_set<uint64_t> requiredIds;
	for (const SceneEntity& entity : document->GetEntities()) {
		const SceneComponent* meshRenderer =
			FindEnabledComponent(entity, "MeshRenderer");
		if (
			!meshRenderer ||
			meshRenderer->modelPath.empty() ||
			!IsEntityActiveInHierarchy(*document, entity)
		) {
			continue;
		}

		requiredIds.insert(entity.id);
		auto found = ghostModels_.find(entity.id);
		if (
			found != ghostModels_.end() &&
			found->second.modelPath != meshRenderer->modelPath
		) {
			ghostModels_.erase(found);
			found = ghostModels_.end();
		}
		if (found == ghostModels_.end()) {
			ModelManager* modelManager = ModelManager::GetInstance();
			modelManager->LoadModel(meshRenderer->modelPath);
			Model* model = modelManager->FindModel(meshRenderer->modelPath);
			if (!model) {
				continue;
			}

			ModelRuntime runtime{};
			runtime.object = std::make_unique<Object3d>();
			runtime.object->Initialize(Object3dCommon::GetInstance());
			runtime.object->SetModel(model);
			runtime.object->SetCamera(camera_);
			// GhostはBase Poseと区別するため、Scene側のLight Bindingを使わず色を固定する。
			runtime.object->SetEnableLighting(false);
			runtime.object->SetEnvironmentCoefficient(0.0f);
			runtime.modelPath = meshRenderer->modelPath;
			found = ghostModels_.emplace(entity.id, std::move(runtime)).first;
		}

		Object3d& object = *found->second.object;
		object.SetScale({ 1.0f, 1.0f, 1.0f });
		object.SetRotateQuaternion(MakeIdentityQuaternion());
		object.SetTranslate({ 0.0f, 0.0f, 0.0f });
		object.SetParentMatrixOverride(
			ResolveSceneWorldMatrix(*document, entity)
		);
		object.SetCullMode(ResolveCullMode(meshRenderer->meshCullMode));
		object.SetMaterialOverrides(BuildMaterialOverrides(*meshRenderer));
		object.SetColor({ 0.25f, 0.75f, 1.0f, 0.30f });
	}

	for (auto it = ghostModels_.begin(); it != ghostModels_.end();) {
		if (!requiredIds.contains(it->first)) {
			it = ghostModels_.erase(it);
		} else {
			++it;
		}
	}
}

void PrefabPreviewRenderer::DrawEditorOverlays(
	const SceneDocument& document,
	const SceneDocument* ghostDocument,
	const OverlayOptions& options
) {
#if defined(_DEBUG) || defined(DEVELOPMENT)
	DebugRenderer* debugRenderer = DebugRenderer::GetInstance();
	// Prefab Previewは通常SceneのDebug描画後に実行される。収集済み頂点を
	// 持ち込むとScene側の形状までPrefab RenderTargetへ再描画されるため分離する。
	debugRenderer->ClearGeometry();
	if (options.showGrid) {
		EditorGridSettings gridSettings{};
		gridSettings.extent = std::clamp(fitDistance_ * 4.0f, 10.0f, 200.0f);
		EditorGridRenderer::AddGrid(*debugRenderer, gridSettings);
	}

	if (options.showSkeleton) {
		const float jointRadius = std::clamp(
			fitDistance_ * 0.004f,
			0.006f,
			0.08f
		);
		for (const auto& [entityId, runtime] : models_) {
			(void)entityId;
			runtime.object->DrawSkeletonDebug(
				false,
				options.showJointAxes,
				jointRadius,
				jointRadius * 3.2f
			);
		}
	}

	for (const SceneEntity& entity : document.GetEntities()) {
		const SceneComponent* colliderComponent =
			FindEnabledComponent(entity, "OBBCollider");
		if (!colliderComponent) {
			continue;
		}
		if (
			options.isolateSelectedCollider &&
			entity.id != options.selectedEntityId
		) {
			continue;
		}

		const bool isHitBox =
			FindEnabledComponent(entity, "HitBox") != nullptr;
		const bool isHurtBox =
			FindEnabledComponent(entity, "HurtBox") != nullptr;
		const bool isCombatVolume = isHitBox || isHurtBox;
		const bool shouldDraw = isCombatVolume
			? options.showCombatVolumes || options.showColliders
			: options.showColliders;
		if (!shouldDraw) {
			continue;
		}

		Vector4 color = colliderComponent->colliderDebugColor;
		if (options.showCombatVolumes && isHitBox && isHurtBox) {
			color = { 0.95f, 0.25f, 1.0f, 1.0f };
		} else if (options.showCombatVolumes && isHitBox) {
			color = { 1.0f, 0.22f, 0.12f, 1.0f };
		} else if (options.showCombatVolumes && isHurtBox) {
			color = { 0.15f, 0.82f, 1.0f, 1.0f };
		}
		if (entity.id == options.selectedEntityId) {
			color = { 1.0f, 0.95f, 0.18f, 1.0f };
		} else if (
			!colliderComponent->colliderActive ||
			!IsEntityActiveInHierarchy(document, entity)
		) {
			color.w *= 0.35f;
		}

		const bool drawWire =
			colliderComponent->colliderDebugDrawMode != "Solid";
		const bool drawSolid =
			colliderComponent->colliderDebugDrawMode != "Wireframe";
		Vector4 solidColor = color;
		solidColor.w = (std::min)(solidColor.w * 0.35f, 0.18f);
		const uint32_t segments = static_cast<uint32_t>(std::clamp(
			colliderComponent->colliderDebugSegments,
			4,
			64
		));
		const Matrix4x4 worldMatrix =
			ResolveSceneWorldMatrix(document, entity);

		if (colliderComponent->colliderShape == "Sphere") {
			SphereCollider collider;
			collider.SetWorldMatrix(&worldMatrix);
			collider.SetOffset(colliderComponent->colliderOffset);
			collider.SetRadius((std::max)(
				colliderComponent->colliderSphereRadius,
				0.001f
			));
			if (drawSolid) {
				debugRenderer->AddSolidSphere(
					collider.GetWorldCenter(),
					collider.GetRadius(),
					solidColor,
					segments
				);
			}
			if (drawWire) {
				debugRenderer->AddSphere(
					collider.GetWorldCenter(),
					collider.GetRadius(),
					color,
					segments
				);
			}
			continue;
		}

		OBBCollider collider;
		collider.SetWorldMatrix(&worldMatrix);
		collider.SetOffset(colliderComponent->colliderOffset);
		collider.SetHalfSize({
			(std::max)(std::abs(colliderComponent->colliderSizeMultiplier.x), 0.001f),
			(std::max)(std::abs(colliderComponent->colliderSizeMultiplier.y), 0.001f),
			(std::max)(std::abs(colliderComponent->colliderSizeMultiplier.z), 0.001f)
		});
		const OBBCollider::OBB obb = collider.GetOBB();
		if (drawSolid) {
			debugRenderer->AddSolidOBB(
				obb.center,
				obb.axis,
				obb.halfSize,
				solidColor
			);
		}
		if (drawWire) {
			debugRenderer->AddOBB(
				obb.center,
				obb.axis,
				obb.halfSize,
				color
			);
		}
	}

	if (
		ghostDocument &&
		options.isolateSelectedCollider &&
		options.selectedEntityId != 0
	) {
		const SceneEntity* ghostEntity = ghostDocument->FindEntity(
			options.selectedEntityId
		);
		const SceneComponent* colliderComponent = ghostEntity
			? FindEnabledComponent(*ghostEntity, "OBBCollider")
			: nullptr;
		if (colliderComponent) {
			const bool isCombatVolume =
				FindEnabledComponent(*ghostEntity, "HitBox") != nullptr ||
				FindEnabledComponent(*ghostEntity, "HurtBox") != nullptr;
			const bool shouldDraw = isCombatVolume
				? options.showCombatVolumes || options.showColliders
				: options.showColliders;
			if (shouldDraw) {
				const Vector4 color = { 0.18f, 0.72f, 1.0f, 0.88f };
				Vector4 solidColor = color;
				solidColor.w = 0.14f;
				const bool drawWire =
					colliderComponent->colliderDebugDrawMode != "Solid";
				const bool drawSolid =
					colliderComponent->colliderDebugDrawMode != "Wireframe";
				const uint32_t segments = static_cast<uint32_t>(std::clamp(
					colliderComponent->colliderDebugSegments,
					4,
					64
				));
				const Matrix4x4 worldMatrix =
					ResolveSceneWorldMatrix(*ghostDocument, *ghostEntity);
				if (colliderComponent->colliderShape == "Sphere") {
					SphereCollider collider;
					collider.SetWorldMatrix(&worldMatrix);
					collider.SetOffset(colliderComponent->colliderOffset);
					collider.SetRadius((std::max)(
						colliderComponent->colliderSphereRadius,
						0.001f
					));
					if (drawSolid) {
						debugRenderer->AddSolidSphere(
							collider.GetWorldCenter(), collider.GetRadius(),
							solidColor, segments
						);
					}
					if (drawWire) {
						debugRenderer->AddSphere(
							collider.GetWorldCenter(), collider.GetRadius(),
							color, segments
						);
					}
				} else {
					OBBCollider collider;
					collider.SetWorldMatrix(&worldMatrix);
					collider.SetOffset(colliderComponent->colliderOffset);
					collider.SetHalfSize({
						(std::max)(std::abs(colliderComponent->colliderSizeMultiplier.x), 0.001f),
						(std::max)(std::abs(colliderComponent->colliderSizeMultiplier.y), 0.001f),
						(std::max)(std::abs(colliderComponent->colliderSizeMultiplier.z), 0.001f)
					});
					const OBBCollider::OBB obb = collider.GetOBB();
					if (drawSolid) {
						debugRenderer->AddSolidOBB(
							obb.center, obb.axis, obb.halfSize, solidColor
						);
					}
					if (drawWire) {
						debugRenderer->AddOBB(
							obb.center, obb.axis, obb.halfSize, color
						);
					}
				}
			}
		}
	}

	debugRenderer->Draw(camera_);
	debugRenderer->ClearGeometry();
#else
	(void)document;
	(void)ghostDocument;
	(void)options;
#endif
}

void PrefabPreviewRenderer::UpdateFraming(
	const SceneDocument& document,
	const OverlayOptions& options
) {
	const float maximum = (std::numeric_limits<float>::max)();
	Vector3 boundsMin{ maximum, maximum, maximum };
	Vector3 boundsMax{ -maximum, -maximum, -maximum };
	bool hasBounds = false;
	const auto includePoint = [&](const Vector3& point) {
		boundsMin.x = (std::min)(boundsMin.x, point.x);
		boundsMin.y = (std::min)(boundsMin.y, point.y);
		boundsMin.z = (std::min)(boundsMin.z, point.z);
		boundsMax.x = (std::max)(boundsMax.x, point.x);
		boundsMax.y = (std::max)(boundsMax.y, point.y);
		boundsMax.z = (std::max)(boundsMax.z, point.z);
		hasBounds = true;
	};

	for (const SceneEntity& entity : document.GetEntities()) {
		if (!IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		const auto runtime = models_.find(entity.id);
		if (runtime == models_.end()) {
			continue;
		}
		Model* model = ModelManager::GetInstance()->FindModel(
			runtime->second.modelPath
		);
		Vector3 localMin{};
		Vector3 localMax{};
		if (!model || !model->GetLocalBounds(localMin, localMax)) {
			continue;
		}

		Matrix4x4 modelWorld = ResolveSceneWorldMatrix(document, entity);
		if (!model->HasSkinning()) {
			modelWorld = Multiply(model->GetRootNodeLocalMatrix(), modelWorld);
		}
		for (int corner = 0; corner < 8; ++corner) {
			const Vector3 localPoint{
				(corner & 1) != 0 ? localMax.x : localMin.x,
				(corner & 2) != 0 ? localMax.y : localMin.y,
				(corner & 4) != 0 ? localMax.z : localMin.z
			};
			includePoint(TransformPoint(localPoint, modelWorld));
		}
	}

	for (const SceneEntity& entity : document.GetEntities()) {
		if (!IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		const SceneComponent* colliderComponent =
			FindEnabledComponent(entity, "OBBCollider");
		if (!colliderComponent) {
			continue;
		}
		const bool isCombatVolume =
			FindEnabledComponent(entity, "HitBox") != nullptr ||
			FindEnabledComponent(entity, "HurtBox") != nullptr;
		const bool shouldInclude = isCombatVolume
			? options.showCombatVolumes || options.showColliders
			: options.showColliders;
		if (!shouldInclude) {
			continue;
		}

		const Matrix4x4 worldMatrix =
			ResolveSceneWorldMatrix(document, entity);
		if (colliderComponent->colliderShape == "Sphere") {
			SphereCollider collider;
			collider.SetWorldMatrix(&worldMatrix);
			collider.SetOffset(colliderComponent->colliderOffset);
			collider.SetRadius((std::max)(
				colliderComponent->colliderSphereRadius,
				0.001f
			));
			const Vector3 center = collider.GetWorldCenter();
			const float radius = collider.GetRadius();
			includePoint({ center.x - radius, center.y - radius, center.z - radius });
			includePoint({ center.x + radius, center.y + radius, center.z + radius });
			continue;
		}

		OBBCollider collider;
		collider.SetWorldMatrix(&worldMatrix);
		collider.SetOffset(colliderComponent->colliderOffset);
		collider.SetHalfSize({
			(std::max)(std::abs(colliderComponent->colliderSizeMultiplier.x), 0.001f),
			(std::max)(std::abs(colliderComponent->colliderSizeMultiplier.y), 0.001f),
			(std::max)(std::abs(colliderComponent->colliderSizeMultiplier.z), 0.001f)
		});
		const OBBCollider::OBB obb = collider.GetOBB();
		for (int corner = 0; corner < 8; ++corner) {
			Vector3 worldPoint = obb.center;
			worldPoint = Math::Add(worldPoint, Math::Multiply(
				obb.axis[0],
				(corner & 1) != 0 ? obb.halfSize.x : -obb.halfSize.x
			));
			worldPoint = Math::Add(worldPoint, Math::Multiply(
				obb.axis[1],
				(corner & 2) != 0 ? obb.halfSize.y : -obb.halfSize.y
			));
			worldPoint = Math::Add(worldPoint, Math::Multiply(
				obb.axis[2],
				(corner & 4) != 0 ? obb.halfSize.z : -obb.halfSize.z
			));
			includePoint(worldPoint);
		}
	}

	if (!hasBounds) {
		orbitTarget_ = {};
		fitDistance_ = 5.0f;
		return;
	}

	orbitTarget_ = {
		(boundsMin.x + boundsMax.x) * 0.5f,
		(boundsMin.y + boundsMax.y) * 0.5f,
		(boundsMin.z + boundsMax.z) * 0.5f
	};
	const Vector3 halfExtent{
		(boundsMax.x - boundsMin.x) * 0.5f,
		(boundsMax.y - boundsMin.y) * 0.5f,
		(boundsMax.z - boundsMin.z) * 0.5f
	};
	const float radius = (std::max)(
		std::sqrt(
			halfExtent.x * halfExtent.x +
			halfExtent.y * halfExtent.y +
			halfExtent.z * halfExtent.z
		),
		0.01f
	);
	fitDistance_ = radius / std::tan(camera_->GetFovY() * 0.5f) * 1.25f;
}
