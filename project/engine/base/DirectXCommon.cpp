// 役割: Direct3D12の初期化、フレーム開始、フレーム終了を実装する。
#include "DirectXCommon.h"
#include "../utility/EditableResourcePath.h"
#include <cassert>
#include "../../externals/DirectXTex/d3dx12.h"
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#include <filesystem>
#include <format>
#include <stdexcept>

using namespace Microsoft::WRL;
using namespace Logger;

namespace {
std::wstring ResolveRuntimePath(const std::wstring& filePath) {
	return EditableResourcePath::ResolveResource(
		std::filesystem::path(filePath)
	).wstring();
}
}

void DirectXCommon::Initialize(WinApp* winApp){
	//NULL検出
	assert(winApp);
	//メンバ変数に記録
	winApp_ = winApp;
	
	clientWidth_ = winApp_->GetClientWidth();
	clientHeight_ = winApp_->GetClientHeight();

	//デバイスの生成
	DeviceInitialize();
	//コマンド関連の初期化
	CommandInitialize();
	//スワップチェーンの生成
	SwapChainGenerate();
	//深度バッファの生成
	CreateDepthStencilTextureResource();
	//各種デスクリプタヒープの生成
	DescriptorHeapGenerate();
	//レンダーターゲットビューの生成
	RenderTargetViewInitialize();
	//深度ステンシルビューの初期化
	DepthStencilInitialize();
	//フェンスの生成
	FenceGenerate();
	//ビューポート矩形の初期化
	ViewportRectInitialize();
	//シザリング矩形の初期化
	ScissorRectInitialize();
	//DXCコンパイラの生成
	DXCCompilerGenerate();

#ifdef _DEBUG
	ID3D12InfoQueue* infoQueue = nullptr;
	if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&infoQueue)))){
		// ヤバイエラー時に止まる
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
		// エラー時に止まる
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);

		//リソースのログを知りたいのでコメントアウト
		//// 警告時に止まる
		//infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);


		// 解放
		infoQueue->Release();

		// 抑制するメッセージのID
		D3D12_MESSAGE_ID denyIds[] = {
			// Windows11でのDXGIデバッグレイヤーとDX12デバッグレイヤーの相互作用バグによるエラーメッセージ
			// https://stackoverflow.com/questions/69805245/directx-12-application-is-crashing-in-windows-11
			D3D12_MESSAGE_ID_RESOURCE_BARRIER_MISMATCHING_COMMAND_LIST_TYPE
		};

		// 抑制するレベル
		D3D12_MESSAGE_SEVERITY severities[] = { D3D12_MESSAGE_SEVERITY_INFO };
		D3D12_INFO_QUEUE_FILTER filter{};
		filter.DenyList.NumIDs = _countof(denyIds);
		filter.DenyList.pIDList = denyIds;
		filter.DenyList.NumSeverities = _countof(severities);
		filter.DenyList.pSeverityList = severities;

		// 指定したメッセージの表示を抑制する
		infoQueue->PushStorageFilter(&filter);
	}
#endif
}

void DirectXCommon::PreDraw(){

	// これから書き込むバックバッファのインデックスを取得
	UINT backBufferIndex = swapChain->GetCurrentBackBufferIndex();

	// TransitionBarrierの設定
	// 今回のバリアはTransition
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	// Noneにしておく
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	// バリアを張る対象のリソース。現在のバックバッファに対して行う
	barrier.Transition.pResource = *&swapChainResources[backBufferIndex];
	// 遷移前（現在）のResourceState
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	// 遷移後のResourceState
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	// TransitionBarrierを張る
	commandList->ResourceBarrier(1, &barrier);

	// 描画先のRTVとDSVを設定する
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = GetCPUDescriptorHandle(dsvDescriptorHeap, descriptorSizeDSV, 0);
	commandList->OMSetRenderTargets(1, &rtvHandles[backBufferIndex], false, &dsvHandle);
	/*commandList->OMSetRenderTargets(1, &rtvHandles[backBufferIndex], false, nullptr);*/

	commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	// 指定した色で画面全体をクリアする
	float clearColor[] = { 0.006f, 0.007f, 0.009f, 1.0f };
	commandList->ClearRenderTargetView(rtvHandles[backBufferIndex], clearColor, 0, nullptr);

	////ImGui描画用のDescriptorHeapの設定
	//ID3D12DescriptorHeap* descriptorHeaps[] = { *&srvDescriptorHeap };
	//commandList->SetDescriptorHeaps(1, descriptorHeaps);

	commandList->RSSetViewports(1, &viewport);           // Viewportを設定
	commandList->RSSetScissorRects(1, &scissorRect);     // Scissorを設定
}

