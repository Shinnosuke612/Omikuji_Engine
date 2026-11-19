// 役割: COPYバッチの提出、完了証跡、非待機Pollと終了専用回収を実装する。
#include "SceneCopyBatch.h"

#include <dxgi.h>
#include <limits>
#include <utility>

namespace {
constexpr auto kInvalidFenceValue = (std::numeric_limits<std::uint64_t>::max)();
}

SceneCopyBatch::~SceneCopyBatch() {
	// Closeに失敗した未提出listも、最後のresource参照より先に破棄する。
	list_.Reset();
	submittedResources_.clear();
	if (shutdownEvent_) CloseHandle(shutdownEvent_);
}

void SceneCopyBatch::RecordFailure(HRESULT result, const char* operation) noexcept {
	completion_.error = result;
	completion_.failedOperation = operation;
}

bool SceneCopyBatch::Fail(std::string& error, const char* operation, HRESULT result) {
	RecordFailure(result, operation);
	error = std::string(operation) + " failed (HRESULT " + std::to_string(result) + ")";
	return false;
}

bool SceneCopyBatch::Initialize(ID3D12Device& device, std::string& error) {
	if (queue_ || shuttingDown_) {
		error = "COPY batch is already initialized";
		return false;
	}
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
	Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
	Microsoft::WRL::ComPtr<ID3D12Fence> fence;
	D3D12_COMMAND_QUEUE_DESC desc{};
	desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
	HRESULT result = device.CreateCommandQueue(&desc, IID_PPV_ARGS(queue.GetAddressOf()));
	if (FAILED(result)) return Fail(error, "CreateCommandQueue", result);
	result = device.CreateCommandAllocator(desc.Type, IID_PPV_ARGS(allocator.GetAddressOf()));
	if (FAILED(result)) return Fail(error, "CreateCommandAllocator", result);
	result = device.CreateCommandList(0, desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(list.GetAddressOf()));
	if (FAILED(result)) return Fail(error, "CreateCommandList", result);
	result = list->Close();
	if (FAILED(result)) return Fail(error, "Initial command list Close", result);
	result = device.CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
	if (FAILED(result)) return Fail(error, "CreateFence", result);

	queue_ = std::move(queue);
	allocator_ = std::move(allocator);
	list_ = std::move(list);
	fence_ = std::move(fence);
	device_ = &device;
	completion_ = {};
	error.clear();
	return true;
}

bool SceneCopyBatch::TryBegin(std::string& error) {
	if (!queue_ || shuttingDown_ || status_ != Status::Idle) {
		error = "COPY batch is not ready for recording";
		return false;
	}
	HRESULT result = allocator_->Reset();
	if (SUCCEEDED(result)) {
		result = list_->Reset(allocator_.Get(), nullptr);
		if (SUCCEEDED(result)) {
			completion_ = {};
			status_ = Status::Recording;
			error.clear();
			return true;
		}
		status_ = Status::Faulted;
		completion_ = {}; // Reset失敗は新しい提出を行っていない。
		return Fail(error, "COPY command list Reset", result);
	}
	status_ = Status::Faulted;
	completion_ = {};
	return Fail(error, "COPY allocator Reset", result);
}

ID3D12GraphicsCommandList* SceneCopyBatch::GetRecordingList() const {
	return status_ == Status::Recording ? list_.Get() : nullptr;
}

bool SceneCopyBatch::Submit(std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> resources,
	std::string& error) {
	if (status_ != Status::Recording) {
		error = "COPY batch is not recording";
		return false;
	}
	// Close失敗時もlistの記録が参照する全資源を保持する。
	submittedResources_ = std::move(resources);
	const HRESULT closeResult = list_->Close();
	if (FAILED(closeResult)) {
		status_ = Status::Faulted;
		return Fail(error, "COPY command list Close", closeResult);
	}
	if (submittedValue_ == kInvalidFenceValue - 1) {
		status_ = Status::Faulted;
		RecordFailure(E_FAIL, "COPY fence value allocation");
		error = "COPY fence value exhausted";
		return false;
	}

	ID3D12CommandList* lists[] = { list_.Get() };
	queue_->ExecuteCommandLists(1, lists);
	completion_.gpuState = GpuState::Unconfirmed;
	++submittedValue_;
	const HRESULT signalResult = queue_->Signal(fence_.Get(), submittedValue_);
	if (FAILED(signalResult)) {
		status_ = Status::Faulted;
		if (FAILED(device_->GetDeviceRemovedReason())) completion_.gpuState = GpuState::DeviceRemoved;
		return Fail(error, "COPY queue Signal", signalResult);
	}
	status_ = Status::Pending;
	completion_.gpuState = GpuState::Pending;
	completion_.signaledFence = submittedValue_;
	error.clear();
	return true;
}

