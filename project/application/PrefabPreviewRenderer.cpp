// 役割: Prefab DocumentをPreview専用の既存3D/UI描画Systemへ同期し、全階層を描画する。
#include "PrefabPreviewRenderer.h"
#include <new>
#include <stdexcept>

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
#include "scene/systems/rendering/SceneObjectSystem.h"
#include "scene/systems/rendering/SceneUIRenderSystem.h"
#include "scene/systems/text/SceneTextRenderSystem.h"

#include <algorithm>
#include <cmath>
#include <cfloat>
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

PrefabPreviewRenderer::PrefabPreviewRenderer() = default;

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

	uiObjects_ = std::make_unique<SceneObjectSystem>();
	uiTexts_ = std::make_unique<SceneTextRenderSystem>();
	uiTexts_->Initialize(dxCommon_, "PrefabUILayoutPreview");
	uiRenderer_ = std::make_unique<SceneUIRenderSystem>();
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
	const OverlayOptions& overlayOptions,
    const SceneReplayFramingRequest* replayFraming, const PlayerCombatPreview::ViewRequest* attackView,
    const PrefabAnimationGhost::Frame* animationGhost
) {
	replayFramingValid_=false; attackViewResult_={};
	animationGhostResult_ = {};
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
	// 攻撃PreviewだけはEditor所有の絶対orbitを採用。通常Prefabの内部Framingは維持。
	if (attackView && attackView->enabled) {
		attackViewResult_.request=*attackView;
		auto state=attackView->state;
		bool valid=true;
		if (attackView->fit!=PlayerCombatPreview::Fit::None || !state.initialized) {
			const auto oldTarget=orbitTarget_; const float oldDistance=fitDistance_;
			UpdateFraming(document,overlayOptions);
			Vector3 center=orbitTarget_;
			float radius=fitDistance_*std::tan(.35f)/1.25f;
			orbitTarget_=oldTarget;fitDistance_=oldDistance;
			const auto& v=attackView->volume;
			if(v.valid) {
				Vector3 p{v.offset.x*v.world.m[0][0]+v.offset.y*v.world.m[1][0]+v.offset.z*v.world.m[2][0]+v.world.m[3][0],v.offset.x*v.world.m[0][1]+v.offset.y*v.world.m[1][1]+v.offset.z*v.world.m[2][1]+v.world.m[3][1],v.offset.x*v.world.m[0][2]+v.offset.y*v.world.m[1][2]+v.offset.z*v.world.m[2][2]+v.world.m[3][2]};
				float lengths[3];for(int i=0;i<3;++i)lengths[i]=Math::Length({v.world.m[i][0],v.world.m[i][1],v.world.m[i][2]});
				const float r=v.shape=="Sphere" ? v.radius*(std::max)({lengths[0],lengths[1],lengths[2]}) : Math::Length({v.halfSize.x*lengths[0],v.halfSize.y*lengths[1],v.halfSize.z*lengths[2]});
				if(attackView->fit==PlayerCombatPreview::Fit::Selected){center=p;radius=r;}
				else {
					const auto delta=Math::Subtract(p,center);const float d=Math::Length(delta);
					if(r>=d+radius){center=p;radius=r;}
					else if(d+r>radius && d>0){const float combined=(d+r+radius)*.5f;center=Math::Add(center,Math::Multiply(delta,(combined-radius)/d));radius=combined;}
				}
			} else if(attackView->fit==PlayerCombatPreview::Fit::Selected) valid=false;
			const float halfFov=(std::min)(.35f,std::atan(std::tan(.35f)*camera_->GetAspectRatio()));
			const float distance=(std::max)(radius,.01f)/std::sin(halfFov)*1.25f;
			valid &= std::isfinite(distance) && distance<=5000 && distance-radius>=.01f && distance+radius<=10000;
			if(valid){state.target=center;state.distance=distance;state.initialized=true;}
		}
		valid &= std::isfinite(state.target.x)&&std::isfinite(state.target.y)&&std::isfinite(state.target.z)&&std::isfinite(state.distance)&&std::isfinite(state.yaw)&&std::isfinite(state.pitch);
		const auto& applied=valid?state:attackView->state;
		if(applied.initialized){camera_->SetOrbitTarget(applied.target);camera_->SetOrbitAngle(applied.yaw,applied.pitch);camera_->SetOrbitDistance(applied.distance);}
		attackViewResult_.valid=valid;attackViewResult_.state=state;
	}
	camera_->UpdatePreviewMatrices();
    const Camera editorCamera=*camera_;
    if (replayFraming && replayFraming->generation==document.GetContentGeneration()) {
        try {
            // SyncModels直後の親TransformをCPU姿勢へ反映してから測る。Animation時計は進めない。
            for (auto& [id,runtime] : models_) { (void)id; runtime.object->UpdateForCamera(camera_); }
            SceneReplayFramingBasis basis;
            replayFramingValid_=TryGetReplayFraming(document,replayFraming->owner,basis) &&
                ResolveReplayCamera(replayFraming->view,basis.world,camera_->GetWorldMatrix(),basis.min,basis.max,
                    basis.min,basis.max,camera_->GetAspectRatio(),replayFramingCamera_,&replayFramingManual_);
            if (replayFramingValid_ && replayFraming->preview) *camera_=replayFramingCamera_;
        } catch (const std::bad_alloc&) { replayFramingValid_=false; }
          catch (const std::length_error&) { replayFramingValid_=false; }
    }
	viewMatrix_ = camera_->GetViewMatrix();
	projectionMatrix_ = camera_->GetProjectionMatrix();
	SyncAnimationGhostModels(!ghostDocument && !replayFraming && !attackView ? animationGhost : nullptr);
	attackViewResult_.view=viewMatrix_;attackViewResult_.projection=projectionMatrix_;
	attackViewResult_.width=renderTarget_->GetWidth();attackViewResult_.height=renderTarget_->GetHeight();

	for (auto& [entityId, runtime] : models_) {
		(void)entityId;
		runtime.object->UpdateForCamera(camera_);
	}
	for (auto& [entityId, runtime] : ghostModels_) {
		(void)entityId;
		runtime.object->UpdateForCamera(camera_);
	}

	// Overlay用Display PSOをHDRへ向けない。複製内のCanvasだけをScene2Dへ写し、
	// 元の表示先ごとに2段階で描く。元Document/保存値/Runtime描画順は変更しない。
	SceneDocument uiDocument = document;
	uint32_t legacyWidth = 1280, legacyHeight = 720;
	for (const auto& entity : document.GetEntities()) if (const auto* canvas = SceneEntityQuery::FindComponent(entity, "UICanvas")) {
		if(canvas->uiReferenceSize[0] && canvas->uiReferenceSize[1]) {
			legacyWidth = canvas->uiReferenceSize[0]; legacyHeight = canvas->uiReferenceSize[1];
		}
		break;
	}
	for (auto& entity : uiDocument.GetEntities()) {
		if (auto* text = SceneEntityQuery::FindComponent(entity, "TextRenderer"); text && text->textRenderSpace == "ScreenOverlay") {
			// Legacy OverlayもHDR Previewに限り同じbitmap/位置規則をScene2D PSOで描く。
			Text2DPlacement p = text->textHasPlacementProfiles ? text->textOverlayPlacement : Text2DPlacement{};
			if (!text->textHasPlacementProfiles) {
				const auto t = SceneTransformResolver::ResolveScene2DTransform(document, entity);
				p.position={t.translate.x,t.translate.y}; p.rotation=t.rotate.z; p.scale={t.scale.x,t.scale.y};
				p.pivot=text->textPivot; p.viewportAnchor=text->textViewportAnchor; p.sortingOrder=text->textSortingOrder;
			}
			p.position.x += p.viewportAnchor.x * legacyWidth; p.position.y += p.viewportAnchor.y * legacyHeight;
			text->textScene2DPlacement=p; text->textHasPlacementProfiles=true; text->textRenderSpace="Scene2D";
		}
	}
	uiObjects_->SyncSprites(&uiDocument);
	uiTexts_->Sync(&uiDocument);
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
	DrawAnimationGhostModels();
	DrawEditorOverlays(document, ghostDocument, overlayOptions);
	uiObjects_->DrawSprites(uiDocument, 0);
	uiTexts_->DrawScene2D(uiDocument, legacyWidth, legacyHeight);
	for (const char* space : { "Scene2D", "ScreenOverlay" }) {
		for (auto& entity : uiDocument.GetEntities()) {
			auto* canvas = SceneEntityQuery::FindComponent(entity, "UICanvas");
			const auto* original = document.FindEntity(entity.id);
			const auto* sourceCanvas = original ? SceneEntityQuery::FindComponent(*original, "UICanvas") : nullptr;
			if (canvas && sourceCanvas) { canvas->enabled=sourceCanvas->enabled && sourceCanvas->uiRenderSpace==space; canvas->uiRenderSpace="Scene2D"; }
		}
		uiRenderer_->Draw(uiDocument, *uiObjects_, *uiTexts_, "Scene2D");
	}
	renderTarget_->End();
    *camera_=editorCamera; // 確認用構図を次frameの編集orbit設定へ保存しない。
}