void DirectXCommon::PostDraw(){


	//バックバッファのインデックスを取得
	UINT backBufferIndex = swapChain->GetCurrentBackBufferIndex();

	HRESULT hr;
	// 画面に描く処理はすべて終わり、画面に映すので、状態を遷移
			// 今回はRenderTargetからPresentにする
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	// TransitionBarrierを張る
	commandList->ResourceBarrier(1, &barrier);

	// コマンドリストの内容を確定させる。すべてのコマンドを積んでからCloseすること
	hr = commandList->Close();
	assert(SUCCEEDED(hr));

	// GPUにコマンドリストの実行を行わせる
	ID3D12CommandList* commandLists[] = { commandList.Get()};
	commandQueue->ExecuteCommandLists(1, commandLists);

	// GPUとOSに画面の交換を行うよう通知する
	swapChain->Present(1, 0);

	// Fenceの値を更新
	fenceValue++;
	// GPUがここまでたどり着いたときに、Fenceの値を指定した値に代入するようにSignalを送る
	commandQueue->Signal(*&fence, fenceValue);

	// Fenceの値が指定したSignal値にたどり着いているか確認する
	 // GetCompletedValueの初期値はFence作成時に渡した初期値
	if(fence->GetCompletedValue() < fenceValue){
		// 指定したSignalにたどりついていないので、たどり着くまで待つようにイベントを設定する
		fence->SetEventOnCompletion(fenceValue, fenceEvent);
		// イベント待つ
		WaitForSingleObject(fenceEvent, INFINITE);
	}

	// 次のフレーム用のコマンドリストを準備
	hr = commandAllocator->Reset();
	assert(SUCCEEDED(hr));

	hr = commandList->Reset(commandAllocator, nullptr);
	assert(SUCCEEDED(hr));

}

void DirectXCommon::DeviceInitialize(){
	HRESULT hr;

#ifdef _DEBUG
	ID3D12Debug1* debugController = nullptr;
	if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))){
		// デバッグレイヤーを有効化する
		debugController->EnableDebugLayer();
		// GPUベースバリデーションは重いので、普段の実行では無効にする
		debugController->SetEnableGPUBasedValidation(FALSE);
	}
#endif

	// DXGIファクトリーの生成
	dxgiFactory = nullptr;
	// HRESULTはWindows系のエラーコードであり、
	// 関数が成功したかどうかをSUCCEEDEDマクロで判定できる
	hr = CreateDXGIFactory(IID_PPV_ARGS(&dxgiFactory));
	// 初期化の根本的な部分でエラーが出た場合はプログラムが間違っているか、どうにもできない場合が多いのでassertにしておく
	assert(SUCCEEDED(hr));

	//使用するアダプタ用の変数。最初にnullptrを入れておく
	IDXGIAdapter4* useAdapter = nullptr;
	// 良い順にアダプタを頼む
	for(UINT i = 0; dxgiFactory->EnumAdapterByGpuPreference(i,
		DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&useAdapter)) !=
		DXGI_ERROR_NOT_FOUND; ++i){

		// アダプターの情報を取得する
		DXGI_ADAPTER_DESC3 adapterDesc{};
		hr = useAdapter->GetDesc3(&adapterDesc);
		assert(SUCCEEDED(hr)); // 取得できないのは一大事

		// ソフトウェアアダプタでなければ採用
		if(!(adapterDesc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE)){
			//採用したアダプタ情報をログに出力。wstringのほうなので注意
			Log(std::format(L"Use Adapter:{}\n", adapterDesc.Description));
			break;
		}

		useAdapter = nullptr; // ソフトウェアアダプタの場合は見なかったことにする
	}
	// 適切なアダプタが見つからなかったので起動できない
	assert(useAdapter != nullptr);

	device = nullptr;
	// 機能レベルとログ出力用の文字列
	D3D_FEATURE_LEVEL featureLevels[] = {
		D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0
	};
	const char* featureLevelStrings[] = { "12.2", "12.1", "12.0" };

	// 高い順に生成できるか試していく
	for(size_t i = 0; i < _countof(featureLevels); ++i){
		// 採用したアダプターでデバイスを生成
		hr = D3D12CreateDevice(useAdapter, featureLevels[i], IID_PPV_ARGS(&device));
		// 指定した機能レベルでデバイスが生成できたかを確認
		if(SUCCEEDED(hr)){
			// 生成できたのでログ出力を行ってループを抜ける
			Log(std::format("FeatureLevel : {}\n", featureLevelStrings[i]));
			break;
		}
	}

	// デバイスの生成がうまくいかなかったので起動できない
	assert(device != nullptr);
	Log("Complete create D3D12Device!!!\n"); // 初期化完了のログをだす
}

