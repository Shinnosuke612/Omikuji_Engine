// 役割: Object3dの更新、アニメーション遷移、モデル描画を実装する。
#include <limits>
#include "Object3d.h"
#include "Object3dCommon.h"
#include "../base/DirectXCommon.h"
#include "../2d/TextureManager.h"
#include "ModelManager.h"
#include "Model.h"
#include "Camera.h"
#include "SrvManager.h"
#include "../debug/DebugRenderer.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <type_traits>
#include <utility>

namespace {

Vector4 MultiplyColor(const Vector4& left, const Vector4& right) {
	return {
		left.x * right.x,
		left.y * right.y,
		left.z * right.z,
		left.w * right.w
	};
}

bool IsSameColor(const Vector4& left, const Vector4& right) {
	return left.x == right.x && left.y == right.y &&
		left.z == right.z && left.w == right.w;
}

} // namespace

void Object3d::Initialize(Object3dCommon* object3dCommon){
	this->object3dCommon = object3dCommon;

	CreateTransformationMatrixResource();
	CreateCameraResource();
	CreateShadowTransformationMatrixResource();
	environmentTextureFilePath_ = "resources/rostock_laage_airport_4k.dds";
	TextureManager::GetInstance()->LoadTexture(environmentTextureFilePath_);

	//Transform変数を作る
	transform = { {1.0f,1.0f,1.0f},{0.0f,0.0f,0.0f},{0.0f,0.0f,0.0f} };
	this->camera = object3dCommon->GetDefaultCamera();
}

bool Object3d::BeginPreparedInitialize(Object3dCommon& common, Camera& candidateCamera,
	Model* preparedModel, const std::string& key, std::string& diagnostic) {
	auto& textures = *TextureManager::GetInstance();
	if (object3dCommon || !common.GetDxCommon() || !textures.HasTexture(key) ||
		!textures.GetSrvHandleGPU(key).ptr || !textures.GetMetaData(key).IsCubemap()) {
		diagnostic = "Object3d requires an unused instance, a device and a published environment cube.";
		return false;
	}
	if (preparedModel && preparedModel->HasSkinning()) {
		preparedStage_ = -2;
		diagnostic = "Prepared Object3d does not support skinned models.";
		return false;
	}
	try {
		const size_t count = preparedModel ? preparedModel->GetMaterialSlots().size() : 0;
		preparedResources_.resize(3 + count);
		materialSlots_.resize(count);
		materialResourcesForDraw_.resize(count);
		materialTexturesForDraw_.resize(count);
		environmentTextureFilePath_ = key;
	} catch (const std::exception&) {
		preparedStage_ = -1;
		diagnostic = "Failed to allocate prepared Object3d storage.";
		return false;
	}
	object3dCommon = &common;
	camera = &candidateCamera;
	model = preparedModel;
	transform = {{1,1,1},{0,0,0},{0,0,0}};
	preparedStage_ = 1;
	diagnostic.clear();
	return true;
}

RuntimePrepareStep Object3d::StepPreparedInitialize(SceneRuntimePrepareBudget& remaining, std::string& diagnostic) {
	using Step = RuntimePrepareStep;
	diagnostic.clear();
	if (preparedStage_ == 6) return Step::Ready;
	if (preparedStage_ <= 0) {
		diagnostic = "Object3d preparation was not started or failed.";
		return preparedStage_ == -2 ? Step::Unsupported : Step::Failed;
	}
	if (!remaining.maxCopyBytes) return Step::Pending;
	// preparedResources_ owns buffers; existing draw fields are borrowed aliases.
	const auto prepare = [&](size_t slot, auto*& data, size_t bytes) {
		auto& resource = preparedResources_[slot];
		if (!resource) {
			if (!remaining.maxResourceCreates) return false;
			--remaining.maxResourceCreates;
			void* mapped = nullptr;
			D3D12_RANGE readRange{0, 0};
			if (!object3dCommon->GetDxCommon()->TryCreateBufferResource(bytes, resource) || !resource ||
				FAILED(resource->Map(0, &readRange, &mapped)) || !mapped) {
				preparedStage_ = -1;
				diagnostic = "Failed to create or map a prepared Object3d buffer.";
				return false;
			}
			data = static_cast<std::remove_reference_t<decltype(*data)>*>(mapped);
		}
		if (remaining.maxCopyBytes < bytes) return false;
		remaining.maxCopyBytes -= bytes;
		return true;
	};
	const auto pendingOrFailed = [&] { return preparedStage_ < 0 ? Step::Failed : Step::Pending; };
	switch (preparedStage_) {
	case 1:
		if (!prepare(0, transformationMatrixData, sizeof(TransformationMatrix))) return pendingOrFailed();
		transformationMatrixResource = preparedResources_[0].Get();
		*transformationMatrixData = {MakeIdentity4x4(), MakeIdentity4x4()};
		break;
	case 2:
		if (!prepare(1, cameraData, sizeof(CameraForGPU))) return pendingOrFailed();
		cameraResource = preparedResources_[1].Get();
		*cameraData = {{0,0,-10}, 0};
		break;
	case 3:
		if (!prepare(2, shadowTransformationMatrixData, sizeof(ShadowTransformationMatrix))) return pendingOrFailed();
		shadowTransformationMatrixResource = preparedResources_[2].Get();
		*shadowTransformationMatrixData = {MakeIdentity4x4()};
		break;
	case 4:
		if (preparedMaterialIndex_ < materialSlots_.size()) {
			const auto i = preparedMaterialIndex_;
			auto& material = materialSlots_[i];
			if (!prepare(3 + i, material.data, sizeof(Material))) return pendingOrFailed();
			material.resource = preparedResources_[3 + i].Get();
			material.modelColor = model->GetMaterialSlots()[i].baseColor;
			UpdateMaterialResource(i);
			if (++preparedMaterialIndex_ < materialSlots_.size()) return Step::Progress;
		}
		break;
	case 5:
		try {
			animationPlayer_.SetAnimations(model ? &model->GetAnimations() : nullptr);
			skeleton_ = model ? CreateSkeleton(model->GetRootNode()) : Skeleton{};
		} catch (const std::exception&) {
			preparedStage_ = -1;
			diagnostic = "Failed to initialize the prepared Object3d animation state.";
			return Step::Failed;
		}
		animationPoseDirty_ = true;
		skinningDispatchPending_ = true;
		break;
	default: return Step::Failed;
	}
	++preparedStage_;
	return preparedStage_ == 6 ? Step::Ready : Step::Progress;
}

