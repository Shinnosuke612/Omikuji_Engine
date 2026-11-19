// 役割: Skybox用のCubeMap描画とGPUリソース設定を実装する。
#include "Skybox.h"
#include <type_traits>
#include <exception>
#include "Object3dCommon.h"
#include "Camera.h"
#include "../base/DirectXCommon.h"
#include "../2d/TextureManager.h"
#include <cassert>

void Skybox::Initialize(Object3dCommon* object3dCommon, const std::string& textureFilePath) {
	assert(object3dCommon);

	object3dCommon_ = object3dCommon;
	camera_ = object3dCommon_->GetDefaultCamera();
	textureFilePath_ = textureFilePath;

	TextureManager::GetInstance()->LoadTexture(textureFilePath_);

	CreateTransformationMatrixResource();
	CreateMaterialResource();
}

bool Skybox::TryInitializeReplay(Object3dCommon* common, const std::string& key) {
    if (!common || !common->GetDxCommon() || object3dCommon_ || !TextureManager::GetInstance()->HasTexture(key)) return false;
    Microsoft::WRL::ComPtr<ID3D12Resource> transform, material;
    TransformationMatrix* td = nullptr; Material* md = nullptr;
    auto* dx = common->GetDxCommon();
    if (!dx->TryCreateBufferResource(sizeof(TransformationMatrix), transform) ||
        !dx->TryCreateBufferResource(sizeof(Material), material) ||
        FAILED(transform->Map(0, nullptr, reinterpret_cast<void**>(&td))) || !td ||
        FAILED(material->Map(0, nullptr, reinterpret_cast<void**>(&md))) || !md) return false;
    textureFilePath_ = key;
    *td = {MakeIdentity4x4(), {0,0,0,1}}; *md = {{1,1,1,1}};
    transformationMatrixResource_ = std::move(transform); materialResource_ = std::move(material);
    transformationMatrixData_ = td; materialData_ = md;
    object3dCommon_ = common; camera_ = common->GetDefaultCamera(); return true;
}

bool Skybox::BeginPreparedInitialize(Object3dCommon& common, Camera& candidateCamera, const std::string& key, std::string& diagnostic) {
	auto& textures = *TextureManager::GetInstance();
	if (object3dCommon_ || !common.GetDxCommon() || !textures.HasTexture(key) ||
		!textures.GetSrvHandleGPU(key).ptr || !textures.GetMetaData(key).IsCubemap()) {
		diagnostic = "Skybox requires an unused instance, a device and a published cube key.";
		return false;
	}
	try {
		textureFilePath_ = key;
	} catch (const std::exception&) {
		diagnostic = "Failed to retain the prepared Skybox cube key.";
		return false;
	}
	object3dCommon_ = &common;
	camera_ = &candidateCamera;
	preparedStage_ = 1;
	diagnostic.clear();
	return true;
}

RuntimePrepareStep Skybox::StepPreparedInitialize(SceneRuntimePrepareBudget& remaining, std::string& diagnostic) {
	using Step = RuntimePrepareStep;
	diagnostic.clear();
	if (preparedStage_ == 3) return Step::Ready;
	if (preparedStage_ <= 0) {
		diagnostic = "Skybox preparation was not started or failed.";
		return Step::Failed;
	}
	if (!remaining.maxCopyBytes) return Step::Pending;
	// 確保後にcopy枠が尽きても、所有bufferとMapを次回へ持ち越す。
	const auto prepare = [&](auto& resource, auto*& data, size_t bytes) {
		if (!resource) {
			if (!remaining.maxResourceCreates) return false;
			--remaining.maxResourceCreates;
			void* mapped = nullptr;
			D3D12_RANGE readRange{0, 0};
			if (!object3dCommon_->GetDxCommon()->TryCreateBufferResource(bytes, resource) || !resource ||
				FAILED(resource->Map(0, &readRange, &mapped)) || !mapped) {
				preparedStage_ = -1;
				diagnostic = "Skybox upload buffer creation or mapping failed.";
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
	case 1: {
		if (!prepare(transformationMatrixResource_, transformationMatrixData_, sizeof(TransformationMatrix))) return pendingOrFailed();
		*transformationMatrixData_ = {MakeIdentity4x4(), {0,0,0,1}};
		break;
	}
	case 2: {
		if (!prepare(materialResource_, materialData_, sizeof(Material))) return pendingOrFailed();
		*materialData_ = {{1,1,1,1}};
		break;
	}
	default: return Step::Failed;
	}
	++preparedStage_;
	return preparedStage_ == 3 ? Step::Ready : Step::Pending;
}

void Skybox::Update() {
	if (camera_) {
		transformationMatrixData_->inverseViewProjection =
			Inverse(camera_->GetViewProjectionMatrix());
		const Vector3& cameraPosition = camera_->GetTranslate();
		transformationMatrixData_->cameraPosition = {
			cameraPosition.x,
			cameraPosition.y,
			cameraPosition.z,
			1.0f
		};
	} else {
		transformationMatrixData_->inverseViewProjection = MakeIdentity4x4();
		transformationMatrixData_->cameraPosition = { 0.0f, 0.0f, 0.0f, 1.0f };
	}
}

void Skybox::Draw() {
	auto* commandList = object3dCommon_->GetDxCommon()->GetCommandList();

	object3dCommon_->SetSkyboxRenderState();

	commandList->SetGraphicsRootConstantBufferView(
		0,
		materialResource_->GetGPUVirtualAddress()
	);

	commandList->SetGraphicsRootConstantBufferView(
		1,
		transformationMatrixResource_->GetGPUVirtualAddress()
	);

	commandList->SetGraphicsRootDescriptorTable(
		2,
		TextureManager::GetInstance()->GetSrvHandleGPU(textureFilePath_)
	);
	commandList->SetGraphicsRootDescriptorTable(
		7,
		TextureManager::GetInstance()->GetSrvHandleGPU(textureFilePath_)
	);

	commandList->DrawInstanced(3, 1, 0, 0);
}

void Skybox::CreateTransformationMatrixResource() {
	transformationMatrixResource_ = object3dCommon_->GetDxCommon()->CreateBufferResource(
		sizeof(TransformationMatrix)
	);

	transformationMatrixResource_->Map(
		0,
		nullptr,
		reinterpret_cast<void**>(&transformationMatrixData_)
	);

	transformationMatrixData_->inverseViewProjection = MakeIdentity4x4();
	transformationMatrixData_->cameraPosition = { 0.0f, 0.0f, 0.0f, 1.0f };
}

void Skybox::CreateMaterialResource() {
	materialResource_ = object3dCommon_->GetDxCommon()->CreateBufferResource(
		sizeof(Material)
	);

	materialResource_->Map(
		0,
		nullptr,
		reinterpret_cast<void**>(&materialData_)
	);

	materialData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };
}