void PrefabPreviewRenderer::Finalize() {
	if (uiTexts_) uiTexts_->Finalize();
	uiRenderer_.reset(); uiTexts_.reset(); uiObjects_.reset();
	models_.clear();
	ghostModels_.clear();
	animationGhostModels_.clear(); animationGhostDraws_.clear();
	animationGhostPoseSerial_ = 0; animationGhostResult_ = {};
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

void PrefabPreviewRenderer::SyncAnimationGhostModels(const PrefabAnimationGhost::Frame* frame) {
    animationGhostDraws_.clear();
#if defined(_DEBUG) || defined(DEVELOPMENT)
    using namespace PrefabAnimationGhost;
    size_t meshCount = 0;
    if (frame) for (const auto& pose : frame->poses) meshCount += pose.meshes.size();
    if (!frame || !frame->poseSerial || frame->poses.size() > kPosesPerPage || meshCount > kMaxMeshes) {
        animationGhostModels_.clear(); animationGhostPoseSerial_ = 0;
        return;
    }
    // 一つのPreview Render/Frame。前FrameのPostDraw/platform Fenceが完了してから更新する。
    if (animationGhostPoseSerial_ != frame->poseSerial) {
        animationGhostModels_.clear();
        animationGhostPoseSerial_ = frame->poseSerial;
    }
    animationGhostResult_.context = frame->context;
    animationGhostResult_.poseSerial = frame->poseSerial;
    animationGhostResult_.requestSerial = frame->requestSerial;
    animationGhostResult_.view = viewMatrix_; animationGhostResult_.projection = projectionMatrix_;
    animationGhostResult_.width = renderTarget_->GetWidth(); animationGhostResult_.height = renderTarget_->GetHeight();
    const auto finite = [](const Vector3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); };
    std::map<std::pair<uint64_t, uint64_t>, bool> required;
    for (const auto& pose : frame->poses) {
        PoseResult result; result.id = pose.id; result.time = pose.time; result.origin = pose.origin;
        for (const auto& mesh : pose.meshes) {
            auto* manager = ModelManager::GetInstance();
            manager->LoadModel(mesh.modelPath);
            auto* model = manager->FindModel(mesh.modelPath);
            if (!model || model->HasSkinning()) {
                result.issue = model ? Issue::Skinning : Issue::MissingModel; result.subject = mesh.modelPath;
                continue;
            }
            Vector3 localMin{}, localMax{};
            Bounds bounds; bounds.entity = mesh.entity;
            bounds.min = { FLT_MAX, FLT_MAX, FLT_MAX }; bounds.max = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            bool valid = model->GetLocalBounds(localMin, localMax) && finite(localMin) && finite(localMax);
            const auto world = Multiply(model->GetRootNodeLocalMatrix(), mesh.world);
            for (int i = 0; valid && i < 8; ++i) {
                const auto p = TransformPoint({ i & 1 ? localMax.x : localMin.x,
                    i & 2 ? localMax.y : localMin.y, i & 4 ? localMax.z : localMin.z }, world);
                valid = finite(p);
                bounds.min = { (std::min)(bounds.min.x,p.x), (std::min)(bounds.min.y,p.y), (std::min)(bounds.min.z,p.z) };
                bounds.max = { (std::max)(bounds.max.x,p.x), (std::max)(bounds.max.y,p.y), (std::max)(bounds.max.z,p.z) };
            }
            if (!valid) { result.issue = Issue::InvalidBounds; result.subject = mesh.modelPath; continue; }
            result.bounds.push_back(bounds);
            const auto key = std::make_pair(pose.id, mesh.entity);
            required[key] = true;
            auto& runtime = animationGhostModels_[key];
            if (!runtime.object || runtime.modelPath != mesh.modelPath) {
                runtime.object = std::make_unique<Object3d>();
                runtime.object->Initialize(Object3dCommon::GetInstance());
                runtime.object->SetModel(model); runtime.modelPath = mesh.modelPath;
                runtime.object->SetEnableLighting(false); runtime.object->SetEnvironmentCoefficient(0);
            }
            auto& object = *runtime.object;
            object.SetScale({1,1,1}); object.SetRotateQuaternion(MakeIdentityQuaternion()); object.SetTranslate({0,0,0});
            object.SetParentMatrixOverride(mesh.world); object.SetCullMode(ResolveCullMode(mesh.cullMode));
            std::vector<Object3d::MaterialOverride> materials;
            for (const auto& material : mesh.materials) materials.push_back({ material.materialName, material.enabled,
                material.colorOverrideEnabled, material.color, material.texturePath });
            object.SetMaterialOverrides(materials);
            const float alpha = std::isfinite(frame->opacity) ? std::clamp(frame->opacity,.05f,.60f) : .20f;
            object.SetColor(pose.id == frame->selected ? Vector4{1,.65f,.2f,(std::min)(.80f,alpha*2)} : Vector4{.25f,.75f,1,alpha});
            object.UpdateForCamera(camera_);
            // 同時刻の実体は二重に塗らず、UIの枠と時刻だけ残す。
            if (pose.time != frame->time) {
                const Vector3 center{bounds.min.x*.5f+bounds.max.x*.5f,bounds.min.y*.5f+bounds.max.y*.5f,bounds.min.z*.5f+bounds.max.z*.5f};
                const auto eyeSpace = TransformPoint(center, viewMatrix_);
                const float distance = eyeSpace.x*eyeSpace.x + eyeSpace.y*eyeSpace.y + eyeSpace.z*eyeSpace.z;
                if (std::isfinite(distance)) animationGhostDraws_.push_back({key,distance});
            }
        }
        animationGhostResult_.poses.push_back(std::move(result));
    }
    for (auto it=animationGhostModels_.begin(); it!=animationGhostModels_.end();) {
        if (!required.contains(it->first)) it=animationGhostModels_.erase(it); else ++it;
    }
    std::stable_sort(animationGhostDraws_.begin(),animationGhostDraws_.end(),[](const auto& a,const auto& b) {
        return a.distance != b.distance ? a.distance > b.distance : a.key < b.key;
    });
#else
    (void)frame;
#endif
}

