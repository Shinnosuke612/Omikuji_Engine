// 役割: シーン描画結果を保持するRenderTargetとSRVを管理する。
#pragma once

#include <cstdint>

#include <d3d12.h>
#include <dxgiformat.h>
#include <wrl.h>

class DirectXCommon;
class SrvManager;

class SceneRenderTarget {
public:
	SceneRenderTarget() = default;
	~SceneRenderTarget();
	SceneRenderTarget(const SceneRenderTarget&) = delete;
	SceneRenderTarget& operator=(const SceneRenderTarget&) = delete;
	SceneRenderTarget(SceneRenderTarget&&) = delete;
	SceneRenderTarget& operator=(SceneRenderTarget&&) = delete;

	struct Desc {
		uint32_t width = 1;
		uint32_t height = 1;
		DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		bool createDepth = true;
		float clearColor[4]{ 0.1f, 0.2f, 0.8f, 1.0f };
	};

	bool Initialize(
		DirectXCommon* dxCommon,
		SrvManager* srvManager,
		uint32_t width,
		uint32_t height
	);
	bool Initialize(
		DirectXCommon* dxCommon,
		SrvManager* srvManager,
		const Desc& desc
	);
	void Resize(uint32_t width, uint32_t height);
	void Begin();
	void Begin(bool clearColor, bool clearDepth);
	void End();

	uint32_t GetWidth() const { return width_; }
	uint32_t GetHeight() const { return height_; }
	DXGI_FORMAT GetFormat() const { return format_; }
	D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpuHandle() const;
	D3D12_GPU_DESCRIPTOR_HANDLE GetDepthSrvGpuHandle() const;

private:
	bool CreateResources();
	void ReleaseDescriptors();

	DirectXCommon* dxCommon_ = nullptr;
	SrvManager* srvManager_ = nullptr;

	Microsoft::WRL::ComPtr<ID3D12Resource> colorResource_;
	Microsoft::WRL::ComPtr<ID3D12Resource> depthResource_;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;

	uint32_t srvIndex_ = UINT32_MAX;
	uint32_t depthSrvIndex_ = UINT32_MAX;
	uint32_t width_ = 1;
	uint32_t height_ = 1;
	DXGI_FORMAT format_ = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	bool createDepth_ = true;
	float clearColor_[4]{ 0.1f, 0.2f, 0.8f, 1.0f };
	bool initialized_ = false;
	bool colorReadable_ = true;
	bool depthReadable_ = false;
};
