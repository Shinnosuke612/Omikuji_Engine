// 役割: DescriptorHeapの生成、Descriptor確保、GPU可視化設定を実装する。
#include "SrvManager.h"
#include <cassert>
#include <stdexcept>
SrvManager* SrvManager::instance_ = nullptr;

SrvManager* SrvManager::GetInstance() {
	assert(instance_);
	return instance_;
}

void SrvManager::Initialize(DirectXCommon* dxCommon){
	if (!dxCommon) {
		throw std::runtime_error("SrvManager initialization requires DirectXCommon");
	}
	instance_ = this;
	this->directXCommon = dxCommon;
	//デスクリプタヒープの生成
	descriptorHeap = directXCommon->CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kMaxSRVCount, true);
	//デスクリプタ一個分のサイズを取得して記録
	descriptorSize = directXCommon->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	useIndex = 0;
	allocatedIndices_.fill(false);
	freeIndices_.clear();
	shadowFallbackDescriptorReady_ = CreateShadowFallbackDescriptor();
	if (!shadowFallbackDescriptorReady_) {
		throw std::runtime_error("SrvManager shadow fallback descriptor initialization failed");
	}
}

bool SrvManager::TryAllocate(uint32_t& outIndex) {
	outIndex = kInvalidIndex;
	if (!freeIndices_.empty()) {
		const uint32_t index = freeIndices_.back();
		if (index >= kDynamicSRVCount || allocatedIndices_[index]) {
			assert(false && "SrvManager free list is invalid");
			return false;
		}
		freeIndices_.pop_back();
		allocatedIndices_[index] = true;
		outIndex = index;
		return true;
	}
	if (useIndex >= kDynamicSRVCount) {
		return false;
	}

	const uint32_t index = useIndex;
	if (allocatedIndices_[index]) {
		assert(false && "SrvManager allocation state is invalid");
		return false;
	}
	++useIndex;
	allocatedIndices_[index] = true;
	outIndex = index;
	return true;
}

bool SrvManager::CanAllocate() const{
	return !freeIndices_.empty() || useIndex < kDynamicSRVCount;
}

bool SrvManager::Free(uint32_t index) {
	if (index == kShadowFallbackDescriptorIndex) {
		return false;
	}
	if (!IsAllocated(index)) {
		return false;
	}
	allocatedIndices_[index] = false;
	freeIndices_.push_back(index);
	return true;
}

bool SrvManager::Free(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle) {
	if (!descriptorHeap || descriptorSize == 0) {
		return false;
	}
	const D3D12_CPU_DESCRIPTOR_HANDLE heapStart =
		descriptorHeap->GetCPUDescriptorHandleForHeapStart();
	if (cpuHandle.ptr < heapStart.ptr) {
		return false;
	}
	const SIZE_T offset = cpuHandle.ptr - heapStart.ptr;
	if (offset % descriptorSize != 0) {
		return false;
	}
	const SIZE_T index = offset / descriptorSize;
	if (index >= kDynamicSRVCount) {
		return false;
	}
	return Free(static_cast<uint32_t>(index));
}

bool SrvManager::IsAllocated(uint32_t index) const {
	return index < kDynamicSRVCount && allocatedIndices_[index];
}

D3D12_CPU_DESCRIPTOR_HANDLE SrvManager::GetCPUDescriptorHandle(uint32_t index){
	if (!descriptorHeap || !IsAllocated(index)) {
		assert(false && "Invalid SRV CPU descriptor index");
		return {};
	}
	return GetCPUDescriptorHandleInternal(index);
}

D3D12_GPU_DESCRIPTOR_HANDLE SrvManager::GetGPUDescriptorHandle(uint32_t index){
	if (!descriptorHeap || !IsAllocated(index)) {
		assert(false && "Invalid SRV GPU descriptor index");
		return {};
	}
	return GetGPUDescriptorHandleInternal(index);
}

D3D12_GPU_DESCRIPTOR_HANDLE SrvManager::GetShadowFallbackDescriptorHandle() const {
	if (!shadowFallbackDescriptorReady_) {
		return {};
	}
	return GetGPUDescriptorHandleInternal(kShadowFallbackDescriptorIndex);
}