void Object3d::Update(){
	UpdateInternal();
}

void Object3d::UpdateAnimation(float deltaTime) {
	if (replayPoseActive_) return;
	if (
		deltaTime > 0.0f &&
		animationPlayer_.IsEnabled() &&
		animationPlayer_.IsPlaying()
	) {
		animationPoseDirty_ = true;
	}
	animationPlayer_.Update(deltaTime);
}

void Object3d::UpdateForCamera(Camera* camera) {
	this->camera = camera;
	UpdateInternal();
}

void Object3d::UpdateInternal() {
	if (replayPoseActive_) {
		// ReplayのWorldは既にroot/親/表示補正込み。別ViewでもPoseを再評価しない。
		transformationMatrixData->WVP = camera ? Multiply(transformationMatrixData->World, camera->GetViewProjectionMatrix()) : transformationMatrixData->World;
		if (camera) cameraData->worldPosition = camera->GetTranslate();
		return;
	}
	objectWorldMatrix_ = transform.useQuaternionRotation
		? MakeAffineMatrix(
			transform.scale,
			transform.quaternionRotate,
			transform.translate
		)
		: MakeAffineMatrix(
			transform.scale,
			transform.rotate,
			transform.translate
		);
	if (hasParentMatrixOverride_) {
		objectWorldMatrix_ = Multiply(
			objectWorldMatrix_,
			parentMatrixOverride_
		);
	} else if (parent_) {
		objectWorldMatrix_ = Multiply(
			objectWorldMatrix_,
			parent_->objectWorldMatrix_
		);
	}
	const Matrix4x4& worldMatrix = objectWorldMatrix_;

	// ModelのRootNode行列を適用する
	Matrix4x4 modelWorldMatrix = worldMatrix;
	if (model) {
		Matrix4x4 localMatrix = model->HasSkinning()
			? MakeIdentity4x4()
			: model->GetRootNodeLocalMatrix();
		if (animationPoseDirty_) {
			UpdateAnimationPose();
		}

		if (skeleton_.IsValid()) {
			if (!model->HasSkinning()) {
				localMatrix =
					skeleton_.joints[skeleton_.root].skeletonSpaceMatrix;
			}
		}
		modelWorldMatrix = Multiply(localMatrix, worldMatrix);
	}

	// 0倍率も有効。逆行列を作らず、モデルと影の頂点だけを出現原点へ縮退させる。
	modelWorldMatrix = Multiply(modelWorldMatrix, renderWorldDelta_);
	Matrix4x4 worldViewProjectionMatrix;
	if (camera) {
		const Matrix4x4& viewProjectionMatrix = camera->GetViewProjectionMatrix();
		worldViewProjectionMatrix = Multiply(modelWorldMatrix, viewProjectionMatrix);
	}
	else {
		worldViewProjectionMatrix = modelWorldMatrix;
	}

	transformationMatrixData->WVP = worldViewProjectionMatrix;
	transformationMatrixData->World = modelWorldMatrix;

	if (camera) {
		cameraData->worldPosition = camera->GetTranslate();
	}
}

void Object3d::SetModel(Model* model) {
	replayPoseActive_ = false;
	this->model = model;
	CreateMaterialResources();
	animationPlayer_.SetAnimations(
		model ? &model->GetAnimations() : nullptr
	);
	skeleton_ = model
		? CreateSkeleton(model->GetRootNode())
		: Skeleton{};
	skinCluster_.reset();
	if (
		model &&
		model->HasSkinning() &&
		skeleton_.IsValid()
	) {
		skinCluster_ = std::make_unique<SkinCluster>();
		skinCluster_->Initialize(
			object3dCommon->GetDxCommon(),
			SrvManager::GetInstance(),
			skeleton_,
			*model
		);
	}
	animationPoseDirty_ = true;
	skinningDispatchPending_ = true;
}

