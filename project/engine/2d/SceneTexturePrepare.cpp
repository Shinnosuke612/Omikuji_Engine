// 役割: Scene画像の通常入力・灰色base color・HDR環境を既存CPU処理で準備する。
#include "SceneTexturePrepare.h"
#include <chrono>
#include <algorithm>
#include <cstring>
#include <type_traits>
#include <utility>

namespace {
// 例外でも開始済みの区間を残す。ログ出力や共有cacheには触れない。
struct CpuTimingSpan {
	int64_t& elapsed;
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point& end;
	CpuTimingSpan(int64_t& value, std::chrono::steady_clock::time_point& begin,
		std::chrono::steady_clock::time_point& finish) : elapsed(value), end(finish) { begin = start; }
	~CpuTimingSpan() {
		end = std::chrono::steady_clock::now();
		elapsed += std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
	}
};
SceneTextureCpuImageSize ImageSize(const TextureManager::DecodedTexture& texture) noexcept {
	const auto& m = texture.metadata;
	return {true, texture.image.GetPixelsSize(), m.width, m.height, m.depth, m.arraySize, m.mipLevels};
}
}

SceneTextureCpuStatus TryPrepareSceneTexture(
	const SceneTextureCpuSource& source, SceneTextureCpuResult& out, std::string& diagnostic,
	SceneTextureCpuTiming* timing, const SceneTextureCpuLimits* limits) {
	SceneTextureCpuTiming ignored;
	auto& measured = timing ? *timing : ignored;
	measured = {};
	const auto cancelled = [&] { return limits && limits->cancelled && limits->cancelled->load(std::memory_order_relaxed); };
	if (cancelled()) return SceneTextureCpuStatus::Cancelled;
	using Recipe = SceneTextureCpuSource::Recipe;
	using ColorSpace = TextureManager::TextureColorSpace;
	// 最後の置換が例外で中断されないことを型側の変更時にも保証する。
	static_assert(std::is_nothrow_move_assignable_v<SceneTextureCpuResult>);

	switch (source.colorSpace) {
	case ColorSpace::Automatic:
	case ColorSpace::Srgb:
	case ColorSpace::Linear:
		break;
	default:
		diagnostic = "Invalid texture color space";
		return SceneTextureCpuStatus::Failed;
	}

	switch (source.recipe) {
	case Recipe::OrdinaryFile:
		if (source.resolvedPath.empty() || !source.resolvedPath.is_absolute() || !source.bytes.empty()) {
			diagnostic = "File source requires an absolute path and empty bytes";
			return SceneTextureCpuStatus::Failed;
		}
		break;
	case Recipe::OrdinaryMemory:
		if (!source.resolvedPath.empty() || source.bytes.empty()) {
			diagnostic = "Memory source requires an empty path and nonempty bytes";
			return SceneTextureCpuStatus::Failed;
		}
		break;
	case Recipe::ModelGrayscaleBaseColor:
		if (!source.resolvedPath.empty() || source.bytes.empty() || source.colorSpace != ColorSpace::Automatic) {
			diagnostic = "Grayscale base color requires memory bytes and Automatic color space";
			return SceneTextureCpuStatus::Failed;
		}
		break;
	case Recipe::HdrEnvironment:
		if (source.resolvedPath.empty() || !source.resolvedPath.is_absolute() ||
			!source.bytes.empty() || source.colorSpace != ColorSpace::Automatic) {
			diagnostic = "HDR environment requires an absolute path, empty bytes and Automatic color space";
			return SceneTextureCpuStatus::Failed;
		}
		break;
	default:
		diagnostic = "Invalid texture CPU recipe";
		return SceneTextureCpuStatus::Failed;
	}

	SceneTextureCpuResult candidate;
	if (source.recipe == Recipe::HdrEnvironment) {
		measured.gpuDecision = "hdr_combined_cpu";
		// 環境変換はmipまで含む。失敗を元画像で代用せず、通常mip段階へ流さない。
		measured.decodeState = "hdr_combined_attempted";
		measured.mipState = "included_in_hdr_combined";
		{
			CpuTimingSpan span{measured.hdrCombinedUs, measured.hdrStarted, measured.hdrEnded};
			if (!TextureManager::TryPrepareHdrEnvironment(source.resolvedPath, candidate.texture, diagnostic)) {
				measured.decodeState = "hdr_combined_failed";
				return SceneTextureCpuStatus::Failed;
			}
		}
		measured.decodeState = "hdr_combined_prepared";
		measured.final = ImageSize(candidate.texture);
		if (cancelled()) return SceneTextureCpuStatus::Cancelled;
		const auto finalLimit = limits ? (limits->finalBytes ? limits->finalBytes : limits->decodedBytes) : 0;
		if (finalLimit && measured.final.bytes > finalLimit)
			return SceneTextureCpuStatus::ReservationExceeded;
		diagnostic.clear();
		out = std::move(candidate);
		return SceneTextureCpuStatus::Prepared;
	}
	{
		CpuTimingSpan span{measured.decodeUs, measured.decodeStarted, measured.decodeEnded};
		measured.decodeState = "decode_attempted";
		if (source.recipe == Recipe::ModelGrayscaleBaseColor) {
			using GrayStatus = TextureManager::GrayscaleBaseColorDecodeStatus;
			switch (TextureManager::TryDecodeGrayscaleBaseColorPng(source.bytes.data(), source.bytes.size(), candidate.texture)) {
			case GrayStatus::Expanded:
				measured.decodeState = "gray_expanded";
				break;
			case GrayStatus::UseOriginalPng:
				measured.decodeState = "gray_redecode_attempted";
				// 同期Modelと同じく元PNGを再decodeし、既展開の色や透過を維持する。
				if (!TextureManager::TryDecodeTextureMemory(source.bytes.data(), source.bytes.size(),
					"png", ColorSpace::Automatic, candidate.texture, diagnostic)) {
					measured.decodeState = "gray_redecode_failed";
					return SceneTextureCpuStatus::Failed;
				}
				measured.decodeState = "gray_redecoded";
				break;
			case GrayStatus::Failed:
				measured.decodeState = "gray_failed";
				diagnostic = "Failed to decode grayscale base color";
				return SceneTextureCpuStatus::Failed;
			}
		} else {
			const bool decoded = source.recipe == Recipe::OrdinaryFile
				? TextureManager::TryDecodeTextureFile(source.resolvedPath, source.colorSpace, candidate.texture, diagnostic)
				: TextureManager::TryDecodeTextureMemory(source.bytes.data(), source.bytes.size(),
					source.formatHint, source.colorSpace, candidate.texture, diagnostic);
			if (!decoded) {
				measured.decodeState = "decode_failed";
				return SceneTextureCpuStatus::Failed;
			}
			measured.decodeState = "decoded";
		}
	}
	measured.decoded = ImageSize(candidate.texture);
	if (cancelled()) return SceneTextureCpuStatus::Cancelled;
	if (limits && limits->decodedBytes && measured.decoded.bytes > limits->decodedBytes)
		return SceneTextureCpuStatus::ReservationExceeded;
	if (limits && limits->finalBytes) {
		const auto& m = candidate.texture.metadata;
		uint64_t bound = 0;
		bool valid = m.width == limits->width && m.height == limits->height &&
			m.arraySize == 1 && m.depth == 1 && m.mipLevels == 1 &&
			candidate.texture.image.GetPixelsSize() <= limits->decodedBytes;
		for (size_t w = m.width, h = m.height; valid;) {
			size_t row = 0, slice = 0;
			if (FAILED(DirectX::ComputePitch(m.format, w, h, row, slice)) ||
				slice > limits->finalBytes - bound) { valid = false; break; }
			bound += slice;
			if (w == 1 && h == 1) break;
			w = (std::max)(size_t{1}, w / 2); h = (std::max)(size_t{1}, h / 2);
		}
		if (!valid) return SceneTextureCpuStatus::ReservationExceeded;
	}

	// 型/出所をdecode後に確認する。linearは既存WIC、特殊/既存mipは旧CPU契約のまま。
	const auto& m = candidate.texture.metadata;
	const uint8_t pngSignature[] = {137,80,78,71,13,10,26,10};
	const bool png8 = source.bytes.size() >= 33 && !std::memcmp(source.bytes.data(),pngSignature,8) &&
		source.bytes[24] <= 8;
	// JPEGの最終適格性は既存WICの展開結果で判定する。予約の対応範囲とは独立し、色/alphaを変更しない。
	const bool jpeg = source.recipe == Recipe::OrdinaryMemory &&
		source.bytes.size() >= 3 && source.bytes[0] == 0xff && source.bytes[1] == 0xd8 && source.bytes[2] == 0xff;
	const auto alpha = m.GetAlphaMode();
	measured.gpuDecision = "source_or_format";
	measured.gpuEligible = (png8 || jpeg) && source.resolvedPath.empty() &&
		(source.recipe == Recipe::OrdinaryMemory || source.recipe == Recipe::ModelGrayscaleBaseColor) &&
		m.width == kSceneGpuMipSize && m.height == kSceneGpuMipSize && m.depth == 1 && m.arraySize == 1 &&
		m.dimension == DirectX::TEX_DIMENSION_TEXTURE2D && !m.IsCubemap() && m.mipLevels == 1 &&
		m.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
		(alpha == DirectX::TEX_ALPHA_MODE_UNKNOWN || alpha == DirectX::TEX_ALPHA_MODE_STRAIGHT || alpha == DirectX::TEX_ALPHA_MODE_OPAQUE);
	if (!measured.gpuEligible) {
		measured.gpuDecision = source.recipe == Recipe::OrdinaryFile ? "file_recipe" :
			m.IsCubemap() || m.arraySize != 1 || m.depth != 1 ? "array_cube_or_depth" :
			m.mipLevels != 1 ? "existing_mips" :
			m.width != kSceneGpuMipSize || m.height != kSceneGpuMipSize ? "dimensions" :
			m.format == DXGI_FORMAT_R8G8B8A8_UNORM ? "linear_format" :
			m.format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? "unsupported_format" :
			!(png8 || jpeg) ? "not_png8_or_jpeg" : "alpha_mode";
	}
	if (measured.gpuEligible) {
		measured.gpuDecision = limits && limits->forceCpuMips ? "forced_cpu_retry" :
			limits && limits->allowGpuMips ? "requested" : "device_or_feature_disabled";
		if (limits && limits->allowGpuMips && !limits->forceCpuMips) {
			candidate.mipPolicy = SceneTextureMipPolicy::GpuSrgbBox4096;
			measured.mipState = "gpu_pending";
			measured.final = ImageSize(candidate.texture); // 実CPU payloadはmip1。target13へ偽装しない。
			diagnostic.clear(); out = std::move(candidate);
			return SceneTextureCpuStatus::Prepared;
		}
	}

	// 既存mip APIは失敗時の入力保持とin-place成功置換に対応する。
	// 元画像fallbackもlocalで完成させ、呼出元の以前の結果は成功まで触らない。
	const bool existingMips = candidate.texture.metadata.mipLevels > 1;
	const bool compressed = DirectX::IsCompressed(candidate.texture.metadata.format);
	measured.mipState = "mip_attempted";
	{
		CpuTimingSpan span{measured.mipUs, measured.mipStarted, measured.mipEnded};
		candidate.mipFallbackUsed = !TextureManager::TryGenerateTextureMipChain(candidate.texture, candidate.texture);
	}
	measured.mipState = existingMips ? "kept_existing_mips" : compressed ? "kept_compressed" :
		candidate.mipFallbackUsed ? "generation_failed_kept_source" : "generated";
	measured.final = ImageSize(candidate.texture);
	if (cancelled()) return SceneTextureCpuStatus::Cancelled;
	if (limits && limits->finalBytes && measured.final.bytes > limits->finalBytes)
		return SceneTextureCpuStatus::ReservationExceeded;
	if (limits && !limits->finalBytes && limits->decodedBytes && measured.final.bytes > limits->decodedBytes)
		return SceneTextureCpuStatus::ReservationExceeded;
	diagnostic.clear();
	out = std::move(candidate);
	return SceneTextureCpuStatus::Prepared;
}
