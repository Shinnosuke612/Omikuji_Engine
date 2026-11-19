// 役割: 確定済みの移動範囲を通常のScene HDRパスへ描画する。
#pragma once

#include <cstdint>
#include <d3d12.h>
#include <wrl.h>
#include "../math/Vector3.h"
#include "../scene/SceneMovementBounds.h"
#include "../scene/SceneRuntimePreparation.h"
#include <string>

class Camera;
class DirectXCommon;

class MovementBoundaryRenderer {
public:
	// Deviceは借用。GPU資源はRendererが所有し、Sceneと同じ寿命で解放する。
	explicit MovementBoundaryRenderer(DirectXCommon* device) : device_(device) {}
	// 同じserialの複数Viewで使用するUpload範囲を重ねない。
	void BeginFrame(uint64_t serial);
	void Draw(const Camera* camera, const SceneMovementBoundsFrame& frame);
	// main専用。初回呼出しでDrawの遅延作成を無効にし、既存描画資源を残枠で準備する。
	RuntimePrepareStep StepPrepared(SceneRuntimePrepareBudget& remaining, std::string& diagnostic);

private:
	struct Vertex { Vector3 position; Vector4 color; };
	static constexpr UINT kCircleSegments = 96;
	static constexpr UINT kVerticesPerView = kCircleSegments * 6;
	static constexpr UINT kViews = 8;
	static constexpr UINT kCameraStride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
	bool CreateRoot();
	bool CreatePipeline(std::string& diagnostic);
	enum class PrepareStage { Root, Pipeline, Vertices, Cameras, Ready, Failed };
	PrepareStage prepareStage_ = PrepareStage::Root;

	DirectXCommon* device_;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
	Microsoft::WRL::ComPtr<ID3D12Resource> vertices_, cameras_;
	Vertex* vertexData_ = nullptr;
	uint8_t* cameraData_ = nullptr;
	uint64_t serial_ = 0;
	UINT slot_ = 0;
	bool attempted_ = false;
};
