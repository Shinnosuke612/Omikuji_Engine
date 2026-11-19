// 役割: Direct3D12のデバイス、CommandList、SwapChain、同期を管理する。
#pragma once
#include <d3d12.h>
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#include <dxgi1_6.h>
#include <dxcapi.h>
#pragma comment(lib,"dxcompiler.lib")
#include <wrl.h>
#include "WinApp.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// 参照引数の宣言に必要な型だけを示し、DirectXTexの完全な定義は実装側で読み込む。
namespace DirectX {
    struct TexMetadata;
    class ScratchImage;
}

enum class DirectFrameGpuState { NotSubmitted, Completed, DeviceRemoved, Unconfirmed };

// 同一のPostDraw提出に限った証跡。Signal未成功・完了未確認のFence値はそれぞれ0。
// mainのV2表示進行は実PresentのS_OKとGPU完了を確認し、出力採用/可視条件をhostで照合する。
struct DirectFrameResult {
	DirectFrameGpuState gpuState = DirectFrameGpuState::NotSubmitted;
	uint64_t signaledFence = 0;
	uint64_t completedFence = 0;
	HRESULT error = S_OK;
	const char* failedOperation = nullptr;
	HRESULT presentResult = E_PENDING; // 実Present未呼出し。occluded等の成功statusもそのまま保持する。

	bool CanContinue() const noexcept {
		return gpuState == DirectFrameGpuState::Completed && SUCCEEDED(error);
	}
};

class DirectXCommon{
public:
	// 未提出listを破棄してから、そのlist用に保持した資源を解放する。
	~DirectXCommon();
	// main threadの通常Draw末尾専用。null/保持中なら拒否する。
	// 成功後は次のPostDrawまで別提出を挟まない。GPU完了またはlist破棄までAddRefを保持する。
	bool TryRetainResourceForNextPostDraw(ID3D12Resource* resource) noexcept;
	// 同じslotへ全参照を原子的に受付。成功時だけrefsを消費し、完了またはlist破棄まで保持する。
	bool TryRetainObjectsForNextPostDraw(std::vector<Microsoft::WRL::ComPtr<IUnknown>>& refs) noexcept;
	//初期化
	void Initialize(WinApp* winApp);
	//描画前処理
	void PreDraw();
	// 通常描画を提出し、同じ提出のGPU完了と次Frameの描画可否を返す。
	[[nodiscard]] DirectFrameResult PostDraw();
	//デバイスの初期化
	void DeviceInitialize();
	//コマンド関連の初期化
	void CommandInitialize();
	//スワップチェーンの生成
	void SwapChainGenerate();
	//深度バッファの生成
	void CreateDepthStencilTextureResource();
	//各種デスクリプターヒープの生成
	void DescriptorHeapGenerate();
	//デスクリプタヒープを生成する
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE heapType, UINT numDescriptors, bool shaderVisible);
	//レンダーターゲットビューの初期化
	void RenderTargetViewInitialize();
	//深度ステンシルビューの初期化
	void DepthStencilInitialize();
	//フェンスの生成
	void FenceGenerate();
	//ビューポート矩形の初期化
	void ViewportRectInitialize();
	//シザリング矩形の初期化
	void ScissorRectInitialize();
	//DXCコンパイラの生成
	void DXCCompilerGenerate();
	//実行から待ち、リセットを行う関数
	void ExecuteCommandListAndWait();


	Microsoft::WRL::ComPtr<IDxcBlob> CompileShader(
		// CompilerするShaderファイルへのパス
		const std::wstring& filePath,
		// Compilerに使用するProfile
		const wchar_t* profile);
	// 同じresolver/flags/DXCを使う準備用入口。警告も失敗とし、失敗時outは不変。
	// diagnosticは今回の結果へ置換する。GPU使用中のDrawから呼ばない。
	bool TryCompileShader(const std::wstring& filePath, const wchar_t* profile,
		Microsoft::WRL::ComPtr<IDxcBlob>& out, std::string& diagnostic);

	Microsoft::WRL::ComPtr <ID3D12Resource> CreateBufferResource(size_t sizeInBytes);
    // Replay準備用。失敗時out不変、assert/Map/command送信なし。
    bool TryCreateBufferResource(size_t sizeInBytes, Microsoft::WRL::ComPtr<ID3D12Resource>& out);

	Microsoft::WRL::ComPtr <ID3D12Resource> CreateTextureResource(const DirectX::TexMetadata& metadate);

	ID3D12Resource* UploadTextureData(const Microsoft::WRL::ComPtr <ID3D12Resource>& texture, const DirectX::ScratchImage& mipImages);


	//getter
	ID3D12GraphicsCommandList* GetCommandList() const{
		return commandList.Get();
	}
	ID3D12Device* GetDevice() const{
		return device.Get();
	}
	ID3D12CommandQueue* GetCommandQueue() const{
		return commandQueue;
	}
	uint32_t GetSwapChainResourcesNum() const{
		return static_cast<uint32_t>(swapChainResources.size());
	}

	// 画面サイズ変更
	void ResizeSwapChain(uint32_t width, uint32_t height);

	uint32_t GetClientWidth() const { return clientWidth_; }
	uint32_t GetClientHeight() const { return clientHeight_; }

