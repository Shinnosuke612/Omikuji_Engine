// 役割: SRV、UAV、CBV用DescriptorHeapの割り当てとGPUハンドル取得を管理する。
#pragma once
#include "../base/DirectXCommon.h"
#include <array>
#include <cstdint>
#include <vector>
class SrvManager{

public:
	static SrvManager* GetInstance();

	//初期化
	void Initialize(DirectXCommon* dxCommon);

	static constexpr uint32_t kInvalidIndex = UINT32_MAX;

	// slotを確保できた時だけoutIndexへ有効なindexを入れる。失敗時は状態を変えずkInvalidIndexを返す。
	bool TryAllocate(uint32_t& outIndex);
	// TryAllocateで取得したslotだけを返却する。範囲外、未割当、二重返却はfalseで状態を変えない。
	bool Free(uint32_t index);
	bool Free(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle);
	bool IsAllocated(uint32_t index) const;

	// SRV確保可能チェック
	bool CanAllocate() const;

	D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandle(uint32_t index);
	D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandle(uint32_t index);
	// Shadow fallback専用の型付きnull Texture2DArray SRVを返す。
	D3D12_GPU_DESCRIPTOR_HANDLE GetShadowFallbackDescriptorHandle() const;

	// SRV生成(テクスチャ用)。未割当slotまたはnull resourceでは生成せずfalseを返す。
	bool CreateSRVforTexture2D(uint32_t srvIndex, ID3D12Resource* pResource, DXGI_FORMAT Format, UINT MipLevels);
	bool CreateSRVforTexture2DArray(uint32_t srvIndex, ID3D12Resource* pResource, DXGI_FORMAT Format, UINT MipLevels, UINT arraySize);
	// SRV生成(Structured Buffer用)。未割当slotまたはnull resourceでは生成せずfalseを返す。
	bool CreateSRVforStructuredBuffer(uint32_t srvIndex, ID3D12Resource* pResource, UINT numElements,UINT structureByteStride);
	// UAV生成(Structured Buffer用)。未割当slotまたはnull resourceでは生成せずfalseを返す。
	bool CreateUAVforStructuredBuffer(uint32_t srvIndex, ID3D12Resource* pResource, UINT numElements, UINT structureByteStride);

	ID3D12DescriptorHeap* GetDescriptorHeap() const;

	void PreDraw();

	// 未割当slotではroot tableを変更せずfalseを返す。
	bool SetGraphicsRootDescriptorTable(UINT RootParameterIndex, uint32_t srvIndex);
	bool SetComputeRootDescriptorTable(UINT RootParameterIndex, uint32_t srvIndex);

private:
	DirectXCommon* directXCommon = nullptr;
	static SrvManager* instance_;

	// 最大SRV数（最大テクスチャ枚数）
	static constexpr uint32_t kMaxSRVCount = 512;
	static constexpr uint32_t kShadowFallbackDescriptorIndex = kMaxSRVCount - 1;
	static constexpr uint32_t kDynamicSRVCount = kShadowFallbackDescriptorIndex;

	bool CreateShadowFallbackDescriptor();
	D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandleInternal(uint32_t index) const;
	D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandleInternal(uint32_t index) const;
	// SRV用のデスクリプタサイズ
	uint32_t descriptorSize = 0;
	// SRV用デスクリプタヒープ
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap;

	//次に使用するSRVインデックス
	uint32_t useIndex = 0;
	std::array<bool, kMaxSRVCount> allocatedIndices_{};
	std::vector<uint32_t> freeIndices_;
	bool shadowFallbackDescriptorReady_ = false;
};

