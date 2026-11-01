// 役割: 環境CubeMapを使うSkyboxの描画状態を管理する。
#pragma once
#include <string>
#include <d3d12.h>
#include "../math/Vector3.h"
#include "../math/Vector4.h"
#include "../math/Matrix4x4.h"
#include "../math/Transform.h"
#include <wrl/client.h>

class Object3dCommon;
class Camera;

class Skybox {
private:
	struct TransformationMatrix {
		Matrix4x4 inverseViewProjection;
		Vector4 cameraPosition;
	};

	struct Material {
		Vector4 color;
	};

public:
	void Initialize(Object3dCommon* object3dCommon, const std::string& textureFilePath);
    // 新規instance専用。cacheだけを参照し、全buffer/Map成功後に公開する。
    bool TryInitializeReplay(Object3dCommon* common, const std::string& textureKey);
    const std::string& GetTextureKey() const { return textureFilePath_; }
    Vector4 GetColor() const { return materialData_ ? materialData_->color : Vector4{}; }
	void Update();
	void Draw();

	void SetScale(const Vector3& scale) { (void)scale; }
	void SetColor(const Vector4& color) { materialData_->color = color; }
	void SetCamera(Camera* camera) { camera_ = camera; }

private:
	void CreateTransformationMatrixResource();
	void CreateMaterialResource();

private:
	Object3dCommon* object3dCommon_ = nullptr;
	Camera* camera_ = nullptr;

	std::string textureFilePath_;

	Microsoft::WRL::ComPtr<ID3D12Resource> transformationMatrixResource_ = nullptr;
	TransformationMatrix* transformationMatrixData_ = nullptr;

	Microsoft::WRL::ComPtr<ID3D12Resource> materialResource_ = nullptr;
	Material* materialData_ = nullptr;
};