void Object3d::UpdateAnimationPose() {
	if (!model || !skeleton_.IsValid()) {
		animationPoseDirty_ = false;
		return;
	}

	const Animation* animation = animationPlayer_.GetCurrentAnimation();
	if (animationPlayer_.IsEnabled() && animation) {
		if (const Animation* previous =
			animationPlayer_.GetPreviousAnimation()) {
			ApplyAnimationBlend(
				skeleton_,
				*previous,
				animationPlayer_.GetPreviousTime(),
				*animation,
				animationPlayer_.GetTime(),
				animationPlayer_.GetBlendWeight()
			);
		} else {
			ApplyAnimation(
				skeleton_,
				*animation,
				animationPlayer_.GetTime()
			);
		}
	} else {
		ResetSkeletonPose(skeleton_);
	}

	UpdateSkeleton(skeleton_);
	if (skinCluster_) {
		skinCluster_->Update(skeleton_);
		skinningDispatchPending_ = true;
	}
	animationPoseDirty_ = false;
}

void Object3d::DispatchSkinningIfNeeded() {
	if (
		!skinningDispatchPending_ ||
		!skinCluster_ ||
		!skinCluster_->IsValid()
	) {
		return;
	}
	object3dCommon->DispatchSkinning(*skinCluster_);
	skinningDispatchPending_ = false;
}

void Object3d::SetRotateQuaternion(const Quaternion& rotate) {
	transform.quaternionRotate = Normalize(rotate);
	transform.useQuaternionRotation = true;
}

void Object3d::Draw(){
	DrawInternal(false, 1.0f);
}

void Object3d::DrawWithCoverage(float coverage) {
	DrawInternal(false, std::clamp(coverage, 0.0f, 1.0f));
}

void Object3d::DrawEditorGhost() {
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (!model || model->HasSkinning()) return;
	DrawInternal(true, 1.0f);
#endif
}

void Object3d::DrawInternal(bool editorGhost, float coverage) {
	if (!model) {
		return;
	}

	auto* commandList = object3dCommon->GetDxCommon()->GetCommandList();
	if (editorGhost) {
		object3dCommon->SetEditorGhostRenderState(cullMode_);
	} else if (skinCluster_ && skinCluster_->IsValid()) {
		DispatchSkinningIfNeeded();
		object3dCommon->SetCommonRenderState(cullMode_);
	}
	else {
		object3dCommon->SetCommonRenderState(cullMode_);
	}

	// 署名の設定後にコマンドへ値を記録し、監視ビューと材質CBを共有したままにする。
	if (coverage < 1.0f) {
		commandList->SetGraphicsRoot32BitConstant(9, std::bit_cast<uint32_t>(coverage), 0);
	}

	// 座標変換行列CBufferの場所を設定
	commandList->SetGraphicsRootConstantBufferView(1, transformationMatrixResource->GetGPUVirtualAddress());

	commandList->SetGraphicsRootConstantBufferView(4, cameraResource->GetGPUVirtualAddress());

	commandList->SetGraphicsRootDescriptorTable(
		7,
		TextureManager::GetInstance()->GetSrvHandleGPU(environmentTextureFilePath_)
	);

	if (skinCluster_ && skinCluster_->IsValid()) {
		model->DrawWithVertexBufferAndMaterialSlots(
			skinCluster_->GetSkinnedVertexBufferView(),
			materialResourcesForDraw_,
			materialTexturesForDraw_
		);
	}
	else {
		model->DrawWithMaterialSlots(
			materialResourcesForDraw_,
			materialTexturesForDraw_
		);
	}
}

void Object3d::DrawShadow(const Matrix4x4& lightViewProjection) {
	if (!model) {
		return;
	}
	if (materialDissolveAmount_ >= 0.98f) {
		return;
	}

	shadowTransformationMatrixData->WVP = Multiply(transformationMatrixData->World, lightViewProjection);

	auto* commandList = object3dCommon->GetDxCommon()->GetCommandList();
	if (skinCluster_ && skinCluster_->IsValid()) {
		DispatchSkinningIfNeeded();
	}
	// スキニング済み頂点Bufferを渡すため、ShadowMap.VS と同じ入力レイアウトを使う。
	// SkinningShadowMap.VS は未スキニング頂点/Influence/Paletteを前提にしており、
	// ここで選ぶと未バインドの入力を参照して巨大な三角形をShadowMapへ書き込む。
	object3dCommon->SetShadowRenderState(cullMode_);
	commandList->SetGraphicsRootConstantBufferView(
		0,
		shadowTransformationMatrixResource->GetGPUVirtualAddress()
	);

	if (skinCluster_ && skinCluster_->IsValid()) {
		model->DrawForShadowWithVertexBuffer(
			skinCluster_->GetSkinnedVertexBufferView()
		);
	}
	else {
		model->DrawForShadow();
	}
}