void PrefabPreviewRenderer::DrawAnimationGhostModels() {
#if defined(_DEBUG) || defined(DEVELOPMENT)
    for (const auto& draw : animationGhostDraws_) animationGhostModels_.at(draw.key).object->DrawEditorGhost();
    if (!animationGhostDraws_.empty()) Object3dCommon::GetInstance()->SetCommonRenderState();
#endif
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

	// 編集枠は実判定とは別の値。inactive/先行Windowを有効化しない。
	const auto& edit=options.editVolume;
	if(edit.valid && !edit.active){
		const Vector4 color{1,.65f,.1f,1};
		if(edit.shape=="Sphere"){
			SphereCollider collider;collider.SetWorldMatrix(&edit.world);collider.SetOffset(edit.offset);collider.SetRadius(edit.radius);
			debugRenderer->AddSphere(collider.GetWorldCenter(),collider.GetRadius(),color,24);
		}else{
			OBBCollider collider;collider.SetWorldMatrix(&edit.world);collider.SetOffset(edit.offset);collider.SetHalfSize(edit.halfSize);
			const auto obb=collider.GetOBB();debugRenderer->AddOBB(obb.center,obb.axis,obb.halfSize,color);
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

bool PrefabPreviewRenderer::TryGetReplayFraming(const SceneDocument& document, uint64_t owner, SceneReplayFramingBasis& out) const {
    const auto* entity=document.FindEntity(owner);
    if (!entity || !IsEntityActiveInHierarchy(document,*entity)) return false;
    const auto root=models_.find(owner);
    SceneReplayFramingBasis basis;
    basis.world=root!=models_.end() && root->second.object ? root->second.object->GetWorldMatrix() : ResolveSceneWorldMatrix(document,*entity);
    for (const auto& [id,runtime] : models_) {
        if (id!=owner && !document.IsDescendantOf(id,owner)) continue;
        const auto* child=document.FindEntity(id);
        if (!child || SceneEntityQuery::FindComponent(*child,"WaterVolume")) continue;
        Object3d::ReplayPose pose; Object3d::ReplayBounds bounds;
        if (!runtime.object || !runtime.object->CaptureReplayPose(runtime.modelPath,pose) ||
            !Object3d::ComputeReplayBounds(pose,bounds) || !bounds.valid) return false;
        if (!basis.valid) { basis.min=bounds.min; basis.max=bounds.max; basis.valid=true; }
        else {
            basis.min={(std::min)(basis.min.x,bounds.min.x),(std::min)(basis.min.y,bounds.min.y),(std::min)(basis.min.z,bounds.min.z)};
            basis.max={(std::max)(basis.max.x,bounds.max.x),(std::max)(basis.max.y,bounds.max.y),(std::max)(basis.max.z,bounds.max.z)};
        }
    }
    if (!basis.valid) return false;
    out=basis; return true;
}
