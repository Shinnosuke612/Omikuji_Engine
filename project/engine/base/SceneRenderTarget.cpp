// 役割: SceneRenderTargetのGPUリソース生成と状態遷移を実装する。
#include "SceneRenderTarget.h"

#include <algorithm>
#include <cassert>

#include "DirectXCommon.h"
#include "RenderFormats.h"
#include "../3d/SrvManager.h"

SceneRenderTarget::~SceneRenderTarget() {
	ReleaseDescriptors();
}

void SceneRenderTarget::ReleaseDescriptors() {
	if (srvManager_) {
		if (depthSrvIndex_ != SrvManager::kInvalidIndex) {
			srvManager_->Free(depthSrvIndex_);
			depthSrvIndex_ = SrvManager::kInvalidIndex;
		}
		if (srvIndex_ != SrvManager::kInvalidIndex) {
			srvManager_->Free(srvIndex_);
			srvIndex_ = SrvManager::kInvalidIndex;
		}
	}
	srvManager_ = nullptr;
}

bool SceneRenderTarget::Initialize(
	DirectXCommon* dxCommon,
	SrvManager* srvManager,
	uint32_t width,
	uint32_t height
) {
	Desc desc{};
	desc.width = width;
	desc.height = height;
	return Initialize(dxCommon, srvManager, desc);
}

bool SceneRenderTarget::Initialize(
	DirectXCommon* dxCommon,
	SrvManager* srvManager,
	const Desc& desc
) {
	assert(dxCommon);
	assert(srvManager);
	if (!dxCommon || !srvManager || initialized_) {
		return false;
	}

	uint32_t srvIndex = SrvManager::kInvalidIndex;
	if (!srvManager->TryAllocate(srvIndex)) {
		return false;
	}
	srvManager_ = srvManager;
	srvIndex_ = srvIndex;

	uint32_t depthSrvIndex = SrvManager::kInvalidIndex;
	if (desc.createDepth && !srvManager->TryAllocate(depthSrvIndex)) {
		ReleaseDescriptors();
		return false;
	}

	dxCommon_ = dxCommon;
	depthSrvIndex_ = depthSrvIndex;
	format_ = desc.format;
	createDepth_ = desc.createDepth;
	for (uint32_t index = 0; index < 4; ++index) {
		clearColor_[index] = desc.clearColor[index];
	}
	rtvHeap_ = dxCommon_->CreateDescriptorHeap(
		D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
		1,
		false
	);
	if (createDepth_) {
		dsvHeap_ = dxCommon_->CreateDescriptorHeap(
			D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
			1,
			false
		);
	}

	width_ = (std::max)(desc.width, 1u);
	height_ = (std::max)(desc.height, 1u);
	if (!CreateResources()) {
		colorResource_.Reset();
		depthResource_.Reset();
		rtvHeap_.Reset();
		dsvHeap_.Reset();
		ReleaseDescriptors();
		dxCommon_ = nullptr;
		return false;
	}
	initialized_ = true;
	return true;
}

void SceneRenderTarget::Resize(uint32_t width, uint32_t height) {
	width = std::clamp(width, 1u, 4096u);
	height = std::clamp(height, 1u, 4096u);
	if (!initialized_ || (width_ == width && height_ == height)) {
		return;
	}

	width_ = width;
	height_ = height;
	colorResource_.Reset();
	depthResource_.Reset();
	colorReadable_ = true;
	depthReadable_ = false;
	if (!CreateResources()) {
		initialized_ = false;
	}
}

void SceneRenderTarget::Begin() {
	Begin(true, true);
}

void SceneRenderTarget::Begin(bool clearColor, bool clearDepth) {
	if (!initialized_) {
		return;
	}

	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = colorResource_.Get();
	barrier.Transition.Subresource =
		D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = colorReadable_
		? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
		: D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter =
		D3D12_RESOURCE_STATE_RENDER_TARGET;
	if (colorReadable_) {
		dxCommon_->GetCommandList()->ResourceBarrier(1, &barrier);
		colorReadable_ = false;
	}

	if (createDepth_ && depthReadable_) {
		barrier.Transition.pResource = depthResource_.Get();
		barrier.Transition.StateBefore =
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barrier.Transition.StateAfter =
			D3D12_RESOURCE_STATE_DEPTH_WRITE;
		dxCommon_->GetCommandList()->ResourceBarrier(1, &barrier);
		depthReadable_ = false;
	}

	const D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
		rtvHeap_->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
	D3D12_CPU_DESCRIPTOR_HANDLE* dsvHandlePtr = nullptr;
	if (createDepth_) {
		dsvHandle = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
		dsvHandlePtr = &dsvHandle;
	}
	dxCommon_->GetCommandList()->OMSetRenderTargets(
		1,
		&rtvHandle,
		false,
		dsvHandlePtr
	);

	if (clearColor) {
		dxCommon_->GetCommandList()->ClearRenderTargetView(
			rtvHandle,
			clearColor_,
			0,
			nullptr
		);
	}
	if (createDepth_ && clearDepth) {
		dxCommon_->GetCommandList()->ClearDepthStencilView(
			dsvHandle,
			D3D12_CLEAR_FLAG_DEPTH,
			1.0f,
			0,
			0,
			nullptr
		);
	}

	D3D12_VIEWPORT viewport{};
	viewport.Width = static_cast<float>(width_);
	viewport.Height = static_cast<float>(height_);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	dxCommon_->GetCommandList()->RSSetViewports(1, &viewport);

	D3D12_RECT scissorRect{};
	scissorRect.right = static_cast<LONG>(width_);
	scissorRect.bottom = static_cast<LONG>(height_);
	dxCommon_->GetCommandList()->RSSetScissorRects(1, &scissorRect);
}

