// 役割: テクスチャとHDR環境CubeMapの読み込み・生成・GPU登録を実装する。
#include "TextureManager.h"
#include "TextureFormat.h"
#include "../base/DirectXCommon.h"
#include "../3d/SrvManager.h"
#include "../utility/StringUtility.h"
#include "../utility/Logger.h"
#include "../utility/EditableResourcePath.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

namespace {

	DirectX::TGA_FLAGS GetTgaFlags(
		TextureManager::TextureColorSpace colorSpace,
		TextureFormat::DefaultColorSpace defaultColorSpace
	) {
		if (colorSpace == TextureManager::TextureColorSpace::Srgb) {
			return DirectX::TGA_FLAGS_FORCE_SRGB;
		}
		if (colorSpace == TextureManager::TextureColorSpace::Linear) {
			return DirectX::TGA_FLAGS_FORCE_LINEAR;
		}
		return defaultColorSpace == TextureFormat::DefaultColorSpace::Srgb
			? DirectX::TGA_FLAGS_DEFAULT_SRGB
			: DirectX::TGA_FLAGS_NONE;
	}

	DirectX::WIC_FLAGS GetWicFlags(
		TextureManager::TextureColorSpace colorSpace,
		TextureFormat::DefaultColorSpace defaultColorSpace
	) {
		if (colorSpace == TextureManager::TextureColorSpace::Srgb) {
			return DirectX::WIC_FLAGS_FORCE_SRGB;
		}
		if (colorSpace == TextureManager::TextureColorSpace::Linear) {
			return DirectX::WIC_FLAGS_FORCE_LINEAR;
		}
		return defaultColorSpace == TextureFormat::DefaultColorSpace::Srgb
			? DirectX::WIC_FLAGS_DEFAULT_SRGB
			: DirectX::WIC_FLAGS_NONE;
	}

	void ApplyColorSpace(
		DirectX::ScratchImage& image,
		DirectX::TexMetadata& metadata,
		TextureManager::TextureColorSpace colorSpace,
		TextureFormat::DefaultColorSpace defaultColorSpace
	) {
		const bool useSrgb = colorSpace == TextureManager::TextureColorSpace::Srgb ||
			(colorSpace == TextureManager::TextureColorSpace::Automatic &&
			 defaultColorSpace == TextureFormat::DefaultColorSpace::Srgb);
		const bool useLinear = colorSpace == TextureManager::TextureColorSpace::Linear ||
			(colorSpace == TextureManager::TextureColorSpace::Automatic &&
			 defaultColorSpace == TextureFormat::DefaultColorSpace::Linear);
		if (!useSrgb && !useLinear) {
			return;
		}

		const DXGI_FORMAT targetFormat = useSrgb
			? DirectX::MakeSRGB(metadata.format)
			: DirectX::MakeLinear(metadata.format);
		if (targetFormat != metadata.format && image.OverrideFormat(targetFormat)) {
			metadata = image.GetMetadata();
		}
	}

	std::string FormatHResult(HRESULT result) {
		std::ostringstream stream;
		stream << "0x" << std::uppercase << std::hex
			<< static_cast<unsigned long>(result);
		return stream.str();
	}

	struct EnvironmentDirection {
		double x, y, z;
	};

	struct EnvironmentFaceBasis {
		EnvironmentDirection forward, right, down;
	};

	// DirectX Cube面順。既存StarFieldGeneratorと同じbasisでShader側の回転を不要にする。
	constexpr std::array<EnvironmentFaceBasis, 6> kEnvironmentFaces = {{
		{{ 1, 0, 0 }, { 0, 0,-1 }, { 0,-1, 0 }},
		{{-1, 0, 0 }, { 0, 0, 1 }, { 0,-1, 0 }},
		{{ 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }},
		{{ 0,-1, 0 }, { 1, 0, 0 }, { 0, 0,-1 }},
		{{ 0, 0, 1 }, { 1, 0, 0 }, { 0,-1, 0 }},
		{{ 0, 0,-1 }, {-1, 0, 0 }, { 0,-1, 0 }}
	}};