void DirectXCommon::CommandInitialize(){
	HRESULT hr;

	// コマンドキューを生成する
	commandQueue = nullptr;
	D3D12_COMMAND_QUEUE_DESC commandQueueDesc{};
	hr = device->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&commandQueue));

	// コマンドキューの生成がうまくいかなかったので起動できない
	assert(SUCCEEDED(hr));

	// コマンドアロケータを生成する
	commandAllocator = nullptr;
	hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));
	// コマンドアロケータの生成がうまくいかなかったので起動できない
	assert(SUCCEEDED(hr));

	// コマンドリストを生成する
	commandList = nullptr;
	hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator, nullptr,
								   IID_PPV_ARGS(&commandList));
	// コマンドリストの生成がうまくいかなかったので起動できない
	assert(SUCCEEDED(hr));
}

void DirectXCommon::SwapChainGenerate(){
	HRESULT hr;

	// スワップチェーンを生成する
	swapChain = nullptr;
	swapChainDesc.Width = clientWidth_; // 画面の幅。ウィンドウのクライアント領域を同じものにしておく
	swapChainDesc.Height = clientHeight_;  // 画面の高さ。ウィンドウのクライアント領域を同じものにしておく
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // 色の形式
	swapChainDesc.SampleDesc.Count = 1;  // マルチサンプルしない
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;  // 描画のターゲットとして利用する
	swapChainDesc.BufferCount = 2;  // ダブルバッファ
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;  // モニタにうつしたら、中身を破棄

	// コマンドキュー、ウィンドウハンドル、設定を渡して生成する
	hr = dxgiFactory->CreateSwapChainForHwnd(commandQueue, winApp_->GetHwnd(), &swapChainDesc, nullptr, nullptr, reinterpret_cast<IDXGISwapChain1**>(&swapChain));
	assert(SUCCEEDED(hr));

}

void DirectXCommon::CreateDepthStencilTextureResource(){
	//生成するリソースの設定
	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Width = clientWidth_;
	resourceDesc.Height = clientHeight_;
	resourceDesc.MipLevels = 1;
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_HEAP_PROPERTIES heapProperties{};
	heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

	//深度値のクリア設定
	D3D12_CLEAR_VALUE depthClearValue{};
	depthClearValue.DepthStencil.Depth = 1.0f;
	depthClearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;

	//Resourceの生成
	depthStencilResource = nullptr;
	HRESULT hr = device->CreateCommittedResource(
		&heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE,
		&depthClearValue,
		IID_PPV_ARGS(&depthStencilResource));
	assert(SUCCEEDED(hr));
}

void DirectXCommon::DescriptorHeapGenerate(){
	//DescriptorSizeを取得しておく
	descriptorSizeRTV = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	descriptorSizeDSV = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

	// ディスクリプタヒープの生成
	rtvDescriptorHeap = CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, false);

	//DSV用のヒープでディスクリプタの数は1。DSVはShader内で触るものではないので、ShaderVisibleはfalse
	dsvDescriptorHeap = CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);

}

Microsoft::WRL::ComPtr <ID3D12DescriptorHeap> DirectXCommon::CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE heapType, UINT numDescriptors, bool shaderVisible){

	ID3D12DescriptorHeap* descriptorHeap = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc{};
	descriptorHeapDesc.Type = heapType;  // レンダーターゲットビュー用
	descriptorHeapDesc.NumDescriptors = numDescriptors;  // ダブルバッファ用に2つ。多くても別に構わない
	descriptorHeapDesc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	HRESULT hr = device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&descriptorHeap));
	// ディスクリプタヒープが作れなかったので起動できない
	assert(SUCCEEDED(hr));

	return descriptorHeap;
}

