// 役割: 限定GPU mipの能力/PSO/view準備と一frame全段の依存barrierを管理する。
#include "SceneTextureMipGenerator.h"
#include "../base/DirectXCommon.h"
#include <algorithm>
#include <utility>

bool SceneTextureMipGenerator::Initialize(DirectXCommon& dx, std::string& diagnostic) {
	if (attempted_) return ready_;
	attempted_ = true;
	if (!kEnableGpuMips) { diagnostic = "disabled_by_comparison_setting"; return false; }
	device_ = dx.GetDevice();
	D3D12_FEATURE_DATA_FORMAT_SUPPORT linear{DXGI_FORMAT_R8G8B8A8_UNORM};
	D3D12_FEATURE_DATA_FORMAT_SUPPORT srgb{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
	D3D12_FEATURE_DATA_FORMAT_INFO info{DXGI_FORMAT_R8G8B8A8_UNORM};
	const auto required = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
		D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
	if (!device_ || FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&linear,sizeof(linear))) ||
		(linear.Support1 & required) != required || !(linear.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) ||
		FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&srgb,sizeof(srgb))) ||
		!(srgb.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) || !(srgb.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
		FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO,&info,sizeof(info))) || info.PlaneCount != 1) {
		diagnostic = "rgba8_gpu_mip_capability_unavailable"; return false;
	}
	Microsoft::WRL::ComPtr<IDxcBlob> shader;
	if (!dx.TryCompileShader(L"resources/shaders/SceneTextureMip.CS.hlsl",L"cs_6_0",shader,diagnostic)) return false;
	D3D12_DESCRIPTOR_RANGE ranges[2]{};
	ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
	ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
	D3D12_ROOT_PARAMETER parameters[3]{};
	for (UINT i=0;i<2;++i) {
		parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[i].DescriptorTable = {1,&ranges[i]};
		parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	}
	parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[2].Constants = {0,0,4};
	D3D12_ROOT_SIGNATURE_DESC rootDesc{};
	rootDesc.NumParameters = 3; rootDesc.pParameters = parameters;
	Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
	if (FAILED(D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors)) || !blob ||
		FAILED(device_->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root_)))) {
		diagnostic = "gpu_mip_root_creation_failed"; return false;
	}
	D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
	pso.pRootSignature = root_.Get(); pso.CS = {shader->GetBufferPointer(),shader->GetBufferSize()};
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = (kSceneGpuMipCount-1)*2;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(device_->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipeline_))) ||
		FAILED(device_->CreateDescriptorHeap(&heapDesc,IID_PPV_ARGS(&heap_)))) {
		diagnostic = "gpu_mip_pipeline_or_heap_failed"; return false;
	}
	increment_ = device_->GetDescriptorHandleIncrementSize(heapDesc.Type);
	ready_ = true; diagnostic.clear(); return true;
}

bool SceneTextureMipGenerator::Prepare(ID3D12Resource& texture,
	std::vector<Microsoft::WRL::ComPtr<IUnknown>>& out, std::string& diagnostic) {
	const auto desc = texture.GetDesc();
	Microsoft::WRL::ComPtr<ID3D12Device> owner;
	if (!ready_ || FAILED(texture.GetDevice(IID_PPV_ARGS(&owner))) || owner.Get() != device_ ||
		desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != kSceneGpuMipSize ||
		desc.Height != kSceneGpuMipSize || desc.DepthOrArraySize != 1 || desc.MipLevels != kSceneGpuMipCount ||
		desc.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS || desc.SampleDesc.Count != 1 ||
		!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
		diagnostic = "gpu_mip_resource_or_backend_mismatch"; return false;
	}
	// 記録前に全確保。descriptorは各段に固有で、GPU参照中に上書きしない。
	std::vector<Microsoft::WRL::ComPtr<IUnknown>> refs;
	refs.reserve(4);
	refs.emplace_back(&texture); refs.emplace_back(root_.Get()); refs.emplace_back(pipeline_.Get()); refs.emplace_back(heap_.Get());
	auto handle = heap_->GetCPUDescriptorHandleForHeapStart();
	for (UINT mip=1;mip<kSceneGpuMipCount;++mip) {
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MostDetailedMip = mip-1; srv.Texture2D.MipLevels = 1;
		device_->CreateShaderResourceView(&texture,&srv,handle); handle.ptr += increment_;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		uav.Texture2D.MipSlice = mip;
		device_->CreateUnorderedAccessView(&texture,nullptr,&uav,handle); handle.ptr += increment_;
	}
	out = std::move(refs); diagnostic.clear(); return true;
}

void SceneTextureMipGenerator::Record(ID3D12GraphicsCommandList& list, ID3D12Resource& texture) const noexcept {
	const auto transition = [&](UINT mip,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition = {&texture,mip,from,to}; list.ResourceBarrier(1,&barrier);
	};
	ID3D12DescriptorHeap* heaps[] = {heap_.Get()}; list.SetDescriptorHeaps(1,heaps);
	list.SetComputeRootSignature(root_.Get()); list.SetPipelineState(pipeline_.Get());
	transition(0,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	auto handle = heap_->GetGPUDescriptorHandleForHeapStart();
	for (UINT mip=1;mip<kSceneGpuMipCount;++mip) {
		transition(mip,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		list.SetComputeRootDescriptorTable(0,handle); handle.ptr += increment_;
		list.SetComputeRootDescriptorTable(1,handle); handle.ptr += increment_;
		const UINT size = kSceneGpuMipSize >> mip;
		const UINT constants[] = {size*2,size*2,size,size};
		list.SetComputeRoot32BitConstants(2,4,constants,0);
		list.Dispatch((size+7)/8,(size+7)/8,1);
		D3D12_RESOURCE_BARRIER ordering{}; ordering.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		ordering.UAV.pResource = &texture; list.ResourceBarrier(1,&ordering);
		transition(mip,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}
	transition(D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_GENERIC_READ);
}