void Object3d::DrawSkeletonDebug(
	bool drawJointNames,
	bool drawJointAxes,
	float jointRadius,
	float axisLength
) const {
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (!skeleton_.IsValid() || !camera || !transformationMatrixData) {
		return;
	}

	DebugRenderer* debugRenderer = DebugRenderer::GetInstance();
	const Vector4 lineColor = { 1.0f, 0.65f, 0.12f, 1.0f };
	const Vector4 jointColor = { 0.2f, 0.85f, 1.0f, 1.0f };
	const Vector4 rootColor = { 1.0f, 0.95f, 0.35f, 1.0f };
	const Matrix4x4 objectWorldMatrix = Multiply(objectWorldMatrix_, renderWorldDelta_);

	std::vector<Matrix4x4> jointWorldMatrices(skeleton_.joints.size());
	for (const Joint& joint : skeleton_.joints) {
		jointWorldMatrices[joint.index] = Multiply(
			joint.skeletonSpaceMatrix,
			objectWorldMatrix
		);
	}

	for (const Joint& joint : skeleton_.joints) {
		const Matrix4x4& jointWorldMatrix =
			jointWorldMatrices[joint.index];
		const Vector3 jointPosition = {
			jointWorldMatrix.m[3][0],
			jointWorldMatrix.m[3][1],
			jointWorldMatrix.m[3][2]
		};

		debugRenderer->AddSphere(
			jointPosition,
			joint.index == skeleton_.root
				? jointRadius * 1.35f
				: jointRadius,
			joint.index == skeleton_.root ? rootColor : jointColor
		);
		if (drawJointNames) {
			// Screen座標への投影はScene画像Rectを所有するImGuiManagerへ委譲する。
			debugRenderer->AddWorldLabel(
				jointPosition,
				joint.name,
				{ 1.0f, 1.0f, 1.0f, 1.0f }
			);
		}

		if (drawJointAxes) {
			debugRenderer->AddAxis(jointWorldMatrix, axisLength);
		}

		if (joint.parent.has_value()) {
			const Matrix4x4& parentWorldMatrix =
				jointWorldMatrices[*joint.parent];
			const Vector3 parentPosition = {
				parentWorldMatrix.m[3][0],
				parentWorldMatrix.m[3][1],
				parentWorldMatrix.m[3][2]
			};
			debugRenderer->AddLine(
				parentPosition,
				jointPosition,
				lineColor
			);
		}
	}

#else
	(void)drawJointNames;
	(void)drawJointAxes;
	(void)jointRadius;
	(void)axisLength;
#endif
}

void Object3d::SetModel(const std::string& filePath){
//モデルを検索してセットする
	SetModel(ModelManager::GetInstance()->FindModel(filePath));
}

bool Object3d::HasAnimation() const {
	return animationPlayer_.HasAnimations();
}

float Object3d::GetAnimationDuration() const {
	return animationPlayer_.GetDuration();
}

size_t Object3d::GetAnimationClipCount() const {
	return model ? model->GetAnimations().size() : 0;
}

const std::string& Object3d::GetAnimationClipName(size_t clipIndex) const {
	static const std::string emptyName;
	if (!model || clipIndex >= model->GetAnimations().size()) {
		return emptyName;
	}
	return model->GetAnimations()[clipIndex].name;
}

void Object3d::SetEnvironmentMap(const std::string& textureFilePath, float coefficient) {
	environmentTextureFilePath_ = textureFilePath;
	if (!environmentTextureFilePath_.empty()) {
		TextureManager::GetInstance()->LoadTexture(environmentTextureFilePath_);
	}
	SetEnvironmentCoefficient(coefficient);
}

bool Object3d::TrySetPreparedEnvironmentMap(const std::string& key, float coefficient) {
	auto& textures = *TextureManager::GetInstance();
	if (preparedStage_ != 6 || !std::isfinite(coefficient) || !textures.HasTexture(key) ||
		!textures.GetSrvHandleGPU(key).ptr || !textures.GetMetaData(key).IsCubemap()) return false;
	try {
		environmentTextureFilePath_ = key;
	} catch (const std::exception&) {
		return false;
	}
	SetEnvironmentCoefficient(coefficient);
	return true;
}

bool Object3d::TrySetPreparedMaterialOverrides(const std::vector<MaterialOverride>& overrides) {
	if (preparedStage_ != 6) return false;
	auto& textures = *TextureManager::GetInstance();
	try {
		// 元slotを維持し、実際に選ぶ最初の有効な上書きだけをすべて検証する。
		auto candidate = materialSlots_;
		for (size_t i = 0; i < candidate.size(); ++i) {
			auto& slot = candidate[i];
			slot.colorOverrideEnabled = false;
			slot.textureOverride = {};
			slot.textureOverrideKey.clear();
			const auto found = std::find_if(overrides.begin(), overrides.end(), [&](const auto& item) {
				return item.enabled && !item.materialName.empty() &&
					item.materialName == model->GetMaterialSlots()[i].name;
			});
			if (found == overrides.end()) continue;
			slot.colorOverrideEnabled = found->colorOverrideEnabled;
			slot.colorOverride = found->color;
			if (!found->texturePath.empty()) {
				if (!textures.HasTexture(found->texturePath)) return false;
				slot.textureOverride = textures.GetSrvHandleGPU(found->texturePath);
				if (!slot.textureOverride.ptr) return false;
				slot.textureOverrideKey = found->texturePath;
			}
		}
		materialSlots_.swap(candidate);
	} catch (const std::exception&) {
		return false;
	}
	UpdateMaterialResources();
	return true;
}

void Object3d::SetEnvironmentCoefficient(float coefficient) {
	cameraData->environmentCoefficient = std::clamp(coefficient, 0.0f, 1.0f);
}

void Object3d::SetColor(const Vector4& color) {
	if (IsSameColor(materialColorMultiplier_, color)) {
		return;
	}
	materialColorMultiplier_ = color;
	UpdateMaterialResources();
}