void DirectXCommon::RenderTargetViewInitialize(){
	HRESULT hr;

	// SwapChainからResourceを引っ張ってくる
	hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&swapChainResources[0]));
	// うまく取得できなければ起動できない
	assert(SUCCEEDED(hr));
	hr = swapChain->GetBuffer(1, IID_PPV_ARGS(&swapChainResources[1]));
	assert(SUCCEEDED(hr));

	// RTVの設定
	rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;  // 出力結果をSRGBに変換して書き込む
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;  // 2dテクスチャとして書き込む

	// ディスクリプタの先頭を取得する
	D3D12_CPU_DESCRIPTOR_HANDLE rtvStartHandle = GetCPUDescriptorHandle(rtvDescriptorHeap, descriptorSizeRTV, 0);



	// まず1つ目を作る。1つ目は最初のところに作る。格納場所をこちらで指定してあげる必要がある
	rtvHandles[0] = rtvStartHandle;
	device->CreateRenderTargetView(*(&swapChainResources[0]), &rtvDesc, rtvHandles[0]);

	// 2つ目のディスクリプタハンドルを得る（自力で）
	rtvHandles[1].ptr = rtvHandles[0].ptr + device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	// 2つ目を作る
	device->CreateRenderTargetView(*(&swapChainResources[1]), &rtvDesc, rtvHandles[1]);
}

void DirectXCommon::DepthStencilInitialize(){
	//DSVの設定
	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
	dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	//DSVHeapの先頭にDSVを作る
	device->CreateDepthStencilView(depthStencilResource, &dsvDesc, GetCPUDescriptorHandle(dsvDescriptorHeap, descriptorSizeDSV, 0));
}

void DirectXCommon::FenceGenerate(){
	HRESULT hr;
	// 初期値0でFenceを作る
	fence = nullptr;
	hr = device->CreateFence(fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	assert(SUCCEEDED(hr));
	// FenceのSignalを待つためのイベントを作成する
	fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	assert(fenceEvent != nullptr);
}

void DirectXCommon::ViewportRectInitialize(){
	// クライアント領域のサイズと一緒にして画面全体に表示
	viewport.Width = static_cast<float>(clientWidth_);
	viewport.Height = static_cast<float>(clientHeight_);
	viewport.TopLeftX = 0;
	viewport.TopLeftY = 0;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
}

void DirectXCommon::ScissorRectInitialize(){
	// 基本的にビューポートと同じ矩形が構成されるようにする
	scissorRect.left = 0;
	scissorRect.right = static_cast<LONG>(clientWidth_);
	scissorRect.top = 0;
	scissorRect.bottom = static_cast<LONG>(clientHeight_);
}

void DirectXCommon::DXCCompilerGenerate(){
	HRESULT hr;
	// dxcCompilerを初期化
	dxcUtils = nullptr;
	dxcCompiler = nullptr;
	hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxcUtils));
	assert(SUCCEEDED(hr));
	hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxcCompiler));
	assert(SUCCEEDED(hr));

	// 現時点でincludeはしないが、includeに対応するための設定を行っておく
	includeHandler = nullptr;
	hr = dxcUtils->CreateDefaultIncludeHandler(&includeHandler);
	assert(SUCCEEDED(hr));
}

void DirectXCommon::ExecuteCommandListAndWait(){
	HRESULT hr = commandList->Close();
	assert(SUCCEEDED(hr));

	ID3D12CommandList* lists[] = { commandList.Get() };
	commandQueue->ExecuteCommandLists(1, lists);

	++fenceValue;
	commandQueue->Signal(fence.Get(), fenceValue);

	if(fence->GetCompletedValue() < fenceValue){
		fence->SetEventOnCompletion(fenceValue, fenceEvent);
		WaitForSingleObject(fenceEvent, INFINITE);
	}

	hr = commandAllocator->Reset();
	assert(SUCCEEDED(hr));
	hr = commandList->Reset(commandAllocator, nullptr);
	assert(SUCCEEDED(hr));
}

Microsoft::WRL::ComPtr<IDxcBlob> DirectXCommon::CompileShader(const std::wstring& filePath, const wchar_t* profile) {
	Microsoft::WRL::ComPtr<IDxcBlob> shader;
	std::string diagnostic;
	if (!TryCompileShader(filePath, profile, shader, diagnostic)) {
		Log(diagnostic);
		assert(false);
		throw std::runtime_error(diagnostic);
	}
	return shader;
}

