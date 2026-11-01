// 役割: SkinClusterのGPU Buffer生成とJointPalette更新を実装する。
#include "SkinCluster.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <numeric>

#include "Model.h"
#include "Skeleton.h"
#include "SrvManager.h"
#include "../base/DirectXCommon.h"

namespace {

Microsoft::WRL::ComPtr<ID3D12Resource> CreateUavBufferResource(
	ID3D12Device* device,
	size_t sizeInBytes, bool recoverable
) {
	assert(device);

	D3D12_HEAP_PROPERTIES heapProperties{};
	heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resourceDesc.Width = sizeInBytes;
	resourceDesc.Height = 1;
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.MipLevels = 1;
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	Microsoft::WRL::ComPtr<ID3D12Resource> resource;
	const HRESULT hr = device->CreateCommittedResource(
		&heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(&resource)
	);
	if (!recoverable) assert(SUCCEEDED(hr));
	return resource;
}

} // namespace

SkinCluster::~SkinCluster() {
	ReleaseDescriptors();
}

void SkinCluster::ReleaseDescriptors() {
	if (!srvManager_) {
		return;
	}

	auto release = [this](uint32_t& index) {
		if (index != UINT32_MAX) {
			srvManager_->Free(index);
			index = UINT32_MAX;
		}
	};
	release(paletteSrvIndex_);
	release(inputVertexSrvIndex_);
	release(influenceSrvIndex_);
	release(outputVertexUavIndex_);
	srvManager_ = nullptr;
}