bool Object3d::TryGetJointWorldMatrix(
	const std::string& jointName,
	Matrix4x4& worldMatrix
) const {
	if (!skeleton_.IsValid()) {
		return false;
	}
	const auto found = skeleton_.jointMap.find(jointName);
	if (
		found == skeleton_.jointMap.end() ||
		found->second < 0 ||
		found->second >= static_cast<int32_t>(skeleton_.joints.size())
	) {
		return false;
	}
	worldMatrix = Multiply(
		skeleton_.joints[found->second].skeletonSpaceMatrix,
		objectWorldMatrix_
	);
	return true;
}

void Object3d::SetEnableLighting(bool enableLighting) {
	if (materialEnableLighting_ == enableLighting) {
		return;
	}
	materialEnableLighting_ = enableLighting;
	UpdateMaterialResources();
}

void Object3d::SetEmissive(float intensity, const Vector4& color) {
	const float clampedIntensity = (std::max)(0.0f, intensity);
	if (materialEmissiveIntensity_ == clampedIntensity &&
		IsSameColor(materialEmissiveColor_, color)) {
		return;
	}
	materialEmissiveIntensity_ = clampedIntensity;
	materialEmissiveColor_ = color;
	UpdateMaterialResources();
}

void Object3d::SetDissolve(
	float amount,
	float edgeWidth,
	float noiseScale
) {
	const float clampedAmount = std::clamp(amount, 0.0f, 1.0f);
	const float clampedEdgeWidth = (std::max)(edgeWidth, 0.0f);
	const float clampedNoiseScale = (std::max)(noiseScale, 0.001f);
	if (materialDissolveAmount_ == clampedAmount &&
		materialDissolveEdgeWidth_ == clampedEdgeWidth &&
		materialDissolveNoiseScale_ == clampedNoiseScale) {
		return;
	}
	materialDissolveAmount_ = clampedAmount;
	materialDissolveEdgeWidth_ = clampedEdgeWidth;
	materialDissolveNoiseScale_ = clampedNoiseScale;
	UpdateMaterialResources();
}

void Object3d::CreateTransformationMatrixResource(){
	//WVP用リソースのリソースを作る。Matrix4x4 1つ分のサイズを用意する
	transformationMatrixResource = *&object3dCommon->GetDxCommon()->CreateBufferResource(sizeof(TransformationMatrix));
	//書き込むためのアドレス取得
	transformationMatrixResource->Map(0, nullptr, reinterpret_cast<void**>(&transformationMatrixData));
	//単位行列を書き込んでおく
	transformationMatrixData->WVP = MakeIdentity4x4();
	transformationMatrixData->World = MakeIdentity4x4();
}

void Object3d::CreateShadowTransformationMatrixResource() {
	shadowTransformationMatrixResource = *&object3dCommon->GetDxCommon()->CreateBufferResource(sizeof(ShadowTransformationMatrix));
	shadowTransformationMatrixResource->Map(0, nullptr, reinterpret_cast<void**>(&shadowTransformationMatrixData));
	shadowTransformationMatrixData->WVP = MakeIdentity4x4();
}

void Object3d::CreateCameraResource() {
	cameraResource = *&object3dCommon->GetDxCommon()->CreateBufferResource(sizeof(CameraForGPU));
	cameraResource->Map(0, nullptr, reinterpret_cast<void**>(&cameraData));
	cameraData->worldPosition = { 0.0f, 0.0f, -10.0f };
	cameraData->environmentCoefficient = 0.0f;
}

void Object3d::SetMaterialOverrides(
	const std::vector<MaterialOverride>& overrides
) {
	for (MaterialSlotResource& materialSlot : materialSlots_) {
		materialSlot.colorOverrideEnabled = false;
		materialSlot.textureOverride = {};
		materialSlot.textureOverrideKey.clear();
		for (const MaterialOverride& override : overrides) {
			if (!override.enabled || override.materialName.empty()) {
				continue;
			}
			const size_t materialIndex = &materialSlot - materialSlots_.data();
			if (!model || materialIndex >= model->GetMaterialSlots().size() ||
				model->GetMaterialSlots()[materialIndex].name != override.materialName) {
				continue;
			}
			materialSlot.colorOverrideEnabled = override.colorOverrideEnabled;
			materialSlot.colorOverride = override.color;
			if (!override.texturePath.empty() &&
				TextureManager::GetInstance()->LoadTexture(override.texturePath)) {
				materialSlot.textureOverride =
					TextureManager::GetInstance()->GetSrvHandleGPU(override.texturePath);
				materialSlot.textureOverrideKey = override.texturePath;
			}
			break;
		}
	}
	UpdateMaterialResources();
}

void Object3d::SetTextureOverride(D3D12_GPU_DESCRIPTOR_HANDLE handle) {
	if (textureOverrideHandle_.ptr == handle.ptr) {
		return;
	}
	textureOverrideHandle_ = handle;
	UpdateMaterialResources();
}

void Object3d::ClearTextureOverride() {
	if (textureOverrideHandle_.ptr == 0) {
		return;
	}
	textureOverrideHandle_ = {};
	UpdateMaterialResources();
}

void Object3d::CreateMaterialResources() {
	materialSlots_.clear();
	materialResourcesForDraw_.clear();
	materialTexturesForDraw_.clear();
	if (!model) {
		return;
	}

	const std::vector<Model::MaterialSlot>& modelMaterials =
		model->GetMaterialSlots();
	materialSlots_.resize(modelMaterials.size());
	for (size_t index = 0; index < modelMaterials.size(); ++index) {
		MaterialSlotResource& materialSlot = materialSlots_[index];
		materialSlot.resource = *&object3dCommon->GetDxCommon()
			->CreateBufferResource(sizeof(Material));
		materialSlot.resource->Map(
			0, nullptr, reinterpret_cast<void**>(&materialSlot.data)
		);
		materialSlot.modelColor = modelMaterials[index].baseColor;
	}
	UpdateMaterialResources();
}

