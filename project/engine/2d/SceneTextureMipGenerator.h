// 役割: Scene候補の4096² RGBA8 sRGB mipを、mainのDIRECT frame末尾へ記録する。
#pragma once
#include "TextureManager.h"
#include <vector>
#include <string>
class DirectXCommon;

// Game/device寿命内のmain専用。単一DIRECT slotの完了前にはPrepareを再使用しない。
class SceneTextureMipGenerator final {
public:
	static constexpr bool kEnableGpuMips = true; // CPU比較はこの一か所だけfalseにする。
	// タイトル表示前に一度だけ試行。失敗はoptional経路を無効にしCPUへ戻す。
	bool Initialize(DirectXCommon& dx, std::string& diagnostic);
	bool IsReady() const noexcept { return ready_; }
	// GPU未記録時だけ。viewを確定し、全GPU参照をoutへ作る。確保例外は呼出元でCPU retry。
	bool Prepare(ID3D12Resource& texture, std::vector<Microsoft::WRL::ComPtr<IUnknown>>& out, std::string& diagnostic);
	// Prepare/全参照retain成功後だけ呼ぶ。割当/例外/送信/待機なし。全段をGENERIC_READにする。
	void Record(ID3D12GraphicsCommandList& list, ID3D12Resource& texture) const noexcept;
private:
	ID3D12Device* device_ = nullptr;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
	UINT increment_ = 0;
	bool attempted_ = false, ready_ = false;
};