bool DirectXCommon::TryCompileShader(const std::wstring& filePath, const wchar_t* profile,
	Microsoft::WRL::ComPtr<IDxcBlob>& out, std::string& diagnostic) {
	diagnostic.clear();
	if (!dxcUtils || !dxcCompiler || !includeHandler || !profile || !*profile || filePath.empty()) {
		diagnostic = "Shader compiler or arguments are not ready.";
		return false;
	}
	std::wstring resolvedFilePath;
	try {
		resolvedFilePath = ResolveRuntimePath(filePath);
	} catch (const std::exception& error) {
		diagnostic = std::string("Failed resolve shader path: ") + error.what();
		return false;
	}
	Log(StringUtility::ConvertString(std::format(L"Begin CompileShader, path:{}, profile:{}\n", resolvedFilePath, profile)));
	const auto failed = [&](const char* step, HRESULT hr) {
		diagnostic = std::format("{} shader, path:{}, hr:{:#010x}\n", step,
			StringUtility::ConvertString(resolvedFilePath), static_cast<unsigned int>(hr));
		return false;
	};
	// 途中失敗でもDXCの一時COM objectを解放し、公開済みoutは交換しない。
	Microsoft::WRL::ComPtr<IDxcBlobEncoding> source;
	HRESULT hr = dxcUtils->LoadFile(resolvedFilePath.c_str(), nullptr, &source);
	if (FAILED(hr) || !source) return failed("Failed LoadFile", hr);
	DxcBuffer buffer{};
	buffer.Ptr = source->GetBufferPointer();
	buffer.Size = source->GetBufferSize();
	buffer.Encoding = DXC_CP_UTF8;
	LPCWSTR arguments[] = { resolvedFilePath.c_str(), L"-E", L"main", L"-T", profile,
		L"-Zi", L"-Qembed_debug", L"-Od", L"-Zpr" };
	Microsoft::WRL::ComPtr<IDxcResult> result;
	hr = dxcCompiler->Compile(&buffer, arguments, _countof(arguments), includeHandler.Get(), IID_PPV_ARGS(&result));
	if (FAILED(hr) || !result) return failed("Failed CompileShader", hr);
	Microsoft::WRL::ComPtr<IDxcBlobUtf8> errors;
	hr = result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
	if (FAILED(hr)) return failed("Failed Get diagnostics", hr);
	if (errors && errors->GetStringLength() != 0) {
		diagnostic.assign(errors->GetStringPointer(), errors->GetStringLength());
		return false; // 既存CompileShaderと同じくwarningも拒否する。
	}
	HRESULT status = E_FAIL;
	hr = result->GetStatus(&status);
	if (FAILED(hr) || FAILED(status)) return failed("Failed shader status", FAILED(hr) ? hr : status);
	Microsoft::WRL::ComPtr<IDxcBlob> object;
	hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
	if (FAILED(hr) || !object || object->GetBufferSize() == 0) return failed("Failed Get shader object", hr);
	Log(StringUtility::ConvertString(std::format(L"Compile Succeeded, path:{}, profile:{}\n", resolvedFilePath, profile)));
	out = object;
	return true;
}

Microsoft::WRL::ComPtr<ID3D12Resource> DirectXCommon::CreateBufferResource(size_t sizeInBytes){
	// 頂点リソース用のヒープの設定
	D3D12_HEAP_PROPERTIES uploadHeapProperties{};
	uploadHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD; // UploadHeapを使う

	// 頂点リソースの設定
	D3D12_RESOURCE_DESC vertexResourceDesc{};
	// バッファリソース。テクスチャの場合はまた別の設定をする
	vertexResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	vertexResourceDesc.Width = sizeInBytes; // リソースのサイズ。今回はVector4を3頂点分
	// バッファの高さは1にする決まり
	vertexResourceDesc.Height = 1;
	vertexResourceDesc.DepthOrArraySize = 1;
	vertexResourceDesc.MipLevels = 1;
	vertexResourceDesc.SampleDesc.Count = 1;
	// バッファの場合はこれにされる決まり
	vertexResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	HRESULT hr;

	ID3D12Resource* vertexResource = nullptr;

	hr = device->CreateCommittedResource(&uploadHeapProperties, D3D12_HEAP_FLAG_NONE,
										 &vertexResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
										 IID_PPV_ARGS(&vertexResource));
	assert(SUCCEEDED(hr));

	return vertexResource;
}