void SceneRenderTarget::End() {
	if (!initialized_) {
		return;
	}

	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = colorResource_.Get();
	barrier.Transition.Subresource =
		D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore =
		D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter =
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	dxCommon_->GetCommandList()->ResourceBarrier(1, &barrier);
	colorReadable_ = true;

	if (createDepth_) {
		barrier.Transition.pResource = depthResource_.Get();
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		barrier.Transition.StateAfter =
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		dxCommon_->GetCommandList()->ResourceBarrier(1, &barrier);
		depthReadable_ = true;
	}
}

D3D12_GPU_DESCRIPTOR_HANDLE SceneRenderTarget::GetSrvGpuHandle() const {
	if (!initialized_) {
		return {};
	}
	return srvManager_->GetGPUDescriptorHandle(srvIndex_);
}

D3D12_GPU_DESCRIPTOR_HANDLE
SceneRenderTarget::GetDepthSrvGpuHandle() const {
	if (!initialized_ || !createDepth_) {
		return {};
	}
	return srvManager_->GetGPUDescriptorHandle(depthSrvIndex_);
}

namespace {
DXGI_FORMAT ToResourceFormat(DXGI_FORMAT format) {
	if (format == RenderFormats::kDisplayFormat) {
		return RenderFormats::kDisplayResourceFormat;
	}
	return format;
}
}

bool SceneRenderTarget::CreateResources() {
	D3D12_HEAP_PROPERTIES heapProperties{};
	heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC colorDesc{};
	colorDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	colorDesc.Width = width_;
	colorDesc.Height = height_;
	colorDesc.DepthOrArraySize = 1;
	colorDesc.MipLevels = 1;
	colorDesc.Format = ToResourceFormat(format_);
	colorDesc.SampleDesc.Count = 1;
	colorDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	D3D12_CLEAR_VALUE colorClearValue{};
	colorClearValue.Format = format_;
	for (uint32_t index = 0; index < 4; ++index) {
		colorClearValue.Color[index] = clearColor_[index];
	}

	HRESULT result = dxCommon_->GetDevice()->CreateCommittedResource(
		&heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&colorDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		&colorClearValue,
		IID_PPV_ARGS(&colorResource_)
	);
	assert(SUCCEEDED(result));
	if (!colorResource_ || !rtvHeap_) {
		return false;
	}

	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
	rtvDesc.Format = format_;
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	dxCommon_->GetDevice()->CreateRenderTargetView(
		colorResource_.Get(),
		&rtvDesc,
		rtvHeap_->GetCPUDescriptorHandleForHeapStart()
	);

	if (!createDepth_) {
		return srvManager_->CreateSRVforTexture2D(
			srvIndex_,
			colorResource_.Get(),
			format_,
			1
		);
	}

	D3D12_RESOURCE_DESC depthDesc{};
	depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthDesc.Width = width_;
	depthDesc.Height = height_;
	depthDesc.DepthOrArraySize = 1;
	depthDesc.MipLevels = 1;
	depthDesc.Format = RenderFormats::kDepthResourceFormat;
	depthDesc.SampleDesc.Count = 1;
	depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE depthClearValue{};
	depthClearValue.Format = RenderFormats::kDepthDsvFormat;
	depthClearValue.DepthStencil.Depth = 1.0f;

	result = dxCommon_->GetDevice()->CreateCommittedResource(
		&heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&depthDesc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE,
		&depthClearValue,
		IID_PPV_ARGS(&depthResource_)
	);
	assert(SUCCEEDED(result));
	if (!depthResource_ || !dsvHeap_) {
		return false;
	}

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
	dsvDesc.Format = RenderFormats::kDepthDsvFormat;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dxCommon_->GetDevice()->CreateDepthStencilView(
		depthResource_.Get(),
		&dsvDesc,
		dsvHeap_->GetCPUDescriptorHandleForHeapStart()
	);

	const bool colorSrvCreated = srvManager_->CreateSRVforTexture2D(
		srvIndex_,
		colorResource_.Get(),
		format_,
		1
	);
	const bool depthSrvCreated = srvManager_->CreateSRVforTexture2D(
		depthSrvIndex_,
		depthResource_.Get(),
		RenderFormats::kDepthSrvFormat,
		1
	);
	return colorSrvCreated && depthSrvCreated;
}