private:
	Microsoft::WRL::ComPtr<ID3D12Resource> postDrawRetainedResource_;
	std::vector<Microsoft::WRL::ComPtr<IUnknown>> postDrawRetainedObjects_;
	//コマンド関連
	ID3D12CommandQueue* commandQueue;//コマンドキュー
	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;//コマンドリスト
	ID3D12CommandAllocator* commandAllocator;//コマンドアロケータ
	//スワップチェーン
	IDXGISwapChain4* swapChain;
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};//スワップチェーンデスク
	//深度バッファのリソース
	ID3D12Resource* depthStencilResource;
	//DirectX12デバイス
	Microsoft::WRL::ComPtr<ID3D12Device> device;
	//DXGIファクトリ
	Microsoft::WRL::ComPtr<IDXGIFactory7> dxgiFactory;
	//WindowsAPI
	WinApp* winApp_ = nullptr;
	//デスクリプタヒープ
	Microsoft::WRL::ComPtr <ID3D12DescriptorHeap> rtvDescriptorHeap;
	Microsoft::WRL::ComPtr <ID3D12DescriptorHeap> dsvDescriptorHeap;
	//デスクリプタヒープのサイズ
	uint32_t descriptorSizeRTV;
	uint32_t descriptorSizeDSV;
	//デスクリプタハンドル取得関数
	static D3D12_CPU_DESCRIPTOR_HANDLE  GetCPUDescriptorHandle(const Microsoft::WRL::ComPtr <ID3D12DescriptorHeap>& descriptorHeap, uint32_t descriptorSize, uint32_t index);
	static D3D12_GPU_DESCRIPTOR_HANDLE  GetGPUDescriptorHandle(const Microsoft::WRL::ComPtr <ID3D12DescriptorHeap>& descriptorHeap, uint32_t descriptorSize, uint32_t index);
	//RTVDesc
	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
	//スワップチェーンリソース
	std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> swapChainResources;
	//フェンス
	Microsoft::WRL::ComPtr <ID3D12Fence> fence;
	//フェンス値
	uint64_t fenceValue = 0;
	//フェンスイベント
	HANDLE fenceEvent;
	// ビューポート矩形
	D3D12_VIEWPORT viewport{};
	// シザー矩形
	D3D12_RECT scissorRect{};
	//DXCコンパイラ用
	Microsoft::WRL::ComPtr <IDxcUtils> dxcUtils;
	Microsoft::WRL::ComPtr <IDxcCompiler3> dxcCompiler;
	Microsoft::WRL::ComPtr <IDxcIncludeHandler> includeHandler;
	// RTVを2つ作るのでディスクリプタを2つ用意
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[2];
	//barrier
	D3D12_RESOURCE_BARRIER barrier{};

	uint32_t clientWidth_ = WinApp::kClientWidth;
	uint32_t clientHeight_ = WinApp::kClientHeight;
};