SceneCopyBatch::Status SceneCopyBatch::Poll() noexcept {
	if (status_ != Status::Pending) return status_;

	const auto completed = fence_->GetCompletedValue();
	completion_.completedFence = completed;
	if (completed == kInvalidFenceValue) {
		status_ = Status::Faulted;
		completion_.gpuState = GpuState::DeviceRemoved;
		const HRESULT removed = device_->GetDeviceRemovedReason();
		RecordFailure(FAILED(removed) ? removed : DXGI_ERROR_DEVICE_REMOVED, "COPY fence GetCompletedValue");
		return status_;
	}
	if (completed < completion_.signaledFence) {
		const HRESULT removed = device_->GetDeviceRemovedReason();
		if (FAILED(removed)) {
			status_ = Status::Faulted;
			completion_.gpuState = GpuState::DeviceRemoved;
			RecordFailure(removed, "COPY device removal");
		}
		return status_;
	}

	completion_.gpuState = GpuState::Completed;
	submittedResources_.clear();
	status_ = Status::Idle;
	return Status::Completed;
}

void SceneCopyBatch::AbortRecording() noexcept {
	if (status_ != Status::Recording) return;
	const HRESULT result = list_->Close();
	status_ = SUCCEEDED(result) ? Status::Idle : Status::Faulted;
	if (FAILED(result)) RecordFailure(result, "COPY abort Close");
}

SceneCopyBatch::CompletionSnapshot SceneCopyBatch::DrainForShutdown() noexcept {
	shuttingDown_ = true;
	if (status_ == Status::Recording) {
		AbortRecording();
		list_.Reset(); // 終了専用なので未提出記録を再利用しない。
	}
	if (status_ != Status::Pending) return completion_;
	if (Poll() != Status::Pending) return completion_;

	HRESULT waitError = S_OK;
	const char* waitOperation = nullptr;
	if (!shutdownEvent_) {
		shutdownEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!shutdownEvent_) {
			const DWORD lastError = GetLastError();
			waitError = lastError ? HRESULT_FROM_WIN32(lastError) : E_FAIL;
			waitOperation = "COPY shutdown CreateEvent";
		}
	}
	if (!waitOperation) {
		waitError = fence_->SetEventOnCompletion(completion_.signaledFence, shutdownEvent_);
		if (FAILED(waitError)) {
			waitOperation = "COPY shutdown SetEventOnCompletion";
		} else {
			const DWORD wait = WaitForSingleObject(shutdownEvent_, INFINITE);
			if (wait != WAIT_OBJECT_0) {
				const DWORD lastError = wait == WAIT_FAILED ? GetLastError() : 0;
				waitError = lastError ? HRESULT_FROM_WIN32(lastError) : E_FAIL;
				waitOperation = "COPY shutdown wait";
			}
		}
	}
	// Event成功だけでは完了扱いにしない。失敗時にも同じFenceを再確認する。
	const Status observed = Poll();
	if (observed == Status::Pending) {
		completion_.gpuState = GpuState::Unconfirmed;
		status_ = Status::Faulted;
		RecordFailure(waitOperation ? waitError : E_FAIL,
			waitOperation ? waitOperation : "COPY shutdown completion verification");
	} else if (waitOperation && completion_.gpuState != GpuState::DeviceRemoved) {
		RecordFailure(waitError, waitOperation); // 完了証跡は保持する。
	}
	// 登録済みEventは完了不明でも閉じず、資源とともに所有側の終了判断まで保持する。
	return completion_;
}