void Object3d::UpdateMaterialResources() {
	materialResourcesForDraw_.resize(materialSlots_.size());
	materialTexturesForDraw_.resize(materialSlots_.size());
	for (size_t index = 0; index < materialSlots_.size(); ++index) {
		UpdateMaterialResource(index);
	}
}

void Object3d::UpdateMaterialResource(size_t index) {
		MaterialSlotResource& materialSlot = materialSlots_[index];
		if (!materialSlot.data) {
			return;
		}
		const Vector4 baseColor = materialSlot.colorOverrideEnabled
			? materialSlot.colorOverride
			: materialSlot.modelColor;
		materialSlot.data->color = MultiplyColor(
			baseColor,
			materialColorMultiplier_
		);
		materialSlot.data->enableLighting = materialEnableLighting_ ? 1 : 0;
		materialSlot.data->emissiveIntensity = materialEmissiveIntensity_;
		materialSlot.data->uvTransform = MakeIdentity4x4();
		materialSlot.data->emissiveColor = materialEmissiveColor_;
		materialSlot.data->shininess = 40.0f;
		materialSlot.data->dissolveAmount = materialDissolveAmount_;
		materialSlot.data->dissolveEdgeWidth = materialDissolveEdgeWidth_;
		materialSlot.data->dissolveNoiseScale = materialDissolveNoiseScale_;
		materialResourcesForDraw_[index] = materialSlot.resource;
		materialTexturesForDraw_[index] = textureOverrideHandle_.ptr != 0
			? textureOverrideHandle_
			: materialSlot.textureOverride;
}

