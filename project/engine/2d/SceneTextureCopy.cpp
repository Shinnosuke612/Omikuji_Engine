// 役割: mip・配列面の行帯を単一stagingへ詰め、Fence完了ごとに次のバッチを送る。
#include "SceneTextureCopy.h"
#include "../base/SceneCopyBatch.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace {
bool Reject(std::string& error, const char* reason) {
	error = reason;
	return false;
}

bool CreateResource(ID3D12Device& device, D3D12_HEAP_TYPE heapType,
	const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initialState,
	Microsoft::WRL::ComPtr<ID3D12Resource>& resource, std::string& error) {
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = heapType;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;
	const HRESULT hr = device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
		&desc, initialState, nullptr, IID_PPV_ARGS(resource.GetAddressOf()));
	if (SUCCEEDED(hr)) return true;
	error = "Texture copy resource creation failed (HRESULT " + std::to_string(hr) + ")";
	return false;
}
}

bool SceneTextureCopy::Begin(ID3D12Device& device, SceneCopyBatch& batch,
	TextureManager::DecodedTexture&& image, uint64_t maxStagingBytes, std::string& error, SceneTextureMipPolicy mipPolicy) {
	if (status_ != Status::Idle) return Reject(error, "Texture copy has already started");
	const bool gpuMips = mipPolicy == SceneTextureMipPolicy::GpuSrgbBox4096;
	if (!gpuMips && mipPolicy != SceneTextureMipPolicy::CpuComplete) return Reject(error,"Unknown mip policy");

	const auto& metadata = image.metadata;
	const auto& actual = image.image.GetMetadata();
	if (gpuMips && (metadata.width != kSceneGpuMipSize || metadata.height != kSceneGpuMipSize ||
		metadata.arraySize != 1 || metadata.mipLevels != 1 || metadata.IsCubemap() ||
		metadata.format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) return Reject(error,"Invalid GPU mip0 source");
	if (!maxStagingBytes || metadata.width == 0 || metadata.height == 0 ||
		metadata.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
		metadata.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
		metadata.arraySize == 0 || metadata.arraySize > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION ||
		metadata.mipLevels == 0 || metadata.mipLevels > D3D12_REQ_MIP_LEVELS ||
		metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || metadata.depth != 1 ||
		metadata.width != actual.width || metadata.height != actual.height ||
		metadata.depth != actual.depth || metadata.dimension != actual.dimension ||
		metadata.arraySize != actual.arraySize || metadata.mipLevels != actual.mipLevels ||
		metadata.format != actual.format || metadata.IsCubemap() != actual.IsCubemap() ||
		(metadata.IsCubemap() && (metadata.width != metadata.height || metadata.arraySize % 6 != 0))) {
		return Reject(error, "Invalid Texture2D metadata or staging budget");
	}
	const size_t imageCount = metadata.arraySize * metadata.mipLevels;
	if (imageCount != image.image.GetImageCount()) {
		return Reject(error, "Texture2D subresource count does not match its images");
	}

	const auto format = metadata.format;
	D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
	D3D12_FEATURE_DATA_FORMAT_INFO info{format};
	if (DirectX::IsTypeless(format) || DirectX::IsDepthStencil(format) ||
		DirectX::IsPlanar(format) || DirectX::IsPacked(format) || DirectX::IsVideo(format) ||
		FAILED(device.CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
		!(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
		FAILED(device.CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))) ||
		info.PlaneCount != 1) {
		return Reject(error, "Format is unsupported for partial Texture2D copies");
	}

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = metadata.width;
	desc.Height = static_cast<UINT>(metadata.height);
	desc.DepthOrArraySize = static_cast<UINT16>(metadata.arraySize);
	desc.MipLevels = static_cast<UINT16>(metadata.mipLevels);
	desc.Format = format;
	if (gpuMips) {
		desc.MipLevels = kSceneGpuMipCount;
		desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	}
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	auto copyDesc = desc;
	if (gpuMips) copyDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

	std::vector<Subresource> layouts;
	layouts.reserve(imageCount);
	uint64_t largestPaddedBytes = 0;
	const UINT texelsPerRow = DirectX::IsCompressed(format) ? 4 : 1;
	for (size_t slice = 0; slice < metadata.arraySize; ++slice) {
		for (size_t mip = 0; mip < metadata.mipLevels; ++mip) {
			const auto* source = image.image.GetImage(mip, slice, 0);
			const UINT width = static_cast<UINT>((std::max)(size_t{1}, metadata.width >> mip));
			const UINT height = static_cast<UINT>((std::max)(size_t{1}, metadata.height >> mip));
			const UINT rows = (height + texelsPerRow - 1) / texelsPerRow;
			const UINT paddedWidth = (width + texelsPerRow - 1) / texelsPerRow * texelsPerRow;
			const UINT paddedHeight = rows * texelsPerRow;
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed{};
			UINT rowCount = 0;
			UINT64 rowBytes = 0;
			UINT64 totalBytes = 0;
			const UINT index = static_cast<UINT>(mip + slice * metadata.mipLevels);
			device.GetCopyableFootprints(&copyDesc, index, 1, 0, &placed, &rowCount, &rowBytes, &totalBytes);
			const auto& footprint = placed.Footprint;
			if (!source || !source->pixels || source->width != width || source->height != height ||
				source->format != format || totalBytes == (std::numeric_limits<UINT64>::max)() ||
				placed.Offset != 0 || footprint.Format != copyDesc.Format || footprint.Depth != 1 ||
				(footprint.Width != width && footprint.Width != paddedWidth) ||
				(footprint.Height != height && footprint.Height != paddedHeight) ||
				rowCount != rows || rowBytes == 0 || rowBytes > footprint.RowPitch ||
				footprint.RowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0 ||
				footprint.RowPitch > maxStagingBytes || rowBytes > source->rowPitch ||
				rowBytes > source->slicePitch ||
				// 除算で検証してから行オフセットを乗算する。
				(rows - 1) > (source->slicePitch - rowBytes) / source->rowPitch) {
				return Reject(error, "Invalid subresource footprint or staging budget below one row");
			}
			// row pitchはUINT、rowsはTexture2D上限内。積は64bitに収まる。
			const uint64_t paddedBytes = uint64_t{footprint.RowPitch} * rows;
			largestPaddedBytes = (std::max)(largestPaddedBytes, paddedBytes);
			layouts.push_back({footprint, width, height, rows, static_cast<size_t>(rowBytes)});
		}
	}
	const uint64_t stagingBytes = (std::min)(maxStagingBytes, largestPaddedBytes);
	if (stagingBytes > (std::numeric_limits<size_t>::max)()) {
		return Reject(error, "Staging size is too large for CPU addressing");
	}

	Microsoft::WRL::ComPtr<ID3D12Resource> texture;
	Microsoft::WRL::ComPtr<ID3D12Resource> staging;
	if (!CreateResource(device, D3D12_HEAP_TYPE_DEFAULT, desc,
		D3D12_RESOURCE_STATE_COMMON, texture, error)) return false;
	D3D12_RESOURCE_DESC buffer{};
	buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer.Width = stagingBytes;
	buffer.Height = 1;
	buffer.DepthOrArraySize = 1;
	buffer.MipLevels = 1;
	buffer.SampleDesc.Count = 1;
	buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (!CreateResource(device, D3D12_HEAP_TYPE_UPLOAD, buffer,
		D3D12_RESOURCE_STATE_GENERIC_READ, staging, error)) return false;

	batch_ = &batch;
	metadata_ = metadata;
	sourceMipLevels_ = metadata.mipLevels;
	if (gpuMips) metadata_.mipLevels = kSceneGpuMipCount;
	image_ = std::move(image.image);
	subresources_ = std::move(layouts);
	texture_ = std::move(texture);
	staging_ = std::move(staging);
	stagingBytes_ = stagingBytes;
	texelsPerRow_ = texelsPerRow;
	status_ = Status::Pending;
	error.clear();
	return true;
}

SceneTextureCopy::Status SceneTextureCopy::Pump(std::string& error) {
	if (status_ != Status::Pending) return status_;
	error.clear();

	if (copyInFlight_) {
		const auto result = batch_->Poll();
		if (result == SceneCopyBatch::Status::Pending) return status_;
		if (result != SceneCopyBatch::Status::Completed) return Fail(error, "Texture COPY batch failed");
		subresource_ = submittedSubresource_;
		firstRow_ = submittedFirstRow_;
		copyInFlight_ = false;
	}
	if (cancelRequested_) {
		Cancel();
		return status_;
	}
	if (subresource_ == subresources_.size()) {
		ReleaseCopyInputs();
		status_ = Status::Copied;
		return status_;
	}

	struct Band {
		UINT subresource, firstRow, rowCount;
		uint64_t offset;
	};
	std::vector<Band> bands;
	bands.reserve(subresources_.size() - subresource_);
	UINT nextSubresource = subresource_, nextRow = firstRow_;
	uint64_t usedBytes = 0;
	// 計画中は確定cursorを動かさない。最後の行のpaddingも含めて容量へ収める。
	while (nextSubresource < subresources_.size()) {
		const auto& layout = subresources_[nextSubresource];
		if (nextRow >= layout.rowCount) return Fail(error, "Invalid texture copy row cursor");
		const uint64_t alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
		const uint64_t padding = (alignment - usedBytes % alignment) % alignment;
		// 加算前の減算で配置alignmentとoffsetのoverflow/バッファ超過を避ける。
		if (padding > stagingBytes_ - usedBytes) break;
		const uint64_t offset = usedBytes + padding;
		const UINT rowPitch = layout.footprint.RowPitch;
		const UINT rowCount = static_cast<UINT>((std::min)(
			uint64_t{layout.rowCount - nextRow}, (stagingBytes_ - offset) / rowPitch));
		if (!rowCount) break;
		bands.push_back({nextSubresource, nextRow, rowCount, offset});
		// rowCountは残容量/rowPitch以下なので、積と終点もstagingBytes_以内。
		usedBytes = offset + uint64_t{rowCount} * rowPitch;
		nextRow += rowCount;
		if (nextRow == layout.rowCount) {
			++nextSubresource;
			nextRow = 0;
		}
	}
	if (bands.empty()) return Fail(error, "Texture copy staging cannot fit one padded row");

	void* mapped = nullptr;
	const D3D12_RANGE readRange{0, 0};
	if (FAILED(staging_->Map(0, &readRange, &mapped))) {
		return Fail(error, "Texture copy staging Map failed");
	}
	if (!mapped) {
		staging_->Unmap(0, &readRange);
		return Fail(error, "Texture copy staging Map returned no address");
	}
	auto* bytes = static_cast<uint8_t*>(mapped);
	for (const auto& band : bands) {
		const auto& layout = subresources_[band.subresource];
		const auto* source = image_.GetImage(band.subresource % sourceMipLevels_,
			band.subresource / sourceMipLevels_, 0);
		for (UINT row = 0; row < band.rowCount; ++row) {
			std::memcpy(bytes + static_cast<size_t>(band.offset) + size_t{row} * layout.footprint.RowPitch,
				source->pixels + size_t{band.firstRow + row} * source->rowPitch, layout.rowBytes);
		}
	}
	// 書込hintは配置間gap/行paddingを含む外接範囲。memcpyは各行のpayloadだけを扱う。
	const D3D12_RANGE written{0, static_cast<size_t>(usedBytes)};
	staging_->Unmap(0, &written);

	// 参照の確保を記録前に済ませ、SubmitからFence完了まではbatchが保持する。
	std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> resources{staging_, texture_};
	if (!batch_->TryBegin(error)) return Fail(error, "Texture COPY recording failed");
	auto* list = batch_->GetRecordingList();
	for (const auto& band : bands) {
		const auto& layout = subresources_[band.subresource];
		const UINT destY = band.firstRow * texelsPerRow_;
		const UINT height = (std::min)(layout.height - destY, band.rowCount * texelsPerRow_);
		D3D12_TEXTURE_COPY_LOCATION src{};
		src.pResource = staging_.Get();
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint.Offset = band.offset;
		src.PlacedFootprint.Footprint = layout.footprint;
		src.PlacedFootprint.Footprint.Width = layout.width;
		src.PlacedFootprint.Footprint.Height = height;
		D3D12_TEXTURE_COPY_LOCATION dst{};
		dst.pResource = texture_.Get();
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.SubresourceIndex = band.subresource;
		// BCの途中はblock境界、末尾だけ論理mip端。次の帯も512整列offsetから読む。
		const D3D12_BOX box{0, 0, 0, layout.width, height, 1};
		list->CopyTextureRegion(&dst, 0, destY, 0, &src, &box);
	}
	if (!batch_->Submit(std::move(resources), error)) return Fail(error, "Texture COPY submission failed");
	submittedSubresource_ = nextSubresource;
	submittedFirstRow_ = nextRow;
	copyInFlight_ = true;
	return status_;
}

void SceneTextureCopy::Cancel() noexcept {
	if (status_ != Status::Pending && status_ != Status::Copied) return;
	cancelRequested_ = true;
	if (copyInFlight_) return;
	ReleaseCopyInputs();
	texture_.Reset();
	status_ = Status::Cancelled;
}

bool SceneTextureCopy::TryTakeCopiedTexture(
	Microsoft::WRL::ComPtr<ID3D12Resource>& outResource, DirectX::TexMetadata& outMetadata) noexcept {
	if (status_ != Status::Copied || !texture_) return false;
	outResource = std::move(texture_);
	outMetadata = metadata_;
	return true;
}

void SceneTextureCopy::ReleaseCopyInputs() noexcept {
	image_.Release();
	staging_.Reset();
	subresources_.clear();
}

SceneTextureCopy::Status SceneTextureCopy::Fail(std::string& error, const char* reason) {
	status_ = Status::Failed;
	// Signal失敗やdevice除去時は、提出済みのGPU参照をbatch側が保持している。
	ReleaseCopyInputs();
	texture_.Reset();
	if (error.empty()) error = reason;
	return status_;
}