void SkinCluster::Initialize(DirectXCommon* dx, SrvManager* srv, const Skeleton& skeleton, const Model& model) {
    (void)InitializeInternal(dx, srv, skeleton, model, false);
}
bool SkinCluster::TryInitializeReplay(DirectXCommon* dx, SrvManager* srv, const Skeleton& skeleton, const Model& model) {
    return InitializeInternal(dx, srv, skeleton, model, true);
}
bool SkinCluster::InitializeInternal(DirectXCommon* dxCommon, SrvManager* srvManager,
    const Skeleton& skeleton, const Model& model, bool recoverable) {
	if (!dxCommon || !srvManager || !skeleton.IsValid()) return false;
	assert(dxCommon);
	assert(srvManager);
	assert(skeleton.IsValid());
	assert(!srvManager_);
	if (srvManager_) {
		return false;
	}
	srvManager_ = srvManager;
    auto buffer = [&](size_t bytes) {
        Microsoft::WRL::ComPtr<ID3D12Resource> result;
        if (recoverable) dxCommon->TryCreateBufferResource(bytes, result);
        else result = dxCommon->CreateBufferResource(bytes);
        return result;
    };

	auto allocateDescriptor = [this, srvManager](uint32_t& index) {
		if (!srvManager->TryAllocate(index)) {
			ReleaseDescriptors();
			return false;
		}
		return true;
	};

    if (!dxCommon->GetDevice() || !model.GetVertexResource() || skeleton.joints.size() > UINT32_MAX / sizeof(PaletteWell) ||
        model.GetVertexCount() == 0 || model.GetVertexCount() > UINT32_MAX / sizeof(Model::VertexData) ||
        model.GetVertexCount() > UINT32_MAX / sizeof(VertexInfluence)) return false;
	jointCount_ = static_cast<uint32_t>(skeleton.joints.size());
	const uint32_t vertexCount = model.GetVertexCount();

	paletteResource_ = buffer(
		sizeof(PaletteWell) * jointCount_
	);
	if (!paletteResource_ || FAILED(paletteResource_->Map(
		0,
		nullptr,
		reinterpret_cast<void**>(&mappedPalette_)
	)) || !mappedPalette_) return false;
	if (!allocateDescriptor(paletteSrvIndex_)) {
		return false;
	}
	if (!srvManager->CreateSRVforStructuredBuffer(
		paletteSrvIndex_,
		paletteResource_.Get(),
		jointCount_,
		sizeof(PaletteWell)
	)) {
		ReleaseDescriptors();
		return false;
	}

	if (!allocateDescriptor(inputVertexSrvIndex_)) {
		return false;
	}
	if (!srvManager->CreateSRVforStructuredBuffer(
		inputVertexSrvIndex_,
		model.GetVertexResource(),
		vertexCount,
		sizeof(Model::VertexData)
	)) {
		ReleaseDescriptors();
		return false;
	}

	influenceResource_ = buffer(
		sizeof(VertexInfluence) * vertexCount
	);
	if (!influenceResource_ || FAILED(influenceResource_->Map(
		0,
		nullptr,
		reinterpret_cast<void**>(&mappedInfluences_)
	)) || !mappedInfluences_) return false;
	std::memset(
		mappedInfluences_,
		0,
		sizeof(VertexInfluence) * vertexCount
	);
	influenceBufferView_.BufferLocation =
		influenceResource_->GetGPUVirtualAddress();
	influenceBufferView_.SizeInBytes = static_cast<UINT>(
		sizeof(VertexInfluence) * vertexCount
	);
	influenceBufferView_.StrideInBytes = sizeof(VertexInfluence);

	if (!allocateDescriptor(influenceSrvIndex_)) {
		return false;
	}
	if (!srvManager->CreateSRVforStructuredBuffer(
		influenceSrvIndex_,
		influenceResource_.Get(),
		vertexCount,
		sizeof(VertexInfluence)
	)) {
		ReleaseDescriptors();
		return false;
	}

	outputVertexResource_ = CreateUavBufferResource(
		dxCommon->GetDevice(),
		sizeof(Model::VertexData) * vertexCount, recoverable
	);
	if (!outputVertexResource_) return false;
	skinnedVertexBufferView_.BufferLocation =
		outputVertexResource_->GetGPUVirtualAddress();
	skinnedVertexBufferView_.SizeInBytes = static_cast<UINT>(
		sizeof(Model::VertexData) * vertexCount
	);
	skinnedVertexBufferView_.StrideInBytes = sizeof(Model::VertexData);

	if (!allocateDescriptor(outputVertexUavIndex_)) {
		return false;
	}
	if (!srvManager->CreateUAVforStructuredBuffer(
		outputVertexUavIndex_,
		outputVertexResource_.Get(),
		vertexCount,
		sizeof(Model::VertexData)
	)) {
		ReleaseDescriptors();
		return false;
	}

	skinningInformationResource_ = buffer(
		sizeof(SkinningInformation)
	);
	if (!skinningInformationResource_ || FAILED(skinningInformationResource_->Map(
		0,
		nullptr,
		reinterpret_cast<void**>(&mappedSkinningInformation_)
	)) || !mappedSkinningInformation_) return false;
	mappedSkinningInformation_->numVertices = vertexCount;

    BuildInfluences(skeleton, model, {mappedInfluences_, vertexCount}, inverseBindPoseMatrices_, true);

	Update(skeleton);
	return true;
}

void SkinCluster::Update(const Skeleton& skeleton) {
	if (!IsValid()) {
		return;
	}

	const uint32_t updateCount = (std::min)(
		jointCount_,
		static_cast<uint32_t>(skeleton.joints.size())
	);
	for (uint32_t jointIndex = 0;
		jointIndex < updateCount;
		++jointIndex) {
		const Matrix4x4 skinningMatrix = Multiply(
			inverseBindPoseMatrices_[jointIndex],
			skeleton.joints[jointIndex].skeletonSpaceMatrix
		);
		mappedPalette_[jointIndex].skeletonSpaceMatrix =
			skinningMatrix;
		mappedPalette_[jointIndex]
			.skeletonSpaceInverseTransposeMatrix =
			Transpose(Inverse(skinningMatrix));
	}

}