	bool IsValidEnvironmentFloatImage(const DirectX::Image& image) {
		constexpr size_t pixelBytes = sizeof(float) * 4;
		const size_t maxSize = (std::numeric_limits<size_t>::max)();
		return image.pixels && image.format == DXGI_FORMAT_R32G32B32A32_FLOAT &&
			image.width > 0 && image.height > 0 && image.width <= maxSize / pixelBytes &&
			image.rowPitch >= image.width * pixelBytes &&
			image.height <= maxSize / image.rowPitch &&
			image.slicePitch >= image.height * image.rowPitch;
	}

	bool SampleEnvironmentPanorama(
		const DirectX::Image& image, double u, double v, std::array<float, 4>& output
	) {
		const double px = u * static_cast<double>(image.width) - 0.5;
		const double py = v * static_cast<double>(image.height) - 0.5;
		const int64_t x0 = static_cast<int64_t>(std::floor(px));
		const int64_t y0 = static_cast<int64_t>(std::floor(py));
		const int64_t width = static_cast<int64_t>(image.width);
		const int64_t height = static_cast<int64_t>(image.height);
		const double tx = px - static_cast<double>(x0);
		const double ty = py - static_cast<double>(y0);
		std::array<double, 3> rgb{};
		// 水平seamは負indexもwrapし、極はclamp。paddingを含むrowPitchで参照する。
		for (int row = 0; row < 2; ++row) {
			const size_t y = static_cast<size_t>(std::clamp(y0 + row, int64_t{0}, height - 1));
			for (int column = 0; column < 2; ++column) {
				const size_t x = static_cast<size_t>(((x0 + column) % width + width) % width);
				std::array<float, 4> pixel{};
				std::memcpy(pixel.data(), image.pixels + y * image.rowPitch + x * sizeof(pixel), sizeof(pixel));
				const double weight = (row == 0 ? 1.0 - ty : ty) * (column == 0 ? 1.0 - tx : tx);
				for (size_t channel = 0; channel < rgb.size(); ++channel) {
					if (!std::isfinite(pixel[channel])) {
						return false;
					}
					rgb[channel] += static_cast<double>(pixel[channel]) * weight;
				}
			}
		}
		for (size_t channel = 0; channel < rgb.size(); ++channel) {
			output[channel] = static_cast<float>(rgb[channel]);
			if (!std::isfinite(output[channel])) {
				return false;
			}
		}
		output[3] = 1.0f;
		return true;
	}