bool SrvManager::CreateShadowFallbackDescriptor() {
	if (!descriptorHeap || !directXCommon || !directXCommon->GetDevice() || descriptorSize == 0) {
		return false;
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray.MipLevels = 1;
	srvDesc.Texture2DArray.ArraySize = 1;
	directXCommon->GetDevice()->CreateShaderResourceView(
		nullptr,
		&srvDesc,
		GetCPUDescriptorHandleInternal(kShadowFallbackDescriptorIndex)
	);
	return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE SrvManager::GetCPUDescriptorHandleInternal(uint32_t index) const {
	D3D12_CPU_DESCRIPTOR_HANDLE handleCPU = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
	handleCPU.ptr += descriptorSize * index;
	return handleCPU;
}

D3D12_GPU_DESCRIPTOR_HANDLE SrvManager::GetGPUDescriptorHandleInternal(uint32_t index) const {
	D3D12_GPU_DESCRIPTOR_HANDLE handleGPU = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
	handleGPU.ptr += descriptorSize * index;
	return handleGPU;
}

bool SrvManager::CreateSRVforTexture2D(uint32_t srvIndex, ID3D12Resource* pResource, DXGI_FORMAT Format, UINT MipLevels){
	if (!IsAllocated(srvIndex) || !pResource) {
		assert(false && "Invalid SRV texture resource");
		return false;
	}
D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
srvDesc.Format = Format;
srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
srvDesc.Texture2D.MipLevels = MipLevels;

	directXCommon->GetDevice()->CreateShaderResourceView(pResource, &srvDesc, GetCPUDescriptorHandle(srvIndex));
	return true;
}

bool SrvManager::CreateSRVforTexture2DArray(uint32_t srvIndex, ID3D12Resource* pResource, DXGI_FORMAT Format, UINT MipLevels, UINT arraySize) {
	if (!IsAllocated(srvIndex) || !pResource) {
		assert(false && "Invalid SRV texture array resource");
		return false;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = Format;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray.MipLevels = MipLevels;
	srvDesc.Texture2DArray.ArraySize = arraySize;

	directXCommon->GetDevice()->CreateShaderResourceView(pResource, &srvDesc, GetCPUDescriptorHandle(srvIndex));
	return true;
}

bool SrvManager::CreateSRVforStructuredBuffer(uint32_t srvIndex, ID3D12Resource* pResource, UINT numElements, UINT structureByteStride){
	if (!IsAllocated(srvIndex) || !pResource) {
		assert(false && "Invalid SRV buffer resource");
		return false;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
	srvDesc.Buffer.NumElements = numElements;
	srvDesc.Buffer.StructureByteStride = structureByteStride;
	directXCommon->GetDevice()->CreateShaderResourceView(pResource, &srvDesc, GetCPUDescriptorHandle(srvIndex));
	return true;
}

bool SrvManager::CreateUAVforStructuredBuffer(uint32_t srvIndex, ID3D12Resource* pResource, UINT numElements, UINT structureByteStride) {
	if (!IsAllocated(srvIndex) || !pResource) {
		assert(false && "Invalid UAV buffer resource");
		return false;
	}
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.FirstElement = 0;
	uavDesc.Buffer.NumElements = numElements;
	uavDesc.Buffer.StructureByteStride = structureByteStride;
	uavDesc.Buffer.CounterOffsetInBytes = 0;
	uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
	directXCommon->GetDevice()->CreateUnorderedAccessView(pResource, nullptr, &uavDesc, GetCPUDescriptorHandle(srvIndex));
	return true;
}

ID3D12DescriptorHeap* SrvManager::GetDescriptorHeap() const{
	return descriptorHeap.Get();
}


void SrvManager::PreDraw(){
//描画用のDescriptorHeapの設定
	ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorHeap.Get() };
	directXCommon->GetCommandList()->SetDescriptorHeaps(1, descriptorHeaps);
}

bool SrvManager::SetGraphicsRootDescriptorTable(UINT RootParameterIndex, uint32_t srvIndex){
	if (!IsAllocated(srvIndex)) {
		assert(false && "Invalid graphics SRV descriptor index");
		return false;
	}
	directXCommon->GetCommandList()->SetGraphicsRootDescriptorTable(RootParameterIndex, GetGPUDescriptorHandle(srvIndex));
	return true;
}

bool SrvManager::SetComputeRootDescriptorTable(UINT RootParameterIndex, uint32_t srvIndex) {
	if (!IsAllocated(srvIndex)) {
		assert(false && "Invalid compute SRV descriptor index");
		return false;
	}
	directXCommon->GetCommandList()->SetComputeRootDescriptorTable(RootParameterIndex, GetGPUDescriptorHandle(srvIndex));
	return true;
}
