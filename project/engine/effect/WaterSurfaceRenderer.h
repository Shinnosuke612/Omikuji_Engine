// 役割: 水面の反射、屈折、ライトシャフト描画を管理する。
#pragma once

#include <cstdint>

#include <d3d12.h>
#include <wrl.h>

#include "../math/Matrix4x4.h"
#include "../math/Vector2.h"
#include "../math/Vector3.h"
#include "../math/Vector4.h"

class Camera;
class DirectXCommon;

class WaterSurfaceRenderer {
public:
	struct Settings {
		Vector4 baseColor{ 0.04f, 0.55f, 0.78f, 1.0f };
		Vector4 highlightColor{ 0.42f, 0.95f, 1.20f, 1.0f };
		float alpha = 0.36f;
		float fresnelPower = 3.0f;
		float normalStrength = 0.75f;
		float waveScale = 1.0f;
		bool enabled = true;
	};

	void Initialize(DirectXCommon* dxCommon, uint32_t gridResolution = 72);
	// 新規Replay専用instance。失敗時は破棄し、通常描画へ渡さない。
	bool TryInitializeReplay(DirectXCommon* dxCommon, uint32_t gridResolution = 72);
	// 明示時計で描画し、通常のtime_を変更しない。GPU完了後に一面一回だけ使用。
	void DrawAtTime(const Camera*, const Vector3&, const Vector3&, const Settings&, float time);
	void Finalize();
	void Update(float deltaTime);
	float GetTime() const { return time_; } // 描画に使用する時計の値コピー。進行/巻戻しなし。
	void Draw(
		const Camera* camera,
		const Vector3& center,
		const Vector3& halfSize,
		const Settings& settings
	);

private:
	struct Vertex {
		Vector3 position;
		Vector2 uv;
	};

	struct SurfaceData {
		Matrix4x4 viewProjection;
		Vector4 centerTime;
		Vector4 halfSizeAlpha;
		Vector4 cameraPositionFresnel;
		Vector4 waveA;
		Vector4 waveB;
		Vector4 waveC;
		Vector4 baseColor;
		Vector4 highlightColorNormal;
	};

	bool CreateRootSignature(bool recoverable = false);
	bool CreateGraphicsPipeline(bool recoverable = false);
	bool CreateResources(uint32_t gridResolution, bool recoverable = false);

	DirectXCommon* dxCommon_ = nullptr;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState_;
	Microsoft::WRL::ComPtr<ID3D12Resource> vertexResource_;
	Microsoft::WRL::ComPtr<ID3D12Resource> surfaceResource_;
	SurfaceData* surfaceData_ = nullptr;
	D3D12_VERTEX_BUFFER_VIEW vertexBufferView_{};
	uint32_t vertexCount_ = 0;
	float time_ = 0.0f;
};
