// 役割: テクスチャに依存しない地面ひびの生成、寿命、描画を所有する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <d3d12.h>
#include <wrl.h>
#include "../math/Matrix4x4.h"
#include "../math/Vector3.h"
#include "../math/Vector4.h"
#include "../scene/SceneRuntimePreparation.h"

class Camera;
class DirectXCommon;

class GroundCrackRenderer {
public:
	struct SpawnRequest {
		Vector3 position{}; Vector3 normal{ 0.0f, 1.0f, 0.0f };
		float radius = 3.2f; uint32_t primaryBranchCount = 10; uint32_t segmentsPerBranch = 6;
		float branchProbability = 0.25f; float width = 0.06f; float lifetime = 1.2f;
		float surfaceOffset = 0.03f; uint32_t seed = 1;
	};
	void Initialize(DirectXCommon* dxCommon, uint32_t maxCracks = 16, uint32_t maxVertices = 65536);
	// main専用。新規候補に設定を保持し、StepのReadyまでSpawn/Drawしない。
	bool BeginPrepared(DirectXCommon&, std::string& diagnostic,
		uint32_t maxCracks = 16, uint32_t maxVertices = 65536);
	RuntimePrepareStep StepPrepared(SceneRuntimePrepareBudget& remaining, std::string& diagnostic);
	void Finalize();
	void Update(float deltaTime);
	void Spawn(const SpawnRequest& request);
	void Draw(const Camera* camera);

private:
	// Monitor CameraとMain Cameraが同一Command Listへ描画しても、後続Drawの
	// Upload更新が先行DrawのVertex／Cameraデータを上書きしないようにする。
	static constexpr uint32_t kDrawSlotCount = 8;
	static constexpr uint32_t kCameraSlotSize = 256;

	struct Vertex { Vector3 position; Vector4 color; };
	struct Segment { Vector3 start; Vector3 end; float reveal = 0.0f; };
	struct Crack { std::vector<Segment> segments; float age = 0.0f; float lifetime = 1.0f; float width = 0.05f; Vector3 normal{}; };
	struct CameraData { Matrix4x4 viewProjection; };
	static float Random01(uint32_t& state);
	bool CreateRootSignature();
	bool CreatePipeline(std::string& diagnostic);
	bool CreateVertexBuffer();
	bool CreateCameraBuffer();
	enum class PrepareStage { Inactive, Root, Pipeline, Vertices, Cameras, Ready, Failed };
	PrepareStage prepareStage_ = PrepareStage::Inactive;
	void AddRibbon(const Segment& segment, const Vector3& normal, float width, const Vector4& color);
	void BuildVertices();
	DirectXCommon* dxCommon_ = nullptr;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> basePipelineState_, glowPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12Resource> vertexResource_, cameraResource_;
	Vertex* mappedVertices_ = nullptr;
	uint8_t* mappedCameraData_ = nullptr;
	D3D12_VERTEX_BUFFER_VIEW vertexBufferView_{};
	std::vector<Crack> cracks_; std::vector<Vertex> vertices_;
	uint32_t maxCracks_ = 0, maxVertices_ = 0, baseVertexCount_ = 0;
	uint32_t drawSlotIndex_ = 0;
	bool isInitialized_ = false;
};
