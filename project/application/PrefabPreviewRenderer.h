// 役割: Prefab全階層を通常Sceneから分離したRenderTargetへ描画する。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <d3d12.h>

#include "../engine/math/Matrix4x4.h"
#include "../engine/math/Vector3.h"

class Camera;
class DirectXCommon;
class Object3d;
class SceneDocument;
class SceneRenderTarget;
class SrvManager;

class PrefabPreviewRenderer final {
public:
	struct OverlayOptions {
		uint64_t selectedEntityId = 0;
		bool showSkeleton = true;
		bool showJointAxes = false;
		bool showColliders = true;
		bool showCombatVolumes = true;
		bool isolateSelectedCollider = false;
		bool showGrid = true;
	};

	PrefabPreviewRenderer() = default;
	~PrefabPreviewRenderer();

	bool Initialize(DirectXCommon* dxCommon, SrvManager* srvManager);
	void Render(
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
	);
	void Finalize();

	D3D12_GPU_DESCRIPTOR_HANDLE GetTexture() const;
	uint32_t GetWidth() const;
	uint32_t GetHeight() const;
	const Matrix4x4& GetViewMatrix() const { return viewMatrix_; }
	const Matrix4x4& GetProjectionMatrix() const { return projectionMatrix_; }

private:
	struct ModelRuntime {
		std::unique_ptr<Object3d> object;
		std::string modelPath;
	};

	void SyncModels(const SceneDocument& document);
	void SyncGhostModels(const SceneDocument* document);
	void UpdateFraming(
		const SceneDocument& document,
		const OverlayOptions& options
	);
	void DrawEditorOverlays(
		const SceneDocument& document,
		const SceneDocument* ghostDocument,
		const OverlayOptions& options
	);

	DirectXCommon* dxCommon_ = nullptr;
	SrvManager* srvManager_ = nullptr;
	SceneRenderTarget* renderTarget_ = nullptr;
	Camera* camera_ = nullptr;
	std::unordered_map<uint64_t, ModelRuntime> models_;
	std::unordered_map<uint64_t, ModelRuntime> ghostModels_;
	std::string framedAssetPath_;
	uint64_t appliedFramingSerial_ = 0;
	bool framingInitialized_ = false;
	Vector3 orbitTarget_{};
	float fitDistance_ = 5.0f;
	Matrix4x4 viewMatrix_ = MakeIdentity4x4();
	Matrix4x4 projectionMatrix_ = MakeIdentity4x4();
};