	HRESULT BuildEnvironmentCubemap(
		const DirectX::ScratchImage& panorama, DirectX::ScratchImage& mipChain
	) {
		const auto& metadata = panorama.GetMetadata();
		if (metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D ||
			metadata.arraySize != 1 || metadata.depth != 1 || metadata.mipLevels != 1 ||
			panorama.GetImageCount() != 1 || metadata.width < 4 || metadata.height < 2 ||
			metadata.height > (std::numeric_limits<size_t>::max)() / 2 ||
			metadata.width != metadata.height * 2) {
			return E_INVALIDARG;
		}
		const DirectX::Image* source = panorama.GetImage(0, 0, 0);
		if (!source) {
			return E_INVALIDARG;
		}
		DirectX::ScratchImage converted;
		if (source->format != DXGI_FORMAT_R32G32B32A32_FLOAT) {
			const HRESULT hr = DirectX::Convert(*source, DXGI_FORMAT_R32G32B32A32_FLOAT,
				DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
			if (FAILED(hr)) {
				return hr;
			}
			source = converted.GetImage(0, 0, 0);
		}
		if (!source || !IsValidEnvironmentFloatImage(*source)) {
			return E_INVALIDARG;
		}

		const size_t faceSize = (std::min)(metadata.width / 4, size_t{1024});
		DirectX::ScratchImage cube;
		const HRESULT initializeResult = cube.InitializeCube(
			DXGI_FORMAT_R32G32B32A32_FLOAT, faceSize, faceSize, 1, 1);
		if (FAILED(initializeResult)) {
			return initializeResult;
		}
		constexpr double pi = 3.14159265358979323846;
		for (size_t face = 0; face < kEnvironmentFaces.size(); ++face) {
			const DirectX::Image* target = cube.GetImage(0, face, 0);
			if (!target || !IsValidEnvironmentFloatImage(*target)) {
				return E_INVALIDARG;
			}
			const auto& basis = kEnvironmentFaces[face];
			for (size_t y = 0; y < faceSize; ++y) {
				const double b = 2.0 * (static_cast<double>(y) + 0.5) / static_cast<double>(faceSize) - 1.0;
				for (size_t x = 0; x < faceSize; ++x) {
					const double a = 2.0 * (static_cast<double>(x) + 0.5) / static_cast<double>(faceSize) - 1.0;
					EnvironmentDirection direction{
						basis.forward.x + a * basis.right.x + b * basis.down.x,
						basis.forward.y + a * basis.right.y + b * basis.down.y,
						basis.forward.z + a * basis.right.z + b * basis.down.z
					};
					const double length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
					// 元HDR中央を+X、上を+Yとするゼロ回転。-Xは水平seamになる。
					const double u = 0.5 + std::atan2(direction.z, direction.x) / (2.0 * pi);
					const double v = std::acos(std::clamp(direction.y / length, -1.0, 1.0)) / pi;
					std::array<float, 4> pixel{};
					if (!SampleEnvironmentPanorama(*source, u, v, pixel)) {
						return E_INVALIDARG;
					}
					std::memcpy(target->pixels + y * target->rowPitch + x * sizeof(pixel), pixel.data(), sizeof(pixel));
				}
			}
		}
		if (faceSize == 1) {
			// 1pixel面は基底だけで全ミップ。DirectXTexは追加levelなしの生成を拒否する。
			mipChain = std::move(cube);
			return S_OK;
		}
		// 通常のLinear縮小。PBR prefilterではなく、失敗時に単一ミップへ縮退しない。
		return DirectX::GenerateMipMaps(cube.GetImages(), cube.GetImageCount(),
			cube.GetMetadata(), DirectX::TEX_FILTER_DEFAULT, 0, mipChain);
	}

}

TextureManager* TextureManager::instance = nullptr;

uint32_t TextureManager::kSRVIndexTop = 1;

bool TextureManager::LoadTexture(
	const std::string& filePath,
	TextureColorSpace colorSpace
) {

	// 読み込み済みテクスチャを検索
	if (textureDatas.contains(filePath)) {
		return true;
	}
	if (failedTextureKeys.contains(filePath)) {
		return false;
	}

	DirectX::ScratchImage loadedImage{};
	DirectX::TexMetadata metadata{};
	const TextureFormat::Descriptor* format = TextureFormat::FindByPath(
		StringUtility::ToPath(filePath)
	);
	if (format == nullptr) {
		Logger::Log("Unsupported texture format: " + filePath + "\n");
		failedTextureKeys.insert(filePath);
		return false;
	}

	const std::filesystem::path resolvedPath =
		EditableResourcePath::ResolveResource(StringUtility::ToPath(filePath));
	const std::wstring filePathW = resolvedPath.wstring();

	HRESULT hr = S_OK;
	switch (format->decoder) {
	case TextureFormat::Decoder::Dds:
		hr = DirectX::LoadFromDDSFile(
			filePathW.c_str(),
			DirectX::DDS_FLAGS_NONE,
			&metadata,
			loadedImage
		);
		break;
	case TextureFormat::Decoder::Tga:
		hr = DirectX::LoadFromTGAFile(
			filePathW.c_str(),
			GetTgaFlags(colorSpace, format->colorSpace),
			&metadata,
			loadedImage
		);
		break;
	case TextureFormat::Decoder::Hdr:
		hr = DirectX::LoadFromHDRFile(
			filePathW.c_str(),
			&metadata,
			loadedImage
		);
		break;
	case TextureFormat::Decoder::Wic:
		hr = DirectX::LoadFromWICFile(
			filePathW.c_str(),
			GetWicFlags(colorSpace, format->colorSpace),
			&metadata,
			loadedImage
		);
		break;
	}

	if (FAILED(hr)) {
		Logger::Log(
			"Failed to load texture: " + filePath +
			" (HRESULT " + FormatHResult(hr) + ")\n"
		);
		failedTextureKeys.insert(filePath);
		return false;
	}

	ApplyColorSpace(loadedImage, metadata, colorSpace, format->colorSpace);
	const bool registered = RegisterTexture(filePath, loadedImage, metadata);
	if (registered) {
		failedTextureKeys.erase(filePath);
	}
	else {
		failedTextureKeys.insert(filePath);
	}
	return registered;
}

bool TextureManager::LoadEnvironmentTexture(
	const std::string& sourcePath,
	std::string& outTextureKey
) {
	if (sourcePath.empty() || &sourcePath == &outTextureKey) {
		return false;
	}
	// 解決失敗にも負のcacheを用意し、通常の2D keyを失敗扱いにしない。
	const std::string prefix = "runtime://environment-cubemap/v1/";
	std::string environmentKey = prefix + sourcePath;
	if (failedTextureKeys.contains(environmentKey)) {
		return false;
	}
	const auto fail = [&](const std::string& reason) {
		if (failedTextureKeys.insert(environmentKey).second) {
			Logger::Log("Failed to load environment: " + sourcePath + " (" + reason + ")\n");
		}
		return false;
	};
	try {
		const auto sourceFilePath = StringUtility::ToPath(sourcePath);
		const auto resolvedPath = std::filesystem::absolute(
			EditableResourcePath::ResolveResource(sourceFilePath)).lexically_normal();
		environmentKey = prefix + StringUtility::ToUtf8(resolvedPath);
		if (failedTextureKeys.contains(environmentKey)) {
			return false;
		}
		const auto* format = TextureFormat::FindByPath(sourceFilePath);
		if (!format || (format->decoder != TextureFormat::Decoder::Dds &&
			format->decoder != TextureFormat::Decoder::Hdr)) {
			return fail("expected a cubemap DDS or a 2:1 HDR panorama");
		}
		if (format->decoder == TextureFormat::Decoder::Dds) {
			if (!LoadTexture(sourcePath)) {
				// 通常decoder/登録側が原因を記録済み。Environment再試行だけ抑止する。
				failedTextureKeys.insert(environmentKey);
				return false;
			}
			if (!GetMetaData(sourcePath).IsCubemap()) {
				return fail("DDS is not a cubemap");
			}
			outTextureKey = sourcePath;
			return true;
		}
		if (const auto existing = textureDatas.find(environmentKey); existing != textureDatas.end()) {
			if (!existing->second.metadata.IsCubemap()) {
				return fail("runtime key is not a cubemap");
			}
			outTextureKey = environmentKey;
			return true;
		}
		DirectX::ScratchImage panorama;
		HRESULT hr = DirectX::LoadFromHDRFile(resolvedPath.c_str(), nullptr, panorama);
		if (FAILED(hr)) {
			return fail("HDR decode HRESULT " + FormatHResult(hr));
		}
		DirectX::ScratchImage cube;
		hr = BuildEnvironmentCubemap(panorama, cube);
		if (FAILED(hr)) {
			return fail("2:1 HDR cubemap conversion HRESULT " + FormatHResult(hr));
		}
		// 生成物は既存TextureManager寿命で共有し、Scene切替では解放しない。
		if (!RegisterTexture(environmentKey, cube, cube.GetMetadata())) {
			failedTextureKeys.insert(environmentKey);
			return false;
		}
		outTextureKey = environmentKey;
		return true;
	} catch (const std::exception& error) {
		return fail(error.what());
	}
}

bool TextureManager::ReloadTexture(
	const std::string& filePath,
	TextureColorSpace colorSpace
) {
	textureDatas.erase(filePath);
	failedTextureKeys.erase(filePath);
	return LoadTexture(filePath, colorSpace);
}

bool TextureManager::LoadTextureFromMemory(
	const std::string& textureKey,
	const uint8_t* data,
	size_t dataSize,
	const std::string& formatHint,
	TextureColorSpace colorSpace
) {
	if (textureDatas.contains(textureKey)) {
		return true;
	}
	if (failedTextureKeys.contains(textureKey)) {
		return false;
	}
	if (data == nullptr || dataSize == 0) {
		Logger::Log("Embedded texture data is empty: " + textureKey + "\n");
		failedTextureKeys.insert(textureKey);
		return false;
	}

	const TextureFormat::Descriptor* format =
		TextureFormat::FindByExtension(formatHint);
	const TextureFormat::Decoder decoder = format != nullptr
		? format->decoder
		: TextureFormat::Decoder::Wic;
	const TextureFormat::DefaultColorSpace defaultColorSpace = format != nullptr
		? format->colorSpace
		: TextureFormat::DefaultColorSpace::Srgb;

	DirectX::ScratchImage loadedImage{};
	DirectX::TexMetadata metadata{};
	HRESULT hr = S_OK;
	switch (decoder) {
	case TextureFormat::Decoder::Dds:
		hr = DirectX::LoadFromDDSMemory(
			data,
			dataSize,
			DirectX::DDS_FLAGS_NONE,
			&metadata,
			loadedImage
		);
		break;
	case TextureFormat::Decoder::Tga:
		hr = DirectX::LoadFromTGAMemory(
			data,
			dataSize,
			GetTgaFlags(colorSpace, defaultColorSpace),
			&metadata,
			loadedImage
		);
		break;
	case TextureFormat::Decoder::Hdr:
		hr = DirectX::LoadFromHDRMemory(
			data,
			dataSize,
			&metadata,
			loadedImage
		);
		break;
	case TextureFormat::Decoder::Wic:
		hr = DirectX::LoadFromWICMemory(
			data,
			dataSize,
			GetWicFlags(colorSpace, defaultColorSpace),
			&metadata,
			loadedImage
		);
		break;
	}

	if (FAILED(hr)) {
		Logger::Log(
			"Failed to load embedded texture: " + textureKey +
			" (HRESULT " + FormatHResult(hr) + ")\n"
		);
		failedTextureKeys.insert(textureKey);
		return false;
	}

	ApplyColorSpace(loadedImage, metadata, colorSpace, defaultColorSpace);
	const bool registered = RegisterTexture(textureKey, loadedImage, metadata);
	if (registered) {
		failedTextureKeys.erase(textureKey);
	}
	else {
		failedTextureKeys.insert(textureKey);
	}
	return registered;
}

bool TextureManager::RegisterTexture(
	const std::string& textureKey,
	const DirectX::ScratchImage& loadedImage,
	const DirectX::TexMetadata& metadata
) {
	if (loadedImage.GetImageCount() == 0 || loadedImage.GetImages() == nullptr) {
		Logger::Log("Texture contains no images: " + textureKey + "\n");
		return false;
	}

	auto existing = textureDatas.find(textureKey);
	const bool reusesExistingDescriptor = existing != textureDatas.end();
	if (reusesExistingDescriptor && !srvManager->IsAllocated(existing->second.srvIndex)) {
		Logger::Log("Texture has an invalid SRV slot: " + textureKey + "\n");
		return false;
	}

	DirectX::ScratchImage mipImages{};
	const DirectX::ScratchImage* uploadImage = &loadedImage;
	HRESULT hr = S_OK;

	// 圧縮フォーマットは DirectXTex の GenerateMipMaps が直接扱えないことがある
	if (!DirectX::IsCompressed(metadata.format) && metadata.mipLevels <= 1) {
		const DirectX::TEX_FILTER_FLAGS filter = DirectX::IsSRGB(metadata.format)
			? DirectX::TEX_FILTER_SRGB
			: DirectX::TEX_FILTER_DEFAULT;
		hr = DirectX::GenerateMipMaps(
			loadedImage.GetImages(),
			loadedImage.GetImageCount(),
			loadedImage.GetMetadata(),
			filter,
			0,
			mipImages
		);
		if (SUCCEEDED(hr)) {
			uploadImage = &mipImages;
		}
	}

	TextureData candidate{};
	candidate.metadata = uploadImage->GetMetadata();
	candidate.resource = dxCommon->CreateTextureResource(candidate.metadata);

	// 既存keyの更新はDescriptorを再利用し、Runtime編集でSRVを増やさない。
	if (reusesExistingDescriptor) {
		candidate.srvIndex = existing->second.srvIndex;
		candidate.srvHandleCPU = existing->second.srvHandleCPU;
		candidate.srvHandleGPU = existing->second.srvHandleGPU;
	} else {
		if (!srvManager->TryAllocate(candidate.srvIndex)) {
			Logger::Log("No SRV slot available for texture: " + textureKey + "\n");
			return false;
		}
		candidate.srvHandleCPU = srvManager->GetCPUDescriptorHandle(candidate.srvIndex);
		candidate.srvHandleGPU = srvManager->GetGPUDescriptorHandle(candidate.srvIndex);
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = candidate.metadata.format;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	if (candidate.metadata.IsCubemap()) {
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
		srvDesc.TextureCube.MostDetailedMip = 0;
		srvDesc.TextureCube.MipLevels = UINT(candidate.metadata.mipLevels);
		srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
	}
	else {
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = UINT(candidate.metadata.mipLevels);
	}

	Microsoft::WRL::ComPtr<ID3D12Resource> intermediateResource;
	intermediateResource.Attach(
		dxCommon->UploadTextureData(candidate.resource, *uploadImage)
	);
	if (!intermediateResource) {
		if (!reusesExistingDescriptor && !srvManager->Free(candidate.srvIndex)) {
			Logger::Log("Failed to return SRV slot for texture: " + textureKey + "\n");
		}
		Logger::Log("Failed to upload texture: " + textureKey + "\n");
		return false;
	}

	dxCommon->ExecuteCommandListAndWait();
	dxCommon->GetDevice()->CreateShaderResourceView(
		candidate.resource.Get(),
		&srvDesc,
		candidate.srvHandleCPU
	);
	if (reusesExistingDescriptor) {
		existing->second = std::move(candidate);
	} else {
		textureDatas.emplace(textureKey, std::move(candidate));
	}
	return true;
}

bool TextureManager::UpdateTextureFromPixels(
	const std::string& textureKey,
	const uint8_t* pixels,
	uint32_t width,
	uint32_t height,
	DXGI_FORMAT format
) {
	if (textureKey.empty() || pixels == nullptr || width == 0 || height == 0) {
		return false;
	}

	DirectX::ScratchImage image{};
	const HRESULT initializeResult = image.Initialize2D(
		format,
		width,
		height,
		1,
		1
	);
	if (FAILED(initializeResult) || image.GetPixels() == nullptr) {
		Logger::Log("Failed to create runtime texture: " + textureKey + "\n");
		return false;
	}

	const size_t rowBytes = static_cast<size_t>(width) * 4;
	const DirectX::Image* targetImage = image.GetImage(0, 0, 0);
	if (!targetImage || targetImage->rowPitch < rowBytes) {
		return false;
	}
	for (uint32_t row = 0; row < height; ++row) {
		std::memcpy(
			image.GetPixels() + targetImage->rowPitch * row,
			pixels + rowBytes * row,
			rowBytes
		);
	}

	const bool registered = RegisterTexture(
		textureKey,
		image,
		image.GetMetadata()
	);
	if (registered) {
		failedTextureKeys.erase(textureKey);
	}
	return registered;
}

bool TextureManager::HasTexture(const std::string& textureKey) const {
	return textureDatas.contains(textureKey);
}

bool TextureManager::ReleaseTexture(const std::string& textureKey) {
	const auto found = textureDatas.find(textureKey);
	if (found == textureDatas.end() || !srvManager->Free(found->second.srvIndex)) {
		return false;
	}
	textureDatas.erase(found);
	failedTextureKeys.erase(textureKey);
	return true;
}

void TextureManager::ClearFailedTextureCache() {
	failedTextureKeys.clear();
}

const DirectX::TexMetadata& TextureManager::GetMetaData(const std::string& filePath){
	auto it = textureDatas.find(filePath);
	assert(it != textureDatas.end());
	return it->second.metadata;
}

uint32_t TextureManager::GetSrvIndex(const std::string& filePath){
	auto it = textureDatas.find(filePath);
	assert(it != textureDatas.end());
	return it->second.srvIndex;
}

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::GetSrvHandleGPU(const std::string& filePath){
	auto it = textureDatas.find(filePath);
	assert(it != textureDatas.end());
	return it->second.srvHandleGPU;
}

void TextureManager::Initialize(DirectXCommon* directXCommon, SrvManager* srvManager){
	this->srvManager = srvManager;
	this->dxCommon = directXCommon;

}

TextureManager* TextureManager::GetInstance(){
	if(instance == nullptr){
		instance = new TextureManager;
	}
	return instance;
}

void TextureManager::Finalize(){
	delete instance;
	instance = nullptr;
}