Microsoft::WRL::ComPtr<ID3D12Resource> DirectXCommon::CreateTextureResource(const DirectX::TexMetadata& metadate){
	//1. metadataを基にResourceの設定
	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Width = UINT(metadate.width);//Textureの幅
	resourceDesc.Height = UINT(metadate.height);//Textureの高さ
	resourceDesc.MipLevels = UINT(metadate.mipLevels);//mipmapの数
	resourceDesc.DepthOrArraySize = UINT(metadate.arraySize);//奥行 or 配列Textureの配列数
	resourceDesc.Format = metadate.format;//TextureのFormat
	resourceDesc.SampleDesc.Count = 1;//サンプリングカウント。1固定。
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION(metadate.dimension);//Textureの次元数。普段使っているのは二次元
	//2.利用するHeapの設定
	D3D12_HEAP_PROPERTIES heapPropaties{};
	heapPropaties.Type = D3D12_HEAP_TYPE_DEFAULT;//細かい設定を行う
	//3.Resourを生成する
	ID3D12Resource* resource = nullptr;
	HRESULT hr = device->CreateCommittedResource(
		&heapPropaties,//Heapの設定
		D3D12_HEAP_FLAG_NONE,//Heapの特殊な設定、特になし。
		&resourceDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&resource));
	assert(SUCCEEDED(hr));
	return resource;
}

ID3D12Resource* DirectXCommon::UploadTextureData(const Microsoft::WRL::ComPtr <ID3D12Resource>&  texture, const DirectX::ScratchImage& mipImages){
	if (!texture || mipImages.GetImageCount() == 0 || mipImages.GetImages() == nullptr) {
		return nullptr;
	}

	// 1. 転送するサブリソース情報を作成
	std::vector<D3D12_SUBRESOURCE_DATA> subresources;
	DirectX::PrepareUpload(
		device.Get(),                    // ← ComPtr なので .Get()
		mipImages.GetImages(),
		mipImages.GetImageCount(),
		mipImages.GetMetadata(),
		subresources);

	const UINT subresourceCount = static_cast<UINT>(subresources.size());
	if (subresourceCount == 0) {
		return nullptr;
	}

	// 2. 中間バッファ(UploadHeap)を作成
	uint64_t intermediateSize =
		GetRequiredIntermediateSize(texture.Get(), 0, subresourceCount);

	Microsoft::WRL::ComPtr<ID3D12Resource> intermediateResource =
		CreateBufferResource(intermediateSize);

	// 3. 中間バッファ → テクスチャ へのコピーコマンドを登録
	UpdateSubresources(
		commandList.Get(),               // ← ここも .Get()
		texture.Get(),
		intermediateResource.Get(),
		0, 0,
		subresourceCount,
		subresources.data());

	// 4. テクスチャを COPY_DEST から GENERIC_READ に遷移
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = texture.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;

	commandList->ResourceBarrier(1, &barrier);

	// 中間リソースを呼び出し元に返す
	// Detach で ComPtr から所有権を手放し、生ポインタだけ返す
	return intermediateResource.Detach();

}

void DirectXCommon::ResizeSwapChain(uint32_t width, uint32_t height) {
	if (width == 0 || height == 0) {
		return;
	}

	if (clientWidth_ == width && clientHeight_ == height) {
		return;
	}

	assert(swapChain);
	assert(device);

	// GPUが古いバックバッファを使い終わるまで待つ
	ExecuteCommandListAndWait();

	// 既存のバックバッファ参照を解放
	for (auto& resource : swapChainResources) {
		resource.Reset();
	}

	// DepthStencilも作り直すので解放
	if (depthStencilResource) {
		depthStencilResource->Release();
		depthStencilResource = nullptr;
	}

	clientWidth_ = width;
	clientHeight_ = height;

	swapChainDesc.Width = width;
	swapChainDesc.Height = height;

	HRESULT hr = swapChain->ResizeBuffers(
		static_cast<UINT>(swapChainResources.size()),
		width,
		height,
		swapChainDesc.Format,
		0
	);
	assert(SUCCEEDED(hr));

	// RTV / DSV / Viewport / Scissor を作り直す
	RenderTargetViewInitialize();
	CreateDepthStencilTextureResource();
	DepthStencilInitialize();
	ViewportRectInitialize();
	ScissorRectInitialize();
}

D3D12_GPU_DESCRIPTOR_HANDLE DirectXCommon::GetGPUDescriptorHandle(const Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& descriptorHeap, uint32_t descriptorSize, uint32_t index){

	D3D12_GPU_DESCRIPTOR_HANDLE handleGPU = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
	handleGPU.ptr += (descriptorSize * index);
	return handleGPU;

}

D3D12_CPU_DESCRIPTOR_HANDLE DirectXCommon::GetCPUDescriptorHandle(const Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& descriptorHeap, uint32_t descriptorSize, uint32_t index){

	D3D12_CPU_DESCRIPTOR_HANDLE handleCPU = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
	handleCPU.ptr += (descriptorSize * index);
	return handleCPU;

}

