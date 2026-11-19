// 役割: 専用COPYキューで一件のバッチを提出し、完了までGPU資源を保持する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <d3d12.h>
#include <wrl/client.h>

// メインスレッド専用。破棄前にPollで完了を回収する。
// Pending/Unconfirmedのまま破棄しない。終了時はDrainForShutdownの証跡を所有側が判断する。
// deviceはこのbatchより長く生存させる。デストラクタはGPUを待たない。
class SceneCopyBatch final {
public:
	enum class Status { Idle, Recording, Pending, Completed, Faulted };
	enum class GpuState { NotSubmitted, Pending, Completed, DeviceRemoved, Unconfirmed };
	struct CompletionSnapshot {
		GpuState gpuState = GpuState::NotSubmitted;
		uint64_t signaledFence = 0; // 成功したqueue Signalのみ。0は未成功。
		uint64_t completedFence = 0; // 最後に観測した実値。UINT64_MAXはdevice除去。
		HRESULT error = S_OK;
		const char* failedOperation = nullptr;
	};

	SceneCopyBatch() = default;
	~SceneCopyBatch();
	SceneCopyBatch(const SceneCopyBatch&) = delete;
	SceneCopyBatch& operator=(const SceneCopyBatch&) = delete;

	bool Initialize(ID3D12Device& device, std::string& error);
	bool TryBegin(std::string& error);
	// 記録中だけ借用可能。呼び出し側はCOPY記録のみ行い、資源をSubmitまで保持する。
	ID3D12GraphicsCommandList* GetRecordingList() const;
	// 記録が参照する全資源を渡す。完了後も使う資源は呼び出し側でも所有する。
	bool Submit(std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> resources, std::string& error);
	// Completedは一度だけ返す。完了の回収後に次の記録を開始できる。
	Status Poll() noexcept;
	// PollのCompletedを消費しない。次の記録開始まで前提出の証跡を参照できる。
	// 記録開始自体がResetに失敗した場合は、新しいNotSubmittedの診断に置換する。
	CompletionSnapshot GetCompletionSnapshot() const noexcept { return completion_; }
	// 最終終了専用。新規記録を禁止し、Pendingの成功Signalだけを待つ。
	// Unconfirmedでは資源を保持する。終了/即時終了の判断は呼び出し側の責務。
	CompletionSnapshot DrainForShutdown() noexcept;
	// 記録だけを破棄する。提出済みのバッチはPollで回収する。
	void AbortRecording() noexcept;

private:
	bool Fail(std::string& error, const char* operation, HRESULT result);
	void RecordFailure(HRESULT result, const char* operation) noexcept;
	ID3D12Device* device_ = nullptr; // Initialize成功後だけ借用。
	CompletionSnapshot completion_;
	HANDLE shutdownEvent_ = nullptr;
	bool shuttingDown_ = false;
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
	Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
	Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
	std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> submittedResources_;
	std::uint64_t submittedValue_ = 0;
	Status status_ = Status::Idle;
};