namespace {
bool ReplayFinite(const Matrix4x4& value) {
	for (const auto& row : value.m) for (float v : row) if (!std::isfinite(v)) return false;
	return true;
}
bool ReplayFinite(const Vector4& v) {
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
}
bool ReplayFinite(const Vector3& v) {
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
// 入力を補正して異なる姿勢を作らず、非有限値/退化Quaternionは入口で拒否する。
bool ReplayValuesValid(const Object3d::ReplayPose& pose) {
	if (!ReplayFinite(pose.renderWorld) || !std::isfinite(pose.environmentCoefficient) ||
		pose.cullMode < Object3dCommon::CullMode::kNone || pose.cullMode >= Object3dCommon::CullMode::kCount) return false;
	for (const auto& j : pose.joints) {
		const auto& q = j.transform.rotate;
		const double length = double(q.x)*q.x + double(q.y)*q.y + double(q.z)*q.z + double(q.w)*q.w;
		if (!ReplayFinite(j.transform.scale) || !ReplayFinite(j.transform.translate) ||
			!std::isfinite(length) || length <= 0.0) return false;
	}
	for (const auto& m : pose.materials) {
		if (!ReplayFinite(m.color) || !ReplayFinite(m.emissiveColor) || !ReplayFinite(m.uvTransform) ||
			!std::isfinite(m.emissiveIntensity) || !std::isfinite(m.shininess) || !std::isfinite(m.dissolveAmount) ||
			!std::isfinite(m.dissolveEdgeWidth) || !std::isfinite(m.dissolveNoiseScale) ||
			m.dissolveAmount != pose.materials.front().dissolveAmount) return false;
	}
	return true;
}
}

bool Object3d::EstimateReplayPoseBytes(const std::string& assetKey, uint64_t& out) const {
	if (!model || assetKey.empty() || ModelManager::GetInstance()->FindModel(assetKey) != model ||
		materialSlots_.size() != model->GetMaterialSlots().size()) return false;
	uint64_t bytes = sizeof(ReplayPose) + 128;
	const auto limit = (std::numeric_limits<uint64_t>::max)();
	auto add = [&](uint64_t count, uint64_t size) {
		if (count > (limit - bytes) / size) return false;
		bytes += count * size; return true;
	};
	// reserve/string容量、同時に存在する一個の採取candidateも含め余地を持たせる。
	if (!add(assetKey.size() + 32, 4) || !add(environmentTextureFilePath_.size() + 32, 4) ||
		!add(skeleton_.joints.size(), sizeof(ReplayJoint) * 4) || !add(materialSlots_.size(), sizeof(ReplayMaterial) * 4)) return false;
	for (const auto& j : skeleton_.joints) if (!add(j.name.size() + 32, 4)) return false;
	for (size_t i = 0; i < materialSlots_.size(); ++i) {
		const auto& key = materialSlots_[i].textureOverrideKey.empty() ? model->GetMaterialSlots()[i].textureFilePath : materialSlots_[i].textureOverrideKey;
		if (!add(key.size() + 32, 4)) return false;
	}
	out = bytes; return true;
}

bool Object3d::CaptureReplayPose(const std::string& assetKey, ReplayPose& out) const {
	// CaptureはCPU値のコピーのみ。未評価のAnimationをここで進めない。
	if (!model || assetKey.empty() || ModelManager::GetInstance()->FindModel(assetKey) != model ||
		animationPoseDirty_ || !transformationMatrixData || !cameraData || textureOverrideHandle_.ptr != 0 ||
		materialSlots_.size() != model->GetMaterialSlots().size()) return false;
	auto* textures = TextureManager::GetInstance();
	if (!textures->HasTexture(environmentTextureFilePath_)) return false;
	ReplayPose candidate;
	candidate.joints.reserve(skeleton_.joints.size());
	candidate.materials.reserve(materialSlots_.size());
	candidate.modelKey = assetKey;
	candidate.environmentKey = environmentTextureFilePath_;
	candidate.renderWorld = transformationMatrixData->World;
	candidate.environmentCoefficient = cameraData->environmentCoefficient;
	candidate.cullMode = cullMode_;
	for (const auto& joint : skeleton_.joints)
		candidate.joints.push_back({joint.name, joint.parent.value_or(-1), joint.transform});
	for (size_t i = 0; i < materialSlots_.size(); ++i) {
		const auto& slot = materialSlots_[i];
		if (!slot.data || (slot.textureOverride.ptr != 0 && slot.textureOverrideKey.empty())) return false;
		ReplayMaterial value;
		value.color = slot.data->color;
		value.enableLighting = slot.data->enableLighting;
		value.emissiveIntensity = slot.data->emissiveIntensity;
		value.uvTransform = slot.data->uvTransform;
		value.emissiveColor = slot.data->emissiveColor;
		value.shininess = slot.data->shininess;
		value.dissolveAmount = slot.data->dissolveAmount;
		value.dissolveEdgeWidth = slot.data->dissolveEdgeWidth;
		value.dissolveNoiseScale = slot.data->dissolveNoiseScale;
		value.textureKey = slot.textureOverrideKey.empty() ? model->GetMaterialSlots()[i].textureFilePath : slot.textureOverrideKey;
		if (!value.textureKey.empty() && !textures->HasTexture(value.textureKey)) return false;
		candidate.materials.push_back(std::move(value));
	}
	if (!ReplayValuesValid(candidate)) return false;
	out = std::move(candidate);
	return true;
}

bool Object3d::ApplyReplayPose(const ReplayPose& pose) {
	if (!model || pose.modelKey.empty() || ModelManager::GetInstance()->FindModel(pose.modelKey) != model ||
		!transformationMatrixData || !cameraData || !shadowTransformationMatrixData ||
		pose.joints.size() != skeleton_.joints.size() || pose.materials.size() != materialSlots_.size() ||
		materialTexturesForDraw_.size() != materialSlots_.size() ||
		!ReplayValuesValid(pose) || (model->HasSkinning() && (!skinCluster_ || !skinCluster_->IsValid()))) return false;
	auto* textures = TextureManager::GetInstance();
	if (!textures->HasTexture(pose.environmentKey)) return false;
	for (size_t i = 0; i < pose.materials.size(); ++i) {
		if (!materialSlots_[i].data || (!pose.materials[i].textureKey.empty() && !textures->HasTexture(pose.materials[i].textureKey))) return false;
	}
	// 骨構成と階層計算まで一時コピーで検証し、拒否時は表示先も変更しない。
	Skeleton candidate = skeleton_;
	for (size_t i = 0; i < pose.joints.size(); ++i) {
		const auto& value = pose.joints[i];
		if (value.name != candidate.joints[i].name || value.parent != candidate.joints[i].parent.value_or(-1)) return false;
		candidate.joints[i].transform = value.transform;
	}
	if (candidate.IsValid()) UpdateSkeleton(candidate);
	for (const auto& joint : candidate.joints) if (!ReplayFinite(joint.skeletonSpaceMatrix)) return false;

	skeleton_ = std::move(candidate);
	if (skinCluster_) { skinCluster_->Update(skeleton_); skinningDispatchPending_ = true; }
	animationPoseDirty_ = false;
	replayPoseActive_ = true;
	transformationMatrixData->World = pose.renderWorld;
	cullMode_ = pose.cullMode;
	environmentTextureFilePath_ = pose.environmentKey;
	cameraData->environmentCoefficient = pose.environmentCoefficient;
	textureOverrideHandle_ = {};
	for (size_t i = 0; i < pose.materials.size(); ++i) {
		const auto& value = pose.materials[i];
		auto& slot = materialSlots_[i];
		slot.data->color = value.color;
		slot.data->enableLighting = value.enableLighting;
		slot.data->emissiveIntensity = value.emissiveIntensity;
		slot.data->uvTransform = value.uvTransform;
		slot.data->emissiveColor = value.emissiveColor;
		slot.data->shininess = value.shininess;
		slot.data->dissolveAmount = value.dissolveAmount;
		slot.data->dissolveEdgeWidth = value.dissolveEdgeWidth;
		slot.data->dissolveNoiseScale = value.dissolveNoiseScale;
		slot.textureOverrideKey = value.textureKey;
		slot.textureOverride = value.textureKey.empty() ? D3D12_GPU_DESCRIPTOR_HANDLE{} : textures->GetSrvHandleGPU(value.textureKey);
		materialTexturesForDraw_[i] = slot.textureOverride;
	}
	// 影の非表示判定も、通常描画と同じ確定材質を使う。
	materialDissolveAmount_ = pose.materials.empty() ? 0.0f : pose.materials.front().dissolveAmount;
	UpdateInternal();
	return true;
}


bool Object3d::TryInitializeReplay(Object3dCommon* common, const ReplayPose& pose) {
    if (object3dCommon || !common || !common->GetDxCommon()) return false;
    Model* cached = ModelManager::GetInstance()->FindModel(pose.modelKey);
    if (!cached || cached->GetMaterialSlots().size() != pose.materials.size()) return false;
    object3dCommon = common;
    model = cached;
    camera = common->GetDefaultCamera();
    auto buffer = [&](size_t bytes, ID3D12Resource*& alias, void** mapped) {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        if (!common->GetDxCommon()->TryCreateBufferResource(bytes, resource) ||
            FAILED(resource->Map(0, nullptr, mapped)) || !*mapped) return false;
        replayResources_.push_back(resource);
        alias = resource.Get();
        return true;
    };
    if (!buffer(sizeof(TransformationMatrix), transformationMatrixResource,
            reinterpret_cast<void**>(&transformationMatrixData)) ||
        !buffer(sizeof(CameraForGPU), cameraResource, reinterpret_cast<void**>(&cameraData)) ||
        !buffer(sizeof(ShadowTransformationMatrix), shadowTransformationMatrixResource,
            reinterpret_cast<void**>(&shadowTransformationMatrixData))) return false;
    *transformationMatrixData = { MakeIdentity4x4(), MakeIdentity4x4() };
    *cameraData = {};
    *shadowTransformationMatrixData = { MakeIdentity4x4() };
    materialSlots_.resize(pose.materials.size());
    materialResourcesForDraw_.resize(pose.materials.size());
    materialTexturesForDraw_.resize(pose.materials.size());
    for (size_t i = 0; i < materialSlots_.size(); ++i) {
        auto& slot = materialSlots_[i];
        if (!buffer(sizeof(Material), slot.resource, reinterpret_cast<void**>(&slot.data))) return false;
        *slot.data = {};
        materialResourcesForDraw_[i] = slot.resource;
    }
    skeleton_ = CreateSkeleton(model->GetRootNode());
    if (model->HasSkinning()) {
        skinCluster_ = std::make_unique<SkinCluster>();
        if (!skinCluster_->TryInitializeReplay(common->GetDxCommon(), SrvManager::GetInstance(), skeleton_, *model)) return false;
    }
    return ApplyReplayPose(pose);
}

bool Object3d::ComputeReplayBounds(const ReplayPose& pose, ReplayBounds& out) {
    auto* source = ModelManager::GetInstance()->FindModel(pose.modelKey);
    if (!source || !ReplayValuesValid(pose)) return false;
    Skeleton skeleton = CreateSkeleton(source->GetRootNode());
    if (skeleton.joints.size() != pose.joints.size()) return false;
    for (size_t i = 0; i < skeleton.joints.size(); ++i) {
        if (skeleton.joints[i].name != pose.joints[i].name ||
            skeleton.joints[i].parent.value_or(-1) != pose.joints[i].parent) return false;
        skeleton.joints[i].transform = pose.joints[i].transform;
    }
    if (skeleton.IsValid()) UpdateSkeleton(skeleton);
    std::vector<SkinCluster::VertexInfluence> influences;
    std::vector<Matrix4x4> palette;
    if (source->HasSkinning()) {
        influences.resize(source->GetVertexCount());
        SkinCluster::BuildInfluences(skeleton, *source, influences, palette);
        for (size_t i = 0; i < palette.size(); ++i)
            palette[i] = Multiply(palette[i], skeleton.joints[i].skeletonSpaceMatrix);
    }
    auto transformPoint = [](const Vector4& v, const Matrix4x4& m) {
        return Vector4{
            v.x*m.m[0][0]+v.y*m.m[1][0]+v.z*m.m[2][0]+v.w*m.m[3][0],
            v.x*m.m[0][1]+v.y*m.m[1][1]+v.z*m.m[2][1]+v.w*m.m[3][1],
            v.x*m.m[0][2]+v.y*m.m[1][2]+v.z*m.m[2][2]+v.w*m.m[3][2], 1.0f };
    };
    ReplayBounds result;
    const auto& vertices = source->GetVertices();
    for (size_t i = 0; i < vertices.size(); ++i) {
        Vector4 position = vertices[i].position;
        if (!influences.empty()) {
            Vector4 sum{}; float total = 0;
            for (float weight : influences[i].weights) { if (!std::isfinite(weight)) return false; total += weight; }
            for (size_t w = 0; total > 0 && w < SkinCluster::kMaxInfluence; ++w) {
                const float weight = influences[i].weights[w];
                const auto joint = influences[i].jointIndices[w];
                if (joint >= palette.size()) return false;
                const auto v = transformPoint(position, palette[joint]);
                sum.x += v.x*weight; sum.y += v.y*weight; sum.z += v.z*weight;
            }
            if (total > 0) position = {sum.x, sum.y, sum.z, 1};
        }
        const auto v = transformPoint(position, pose.renderWorld);
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return false;
        if (!result.valid) { result.min = result.max = {v.x,v.y,v.z}; result.valid = true; }
        else {
            result.min = {(std::min)(result.min.x,v.x),(std::min)(result.min.y,v.y),(std::min)(result.min.z,v.z)};
            result.max = {(std::max)(result.max.x,v.x),(std::max)(result.max.y,v.y),(std::max)(result.max.z,v.z)};
        }
    }
    if (!result.valid) return false;
    out = result;
    return true;
}
