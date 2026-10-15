// 役割: Direct3D12のデバイス、CommandList、SwapChain、同期を管理する。
#pragma once
#include <d3d12.h>
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#include <dxgi1_6.h>
#include <dxcapi.h>
#pragma comment(lib,"dxcompiler.lib")
#include <wrl.h>
#include "../utility/Logger.h"
#include "../utility/StringUtility.h"
#include "WinApp.h"
#include <array>
//imguiを使うためのinclude
#include "../../externals/imgui/imgui.h"
#include "../../externals/imgui/imgui_impl_dx12.h"
#include "../../externals/imgui/imgui_impl_win32.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
#include <DirectXTex.h>

class DirectXCommon{
public:
	//初期化
	void Initialize(WinApp* winApp);
	//描画前処理
	void PreDraw();
	// 描画後処理
	void PostDraw();
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

