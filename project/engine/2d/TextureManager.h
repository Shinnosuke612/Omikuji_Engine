// 役割: テクスチャとHDR環境CubeMapの読み込み、SRV割り当て、キャッシュを管理する。
#pragma once
#include "../base/PreparedResourceStatus.h"
#include <string>
#include <filesystem>
#include <DirectXTex.h>
#include <wrl.h>  
#include <d3d12.h>
#include <unordered_map>
#include <unordered_set>

class DirectXCommon;
class SrvManager;

// 候補の搬送だけで使う値。GPU policyは完成証明ではなく、CPU画像はmip0のまま。
enum class SceneTextureMipPolicy { CpuComplete, GpuSrgbBox4096 };
inline constexpr UINT kSceneGpuMipSize = 4096;
inline constexpr UINT kSceneGpuMipCount = 13;

// GPU完了とsample可能状態は呼出元が証跡を照合する。
struct CompletedTextureInput {
	std::string textureKey;
	Microsoft::WRL::ComPtr<ID3D12Resource> resource;
	DirectX::TexMetadata metadata{};
	bool gpuSrgbMipsPrepared = false; // 同DIRECT frameの完了を照合した所有側だけが設定する。
};

class TextureManager{
private:
	//テクスチャ一枚分のデータ
	struct  TextureData{
		DirectX::TexMetadata metadata;
		Microsoft::WRL::ComPtr<ID3D12Resource> resource;
		uint32_t srvIndex;
		D3D12_CPU_DESCRIPTOR_HANDLE srvHandleCPU;
		D3D12_GPU_DESCRIPTOR_HANDLE srvHandleGPU;
	};
private:
	static TextureManager* instance;

	TextureManager() = default;
	~TextureManager() = default;
	TextureManager(TextureManager&) = delete;
	TextureManager& operator=(TextureManager&) = delete;

public:
	using CompletedTextureInput = ::CompletedTextureInput;
	// main専用。Publishedだけがresourceを消費し、同keyは既存資源を維持する。
	PreparedPublishStatus TryPublishCompletedTexture(CompletedTextureInput&, std::string& diagnostic);
	static std::string MakeEnvironmentTextureKey(const std::string& sourcePath);
	enum class TextureColorSpace {
		Automatic,
		Srgb,
		Linear
	};

	struct DecodedTexture {
		DirectX::ScratchImage image;
		DirectX::TexMetadata metadata{};
	};

	enum class GrayscaleBaseColorDecodeStatus { Expanded, UseOriginalPng, Failed };
	// CPU専用。用途を選別済みのbase color bytesを呼出中だけ借用する。
	// WIC用COMは呼出元が初期化。R8/R16だけRGBA SRGBへ展開し、他formatはUseOriginalPng。
	// Expanded時だけoutを置換。失敗/UseOriginalPng/確保例外ではoutを保持、例外は伝播する。
	static GrayscaleBaseColorDecodeStatus TryDecodeGrayscaleBaseColorPng(
		const uint8_t* data, size_t size, DecodedTexture& out);

	// CPU専用。呼び出し側がパス解決とWIC用COM初期化を行う。
	// 入力は呼び出し中だけ借用。成功時だけoutを更新し、失敗理由はerrorに返す。
	static bool TryDecodeTextureFile(const std::filesystem::path& resolvedPath,
		TextureColorSpace colorSpace, DecodedTexture& out, std::string& error);
	static bool TryDecodeTextureMemory(const uint8_t* data, size_t dataSize,
		const std::string& formatHint, TextureColorSpace colorSpace,
		DecodedTexture& out, std::string& error);

	// CPU専用。生成できた場合だけoutを更新する。sourceとoutは同じでもよい。
	static bool TryGenerateTextureMipChain(const DecodedTexture& source, DecodedTexture& out);
	// 解決済みHDRパスから環境用cubeを生成。失敗時はoutを保持し、errorに理由を返す。
	static bool TryPrepareHdrEnvironment(const std::filesystem::path& resolvedPath,
		DecodedTexture& out, std::string& error);

	//初期化
	void Initialize(DirectXCommon* directXCommon,SrvManager* srvManager);
	//シングルトンインスタンスの取得
	static TextureManager* GetInstance();
	//終了
	void Finalize();

	// メタデータの取得
	const DirectX::TexMetadata& GetMetaData(const std::string& filePath);
	// SRVインデックスの取得
	uint32_t GetSrvIndex(const std::string& filePath);
	// GPUハンドルの取得
	D3D12_GPU_DESCRIPTOR_HANDLE GetSrvHandleGPU(const std::string& filePath);


	/// <summary>
/// テクスチャファイルの読み込み
/// </summary>
	bool LoadTexture(
		const std::string& filePath,
		TextureColorSpace colorSpace = TextureColorSpace::Automatic
	);
	// GPU所有threadでDDS Cube/HDR(2:1)を読み、成功時だけ描画用keyを返す。
	// sourcePathとoutTextureKeyは別string。HDR Cubeは通常2D cacheとは分離し、
	// TextureManager終了まで共有する。同じHDRの内容変更は再起動で再生成する。
	// 失敗時はoutTextureKeyと既存登録を保持し、Project Refresh後に再試行できる。
	bool LoadEnvironmentTexture(
		const std::string& sourcePath,
		std::string& outTextureKey
	);
	bool ReloadTexture(
		const std::string& filePath,
		TextureColorSpace colorSpace = TextureColorSpace::Automatic
	);
	// Texture resource、map entry、SRV slotを一組で返却する。未知keyまたはslot返却失敗はfalse。
	bool ReleaseTexture(const std::string& textureKey);
	bool HasTexture(const std::string& textureKey) const;
	bool LoadTextureFromMemory(
		const std::string& textureKey,
		const uint8_t* data,
		size_t dataSize,
		const std::string& formatHint,
		TextureColorSpace colorSpace = TextureColorSpace::Automatic
	);
	// Runtime生成Textureを同じkeyへ再登録する場合は、既存SRVを再利用する。
	bool UpdateTextureFromPixels(
		const std::string& textureKey,
		const uint8_t* pixels,
		uint32_t width,
		uint32_t height,
		DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
	);
	void ClearFailedTextureCache();
private:
	bool RegisterTexture(
		const std::string& textureKey,
		const DirectX::ScratchImage& loadedImage,
		const DirectX::TexMetadata& metadata
	);

	//DirectXCommonを参照
	DirectXCommon* dxCommon;
	//SrvManagerも参照
	SrvManager* srvManager;
	//SRVインデックス開始番号
	static uint32_t kSRVIndexTop;
	//テクスチャデータ
	std::unordered_map<std::string, TextureData> textureDatas;
	// 読み込み失敗を毎フレーム繰り返さないための負のキャッシュ
	std::unordered_set<std::string> failedTextureKeys;
};