void SkinCluster::TransitionOutputResource(
	ID3D12GraphicsCommandList* commandList,
	D3D12_RESOURCE_STATES stateAfter
) {
	if (!outputVertexResource_ || outputVertexResourceState_ == stateAfter) {
		return;
	}

	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = outputVertexResource_.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = outputVertexResourceState_;
	barrier.Transition.StateAfter = stateAfter;
	commandList->ResourceBarrier(1, &barrier);
	outputVertexResourceState_ = stateAfter;
}

void SkinCluster::AddInfluence(
	VertexInfluence& influence,
	float weight,
	uint32_t jointIndex
) {
	for (uint32_t influenceIndex = 0;
		influenceIndex < kMaxInfluence;
		++influenceIndex) {
		if (influence.weights[influenceIndex] == 0.0f) {
			influence.weights[influenceIndex] = weight;
			influence.jointIndices[influenceIndex] = jointIndex;
			return;
		}
	}

	const auto smallestIt = std::min_element(
		influence.weights.begin(),
		influence.weights.end()
	);
	if (smallestIt == influence.weights.end() || weight <= *smallestIt) {
		return;
	}

	const size_t influenceIndex = static_cast<size_t>(
		std::distance(influence.weights.begin(), smallestIt)
	);
	influence.weights[influenceIndex] = weight;
	influence.jointIndices[influenceIndex] = jointIndex;
}

void SkinCluster::BuildInfluences(const Skeleton& skeleton, const Model& model,
    std::span<VertexInfluence> influences, std::vector<Matrix4x4>& inverseBind, bool checkBindPose) {
    const uint32_t vertexCount = model.GetVertexCount();
    assert(influences.size() == vertexCount);
    std::fill(influences.begin(), influences.end(), VertexInfluence{});
    (void)checkBindPose;
	inverseBind.assign(
		skeleton.joints.size(),
		MakeIdentity4x4()
	);

	for (const auto& [jointName, jointWeight] :
		model.GetSkinClusterData()) {
		const auto jointIt = skeleton.jointMap.find(jointName);
		if (jointIt == skeleton.jointMap.end()) {
			continue;
		}

		const uint32_t jointIndex =
			static_cast<uint32_t>(jointIt->second);

		inverseBind[jointIndex] =
			jointWeight.inverseBindPoseMatrix;


#if defined(_DEBUG) || defined(DEVELOPMENT)
		if (checkBindPose) {
			const Matrix4x4 bindCheck = Multiply(
				inverseBind[jointIndex],
				skeleton.joints[jointIndex].skeletonSpaceMatrix
			);

			float maxError = 0.0f;
			for (uint32_t row = 0; row < 4; ++row) {
				for (uint32_t column = 0; column < 4; ++column) {
					const float expected =
						row == column ? 1.0f : 0.0f;
					maxError = (std::max)(
						maxError,
						std::abs(bindCheck.m[row][column] - expected)
					);
				}
			}

			if (maxError > 0.001f) {
				OutputDebugStringA(
					("[SkinCluster] Bind pose mismatch: " +
						jointName +
						" error=" +
						std::to_string(maxError) +
						"\n").c_str()
				);
			}
		}
#endif
		for (const Model::VertexWeightData& vertexWeight :
			jointWeight.vertexWeights) {
			if (vertexWeight.vertexIndex >= vertexCount) {
				continue;
			}
			AddInfluence(
				influences[vertexWeight.vertexIndex],
				vertexWeight.weight,
				jointIndex
			);
		}
	}

	for (uint32_t vertexIndex = 0;
		vertexIndex < vertexCount;
		++vertexIndex) {
		VertexInfluence& influence = influences[vertexIndex];
		const float weightSum = std::accumulate(
			influence.weights.begin(),
			influence.weights.end(),
			0.0f
		);
		if (weightSum > 0.0f) {
			for (float& weight : influence.weights) {
				weight /= weightSum;
			}
		}
	}

}
