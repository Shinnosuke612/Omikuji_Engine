// 役割: ゲーム全体の更新、描画、ポストプロセスとエディタ再生制御を実装する。
#include "Game.h"
#include "PrefabPreviewRenderer.h"
#include "transition/SceneTransitionController.h"
#include "../engine/transition/SwordShatterRenderer.h"
#include "scene/EditorBootstrap.h"
#include "scene/RuntimeBootstrap.h"
#include "scene/SceneFactory.h"
#include "scene/SceneStartupErrorScreen.h"
#include "../engine/scene/EditorSession.h"
#include "../engine/scene/SceneAssetService.h"
#include "../engine/scene/SceneCatalog.h"
#include "../engine/scene/SceneEntityQuery.h"
#include "../engine/scene/SceneExecutionContext.h"
#include "../engine/scene/BaseScene.h"
#include "../engine/scene/SceneTemplateRegistry.h"
#include "../engine/scene/SceneTransformResolver.h"
#include "../engine/scene/SceneValidator.h"

#include "../engine/base/DirectXCommon.h"
#include "../engine/base/SceneCopyBatch.h"
#include "../engine/base/BloomRenderer.h"
#include "../engine/base/FullscreenCopy.h"
#include "../engine/base/ImGuiManager.h"
#include "../engine/base/PostProcessSettingsEditor.h"
#include "../engine/base/RenderFormats.h"
#include "../engine/base/SceneRenderTarget.h"
#include "../engine/io/Input.h"
#include "../engine/2d/Sprite.h"
#include "../engine/2d/SpriteCommon.h"
#include "../engine/2d/TextureManager.h"
#include "../engine/2d/SceneTextureCopy.h"
#include "../engine/2d/SceneTextureMipGenerator.h"
#include <DirectXTex.h>
#include "../engine/3d/Camera.h"
#include "../engine/3d/Object3dCommon.h"
#include "../engine/3d/Object3d.h"
#include "../engine/3d/Model.h"
#include "../engine/3d/ModelManager.h"
#include "../engine/math/Math.h"
#include "../engine/3d/SrvManager.h"
#include "../engine/particle/ParticleCommon.h"
#include "../engine/particle/ParticleManager.h"
#include "../engine/particle/ParticleEmitter.h"
#include "../engine/debug/DebugRenderer.h"
#include "../engine/debug/EditorGridRenderer.h"
#include "../engine/utility/EditableResourcePath.h"
#include "../engine/utility/Logger.h"
#include "../engine/utility/StringUtility.h"
#include "../externals/imgui/imgui.h"
#include "../engine/project/ProjectCompatibilityProbe.h"
#include "../engine/project/ProjectCreationService.h"
#include "../engine/project/ProjectDescriptor.h"
#include "../engine/project/ProjectRegistry.h"
#include "../engine/project/ProjectAccessLock.h"
#include "../engine/project/ProjectSolutionGenerationService.h"
#include "../engine/project/VisualStudioLocator.h"
#include "../engine/project/WindowsProcess.h"

#include <Windows.h>
#include <Psapi.h>
#include <string_view>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <sstream>
#include <utility>
#include <variant>
#include <vector>
#include <iterator>
#include <cstring>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <new>
#include <random>
#include <stdexcept>
#include <unordered_map>

namespace {

    // falseはdevice除去による通常終了。提出済み・生存・完了不明だけ通常破棄を避ける。
    bool HandleTextureCopyCompletion(const SceneCopyBatch::CompletionSnapshot& result,
        ID3D12Device& device, uint64_t uploadId, bool& reported) noexcept {
        using GpuState = SceneCopyBatch::GpuState;
        if (result.gpuState != GpuState::DeviceRemoved && result.gpuState != GpuState::Unconfirmed) return true;
        const HRESULT removed = device.GetDeviceRemovedReason();
        const bool failFast = result.gpuState == GpuState::Unconfirmed && SUCCEEDED(removed);
        if (!reported) {
            reported = true;
            char message[512]{};
            std::snprintf(message, sizeof(message),
                "Texture COPY upload %llu failed at %s (HRESULT %ld, signaled %llu, completed %llu, device %ld).\n",
                static_cast<unsigned long long>(uploadId),
                result.failedOperation ? result.failedOperation : "unknown operation",
                static_cast<long>(result.error), static_cast<unsigned long long>(result.signaledFence),
                static_cast<unsigned long long>(result.completedFence), static_cast<long>(removed));
            try {
                Logger::Log(std::string(message)); // Loggerはファイル出力をflushする。
            } catch (...) {
                // 診断確保の失敗でGPU資源をstack unwindさせない。
                OutputDebugStringA(message);
                std::fputs(message, stderr);
                std::fflush(stderr);
            }
        }
        if (failFast) std::_Exit(EXIT_FAILURE);
        return false;
    }

    bool HandleDirectFrameResult(const DirectFrameResult& result) {
        if (result.CanContinue()) return true;
        Logger::Log(std::string("DIRECT frame failed at ") + result.failedOperation +
            " (HRESULT " + std::to_string(result.error) +
            ", signaled fence " + std::to_string(result.signaledFence) +
            ", completed fence " + std::to_string(result.completedFence) + ")\n");
        if (result.gpuState == DirectFrameGpuState::Unconfirmed) {
            // Loggerは書き込みをflush済み。GPU利用中の可能性がある資源の破棄を避ける。
            std::_Exit(EXIT_FAILURE);
        }
        return false;
    }

#if defined(_DEBUG) || defined(DEVELOPMENT)
    // Object3dとObject3D shaderのCB契約。Previewはshadow/skin資源を持たない。
    struct SpatialGhostMatrix { Matrix4x4 wvp, world; };
    struct SpatialGhostCamera { Vector3 position; float environment = 0; };
    struct SpatialGhostMaterial {
        Vector4 color{};
        int32_t lighting = 0;
        float emissive = 0, padding[2]{};
        Matrix4x4 uv = MakeIdentity4x4();
        Vector4 emissiveColor{1,1,1,1};
        float shininess = 40, dissolve = 0, edgeWidth = 0.08f, noiseScale = 6;
    };
    static_assert(sizeof(SpatialGhostMatrix)==128 && offsetof(SpatialGhostMatrix,world)==64);
    static_assert(sizeof(SpatialGhostCamera)==16 && offsetof(SpatialGhostCamera,environment)==12);
    static_assert(sizeof(SpatialGhostMaterial)==128 && offsetof(SpatialGhostMaterial,uv)==32 &&
        offsetof(SpatialGhostMaterial,emissiveColor)==96 && offsetof(SpatialGhostMaterial,shininess)==112);
    constexpr size_t spatialCbStride=D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    static_assert(sizeof(SpatialGhostMatrix)<=spatialCbStride && sizeof(SpatialGhostMaterial)<=spatialCbStride);

    bool CreateSpatialGhostBuffer(ID3D12Device* device, size_t bytes,
        Microsoft::WRL::ComPtr<ID3D12Resource>& buffer, void** mapped, std::string& issue) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=bytes; desc.Height=1;
        desc.DepthOrArraySize=1; desc.MipLevels=1; desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(buffer.ReleaseAndGetAddressOf()));
        if (SUCCEEDED(hr)) { D3D12_RANGE noRead{0,0}; hr=buffer->Map(0,&noRead,mapped); }
        if (FAILED(hr)) {
            issue="虚像Bufferを作成できません / Ghost Buffer creation failed: "+std::to_string(static_cast<long>(hr));
            buffer.Reset(); *mapped=nullptr; return false;
        }
        return true;
    }
    bool SpatialGhostFinite(const Matrix4x4& matrix) {
        for (const auto& row:matrix.m) for (float value:row) if (!std::isfinite(value)) return false;
        return true;
    }
#endif
	using SceneEntityQuery::FindEnabledComponent;
	using SceneEntityQuery::IsEntityActiveInHierarchy;
	using SceneTransformResolver::ResolveScene3DTransform;

	constexpr SceneBuildConfiguration GetCurrentBuildConfiguration() {
#if defined(_DEBUG)
		return SceneBuildConfiguration::Debug;
#elif defined(DEVELOPMENT)
		return SceneBuildConfiguration::Development;
#else
		return SceneBuildConfiguration::Release;
#endif
	}

	std::string ResolveProjectResourcePath(const std::filesystem::path& relativePath) {
		return StringUtility::ToUtf8(EditableResourcePath::Resolve(relativePath));
	}

	std::string MakeProjectCreationSnapshotIdentity(const ProjectCreationServiceSnapshot& snapshot) {
		std::ostringstream identity;
		identity << static_cast<int>(snapshot.state) << '|';
		if (snapshot.operation.has_value()) {
			identity << snapshot.operation->operationId << '|'
				<< static_cast<int>(snapshot.operation->state) << '|'
				<< snapshot.operation->exitCode;
		}
		identity << '|' << snapshot.statusMessage;
		return identity.str();
	}

	std::string MakeVisualStudioSnapshotIdentity(const std::vector<VisualStudioInstance>& instances) {
		std::ostringstream identity;
		for (const VisualStudioInstance& instance : instances) {
			identity << instance.instanceId << '|'
				<< instance.installationVersion << '|'
				<< instance.openable << '|'
				<< instance.buildReady << ';';
		}
		return identity.str();
	}

	bool NearlyEqual(float a, float b) {
		return std::abs(a - b) <= 0.000001f;
	}

	bool EqualVector(const Vector2& a, const Vector2& b) {
		return NearlyEqual(a.x, b.x) && NearlyEqual(a.y, b.y);
	}

	bool EqualVector(const Vector4& a, const Vector4& b) {
		return
			NearlyEqual(a.x, b.x) &&
			NearlyEqual(a.y, b.y) &&
			NearlyEqual(a.z, b.z) &&
			NearlyEqual(a.w, b.w);
	}

	bool IsPointInsideAabb(
		const Vector3& point,
		const Vector3& center,
		const Vector3& halfSize
	) {
		return
			std::abs(point.x - center.x) <= halfSize.x &&
			std::abs(point.y - center.y) <= halfSize.y &&
			std::abs(point.z - center.z) <= halfSize.z;
	}

	bool EqualPostProcessSettings(
		const ScenePostProcessSettings& a,
		const ScenePostProcessSettings& b
	) {
		return
			a.bloomEnabled == b.bloomEnabled &&
			NearlyEqual(a.baseExposure, b.baseExposure) &&
			a.toneMapMode == b.toneMapMode &&
			NearlyEqual(a.bloomThreshold, b.bloomThreshold) &&
			NearlyEqual(a.bloomSoftKnee, b.bloomSoftKnee) &&
			NearlyEqual(a.bloomIntensity, b.bloomIntensity) &&
			a.bloomBlurIterations == b.bloomBlurIterations &&
			a.bloomDownsampleScale == b.bloomDownsampleScale &&
			NearlyEqual(a.bloomBlurRadius, b.bloomBlurRadius) &&
			a.grayscaleEnabled == b.grayscaleEnabled &&
			a.vignetteEnabled == b.vignetteEnabled &&
			a.boxBlurEnabled == b.boxBlurEnabled &&
			a.gaussianBlurEnabled == b.gaussianBlurEnabled &&
			a.depthOfFieldEnabled == b.depthOfFieldEnabled &&
			a.motionBlurEnabled == b.motionBlurEnabled &&
			NearlyEqual(a.motionBlurStrength, b.motionBlurStrength) &&
			a.motionBlurSamples == b.motionBlurSamples &&
			NearlyEqual(a.motionBlurMaxRadius, b.motionBlurMaxRadius) &&
			a.radialBlurEnabled == b.radialBlurEnabled &&
			a.noiseEnabled == b.noiseEnabled &&
			a.dissolveEnabled == b.dissolveEnabled &&
			a.outlineEnabled == b.outlineEnabled &&
			a.underwaterEnabled == b.underwaterEnabled &&
			a.waterRefractionEnabled == b.waterRefractionEnabled &&
			a.pixelationEnabled == b.pixelationEnabled &&
			a.pixelationBlockSize == b.pixelationBlockSize &&
			a.chromaticAberrationEnabled == b.chromaticAberrationEnabled &&
			NearlyEqual(a.chromaticAberrationCenter.x, b.chromaticAberrationCenter.x) &&
			NearlyEqual(a.chromaticAberrationCenter.y, b.chromaticAberrationCenter.y) &&
			NearlyEqual(a.chromaticAberrationIntensity, b.chromaticAberrationIntensity) &&
			NearlyEqual(a.chromaticAberrationFalloff, b.chromaticAberrationFalloff) &&
			NearlyEqual(a.vignetteScale, b.vignetteScale) &&
			NearlyEqual(a.vignettePower, b.vignettePower) &&
			NearlyEqual(a.vignetteIntensity, b.vignetteIntensity) &&
			a.boxBlurKernelSize == b.boxBlurKernelSize &&
			NearlyEqual(a.boxBlurStrength, b.boxBlurStrength) &&
			a.gaussianBlurKernelSize == b.gaussianBlurKernelSize &&
			NearlyEqual(a.gaussianBlurSigma, b.gaussianBlurSigma) &&
			NearlyEqual(a.gaussianBlurStrength, b.gaussianBlurStrength) &&
			NearlyEqual(a.depthOfFieldFocusDistance, b.depthOfFieldFocusDistance) &&
			NearlyEqual(a.depthOfFieldFocusRange, b.depthOfFieldFocusRange) &&
			NearlyEqual(a.depthOfFieldBlurStrength, b.depthOfFieldBlurStrength) &&
			NearlyEqual(a.depthOfFieldNearStrength, b.depthOfFieldNearStrength) &&
			NearlyEqual(a.depthOfFieldFarStrength, b.depthOfFieldFarStrength) &&
			NearlyEqual(a.depthOfFieldMaxRadius, b.depthOfFieldMaxRadius) &&
			EqualVector(a.radialBlurCenter, b.radialBlurCenter) &&
			NearlyEqual(a.radialBlurWidth, b.radialBlurWidth) &&
			a.radialBlurSamples == b.radialBlurSamples &&
			a.noiseAnimate == b.noiseAnimate &&
			NearlyEqual(a.noiseAmount, b.noiseAmount) &&
			NearlyEqual(a.noiseScale, b.noiseScale) &&
			NearlyEqual(a.noiseSpeed, b.noiseSpeed) &&
			NearlyEqual(a.noiseSeed, b.noiseSeed) &&
			a.dissolveMaskIndex == b.dissolveMaskIndex &&
			NearlyEqual(a.dissolveThreshold, b.dissolveThreshold) &&
			NearlyEqual(a.dissolveEdgeWidth, b.dissolveEdgeWidth) &&
			EqualVector(a.dissolveEdgeColor, b.dissolveEdgeColor) &&
			a.outlineLuminanceEnabled == b.outlineLuminanceEnabled &&
			a.outlineDepthEnabled == b.outlineDepthEnabled &&
			NearlyEqual(a.outlineLuminanceWeight, b.outlineLuminanceWeight) &&
			NearlyEqual(a.outlineDepthWeight, b.outlineDepthWeight) &&
			NearlyEqual(a.outlineThreshold, b.outlineThreshold) &&
			NearlyEqual(a.outlineSoftness, b.outlineSoftness) &&
			NearlyEqual(a.outlineThickness, b.outlineThickness) &&
			EqualVector(a.outlineColor, b.outlineColor) &&
			EqualVector(a.underwaterTintColor, b.underwaterTintColor) &&
			NearlyEqual(a.underwaterIntensity, b.underwaterIntensity) &&
			NearlyEqual(a.underwaterFogDensity, b.underwaterFogDensity) &&
			NearlyEqual(a.underwaterDistortion, b.underwaterDistortion) &&
			EqualVector(a.waterRefractionTintColor, b.waterRefractionTintColor) &&
			NearlyEqual(a.waterRefractionStrength, b.waterRefractionStrength) &&
			NearlyEqual(a.waterRefractionEdgeSoftness, b.waterRefractionEdgeSoftness) &&
			NearlyEqual(a.waterRefractionTintStrength, b.waterRefractionTintStrength);
	}

	const char* ToProjectCreationOperationStateText(ProjectCreationOperationState state) {
		switch (state) {
		case ProjectCreationOperationState::InProgress: return "In Progress";
		case ProjectCreationOperationState::Failed: return "Failed";
		case ProjectCreationOperationState::RegistrationIncomplete: return "Registration Incomplete";
		case ProjectCreationOperationState::Complete: return "Complete";
		default: return "Unknown";
		}
	}

	const char* ToSolutionGenerationOperationStateText(SolutionGenerationOperationState state) {
		switch (state) {
		case SolutionGenerationOperationState::Staged: return "Pending Migration";
		case SolutionGenerationOperationState::CommitInProgress: return "Commit In Progress";
		case SolutionGenerationOperationState::RollbackInProgress: return "Rollback In Progress";
		case SolutionGenerationOperationState::RolledBack: return "Rolled Back";
		case SolutionGenerationOperationState::Complete: return "Complete";
		case SolutionGenerationOperationState::RecoveryRequired: return "Recovery Required";
		default: return "Unknown";
		}
	}
}

// PostDrawと切離しWindowの最終利用完了後だけ、この所有状態を更新/破棄する。
struct Game::PresentedTransitionState {
	using Controller = SceneTransitionController;
	// TEMP: 長いロードの原因を実測する。原因特定と改善前後の比較が済んだら撤去する。
	struct LoadTiming {
		using Clock = std::chrono::steady_clock;
		enum Stage { Setup, Document, Dependencies, Cpu, Upload, Runtime, Presentation, Count };
		Clock::time_point start = Clock::now(), last = start, lastReport = start;
		std::array<Clock::duration, Count> elapsed{};
		Stage stage = Setup;
		uint64_t frames = 0, images = 0, imageBytes = 0, bands = 0, stagingLimit = 0;
		uint64_t uploadUpdates = 0;
		Clock::duration ticketTime{};
		std::optional<Clock::time_point> admittedAt;
		// TEMP: 並列数比較後に撤去。Update全体の所要時間ではなく、同じbridge観測点の間隔。
		Clock::time_point previousUpdate{}, lastMemorySample{};
		Clock::duration maxUpdateInterval{};
		uint64_t updateIntervals = 0, intervalsOver33 = 0, intervalsOver50 = 0;
		uint64_t memorySamples = 0, memoryFailures = 0, workingSetStart = 0, privateStart = 0;
		uint64_t workingSetMax = 0, privateMax = 0;
		int64_t memoryStartOffsetUs = -1;
		// TEMP: 並行化比較後に撤去。要求受領基準であり、入力機器のクリック時刻ではない。
		int64_t firstDrawUs = -1, readyUs = -1, maxBridgeUs = 0;
		uint64_t bridgeOverruns = 0;
		bool firstDrawSubmitted = false;
		void Milestone(const char* event, uint64_t token, uint64_t serial = 0, uint64_t fence = 0) noexcept {
			try {
				const auto now = std::string_view(event) == "request_received" ? start : Clock::now();
				const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(now-start).count();
				Logger::Log(std::format("[LoadTiming] presentation_milestone token={} event={} request_to_us={} steady_us={} recording_serial={} signaled_fence={} time_semantics=host_observed\n",
					token,event,elapsedUs,std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count(),serial,fence));
			} catch (...) {}
		}
		void SampleProcessMemory(bool force = false) noexcept {
			const auto now = Clock::now();
			if (!force && lastMemorySample != Clock::time_point{} && now-lastMemorySample < std::chrono::milliseconds{250}) return;
			lastMemorySample = now;
			// OSの任意診断。kernel32のexportを解決し、追加import library/依存を要求しない。
			using Query = BOOL (WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
			const auto kernel = GetModuleHandleW(L"kernel32.dll");
			const auto query = kernel ? reinterpret_cast<Query>(GetProcAddress(kernel,"K32GetProcessMemoryInfo")) : nullptr;
			PROCESS_MEMORY_COUNTERS_EX counters{};
			counters.cb = static_cast<DWORD>(sizeof(counters));
			if (!query || !query(GetCurrentProcess(),reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters),counters.cb)) {
				++memoryFailures; return;
			}
			if (!memorySamples) {
				workingSetStart = counters.WorkingSetSize; privateStart = counters.PrivateUsage;
				memoryStartOffsetUs = std::chrono::duration_cast<std::chrono::microseconds>(now-start).count();
			}
			++memorySamples;
			workingSetMax = (std::max)(workingSetMax,uint64_t{counters.WorkingSetSize});
			privateMax = (std::max)(privateMax,uint64_t{counters.PrivateUsage});
		}
		void SampleLoadUpdate() noexcept {
			const auto now = Clock::now();
			if (previousUpdate != Clock::time_point{}) {
				const auto interval = now-previousUpdate;
				maxUpdateInterval = (std::max)(maxUpdateInterval,interval);
				++updateIntervals;
				if (interval > std::chrono::milliseconds{33}) ++intervalsOver33;
				if (interval > std::chrono::milliseconds{50}) ++intervalsOver50;
			}
			previousUpdate = now;
			SampleProcessMemory();
		}

		// 前回観測からの実時間を一つの区間だけに計上。uploadにはGPU待ちと次Update待ちを含む。
		void Observe(Stage next) noexcept {
			const auto now = Clock::now();
			elapsed[stage] += now - last;
			last = now;
			stage = next;
		}
		void ObserveScene(ScenePreparePhase phase) noexcept {
			switch (phase) {
			case ScenePreparePhase::ReadingDocument: Observe(Document); break;
			case ScenePreparePhase::ResolvingDependencies: Observe(Dependencies); break;
			case ScenePreparePhase::PreparingCpu: Observe(Cpu); break;
			case ScenePreparePhase::Uploading: Observe(Upload); break;
			case ScenePreparePhase::ConstructingRuntime: Observe(Runtime); break;
			default: Observe(Presentation); break;
			}
		}
		void Report(const char* event, const PresentedTransitionRequest& request, uint64_t token) noexcept {
			try {
				SampleProcessMemory(std::string_view{event} != "progress");
				Observe(stage);
				lastReport = last;
				const auto ms = [](Clock::duration d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); };
				Logger::Log(std::format("[LoadTiming] {} target={} token={} total_ms={} setup_ms={} document_ms={} deps_ms={} "
					"cpu_stage_ms={} upload_stage_ms={} runtime_ms={} presentation_ms={} updates={} images_admitted={} "
					"decoded_bytes_admitted={} copy_submits={} ticket_updates={} ticket_wall_ms={} staging_limit_bytes={} "
					"max_update_interval_us={} update_intervals={} intervals_over_33ms={} intervals_over_50ms={} "
					"process_memory_samples={} process_memory_failures={} memory_start_offset_us={} working_set_start={} private_bytes_start={} "
					"working_set_observed_max={} private_bytes_observed_max={} memory_scope=whole_process_sampled\n",
					event, request.destinationSceneId, token, ms(last-start), ms(elapsed[Setup]), ms(elapsed[Document]),
					ms(elapsed[Dependencies]), ms(elapsed[Cpu]), ms(elapsed[Upload]), ms(elapsed[Runtime]),
					ms(elapsed[Presentation]), frames, images, imageBytes, bands, uploadUpdates,
					ms(ticketTime + (admittedAt ? last - *admittedAt : Clock::duration{})), stagingLimit,
					std::chrono::duration_cast<std::chrono::microseconds>(maxUpdateInterval).count(),updateIntervals,intervalsOver33,intervalsOver50,
					memorySamples,memoryFailures,memoryStartOffsetUs,workingSetStart,privateStart,workingSetMax,privateMax));
				Logger::Log(std::format("[LoadTiming] overlap_presentation token={} end={} request_to_first_draw_us={} request_to_ready_us={} "
					"request_to_end_us={} max_bridge_us={} bridge_overruns={} timing_basis=request_received\n",
					token,event,firstDrawUs,readyUs,std::chrono::duration_cast<std::chrono::microseconds>(last-start).count(),maxBridgeUs,bridgeOverruns));
			} catch (...) { /* 診断の出力失敗で遷移を中断しない。 */ }
		}
	};
	struct LinkedRequest {
		LoadTiming timing;
		PresentedTransitionRequest request;
		SceneTransitionAsset::LoadLinkedGeneration generator;
		SceneTransitionAsset::LoadLinkedGenerationInput input;
		ScenePrepareToken token;
		uint64_t projectEpoch = 0;
		const SceneExecutionContext* context = nullptr; // Game/Managerの寿命内で同一性の照合にだけ使う。
		Controller::LoadLinkedLoadSnapshot load;
		uint64_t destinationRevision = 0, outputGeneration = 1;
		Controller::LoadLinkedDestinationState destinationState = Controller::LoadLinkedDestinationState::Pending;
		SceneInstanceId observedDestination = kInvalidSceneInstanceId;
		bool committed = false;
		// 同じmain提出で採用されたSRVと通知を、直後の実Present/Fenceへ結ぶ。
		struct Ticket {
			Controller::LoadLinkedPresented presented;
			uint64_t srv = 0, rendererGeneration = 0, outputGeneration = 0;
		};
		std::optional<Ticket> recorded, completed;
		SceneDestinationPresentation proof;
		uint64_t lastRetirableSerial = 0;
	};
	struct SeedSequence {
		uint32_t next = 0;
		uint64_t issued = 0;
		bool Resolve(const SceneTransitionAsset::LoadLinkedSeed& seed, uint32_t& out) {
			if (seed.mode == SceneTransitionAsset::LoadLinkedSeedMode::Fixed) { out = seed.value; return true; }
			if (issued == (uint64_t{1} << 32)) return false;
			if (issued == 0) next = static_cast<uint32_t>(std::random_device{}());
			out = next++;
			++issued;
			return true;
		}
	};
	// Managerの稼働ProjectはGameの寿命中固定。Play/Stopの要求世代とは別。
	static constexpr uint64_t preparedProjectEpoch = 1;
	std::filesystem::path preparedResourcesRoot;
	std::unique_ptr<LinkedRequest> linked;
	std::optional<ScenePrepareToken> linkedRetirement;
	uint64_t linkedRequestId = 0;
	SeedSequence linkedCutSeeds, linkedMotionSeeds;
	SceneMenuSource uiSource;
	SceneTransitionController controller;
	std::unique_ptr<SwordShatterRenderer> renderer;
	uint64_t frameSerial = 0, completedSerial = 0;
	uint32_t width = 0, height = 0;
	bool drawFailed = false, renderedTextureUsed = false;
	std::string issue;
};

// COPYから受け取った資源は未公開のまま保持し、次Updateの準備処理へ所有を渡す。
struct Game::TextureDirectReceipt {
	Microsoft::WRL::ComPtr<ID3D12Resource> resource;
	DirectX::TexMetadata metadata{};
	std::string textureKey;
	uint64_t jobGeneration = 0;
	uint64_t recordingSerial = 0;
	uint64_t signaledFence = 0, completedFence = 0;
	SceneTextureMipPolicy mipPolicy = SceneTextureMipPolicy::CpuComplete;
};

struct Game::TextureDirectState {
	enum class Phase { Empty, AwaitRecord, Recorded, ReceiptReady, Failed, RetryCpuMips };
	Phase phase = Phase::Empty;
	TextureDirectReceipt texture;
	DirectFrameResult frameResult;
	uint64_t serialCounter = 0, receiptSerial = 0;
	bool cancelled = false;
	std::string mipDiagnostic;
	std::chrono::steady_clock::time_point mipRecordedAt{};
	int64_t mipRecordCpuUs = 0;

	void ClearSlot() noexcept {
		texture = TextureDirectReceipt{};
		frameResult = DirectFrameResult{};
		receiptSerial = 0;
		cancelled = false;
		mipDiagnostic.clear(); mipRecordedAt = {}; mipRecordCpuUs = 0;
		phase = Phase::Empty;
		// serialCounterはGame存続中に再利用しない。
	}
};

struct Game::TextureUploadOwner {
	SceneInstanceId sourceInstanceId = kInvalidSceneInstanceId;
	uint64_t preparationGeneration = 0, projectEpoch = 0;
	bool Matches(const TextureUploadOwner& other) const noexcept {
		return sourceInstanceId == other.sourceInstanceId &&
			preparationGeneration == other.preparationGeneration && projectEpoch == other.projectEpoch;
	}
};

struct Game::PreparedTextureUpload {
	TextureUploadOwner owner;
	std::string textureKey;
	TextureManager::DecodedTexture texture;
	uint64_t maxStagingBytes = 0;
};

// 呼出し中だけ借用し、受付成功時に画像の所有をGameの搬送枠へ移す。
struct Game::TextureUploadInput {
	TextureUploadOwner owner;
	const std::string& textureKey;
	TextureManager::DecodedTexture& texture;
	uint64_t maxStagingBytes;
	SceneTextureMipPolicy mipPolicy = SceneTextureMipPolicy::CpuComplete;
};

struct Game::TextureUploadResult {
	enum class Outcome { Ready, Failed, Cancelled, RetryCpuMips };
	Outcome outcome = Outcome::Failed;
	uint64_t uploadId = 0;
	TextureUploadOwner owner;
	std::string textureKey;
	Microsoft::WRL::ComPtr<ID3D12Resource> resource;
	DirectX::TexMetadata metadata{};
	uint64_t recordingSerial = 0, signaledFence = 0, completedFence = 0;
	std::string diagnostic;
	const char* fallbackDiagnostic = nullptr; // 診断文字列の確保失敗時も失敗理由を失わない。
	bool gpuSrgbMipsPrepared = false;
};

// 一件の搬送を終端結果の受領まで専有。Sceneの生存は後続producerが照合する。
struct Game::TextureUploadState {
	enum class Phase { BeginCopy, Copying, AwaitDirectSlot, DirectPending, Ready, Failed, Cancelled, RetryCpuMips };
	Phase phase = Phase::BeginCopy;
	TextureUploadResult result;
	TextureManager::DecodedTexture cpu;
	uint64_t maxStagingBytes = 0;
	std::unique_ptr<SceneCopyBatch> batch;
	std::unique_ptr<SceneTextureCopy> copy; // 借用元batchより先に破棄する。
	bool cancelRequested = false, failurePending = false, gpuStopReported = false;
	bool abandonLookahead = false;
	uint64_t precedingUploadId = 0; // TEMP: 先行COPYの実提出をAと照合。再計測後に撤去。
	int64_t copyCompletedUs = 0;
	std::string error;
	SceneTextureMipPolicy mipPolicy = SceneTextureMipPolicy::CpuComplete;

	bool IsTerminal() const noexcept {
		return phase == Phase::Ready || phase == Phase::Failed || phase == Phase::Cancelled || phase == Phase::RetryCpuMips;
	}
	// GPU未提出・完了・device除去の確認後だけ呼ぶ。Pending/Unconfirmedには使わない。
	void FinishWithoutResource(TextureUploadResult::Outcome outcome) noexcept {
		copy.reset();
		batch.reset();
		cpu.image.Release();
		result.resource.Reset();
		result.metadata = {};
		result.recordingSerial = result.signaledFence = result.completedFence = 0;
		result.outcome = outcome;
		if (result.diagnostic.empty()) result.diagnostic.swap(error);
		phase = outcome == TextureUploadResult::Outcome::Cancelled ? Phase::Cancelled :
			outcome == TextureUploadResult::Outcome::RetryCpuMips ? Phase::RetryCpuMips : Phase::Failed;
	}
};

// 所有するprivate状態を含め、完全型がある場所で破棄を定義する。
Game::Game() = default;
Game::~Game() = default;

namespace {
// TEMP: 搬送待ち診断後に撤去。Scene ticketの既存観測を渡すだけで、Poll/所有移動は行わない。
void TraceSceneUpload(SceneManager* manager, const std::optional<SceneTextureUploadTicket>& ticket,
	const char* event, uint64_t sent = 0, uint64_t completed = 0, uint64_t serial = 0,
	int64_t observedUs = 0, int outcome = -1) noexcept {
	if (!manager || !ticket) return;
	SceneTextureTransferObservation value;
	value.token = ticket->key.token; value.uploadId = ticket->uploadId; value.event = event;
	value.signaledFence = sent; value.completedFence = completed; value.recordingSerial = serial;
	value.atUs = observedUs; value.outcome = outcome;
	manager->ObserveSceneTextureTransfer(value);
}
}

bool Game::QueueCopiedTextureForDirect(SceneTextureCopy& source,
	uint64_t jobGeneration, const std::string& textureKey, SceneTextureMipPolicy policy) {
	if (IsEndRequest() || jobGeneration == 0 || textureKey.empty() ||
		(textureDirect_ && textureDirect_->phase != TextureDirectState::Phase::Empty)) return false;
	try {
		// 失敗し得る確保はsourceの一回取得より前。拒否時は既存スロットも保持する。
		std::string ownedKey(textureKey);
		std::unique_ptr<TextureDirectState> newState;
		if (!textureDirect_) newState = std::make_unique<TextureDirectState>();
		auto& state = textureDirect_ ? *textureDirect_ : *newState;
		Microsoft::WRL::ComPtr<ID3D12Resource> resource;
		DirectX::TexMetadata metadata{};
		if (!source.TryTakeCopiedTexture(resource, metadata)) return false;
		state.texture.resource = std::move(resource);
		state.texture.metadata = metadata;
		state.texture.textureKey.swap(ownedKey);
		state.texture.jobGeneration = jobGeneration;
		state.texture.mipPolicy = policy;
		state.phase = TextureDirectState::Phase::AwaitRecord;
		if (newState) textureDirect_ = std::move(newState);
		return true;
	} catch (const std::bad_alloc&) {
		return false;
	} catch (const std::length_error&) {
		return false;
	}
}

void Game::CancelTextureDirect() noexcept {
	if (!textureDirect_) return;
	if (textureDirect_->phase == TextureDirectState::Phase::Recorded) {
		// 通常はRecord/PostDraw間に取消を挟まない。誤って呼ばれてもGPU参照は残す。
		textureDirect_->cancelled = true;
		return;
	}
	// 未提出失敗時の追加参照はDirectXCommonがlist破棄まで保持する。
	textureDirect_->ClearSlot();
}

bool Game::TryTakeTextureDirectReceipt(uint64_t expectedGeneration,
	const std::string& expectedKey, TextureDirectReceipt& out) noexcept {
	if (IsEndRequest() || !textureDirect_) return false;
	auto& state = *textureDirect_;
	const auto& result = state.frameResult;
	constexpr auto invalidFence = (std::numeric_limits<uint64_t>::max)();
	if (state.phase != TextureDirectState::Phase::ReceiptReady || state.cancelled ||
		!state.texture.resource || state.texture.jobGeneration != expectedGeneration ||
		state.texture.textureKey != expectedKey || state.texture.recordingSerial == 0 ||
		state.texture.recordingSerial != state.receiptSerial || !result.CanContinue() ||
		result.signaledFence == 0 || result.signaledFence >= invalidFence ||
		result.completedFence < result.signaledFence || result.completedFence >= invalidFence) return false;
	// string/ComPtrはmoveするので、成功確定後に割当でsourceを失う経路を作らない。
	out = std::move(state.texture);
	state.ClearSlot();
	return true;
}

bool Game::TryBeginTextureUpload(PreparedTextureUpload& input, uint64_t& outId) {
	if (!TryBeginTextureUpload({input.owner, input.textureKey, input.texture, input.maxStagingBytes}, outId)) return false;
	input.textureKey.clear();
	return true;
}

bool Game::TryBeginTextureUpload(const TextureUploadInput& input, uint64_t& outId) {
	if (textureUpload_ || materialCopyLookahead_ ||
		(textureDirect_ && textureDirect_->phase != TextureDirectState::Phase::Empty)) return false;
	return CreateTextureUpload(input, textureUpload_, outId);
}

bool Game::CreateTextureUpload(const TextureUploadInput& input, std::unique_ptr<TextureUploadState>& destination, uint64_t& outId) {
	constexpr auto invalidId = (std::numeric_limits<uint64_t>::max)();
	if (IsEndRequest() || textureUploadStopping_ || startupErrorScreen_ || !dxCommon_ || destination ||
		input.owner.sourceInstanceId == kInvalidSceneInstanceId || input.owner.preparationGeneration == 0 ||
		input.owner.projectEpoch == 0 || input.textureKey.empty() || input.maxStagingBytes == 0 ||
		nextTextureUploadId_ >= invalidId - 1) return false;
	try {
		// 全確保を入力画像のmoveより先に済ませる。ここではGPU APIを呼ばない。
		auto state = std::make_unique<TextureUploadState>();
		state->batch = std::make_unique<SceneCopyBatch>();
		state->copy = std::make_unique<SceneTextureCopy>();
		state->result.textureKey = input.textureKey;
		state->result.owner = input.owner;
		state->maxStagingBytes = input.maxStagingBytes;
		state->mipPolicy = input.mipPolicy;
		state->cpu = std::move(input.texture);
		state->result.uploadId = ++nextTextureUploadId_;
		outId = state->result.uploadId;
		destination = std::move(state);
		return true;
	} catch (const std::bad_alloc&) {
		return false;
	} catch (const std::length_error&) {
		return false;
	}
}

Game::SceneUploadAdmission Game::TryBeginSceneTextureUpload(
	const SceneTextureUploadLoan& loan, uint64_t maxStagingBytes, uint64_t& outId) {
	if (IsEndRequest() || textureUploadStopping_ || startupErrorScreen_ || !dxCommon_ ||
		!loan.texture || !loan.textureKey || loan.textureKey->empty() || maxStagingBytes == 0 ||
		loan.key.token.instanceId == kInvalidSceneInstanceId || loan.key.token.generation == 0 ||
		loan.key.sourceInstanceId == kInvalidSceneInstanceId || loan.key.projectEpoch == 0 || loan.key.entryId == 0 ||
		nextTextureUploadId_ >= (std::numeric_limits<uint64_t>::max)() - 1) {
		return SceneUploadAdmission::Rejected;
	}
	if (textureUpload_ || materialCopyLookahead_ || (textureDirect_ && textureDirect_->phase != TextureDirectState::Phase::Empty)) {
		return SceneUploadAdmission::Busy;
	}
	// 搬送Ownerは旧Scene。候補instance/entryはScene側と共通のticketで保持する。
	const TextureUploadInput input{{loan.key.sourceInstanceId, loan.key.token.generation, loan.key.projectEpoch},
		*loan.textureKey, *loan.texture, maxStagingBytes, loan.mipPolicy};
	return TryBeginTextureUpload(input, outId) ? SceneUploadAdmission::Accepted : SceneUploadAdmission::Rejected;
}

void Game::PumpScenePreparationBridge() {
	using UploadPhase = TextureUploadState::Phase;
	const auto bridgeStart = std::chrono::steady_clock::now();
	auto* linked = presentedTransition_ ? presentedTransition_->linked.get() : nullptr;
	if (sceneManager_) {
		if (sceneTextureUploadTicket_) {
			const auto ticket = *sceneTextureUploadTicket_;
			const bool timedTicket = linked && linked->timing.admittedAt &&
				linked->token.instanceId == ticket.key.token.instanceId && linked->token.generation == ticket.key.token.generation;
			if (timedTicket) ++linked->timing.uploadUpdates;
			if (sceneManager_->ShouldCancelSceneTextureUpload(ticket)) CancelTextureUpload(ticket.uploadId);
			const TextureUploadOwner owner{ticket.key.sourceInstanceId, ticket.key.token.generation, ticket.key.projectEpoch};
			TextureUploadResult result;
			if (TryTakeTextureUploadResult(ticket.uploadId, owner, result)) {
				if (timedTicket) {
					linked->timing.ticketTime += std::chrono::steady_clock::now() - *linked->timing.admittedAt;
					linked->timing.admittedAt.reset();
				}
				sceneTextureUploadTicket_.reset();
				SceneTextureUploadCompletion completion;
				completion.ticket = ticket;
				if (result.outcome == TextureUploadResult::Outcome::Ready) completion.outcome = SceneTextureUploadOutcome::Ready;
				else if (result.outcome == TextureUploadResult::Outcome::Cancelled) completion.outcome = SceneTextureUploadOutcome::Cancelled;
				else if (result.outcome == TextureUploadResult::Outcome::RetryCpuMips) completion.outcome = SceneTextureUploadOutcome::RetryCpuMips;
				completion.texture = {std::move(result.textureKey), std::move(result.resource), result.metadata};
				completion.texture.gpuSrgbMipsPrepared = result.gpuSrgbMipsPrepared;
				completion.recordingSerial = result.recordingSerial;
				completion.signaledFence = result.signaledFence;
				completion.completedFence = result.completedFence;
				completion.diagnostic = std::move(result.diagnostic);
				if (completion.diagnostic.empty() && result.fallbackDiagnostic) {
					try { completion.diagnostic = result.fallbackDiagnostic; }
					catch (const std::bad_alloc&) { /* 終端結果の返却を優先する。 */ }
				}
				sceneManager_->AcceptSceneTextureUpload(std::move(completion));
			}
		}
		// 既定値は準備の各段を一単位に抑える初期枠。搬送中の帯上限は受付時の値を使う。
		constexpr uint64_t mebibyte = 1024ull * 1024;
		ScenePrepareBudget activeBudget{{1, 1, mebibyte}, {4 * mebibyte, 4}, 512 * mebibyte, 256 * mebibyte, 2000};
		activeBudget.allowGpuMips = gpuMipGenerator_ && gpuMipGenerator_->IsReady();
		if (linked) {
			++linked->timing.frames;
			linked->timing.SampleLoadUpdate();
			linked->timing.stagingLimit = activeBudget.maxTextureStagingBytes;
			if (bridgeStart - linked->timing.lastReport >= std::chrono::seconds(10))
				linked->timing.Report("progress", linked->request, linked->token.instanceId);
		}
		const bool canPrepare = !IsEndRequest() && !textureUploadStopping_ && !startupErrorScreen_ &&
			(!executionContext_ || (!executionContext_->IsEditing() && !executionContext_->IsPaused()));
		const auto deadline = bridgeStart + std::chrono::microseconds{activeBudget.softCpuMicroseconds};
		constexpr unsigned maxPreparationPumps = 64;
		activeBudget.runtime.deadline = deadline;
		// TEMP: 実際に受付を試せるservice区間だけ記録。フレーム間の空白は待ちへ加算しない。
		const auto observeAdmission = [&](const char* event) noexcept {
			SceneTextureTransferObservation value;
			if (linked) value.token = linked->token;
			else if (sceneTextureUploadTicket_) value.token = sceneTextureUploadTicket_->key.token;
			value.event = event; value.canPrepare = canPrepare;
			value.deadlineUs = std::chrono::duration_cast<std::chrono::microseconds>(deadline.time_since_epoch()).count();
			value.foreignGpuOwner = (textureUpload_ && (!sceneTextureUploadTicket_ || textureUpload_->result.uploadId != sceneTextureUploadTicket_->uploadId)) ||
				(textureDirect_ && textureDirect_->phase != TextureDirectState::Phase::Empty &&
					(!sceneTextureUploadTicket_ || textureDirect_->texture.jobGeneration != sceneTextureUploadTicket_->uploadId));
			sceneManager_->ObserveSceneTextureTransfer(value);
		};
		const auto admitImage = [&] {
			// 時間切れなら借用前に見送り、Scene側のpending画像を次のUpdateに残す。
			SceneTextureUploadLoan loan;
			if (canPrepare && !sceneTextureUploadTicket_ && !textureUpload_ && !materialCopyLookahead_ &&
				(!textureDirect_ || textureDirect_->phase == TextureDirectState::Phase::Empty) &&
				std::chrono::steady_clock::now() < deadline &&
				sceneManager_->TryBorrowSceneTextureUpload(loan)) {
				uint64_t uploadId = 0;
				const auto decodedBytes = loan.texture ? loan.texture->image.GetPixelsSize() : 0; // Acceptedでmoveする前に観測。
				switch (TryBeginSceneTextureUpload(loan, activeBudget.maxTextureStagingBytes, uploadId)) {
				case SceneUploadAdmission::Accepted:
					if (linked && linked->token.instanceId == loan.key.token.instanceId && linked->token.generation == loan.key.token.generation) {
						linked->timing.admittedAt = std::chrono::steady_clock::now();
						++linked->timing.images;
						linked->timing.imageBytes += decodedBytes;
					}
					sceneTextureUploadTicket_ = SceneTextureUploadTicket{loan.key, uploadId};
					sceneManager_->CommitSceneTextureUpload(*sceneTextureUploadTicket_);
					break;
				case SceneUploadAdmission::Rejected:
					sceneManager_->RejectSceneTextureUpload(loan.key, "Scene texture upload admission failed.");
					break;
				case SceneUploadAdmission::Busy:
					break;
				}
			}
		};
		const auto tryStartLookahead = [&](bool submitFirstBand) {
			using DirectPhase = TextureDirectState::Phase;
			if (!canPrepare || IsEndRequest() || std::chrono::steady_clock::now() >= deadline ||
				!textureUpload_ || !sceneTextureUploadTicket_ || materialCopyLookahead_ ||
				textureUpload_->phase != UploadPhase::DirectPending || textureUpload_->cancelRequested ||
				textureUpload_->mipPolicy != SceneTextureMipPolicy::CpuComplete || !textureUpload_->copyCompletedUs ||
				textureUpload_->batch || textureUpload_->copy || !textureDirect_ ||
				(textureDirect_->phase != DirectPhase::AwaitRecord && textureDirect_->phase != DirectPhase::ReceiptReady) ||
				textureDirect_->cancelled || textureDirect_->texture.jobGeneration != sceneTextureUploadTicket_->uploadId ||
				textureUpload_->result.uploadId != sceneTextureUploadTicket_->uploadId ||
				sceneManager_->ShouldCancelSceneTextureUpload(*sceneTextureUploadTicket_)) return false;

			SceneTextureUploadLoan loan;
			if (!sceneManager_->TryBorrowMaterialCopyLookahead(*sceneTextureUploadTicket_, loan) ||
				std::chrono::steady_clock::now() >= deadline) return false;
			const auto bytes = loan.texture->image.GetPixelsSize();
			const TextureUploadInput input{{loan.key.sourceInstanceId, loan.key.token.generation, loan.key.projectEpoch},
				*loan.textureKey, *loan.texture, activeBudget.maxTextureStagingBytes, loan.mipPolicy};
			uint64_t id = 0;
			if (!CreateTextureUpload(input, materialCopyLookahead_, id)) return false;
			materialCopyLookaheadTicket_ = SceneTextureUploadTicket{loan.key, id};
			auto& lookahead = *materialCopyLookahead_;
			lookahead.precedingUploadId = sceneTextureUploadTicket_->uploadId;
			sceneManager_->CommitMaterialCopyLookahead(*materialCopyLookaheadTicket_, bytes);
			if (linked && linked->token.instanceId == loan.key.token.instanceId &&
				linked->token.generation == loan.key.token.generation) {
				++linked->timing.images;
				linked->timing.imageBytes += bytes;
			}
			if (std::chrono::steady_clock::now() < deadline) {
				PumpTextureUpload(lookahead, materialCopyLookaheadTicket_, true);
				// 翌Updateの再受付だけは初期化後も締切を照合し、AがReadyになる前に一帯を提出できる。
				if (submitFirstBand && lookahead.phase == UploadPhase::Copying &&
					std::chrono::steady_clock::now() < deadline) {
					PumpTextureUpload(lookahead, materialCopyLookaheadTicket_, true);
				}
			}
			return true;
		};
		bool acceptedLookaheadInSceneLoop = false;
		// 搬送可能になった直後に受付。候補構築でCOPY受付が次frameへ押し出されるのを避ける。
		observeAdmission("service_begin");
		for (unsigned pumpCount = 0; pumpCount < maxPreparationPumps; ++pumpCount) {
			const bool hasTime = canPrepare && std::chrono::steady_clock::now() < deadline;
			const auto result = sceneManager_->PumpScenePreparation(hasTime ? activeBudget : ScenePrepareBudget{});
			// 最初のScene処理でworker結果と貸出可否が更新されてから、一度だけ再評価する。
			if (pumpCount == 0 && hasTime) acceptedLookaheadInSceneLoop = tryStartLookahead(true);
			observeAdmission("sample");
			if (hasTime) admitImage();
			if (!hasTime || result != ScenePreparePumpResult::ContinueNow || std::chrono::steady_clock::now() >= deadline) break;
		}
		observeAdmission("service_end");
		if (materialCopyLookaheadTicket_ && !textureUpload_ &&
			sceneManager_->IsSceneTextureUploadHead(*materialCopyLookaheadTicket_)) {
			sceneTextureUploadTicket_ = std::exchange(materialCopyLookaheadTicket_, std::nullopt);
			textureUpload_ = std::move(materialCopyLookahead_);
			if (linked && linked->token.instanceId == sceneTextureUploadTicket_->key.token.instanceId &&
				linked->token.generation == sceneTextureUploadTicket_->key.token.generation)
				linked->timing.admittedAt = std::chrono::steady_clock::now();
			if (textureUpload_->abandonLookahead || sceneManager_->ShouldCancelSceneTextureUpload(*sceneTextureUploadTicket_))
				CancelTextureUpload(sceneTextureUploadTicket_->uploadId);
			else if (textureUpload_->phase == UploadPhase::AwaitDirectSlot && textureUpload_->copyCompletedUs) {
				const auto proof = textureUpload_->batch->GetCompletionSnapshot();
				TraceSceneUpload(sceneManager_, sceneTextureUploadTicket_, "copy_all_done", proof.signaledFence,
					proof.completedFence, 0, textureUpload_->copyCompletedUs);
			}
		}
		if (materialCopyLookaheadTicket_ && sceneManager_->ShouldCancelSceneTextureUpload(*materialCopyLookaheadTicket_))
			CancelMaterialCopyLookahead();

		const bool mayStartLookahead = textureUpload_ && textureUpload_->phase == UploadPhase::Copying;
		if (textureUpload_) PumpTextureUpload(*textureUpload_, sceneTextureUploadTicket_);
		if (materialCopyLookahead_) {
			// Sceneループで受理したBはここで再度進めない。未回収の取消は次Updateで進める。
			if (!acceptedLookaheadInSceneLoop)
				PumpTextureUpload(*materialCopyLookahead_, materialCopyLookaheadTicket_, true);
			if (materialCopyLookahead_->abandonLookahead && materialCopyLookahead_->IsTerminal()) {
				sceneManager_->RetireMaterialCopyLookahead(*materialCopyLookaheadTicket_);
				materialCopyLookahead_.reset();
				materialCopyLookaheadTicket_.reset();
			}
		} else if (mayStartLookahead) {
			// Aの全COPY完了と同じUpdateでは初期化まで。初回帯は次Update。
			tryStartLookahead(false);
		}
	} else if (textureUpload_) {
		PumpTextureUpload(*textureUpload_, sceneTextureUploadTicket_);
	}
	if (linked) {
		const auto bridgeUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-bridgeStart).count();
		linked->timing.maxBridgeUs = (std::max)(linked->timing.maxBridgeUs,bridgeUs);
		if (bridgeUs > 2000) ++linked->timing.bridgeOverruns;
	}
}

void Game::CancelTextureUpload(uint64_t uploadId) noexcept {
	if (!textureUpload_ || textureUpload_->result.uploadId != uploadId) return;
	auto& state = *textureUpload_;
	using Phase = TextureUploadState::Phase;
	if (state.phase == Phase::Failed || state.phase == Phase::Cancelled) return;
	state.cancelRequested = true;
	if (state.copy) state.copy->Cancel();
	if (state.phase == Phase::DirectPending || state.phase == Phase::Ready || state.phase == Phase::RetryCpuMips) {
		// Updateからのみ呼ぶため、DIRECTは未記録または前Drawで回収済み。
		CancelTextureDirect();
		state.FinishWithoutResource(TextureUploadResult::Outcome::Cancelled);
	}
}

void Game::CancelCurrentTextureUpload() noexcept {
	if (textureUpload_) CancelTextureUpload(textureUpload_->result.uploadId);
	CancelMaterialCopyLookahead();
}

void Game::CancelMaterialCopyLookahead() noexcept {
	if (!materialCopyLookahead_) return;
	auto& state = *materialCopyLookahead_;
	state.abandonLookahead = true;
	state.cancelRequested = true;
	if (state.copy) state.copy->Cancel();
	if (state.phase != TextureUploadState::Phase::Copying)
		state.FinishWithoutResource(TextureUploadResult::Outcome::Cancelled);
}

bool Game::TryTakeTextureUploadResult(uint64_t expectedId,
	const TextureUploadOwner& expectedOwner, TextureUploadResult& out) noexcept {
	if (IsEndRequest() || textureUploadStopping_ || !textureUpload_ || !textureUpload_->IsTerminal() ||
		textureUpload_->result.uploadId != expectedId || !textureUpload_->result.owner.Matches(expectedOwner)) return false;
	TraceSceneUpload(sceneManager_,sceneTextureUploadTicket_,"result_taken",0,0,0,0,static_cast<int>(textureUpload_->result.outcome));
	out = std::move(textureUpload_->result);
	textureUpload_.reset();
	return true;
}

void Game::PumpTextureUpload(TextureUploadState& state, const std::optional<SceneTextureUploadTicket>& ticket, bool copyOnly) {
	if (IsEndRequest() || textureUploadStopping_ || state.IsTerminal()) return;
	using Phase = TextureUploadState::Phase;
	using Outcome = TextureUploadResult::Outcome;
	// 例外や失敗後も、提出済みの参照はsnapshotを確認してから回収する。
	const auto fail = [&]() noexcept {
		state.failurePending = true;
		if (!state.result.fallbackDiagnostic) state.result.fallbackDiagnostic = "Texture upload failed";
		if (state.result.diagnostic.empty()) state.result.diagnostic.swap(state.error);
		const auto snapshot = state.batch ? state.batch->GetCompletionSnapshot() : SceneCopyBatch::CompletionSnapshot{};
		if (!HandleTextureCopyCompletion(snapshot, *dxCommon_->GetDevice(), state.result.uploadId, state.gpuStopReported)) {
			endRequest_ = true;
		}
		if (snapshot.gpuState == SceneCopyBatch::GpuState::Pending) {
			state.cancelRequested = true;
			state.copy->Cancel();
			state.phase = Phase::Copying;
			return;
		}
		const bool retry = state.mipPolicy == SceneTextureMipPolicy::GpuSrgbBox4096 && !state.cancelRequested &&
			SUCCEEDED(dxCommon_->GetDevice()->GetDeviceRemovedReason()) &&
			(snapshot.gpuState == SceneCopyBatch::GpuState::NotSubmitted || snapshot.gpuState == SceneCopyBatch::GpuState::Completed);
		state.FinishWithoutResource(retry ? Outcome::RetryCpuMips : Outcome::Failed);
	};
	try {
		if (state.phase == Phase::BeginCopy) {
			if (state.cancelRequested) {
				state.FinishWithoutResource(Outcome::Cancelled);
				return;
			}
			if (!state.batch->Initialize(*dxCommon_->GetDevice(), state.error) ||
				!state.copy->Begin(*dxCommon_->GetDevice(), *state.batch, std::move(state.cpu), state.maxStagingBytes, state.error, state.mipPolicy)) {
				fail();
				return;
			}
			state.phase = Phase::Copying;
			if (!copyOnly) TraceSceneUpload(sceneManager_,ticket,"copy_begin");
			return; // 資源作成と最初の帯提出を同じUpdateで連鎖させない。
		}
		if (state.phase == Phase::Copying) {
			if (state.cancelRequested) state.copy->Cancel();
			const auto previousFence = state.batch->GetCompletionSnapshot().signaledFence;
			const auto status = state.copy->Pump(state.error); // batch.Pollの消費者はcopyだけ。
			const auto currentFence = state.batch->GetCompletionSnapshot().signaledFence;
			if (currentFence > previousFence && ticket && presentedTransition_ && presentedTransition_->linked) {
				auto& linked = *presentedTransition_->linked;
				const auto& token = ticket->key.token;
				if (token.instanceId == linked.token.instanceId && token.generation == linked.token.generation)
					++linked.timing.bands;
			}
			// TEMP: 先行COPYの再計測後に撤去。帯の受付でなく実際のSignal更新を記録。
			if (state.precedingUploadId && !previousFence && currentFence && ticket) {
				const auto at = std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
				try {
					Logger::TryEnqueueUploadTrace(std::format("[MaterialCopyLookahead] token={} generation={} upload={} preceding_upload={} key=\"{}\" submitted_us={} signaled_fence={} before_promotion={}\n",
						ticket->key.token.instanceId, ticket->key.token.generation, ticket->uploadId, state.precedingUploadId,
						state.result.textureKey, at, currentFence, copyOnly));
				} catch (...) { Logger::NoteUploadTraceDrop(); }
			}
			if (status == SceneTextureCopy::Status::Pending) return;
			if (status == SceneTextureCopy::Status::Cancelled) {
				state.FinishWithoutResource(state.failurePending ? Outcome::Failed : Outcome::Cancelled);
				return;
			}
			if (status != SceneTextureCopy::Status::Copied) {
				fail();
				return;
			}
			const auto completedCopy = state.batch->GetCompletionSnapshot(); // cached証跡。追加Pollしない。
			if (completedCopy.gpuState == SceneCopyBatch::GpuState::Completed && completedCopy.signaledFence &&
				completedCopy.completedFence >= completedCopy.signaledFence &&
				completedCopy.completedFence != (std::numeric_limits<uint64_t>::max)()) {
				state.copyCompletedUs = std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
			}
			if (!copyOnly) TraceSceneUpload(sceneManager_,ticket,"copy_all_done",completedCopy.signaledFence,
				completedCopy.completedFence,0,state.copyCompletedUs);
			state.phase = Phase::AwaitDirectSlot;
		}
		if (state.phase == Phase::AwaitDirectSlot) {
			if (state.cancelRequested) {
				state.copy->Cancel();
				state.FinishWithoutResource(state.failurePending ? Outcome::Failed : Outcome::Cancelled);
				return;
			}
			if (copyOnly) return; // BのDIRECTは公開順で先頭になってから。
			if (textureDirect_ && textureDirect_->phase != TextureDirectState::Phase::Empty) return;
			if (!QueueCopiedTextureForDirect(*state.copy, state.result.uploadId, state.result.textureKey, state.mipPolicy)) {
				// 空き枠での失敗は終端へ返し、Scene側がticketを解放できるようにする。
				state.result.fallbackDiagnostic = state.mipPolicy == SceneTextureMipPolicy::GpuSrgbBox4096 ?
					"gpu_mip_direct_slot_prepare_failed" : "Texture DIRECT handoff failed";
				fail();
				return;
			}
			state.phase = Phase::DirectPending;
			TraceSceneUpload(sceneManager_,ticket,"direct_queued");
			state.copy.reset();
			state.batch.reset(); // 全COPY完了、resource所有はDIRECTスロットへ移転済み。
			return;
		}
		if (state.phase == Phase::DirectPending) {
			if (textureDirect_ && textureDirect_->phase == TextureDirectState::Phase::RetryCpuMips) {
				state.error.swap(textureDirect_->mipDiagnostic);
				state.result.fallbackDiagnostic = "gpu_mip_pre_record_retry";
				textureDirect_->ClearSlot();
				state.FinishWithoutResource(state.cancelRequested ? Outcome::Cancelled : Outcome::RetryCpuMips);
				return;
			}
			TextureDirectReceipt receipt;
			if (!TryTakeTextureDirectReceipt(state.result.uploadId, state.result.textureKey, receipt)) return;
			state.result.resource = std::move(receipt.resource);
			state.result.metadata = receipt.metadata;
			state.result.recordingSerial = receipt.recordingSerial;
			state.result.signaledFence = receipt.signaledFence;
			state.result.completedFence = receipt.completedFence;
			state.result.gpuSrgbMipsPrepared = receipt.mipPolicy == SceneTextureMipPolicy::GpuSrgbBox4096;
			state.result.outcome = Outcome::Ready;
			state.phase = Phase::Ready;
			TraceSceneUpload(sceneManager_,ticket,"result_ready",receipt.signaledFence,receipt.completedFence,receipt.recordingSerial);
		}
	} catch (const std::bad_alloc&) {
		state.result.fallbackDiagnostic = "Texture upload allocation failed";
		fail();
	} catch (const std::length_error&) {
		state.result.fallbackDiagnostic = "Texture upload capacity exceeded";
		fail();
	}
}

void Game::DrainTextureUploadForShutdown() {
	textureUploadStopping_ = true;
	CancelCurrentTextureUpload();
	const auto drain = [&](std::unique_ptr<TextureUploadState>& slot) {
		if (!slot) return;
		auto& state = *slot;
		if (state.batch) {
			const auto snapshot = state.batch->DrainForShutdown();
			if (!HandleTextureCopyCompletion(snapshot, *dxCommon_->GetDevice(), state.result.uploadId, state.gpuStopReported)) {
				endRequest_ = true;
			}
		}
		slot.reset(); // copyはbatchより先に破棄する。
	};
	drain(materialCopyLookahead_);
	CancelTextureDirect();
	drain(textureUpload_);
}

void Game::Initialize() {
	// 基底クラスの初期化処理
	Framework::Initialize();
	// optional GPU経路はタイトル前に準備し、ロード中のshader compileを避ける。
	const auto gpuInitStart = std::chrono::steady_clock::now();
	std::string gpuInitDiagnostic;
	try {
		gpuMipGenerator_ = std::make_unique<SceneTextureMipGenerator>();
		gpuMipGenerator_->Initialize(*dxCommon_,gpuInitDiagnostic);
	} catch (...) { gpuMipGenerator_.reset(); }
	// TEMP: GPU mip比較完了後にLoadTiming計測を撤去する。経路自体の初期化は残す。
	try {
		Logger::Log(std::format("[LoadTiming] gpu_mip_init enabled={} ready={} init_wall_us={} reason={}\n",
			SceneTextureMipGenerator::kEnableGpuMips,gpuMipGenerator_ && gpuMipGenerator_->IsReady(),
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-gpuInitStart).count(),gpuInitDiagnostic));
	} catch (...) { /* optional診断は初期化を失敗させない。 */ }

	const std::string sceneCatalogFilePath =
		ResolveProjectResourcePath("resources/scenes/scenes.json");
	auto showSceneStartupError = [this, &sceneCatalogFilePath](
		SceneStartupErrorKind errorKind,
		const std::string& detail
	) {
		Logger::Log(detail + "\n");
		startupErrorScreen_ = new SceneStartupErrorScreen();
		startupErrorScreen_->Show(
			winApp_->GetHwnd(),
			errorKind,
			detail,
			sceneCatalogFilePath
		);
	};

	sceneCatalog_ = new SceneCatalog();
	std::string sceneCatalogError;
	const bool sceneCatalogLoaded = sceneCatalog_->Load(
		sceneCatalogFilePath,
		sceneCatalogError
	);
	if (!sceneCatalogLoaded) {
		showSceneStartupError(
			SceneStartupErrorKind::CatalogLoad,
			sceneCatalogError
		);
		return;
	}
	std::vector<SceneValidationIssue> sceneValidationIssues;
	if (!SceneValidator::ValidateCatalog(
		*sceneCatalog_,
		sceneValidationIssues
	)) {
		showSceneStartupError(
			SceneStartupErrorKind::CatalogValidation,
			SceneValidator::FormatIssues(sceneValidationIssues)
		);
		return;
	}
	if (!sceneValidationIssues.empty()) {
		Logger::Log(SceneValidator::FormatIssues(sceneValidationIssues) + "\n");
	}
	const SceneDescriptor* startScene = sceneCatalog_->GetStartScene();
	if (!startScene) {
		showSceneStartupError(
			SceneStartupErrorKind::StartScene,
			"startScene does not reference a registered Scene"
		);
		return;
	}
	sceneFactory_ = new SceneFactory();
	std::string bootstrapError;
	const SceneStartupMode startupMode =
		sceneCatalog_->GetStartupMode(GetCurrentBuildConfiguration());
	if (startupMode == SceneStartupMode::Editor) {
		editorBootstrap_ = new EditorBootstrap();
		if (!editorBootstrap_->Initialize(
			sceneCatalog_,
			sceneFactory_,
			*startScene,
			bootstrapError
		)) {
			showSceneStartupError(
				SceneStartupErrorKind::EditorStartup,
				bootstrapError
			);
			return;
		}
		sceneManager_ = editorBootstrap_->GetSceneManager();
		editorSession_ = editorBootstrap_->GetEditorSession();
		executionContext_ = editorSession_;
		sceneAssetService_ = editorBootstrap_->GetSceneAssetService();
		sceneTemplateRegistry_ =
			editorBootstrap_->GetSceneTemplateRegistry();
	} else {
		runtimeBootstrap_ = new RuntimeBootstrap();
		if (!runtimeBootstrap_->Initialize(
			sceneCatalog_,
			sceneFactory_,
			*startScene,
			bootstrapError
		)) {
			showSceneStartupError(
				SceneStartupErrorKind::RuntimeStartup,
				bootstrapError
			);
			return;
		}
		sceneManager_ = runtimeBootstrap_->GetSceneManager();
		executionContext_ = sceneManager_->GetExecutionContext();
	}

#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editorSession_) {
		imguiManager_->SetEditorSession(editorSession_);
		imguiManager_->SetSceneCatalog(sceneCatalog_);
		imguiManager_->SetSceneManager(sceneManager_);
		imguiManager_->SetSceneTemplateRegistry(sceneTemplateRegistry_);
		InitializeProjectLauncher();
	}
#endif

	sceneRenderTarget_ = new SceneRenderTarget();
	SceneRenderTarget::Desc sceneTargetDesc{};
	sceneTargetDesc.width = dxCommon_->GetClientWidth();
	sceneTargetDesc.height = dxCommon_->GetClientHeight();
	sceneTargetDesc.format = RenderFormats::kSceneHdrFormat;
	sceneTargetDesc.createDepth = true;
	sceneTargetDesc.clearColor[0] = 0.1f;
	sceneTargetDesc.clearColor[1] = 0.2f;
	sceneTargetDesc.clearColor[2] = 0.8f;
	sceneTargetDesc.clearColor[3] = 1.0f;
	auto initializeRequiredRenderTarget = [this, &showSceneStartupError](
		SceneRenderTarget* renderTarget,
		const SceneRenderTarget::Desc& desc,
		const char* name
	) {
		if (renderTarget->Initialize(dxCommon_, srvManager_, desc)) {
			return true;
		}
		showSceneStartupError(
			SceneStartupErrorKind::RenderResource,
			std::string("Failed to allocate SRV descriptors for ") + name
		);
		return false;
	};
	if (!initializeRequiredRenderTarget(
		sceneRenderTarget_, sceneTargetDesc, "the scene render target"
	)) {
		return;
	}
	for (SceneRenderTarget*& renderTarget : postProcessRenderTargets_) {
		renderTarget = new SceneRenderTarget();
		SceneRenderTarget::Desc postTargetDesc{};
		postTargetDesc.width = dxCommon_->GetClientWidth();
		postTargetDesc.height = dxCommon_->GetClientHeight();
		postTargetDesc.format = RenderFormats::kDisplayFormat;
		postTargetDesc.createDepth = false;
		postTargetDesc.clearColor[0] = 0.0f;
		postTargetDesc.clearColor[1] = 0.0f;
		postTargetDesc.clearColor[2] = 0.0f;
		postTargetDesc.clearColor[3] = 1.0f;
		if (!initializeRequiredRenderTarget(
			renderTarget, postTargetDesc, "a post-process render target"
		)) {
			return;
		}
	}

	textOverlayRenderTarget_ = new SceneRenderTarget();
	SceneRenderTarget::Desc textOverlayDesc{};
	textOverlayDesc.width = dxCommon_->GetClientWidth();
	textOverlayDesc.height = dxCommon_->GetClientHeight();
	textOverlayDesc.format = RenderFormats::kDisplayFormat;
	textOverlayDesc.createDepth = false;
	textOverlayDesc.clearColor[3] = 1.0f;
	if (!initializeRequiredRenderTarget(
		textOverlayRenderTarget_, textOverlayDesc, "the text overlay render target"
	)) {
		return;
	}
	motionBlurHistoryRenderTarget_ = new SceneRenderTarget();
	SceneRenderTarget::Desc motionBlurHistoryDesc{};
	motionBlurHistoryDesc.width = dxCommon_->GetClientWidth();
	motionBlurHistoryDesc.height = dxCommon_->GetClientHeight();
	motionBlurHistoryDesc.format = RenderFormats::kDisplayFormat;
	motionBlurHistoryDesc.createDepth = false;
	motionBlurHistoryDesc.clearColor[3] = 1.0f;
	if (!initializeRequiredRenderTarget(
		motionBlurHistoryRenderTarget_,
		motionBlurHistoryDesc,
		"the motion blur history render target"
	)) {
		return;
	}
	foregroundComposeRenderTarget_ = new SceneRenderTarget();
	SceneRenderTarget::Desc foregroundComposeDesc{};
	foregroundComposeDesc.width = dxCommon_->GetClientWidth();
	foregroundComposeDesc.height = dxCommon_->GetClientHeight();
	foregroundComposeDesc.format = RenderFormats::kSceneHdrFormat;
	foregroundComposeDesc.createDepth = false;
	foregroundComposeDesc.clearColor[0] = 0.0f;
	foregroundComposeDesc.clearColor[1] = 0.0f;
	foregroundComposeDesc.clearColor[2] = 0.0f;
	foregroundComposeDesc.clearColor[3] = 1.0f;
	if (!initializeRequiredRenderTarget(
		foregroundComposeRenderTarget_,
		foregroundComposeDesc,
		"the foreground compose render target"
	)) {
		return;
	}

#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editorSession_) {
		SceneRenderTarget::Desc uiDesc = sceneTargetDesc;
		uiDesc.width = 1; uiDesc.height = 1;
		uiDesc.clearColor[0] = uiDesc.clearColor[1] = uiDesc.clearColor[2] = 0.12f;
		uiEditorSceneTarget_ = new SceneRenderTarget();
		if (!initializeRequiredRenderTarget(uiEditorSceneTarget_, uiDesc, "the UI editing scene target")) return;
		uiDesc.createDepth = false;
		uiEditorForegroundTarget_ = new SceneRenderTarget();
		if (!initializeRequiredRenderTarget(uiEditorForegroundTarget_, uiDesc, "the UI editing foreground target")) return;
		uiDesc.format = RenderFormats::kDisplayFormat;
		for (auto*& target : uiEditorPostTargets_) {
			target = new SceneRenderTarget();
			if (!initializeRequiredRenderTarget(target, uiDesc, "a UI editing post-process target")) return;
		}
		uiEditorOverlayTarget_ = new SceneRenderTarget();
		if (!initializeRequiredRenderTarget(uiEditorOverlayTarget_, uiDesc, "the UI editing overlay target")) return;
		uiEditorCopy_ = new FullscreenCopy();
		uiEditorCopy_->Initialize(dxCommon_);
		uiEditorBloom_ = new BloomRenderer();
		if (!uiEditorBloom_->Initialize(dxCommon_, srvManager_)) { delete uiEditorBloom_; uiEditorBloom_ = nullptr; }
	}
#endif
	fullscreenCopy_ = new FullscreenCopy();
	fullscreenCopy_->Initialize(dxCommon_);
	bloomRenderer_ = new BloomRenderer();
	if (!bloomRenderer_->Initialize(dxCommon_, srvManager_)) {
		delete bloomRenderer_;
		bloomRenderer_ = nullptr;
	}

#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editorSession_) {
		modelPreviewRenderTarget_ = new SceneRenderTarget();
		SceneRenderTarget::Desc modelPreviewDesc{};
		modelPreviewDesc.width = 512;
		modelPreviewDesc.height = 512;
		modelPreviewDesc.format = RenderFormats::kSceneHdrFormat;
		modelPreviewDesc.createDepth = true;
		modelPreviewDesc.clearColor[0] = 0.035f;
		modelPreviewDesc.clearColor[1] = 0.04f;
		modelPreviewDesc.clearColor[2] = 0.05f;
		modelPreviewDesc.clearColor[3] = 1.0f;
		if (!modelPreviewRenderTarget_->Initialize(
			dxCommon_, srvManager_, modelPreviewDesc
		)) {
			delete modelPreviewRenderTarget_;
			modelPreviewRenderTarget_ = nullptr;
		} else {
		modelPreviewCamera_ = new Camera();
		modelPreviewCamera_->SetOrbitMode(true);
		modelPreviewCamera_->SetFovY(0.7f);
		modelPreviewCamera_->SetAspectRatio(1.0f);
		modelPreviewCamera_->SetNearClip(0.01f);
		modelPreviewCamera_->SetFarClip(10000.0f);
		modelPreviewObject_ = new Object3d();
		modelPreviewObject_->Initialize(Object3dCommon::GetInstance());
		modelPreviewObject_->SetCamera(modelPreviewCamera_);
		// Asset previews must not depend on the active scene's light bindings.
		modelPreviewObject_->SetEnableLighting(false);
		}
		prefabPreviewRenderer_ = new PrefabPreviewRenderer();
		if (!prefabPreviewRenderer_->Initialize(dxCommon_, srvManager_)) {
			delete prefabPreviewRenderer_;
			prefabPreviewRenderer_ = nullptr;
		}
	}
#endif
	baseExposure_ = bloomParameters_.exposure;
	currentExposure_ = baseExposure_;

	TextureManager::GetInstance()->LoadTexture("resources/noise0.png");
	TextureManager::GetInstance()->LoadTexture("resources/noise1.png");

	if (executionContext_) {
		const SceneDocument* document = sceneManager_
			? sceneManager_->GetActiveSceneDocument()
			: &executionContext_->GetActiveDocument();
		ApplyPostProcessSettings(document->GetPostProcessSettings());
		appliedPostProcessRevision_ = document->GetRevision();
		appliedPostProcessSourceKey_ = GetActivePostProcessSourceKey();
	}
}

void Game::Update() {
	if (IsEndRequest()) return;
	// 基底クラスの更新処理
	Framework::Update();
	// Scene更新スタックを抜けた次Frameでのみ停止する。ImGui BeginFrameより前に確定する。
	const bool gameExitRequested = sceneManager_ && sceneManager_->ConsumeGameExitRequest();
	if (gameExitRequested && !IsEndRequest()) {
		CancelPresentedTransition();
#if defined(_DEBUG) || defined(DEVELOPMENT)
		if (editorSession_) {
			if (!editorSession_->IsEditing()) editorSession_->Stop();
		} else
#endif
		{ endRequest_ = true; }
	}
	if (!presentedTransition_) presentedTransition_ = std::make_unique<PresentedTransitionState>();
	// UIのStopはここへqueueし、Runtime Documentを切り替える前に取消する。
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (imguiManager_ && imguiManager_->ConsumePresentedTransitionStopRequest()) {
		CancelPresentedTransition();
		if (editorSession_) editorSession_->Stop();
	}
#endif
	++presentedTransition_->frameSerial;
	if (presentedTransition_->renderer) presentedTransition_->renderer->BeginFrame(presentedTransition_->frameSerial);
	if (presentedTransition_->drawFailed) CancelPresentedTransition(presentedTransition_->issue);

	// F1/F2をImGuiの入力処理より先に確定し、Inspectorへ開始キーを流さない。
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editorSession_ && !gameExitRequested && !startupErrorScreen_ && !IsEndRequest()) {
		Input* input = Input::GetInstance();
		if (input->TriggerKey(DIK_F1)) {
			if (editorSession_->IsEditing()) {
				editorSession_->Play();
				if (imguiManager_) imguiManager_->RequestGameInputFocus();
			} else {
				CancelPresentedTransition();
				editorSession_->Stop();
			}
		}
		if (input->TriggerKey(DIK_F2)) {
			if (editorSession_->IsPlaying()) {
				editorSession_->Pause();
			} else if (editorSession_->IsPaused()) {
				editorSession_->Resume();
				if (imguiManager_) imguiManager_->RequestGameInputFocus();
			}
		}
	}
	// 起動エラー画面も同じFrame対を使うため、早期returnより前に開始する。
	if (imguiManager_ && !IsEndRequest()) imguiManager_->BeginFrame();
#endif


	if (startupErrorScreen_) {
		if (startupErrorScreen_->IsCloseRequested()) {
			endRequest_ = true;
		}
		return;
	}
	if (IsEndRequest()) {
		return;
	}

#if defined(_DEBUG) || defined(DEVELOPMENT)
	UpdateProjectLauncher();
#endif
	const float deltaTime = GetDeltaTime();

	if (executionContext_) {
		const SceneDocument* document = sceneManager_
			? sceneManager_->GetActiveSceneDocument()
			: &executionContext_->GetActiveDocument();
		const std::string sourceKey = GetActivePostProcessSourceKey();
		if (
			sourceKey != appliedPostProcessSourceKey_ ||
			document->GetRevision() != appliedPostProcessRevision_
		) {
			const ScenePostProcessSettings& settings =
				document->GetPostProcessSettings();
			if (!EqualPostProcessSettings(CapturePostProcessSettings(), settings)) {
				ApplyPostProcessSettings(settings);
			}
			appliedPostProcessRevision_ = document->GetRevision();
			appliedPostProcessSourceKey_ = sourceKey;
		}
	}


	const bool isEditingAtFrameStart =
		executionContext_ && executionContext_->IsEditing();
	const bool continuedEditing = isEditingAtFrameStart && editorWasEditingLastFrame_;
	if (continuedEditing) {
		editorCameraSnapshot_ = CaptureCameraSnapshot();
	}

#if defined(_DEBUG) || defined(DEVELOPMENT)
	DebugRenderer::GetInstance()->Clear();
	if (editorSession_) {

	ImGui::Begin("Post Process Stack");
	ScenePostProcessSettings postProcessSettings = CapturePostProcessSettings();
	bool postProcessSettingsChanged = false;
	ImGui::Text("Active: %d", GetEnabledPostEffectCount());
	ImGui::SameLine();
	if (ImGui::SmallButton("Disable All")) {
		postProcessSettings.bloomEnabled = false;
		postProcessSettings.grayscaleEnabled = false;
		postProcessSettings.vignetteEnabled = false;
		postProcessSettings.boxBlurEnabled = false;
		postProcessSettings.gaussianBlurEnabled = false;
		postProcessSettings.radialBlurEnabled = false;
		postProcessSettings.depthOfFieldEnabled = false;
		postProcessSettings.noiseEnabled = false;
		postProcessSettings.dissolveEnabled = false;
		postProcessSettings.outlineEnabled = false;
		postProcessSettings.underwaterEnabled = false;
		postProcessSettings.waterRefractionEnabled = false;
		postProcessSettings.pixelationEnabled = false;
		postProcessSettings.chromaticAberrationEnabled = false;
		postProcessSettingsChanged = true;
	}
	ImGui::TextDisabled("Applied from top to bottom");
	ImGui::Text("Current Exposure: %.2f", currentExposure_);
	ImGui::Separator();
	postProcessSettingsChanged |= DrawPostProcessSettingsEditor(postProcessSettings);
	if (ImGui::SmallButton("Reset Noise Time")) {
		noiseTime_ = 0.0f;
		resetViewEffectTimeRequested_ = true;
	}
	if (postProcessSettingsChanged) {
		ApplyPostProcessSettings(postProcessSettings);
	}

	ImGui::End();

	StorePostProcessSettingsToDocument();

	Camera* editorCamera =
		Object3dCommon::GetInstance()->GetDefaultCamera();
	if (
		editorCamera &&
		imguiManager_->GetSceneViewHeight() > 0
	) {
		editorCamera->SetAspectRatio(
			static_cast<float>(imguiManager_->GetSceneViewWidth()) /
			static_cast<float>(imguiManager_->GetSceneViewHeight())
		);
	}
	}
#endif

#if defined(_DEBUG) || defined(DEVELOPMENT)
	ProcessSceneAssetRequests();
	ProcessSceneInstanceRequests();
	ProcessProjectSettingsRequests();

	std::string requestedSceneId;
	bool discardUnsavedChanges = false;
	if (
		editorSession_ &&
		imguiManager_ &&
		imguiManager_->ConsumeOpenSceneRequest(
			requestedSceneId,
			discardUnsavedChanges
		)
	) {
		CancelPresentedTransition();
		const SceneDescriptor* requestedScene = sceneCatalog_
			? sceneCatalog_->Find(requestedSceneId)
			: nullptr;
		if (!requestedScene) {
			Logger::Log(
				"Requested Edit Scene is not registered: " +
				requestedSceneId + "\n"
			);
		} else if (!editorSession_->OpenEditScene(
			requestedScene->id,
			requestedScene->displayName,
			requestedScene->filePath,
			discardUnsavedChanges
		)) {
			Logger::Log(
				"Edit Scene could not be opened: " +
				requestedScene->filePath + "\n"
			);
		} else {
			imguiManager_->NotifyEditSceneOpened();
		}
	}
#endif

	const bool reloadRequested =
		editorSession_ && editorSession_->ConsumeReloadRequest();
	const bool switchingEditScene =
		editorSession_ && editorSession_->IsEditing() &&
		sceneManager_->GetCurrentSceneId() != editorSession_->GetEditSceneId();
	const bool preserveEditorCamera =
		editorSession_ && editorSession_->IsEditing() &&
		reloadRequested && !switchingEditScene;
	const CameraSnapshot editorCameraSnapshot = preserveEditorCamera
		? (
			continuedEditing
				? CaptureCameraSnapshot()
				: editorCameraSnapshot_
		)
		: CameraSnapshot{};
	if (reloadRequested) {
		CancelPresentedTransition();
		sceneManager_->EndTransitionHold();
		sceneManager_->ReloadCurrentScene();
	}
	if (executionContext_ && executionContext_->IsEditing()) {
		// Editor Stop/OpenではRuntime用の保持をEdit Sceneへ持ち越さない。
		sceneManager_->EndTransitionHold();
	}
	const bool paused =
		executionContext_ && executionContext_->IsPaused();
	UpdatePresentedTransitionBeforeScene(deltaTime, paused);
	PumpScenePreparationBridge();
	if (IsEndRequest()) return;
	const bool pauseStarted =
		paused && !wasPausedLastFrame_;
	const bool pauseEnded =
		!paused && wasPausedLastFrame_;
	if (pauseStarted) {
		BeginPauseDebugCamera();
	} else if (pauseEnded) {
		EndPauseDebugCamera();
	}
#if defined(_DEBUG) || defined(DEVELOPMENT)
	sceneManager_->SetEditorUIView(imguiManager_ ? imguiManager_->GetSceneUIEditorView() : SceneUIEditorView{});
#endif
	// UI入力許可だけを渡す。既存Gameplayの入力経路/消費規則は変更しない。
	bool menuInputAllowed = winApp_ && GetForegroundWindow() == winApp_->GetHwnd() && !IsIconic(winApp_->GetHwnd()) &&
        IsWindowVisible(winApp_->GetHwnd()) && IsPresentedTransitionDrawable();
	if (auto* input = Input::GetInstance()) menuInputAllowed = menuInputAllowed && !input->PushKey(DIK_LMENU) && !input->PushKey(DIK_RMENU);
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editorSession_) menuInputAllowed = menuInputAllowed && imguiManager_ && imguiManager_->IsGameInputOwned() && ImGuiManager::IsSceneViewInputActive() &&
		!ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive();
#endif
	SceneMenuPointerFrame menuPointer;
	RECT client{};
	const bool clientValid = winApp_ && GetClientRect(winApp_->GetHwnd(), &client);
	Vector4 pointerRect{float(client.left), float(client.top), float(client.right), float(client.bottom)};
	bool pointerViewValid = clientValid;
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editorSession_) {
		POINT clientOrigin{};
		pointerViewValid = imguiManager_ && imguiManager_->IsSceneViewRenderable() &&
			winApp_ && ClientToScreen(winApp_->GetHwnd(), &clientOrigin);
		if (pointerViewValid) pointerRect = {imguiManager_->GetSceneViewMinX() - clientOrigin.x,
			imguiManager_->GetSceneViewMinY() - clientOrigin.y, imguiManager_->GetSceneViewMaxX() - clientOrigin.x,
			imguiManager_->GetSceneViewMaxY() - clientOrigin.y};
	}
#endif
	const float pointerWidth = pointerRect.z - pointerRect.x, pointerHeight = pointerRect.w - pointerRect.y;
	menuPointer.allowed = menuInputAllowed && pointerViewValid && pointerWidth > 0 && pointerHeight > 0 &&
		!paused && !sceneManager_->IsTransitionHeld();
	if (pointerRect.x != menuPointerRect_.x || pointerRect.y != menuPointerRect_.y ||
		pointerRect.z != menuPointerRect_.z || pointerRect.w != menuPointerRect_.w || menuPointerAllowed_ != menuPointer.allowed) {
		++menuPointerViewportSerial_; menuPointerRect_ = pointerRect; menuPointerAllowed_ = menuPointer.allowed;
	}
	menuPointer.viewportSerial = menuPointerViewportSerial_;
	if (auto* input = Input::GetInstance()) {
		menuPointer.leftDown = input->PushMouse(Input::MouseButton::Left);
		if (menuPointer.allowed) {
			const auto position = input->GetMousePosition();
			menuPointer.normalizedPosition = {(position.x - pointerRect.x) / pointerWidth, (position.y - pointerRect.y) / pointerHeight};
			menuPointer.inside = position.x >= pointerRect.x && position.x < pointerRect.z && position.y >= pointerRect.y && position.y < pointerRect.w;
		}
	} else menuPointer.allowed = false;
	sceneManager_->SetMenuPointerFrame(menuPointer);
	sceneManager_->SetMenuInputAllowed(menuInputAllowed);
	if (!paused) {
		if (sceneManager_->IsTransitionHeld()) {
			sceneManager_->UpdateTransitionHold();
		} else {
			sceneManager_->PrepareGameUpdate(deltaTime);
			// 同FrameのMenu Openを反映し、新しいゲーム音の再生より先に停止理由を渡す。
			if (audio_) audio_->SetGamePause(sceneManager_->IsGamePaused(), sceneManager_->ShouldPauseBgm());
			sceneManager_->Update(deltaTime);
		}
	} else {
		sceneManager_->UpdatePaused();
	}
	if (audio_) audio_->SetGamePause(sceneManager_->IsGamePaused(), sceneManager_->ShouldPauseBgm());
	if (noiseAnimate_ && !sceneManager_->IsGamePaused()) noiseTime_ += deltaTime * noiseSpeed_;
	UpdateViewEffectClock();
	ApplyRuntimePostProcessSettings();
    sceneManager_->AdvanceReplayPresentation(deltaTime,!paused && IsPresentedTransitionDrawable() && IsWindowVisible(winApp_->GetHwnd()) &&
        !sceneManager_->IsTransitionHeld() && !sceneManager_->IsGamePaused() &&
        !(presentedTransition_ && presentedTransition_->controller.IsActive()));
	UpdatePresentedTransitionAfterScene();
	// Scene切替とMenuのPause判定後に反映し、Title復帰やPause開始のFrameで解除する。
	if (Input* input = Input::GetInstance()) {
		const auto& sceneId = sceneManager_->GetCurrentSceneId();
		const bool gameplayScene = sceneId == "gameplay" || sceneId == "gameplay_legacy";
		const bool altHeld = input->PushKey(DIK_LMENU) || input->PushKey(DIK_RMENU);
		const bool foreground = winApp_ && GetForegroundWindow() == winApp_->GetHwnd() &&
			IsWindowVisible(winApp_->GetHwnd()) && !IsIconic(winApp_->GetHwnd());
		bool captureGameInput = gameplayScene && executionContext_ && executionContext_->IsPlaying() &&
			!sceneManager_->IsGamePaused() && !sceneManager_->IsPointerMenuOpen() && foreground && !altHeld;
#if defined(_DEBUG) || defined(DEVELOPMENT)
		if (editorSession_) captureGameInput = captureGameInput && imguiManager_ && imguiManager_->IsGameInputOwned();
#endif
		// 表示と拘束のOS処理はInputに任せ、Editorの入力所有権とは分離する。
		input->SetCursorCapture(captureGameInput);
	}
	if (preserveEditorCamera) {
		RestoreCameraSnapshot(editorCameraSnapshot);
	}
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (imguiManager_) imguiManager_->UpdateSceneSpatialPreview(deltaTime);
	if (imguiManager_) imguiManager_->UpdatePlayerCombatPreview(deltaTime);
	PrepareSceneSpatialPreview();
#endif
	editorWasEditingLastFrame_ =
		executionContext_ && executionContext_->IsEditing();
	wasPausedLastFrame_ = paused;

	ParticleManager::ExposureFlashEvent exposureFlash{};
	while (ParticleManager::GetInstance()->ConsumeExposureFlashEvent(exposureFlash)) {
		currentExposure_ = (std::max)(
			currentExposure_,
			exposureFlash.exposure
		);
		exposureReturnSpeed_ = (std::max)(
			exposureFlash.returnSpeed,
			0.01f
		);
	}

	const float exposureLerp =
		std::clamp(exposureReturnSpeed_ * deltaTime, 0.0f, 1.0f);
	currentExposure_ += (baseExposure_ - currentExposure_) * exposureLerp;
	bloomParameters_.exposure = currentExposure_;

}

void Game::ProcessSceneAssetRequests() {
	if (!imguiManager_ || !sceneAssetService_ || !editorSession_ ||
		!sceneCatalog_) {
		return;
	}
	SceneAssetRequest request{};
	if (!imguiManager_->ConsumeSceneAssetRequest(request)) {
		return;
	}
	CancelPresentedTransition();

	std::string errorMessage;
	bool succeeded = false;
	if (!editorSession_->IsEditing()) {
		errorMessage = "Scene assets can only be managed in Edit Mode";
	} else if (editorSession_->GetEditDocument().IsDirty()) {
		errorMessage = "Save the active Scene before managing Scene assets";
	} else {
		switch (request.operation) {
		case SceneAssetOperation::Create:
			succeeded = sceneAssetService_->CreateScene(
				request.sceneId,
				request.displayName,
				request.assetPath,
				request.templateId,
				errorMessage
			);
			break;
		case SceneAssetOperation::Duplicate:
			succeeded = sceneAssetService_->DuplicateScene(
				request.sourceSceneId,
				request.sceneId,
				request.displayName,
				request.assetPath,
				errorMessage
			);
			break;
		case SceneAssetOperation::Rename:
			if (request.sceneId != editorSession_->GetEditSceneId()) {
				errorMessage = "Only the active Scene can be renamed";
				break;
			}
			succeeded = sceneAssetService_->RenameScene(
				request.sceneId,
				request.displayName,
				request.assetPath,
				errorMessage
			);
			break;
		case SceneAssetOperation::Delete:
			if (request.sceneId == editorSession_->GetEditSceneId()) {
				errorMessage = "The active Scene cannot be deleted";
				break;
			}
			succeeded = sceneAssetService_->DeleteScene(
				request.sceneId,
				errorMessage
			);
			break;
		default:
			errorMessage = "Unknown Scene asset operation";
			break;
		}
	}

	const bool assetChanged = succeeded;
	if (succeeded && request.operation != SceneAssetOperation::Delete) {
		if (!OpenRegisteredEditScene(request.sceneId, true, errorMessage)) {
			succeeded = false;
			if (errorMessage.empty()) {
				errorMessage =
					"Scene asset was updated but could not be opened";
			}
		}
	}
	if (!succeeded && !errorMessage.empty()) {
		Logger::Log(errorMessage + "\n");
	}
	if (assetChanged) {
		imguiManager_->NotifySceneAssetOperationResult(true, {});
	}
	if (!succeeded) {
		imguiManager_->NotifySceneAssetOperationResult(false, errorMessage);
	}
}

void Game::ProcessSceneInstanceRequests() {
	if (!imguiManager_ || !sceneManager_ || !editorSession_) {
		return;
	}
	SceneInstanceRequest request{};
	if (!imguiManager_->ConsumeSceneInstanceRequest(request)) {
		return;
	}
	CancelPresentedTransition();

	bool succeeded = false;
	std::string message;
	if (editorSession_->IsEditing()) {
		message = "Scene Instance operations are available in Play Mode only";
	} else {
		switch (request.operation) {
		case SceneInstanceOperation::LoadAdditive:
			succeeded = sceneManager_->LoadSceneAdditive(
				request.sceneId,
				request.instanceKey
			) != kInvalidSceneInstanceId;
			message = succeeded
				? "Additive Scene load queued."
				: "Additive Scene could not be queued.";
			break;
		case SceneInstanceOperation::Unload:
			succeeded = sceneManager_->UnloadScene(request.instanceId);
			message = succeeded
				? "Scene unload queued."
				: "Scene Instance could not be unloaded.";
			break;
		case SceneInstanceOperation::SetActive:
			succeeded = sceneManager_->SetActiveScene(request.instanceId);
			message = succeeded
				? "Active Scene changed."
				: "Active Scene could not be changed.";
			break;
		case SceneInstanceOperation::SetPersistent:
			succeeded = sceneManager_->SetScenePersistent(
				request.instanceId,
				request.persistent
			);
			message = succeeded
				? "Persistent setting changed."
				: "Persistent setting could not be changed.";
			break;
		default:
			message = "Unknown Scene Instance operation.";
			break;
		}
	}
	if (!succeeded) {
		Logger::Log(message + "\n");
	}
	imguiManager_->NotifySceneInstanceOperationResult(succeeded, message);
}

void Game::ProcessProjectSettingsRequests() {
	if (!imguiManager_ || !sceneCatalog_) {
		return;
	}
	std::string requestedStartSceneId;
	if (imguiManager_->ConsumeStartSceneRequest(requestedStartSceneId)) {
		const SceneCatalog catalogSnapshot = *sceneCatalog_;
		std::string errorMessage;
		const bool succeeded = sceneCatalog_->SetStartScene(
			requestedStartSceneId,
			errorMessage
		) && sceneCatalog_->Save(errorMessage);
		if (!succeeded) {
			*sceneCatalog_ = catalogSnapshot;
			if (!errorMessage.empty()) {
				Logger::Log(errorMessage + "\n");
			}
		}
		imguiManager_->NotifyProjectSettingsResult(
			succeeded,
			errorMessage
		);
	}

	SceneBuildConfiguration configuration{};
	SceneStartupMode mode{};
	if (imguiManager_->ConsumeStartupModeRequest(configuration, mode)) {
		const SceneCatalog catalogSnapshot = *sceneCatalog_;
		std::string errorMessage;
		const bool succeeded = sceneCatalog_->SetStartupMode(
			configuration,
			mode,
			errorMessage
		) && sceneCatalog_->Save(errorMessage);
		if (!succeeded) {
			*sceneCatalog_ = catalogSnapshot;
			if (!errorMessage.empty()) {
				Logger::Log(errorMessage + "\n");
			}
		}
		imguiManager_->NotifyProjectSettingsResult(
			succeeded,
			errorMessage
		);
	}
}

void Game::InitializeProjectLauncher() {
	if (projectLauncherInitialized_ || !imguiManager_) {
		return;
	}
	projectLauncherInitialized_ = true;
	projectRegistry_ = new ProjectRegistry();
	std::string errorMessage;
	if (!projectRegistry_->Load(errorMessage)) {
		projectLauncherStatusMessage_ = "Project Registry: " + errorMessage;
		RefreshProjectLauncherView();
		return;
	}

	const std::filesystem::path currentProjectRoot =
		EditableResourcePath::FindProjectRoot();
	const std::filesystem::path templateSourceRoot =
		currentProjectRoot.empty() ? std::filesystem::path{} : currentProjectRoot.parent_path();
	std::error_code filesystemError;
	if (
		projectRegistry_->GetTemplateSourceRoot().empty() &&
		std::filesystem::is_regular_file(
			templateSourceRoot / L"project" / L"tools" / L"New-GameProject.ps1",
			filesystemError
		)
	) {
		projectRegistry_->SetTemplateSourceRoot(templateSourceRoot);
	}

	projectCreationService_ = new ProjectCreationService(*projectRegistry_);
	projectSolutionGenerationService_ = new ProjectSolutionGenerationService(*projectRegistry_);
	visualStudioLocator_ = new VisualStudioLocator();
	const std::filesystem::path discoveryLog =
		projectRegistry_->GetRegistryPath().parent_path() / L"visual-studio-discovery.log";
	if (!visualStudioLocator_->StartDiscovery(discoveryLog, errorMessage)) {
		projectLauncherStatusMessage_ = "Visual Studio discovery: " + errorMessage;
	} else if (visualStudioLocator_->UsedKnownRootFallback()) {
		projectLauncherStatusMessage_ = "Visual Studio discovery is using known install roots.";
	}
	if (!projectCreationService_->Reconcile(errorMessage) && projectLauncherStatusMessage_.empty()) {
		projectLauncherStatusMessage_ = "Project recovery: " + errorMessage;
	}
	if (!projectSolutionGenerationService_->DiscoverPreviews(errorMessage) && projectLauncherStatusMessage_.empty()) {
		projectLauncherStatusMessage_ = "Solution preview discovery: " + errorMessage;
	} else if (!projectSolutionGenerationService_->ReconcileRecovery(errorMessage) && projectLauncherStatusMessage_.empty()) {
		projectLauncherStatusMessage_ = "Solution generation recovery: " + errorMessage;
	}
	projectLauncherViewDirty_ = true;
	RefreshProjectLauncherView();
	projectLauncherViewDirty_ = false;
}

void Game::UpdateProjectLauncher() {
	if (!projectLauncherInitialized_ || !projectRegistry_ || !projectCreationService_ || !projectSolutionGenerationService_ || !visualStudioLocator_) {
		return;
	}
	const std::string statusBefore = projectLauncherStatusMessage_;
	const std::string creationBefore = MakeProjectCreationSnapshotIdentity(projectCreationService_->GetSnapshot());
	const std::string visualStudioBefore = MakeVisualStudioSnapshotIdentity(visualStudioLocator_->GetInstances());
	std::string errorMessage;
	if (!visualStudioLocator_->PollDiscovery(errorMessage)) {
		projectLauncherStatusMessage_ = "Visual Studio discovery: " + errorMessage;
	}
	if (!projectCreationService_->Update(errorMessage)) {
		projectLauncherStatusMessage_ = "Project creation: " + errorMessage;
	}
	if (pendingCreateOpenSolution_ &&
		projectCreationService_->GetSnapshot().state == ProjectCreationServiceState::Complete &&
		projectCreationService_->GetSnapshot().operation.has_value()) {
		const std::filesystem::path descriptorPath =
			projectCreationService_->GetSnapshot().operation->finalProjectRoot / L"game.project.json";
		if (!LaunchProjectSolution(descriptorPath, pendingCreateVisualStudioInstanceId_, errorMessage)) {
			projectLauncherStatusMessage_ = "Project created, but Solution could not be opened: " + errorMessage;
		}
		pendingCreateOpenSolution_ = false;
		pendingCreateVisualStudioInstanceId_.clear();
	}
	if (creationBefore != MakeProjectCreationSnapshotIdentity(projectCreationService_->GetSnapshot()) ||
		visualStudioBefore != MakeVisualStudioSnapshotIdentity(visualStudioLocator_->GetInstances())) {
		projectLauncherViewDirty_ = true;
	}
	const bool sceneDirty = editorSession_ && editorSession_->GetEditDocument().IsDirty();
	const bool prefabDirty = imguiManager_->HasUnsavedPrefabChanges();
	const bool playBlocked = editorSession_ && !editorSession_->IsEditing();
	if (!projectLauncherDynamicStateInitialized_ ||
		projectLauncherSceneDirty_ != sceneDirty ||
		projectLauncherPrefabDirty_ != prefabDirty ||
		projectLauncherPlayBlocked_ != playBlocked) {
		projectLauncherViewDirty_ = true;
		projectLauncherDynamicStateInitialized_ = true;
		projectLauncherSceneDirty_ = sceneDirty;
		projectLauncherPrefabDirty_ = prefabDirty;
		projectLauncherPlayBlocked_ = playBlocked;
	}
	if (statusBefore != projectLauncherStatusMessage_) {
		projectLauncherViewDirty_ = true;
	}
	if (ProcessProjectLauncherRequest()) {
		projectLauncherViewDirty_ = true;
	}
	if (projectLauncherViewDirty_) {
		RefreshProjectLauncherView();
		projectLauncherViewDirty_ = false;
	}
}

void Game::RefreshProjectLauncherView() {
	if (!imguiManager_) {
		return;
	}
	ProjectLauncherView view{};
	view.statusMessage = projectLauncherStatusMessage_;
	if (!projectRegistry_ || !projectCreationService_ || !projectSolutionGenerationService_ || !visualStudioLocator_) {
		imguiManager_->SetProjectLauncherView(view);
		return;
	}
	view.templateSourceRoot = StringUtility::ToUtf8(projectRegistry_->GetTemplateSourceRoot());
	view.creationInProgress =
		projectCreationService_->GetSnapshot().state == ProjectCreationServiceState::GeneratorRunning;
	view.switchBlockedBySceneDirty = editorSession_ && editorSession_->GetEditDocument().IsDirty();
	view.switchBlockedByPrefabDirty = imguiManager_->HasUnsavedPrefabChanges();
	view.switchBlockedByPlayMode = editorSession_ && !editorSession_->IsEditing();

	const std::optional<VisualStudioInstance> preferredInstance =
		visualStudioLocator_->SelectPreferred({ projectRegistry_->GetPreferredVisualStudioInstanceId(), false, {}, {} });
	bool hasOpenableVisualStudio = false;
	for (const VisualStudioInstance& instance : visualStudioLocator_->GetInstances()) {
		ProjectLauncherVisualStudioView instanceView{};
		instanceView.instanceId = instance.instanceId;
		instanceView.displayName = instance.displayName;
		instanceView.detail = instance.installationVersion + (instance.buildReady ? " (Build Ready)" : " (Openable only)");
		instanceView.openable = instance.openable;
		instanceView.selected = preferredInstance.has_value() && preferredInstance->instanceId == instance.instanceId;
		hasOpenableVisualStudio = hasOpenableVisualStudio || instance.openable;
		view.visualStudioInstances.push_back(std::move(instanceView));
	}

	ProjectCompatibilityProbe compatibilityProbe;
	for (const ProjectRegistryEntry& entry : projectRegistry_->GetProjects()) {
		ProjectLauncherProjectView projectView{};
		projectView.descriptorPath = StringUtility::ToUtf8(entry.descriptorPath);
		projectView.pinned = entry.pinned;
		ProjectDescriptor descriptor{};
		std::string errorMessage;
		if (!descriptor.Load(entry.descriptorPath, errorMessage)) {
			projectView.displayName = entry.descriptorPath.filename().string();
			projectView.status = std::filesystem::exists(entry.descriptorPath) ? "Invalid Descriptor" : "Missing";
			projectView.detail = errorMessage;
			view.projects.push_back(std::move(projectView));
			continue;
		}
		projectView.displayName = descriptor.GetDisplayName();
		projectView.projectId = descriptor.GetProjectId();
		projectView.projectRoot = StringUtility::ToUtf8(descriptor.GetProjectRoot());
		ProjectSolutionPreviewSnapshot generationSnapshot{};
		if (!projectSolutionGenerationService_->InspectProject(entry.descriptorPath, generationSnapshot, errorMessage)) {
			projectView.generationStatus = "Generation Input Error";
			projectView.generationDetail = errorMessage;
		} else {
			projectView.generationStatus = generationSnapshot.state == ProjectSolutionPreviewState::Ready ? "Generation Ready" : "Legacy Manual Solution";
			projectView.generationDetail = generationSnapshot.detail;
			projectView.canGenerateSolutionPreview = generationSnapshot.state == ProjectSolutionPreviewState::Ready;
			projectView.legacyArtifactDirectory = StringUtility::ToUtf8(generationSnapshot.legacyArtifactDirectory);
			projectView.groupedArtifactDirectory = StringUtility::ToUtf8(generationSnapshot.groupedArtifactDirectory);
			projectView.layoutMigrationRequired = generationSnapshot.layoutMigrationRequired;
			projectView.modifiedOwnedArtifactCount = generationSnapshot.modifiedOwnedArtifactCount;
		}
		bool matchesCurrentInput = false;
		std::string selectionError;
		const ProjectSolutionPreviewSnapshot* preview = projectSolutionGenerationService_->FindPreferredPreview(entry.descriptorPath, matchesCurrentInput, selectionError);
		if (preview) {
			projectView.previewOperationId = preview->operationId;
			projectView.previewSolutionPath = StringUtility::ToUtf8(preview->solutionPath);
			projectView.previewArtifactCount = static_cast<uint32_t>(preview->artifacts.size());
			projectView.canOpenSolutionPreview = matchesCurrentInput;
			projectView.retiredArtifactCount = generationSnapshot.retiredArtifactCount;
			projectView.generationStatus = matchesCurrentInput ? "Preview Ready - Not Built" : "Preview Out Of Date";
			std::string operationError;
			if (matchesCurrentInput) {
				projectView.canAdoptSolutionPreview = projectSolutionGenerationService_->CanAdoptPreview(entry.descriptorPath, preview->operationId, operationError);
				if (projectView.canAdoptSolutionPreview) {
					operationError.clear();
				} else if (projectView.layoutMigrationRequired) {
					operationError.clear();
					projectView.canAdoptGroupedSolutionLayout = projectSolutionGenerationService_->CanMigrateOutputLayout(entry.descriptorPath, preview->operationId, operationError);
				} else if (std::filesystem::exists(descriptor.GetProjectRoot() / L"intermediate" / L"project-files" / L"solution-generation.json")) {
					operationError.clear();
					projectView.canRegenerateSolutionPreview = projectSolutionGenerationService_->CanRegeneratePreview(entry.descriptorPath, preview->operationId, operationError);
				}
			} else {
				operationError = "Project has changed. Generate Preview again.";
			}
			projectView.generationDetail = preview->detail;
			if (!generationSnapshot.detail.empty()) projectView.generationDetail += " " + generationSnapshot.detail;
			if (!operationError.empty()) projectView.generationDetail += " " + operationError;
			if (projectView.canAdoptGroupedSolutionLayout && projectView.modifiedOwnedArtifactCount != 0) {
				projectView.generationDetail += " Current generated drift: " + std::to_string(projectView.modifiedOwnedArtifactCount) + " owned artifacts will be replaced, not merged.";
			}
		} else if (!selectionError.empty() && projectView.canGenerateSolutionPreview) {
			projectView.generationDetail += " " + selectionError;
		}
		const ProjectCompatibilityReport report = compatibilityProbe.Probe(descriptor);
		projectView.canOpenSolution = report.solution == ProjectCompatibilityFileState::Available && hasOpenableVisualStudio;
		projectView.canOpenEditor = report.developmentExecutable == ProjectCompatibilityFileState::Available;
		projectView.canSwitchEditor = projectView.canOpenEditor &&
			!view.creationInProgress && !view.switchBlockedBySceneDirty &&
			!view.switchBlockedByPrefabDirty && !view.switchBlockedByPlayMode;
		if (!report.IsDescriptorCompatible()) {
			projectView.status = "Invalid";
			projectView.detail = report.errors.front();
		} else if (!projectView.canOpenEditor) {
			projectView.status = "Not Built";
			projectView.detail = report.warnings.empty() ? "Development executable is unavailable." : report.warnings.front();
		} else if (!hasOpenableVisualStudio) {
			projectView.status = "IDE Missing";
			projectView.detail = "No openable Visual Studio instance is available.";
		} else {
			projectView.status = "Build Ready";
		}
		view.projects.push_back(std::move(projectView));
	}
	for (const ProjectCreationOperation& operation : projectRegistry_->GetCreationOperations()) {
		if (operation.state == ProjectCreationOperationState::Complete) {
			continue;
		}
		ProjectLauncherOperationView operationView{};
		operationView.operationId = operation.operationId;
		operationView.projectId = operation.projectId;
		operationView.state = ToProjectCreationOperationStateText(operation.state);
		operationView.detail = StringUtility::ToUtf8(operation.logPath);
		operationView.canRetryFinalize = operation.state == ProjectCreationOperationState::RegistrationIncomplete || operation.state == ProjectCreationOperationState::Failed;
		view.operations.push_back(std::move(operationView));
	}
	for (const ProjectSolutionRecoverySnapshot& recovery : projectSolutionGenerationService_->GetRecoverySnapshots()) {
		ProjectLauncherGenerationRecoveryView recoveryView{};
		recoveryView.operationId = recovery.operationId;
		recoveryView.projectId = recovery.projectId;
		recoveryView.state = ToSolutionGenerationOperationStateText(recovery.state);
		recoveryView.stagingRoot = StringUtility::ToUtf8(recovery.stagingRoot);
		recoveryView.rollbackRoot = StringUtility::ToUtf8(recovery.rollbackRoot);
		recoveryView.detail = recovery.detail;
		recoveryView.fileCount = recovery.fileCount;
		recoveryView.canRecheck = recovery.canRecheck;
		recoveryView.canCommitStaged = recovery.canCommitStaged;
		recoveryView.canResumeCommit = recovery.canResumeCommit;
		recoveryView.canRestorePrevious = recovery.canRestorePrevious;
		view.generationRecoveries.push_back(std::move(recoveryView));
	}
	imguiManager_->SetProjectLauncherView(view);
}

bool Game::ProcessProjectLauncherRequest() {
	ClearProjectAccessIssue();

	if (!imguiManager_ || !projectRegistry_ || !projectCreationService_ || !projectSolutionGenerationService_) {
		return false;
	}
	ProjectLauncherRequest request{};
	if (!imguiManager_->ConsumeProjectLauncherRequest(request)) {
		return false;
	}
	std::string errorMessage;
	bool succeeded = false;
	switch (request.operation) {
	case ProjectLauncherRequestOperation::Create: {
		ProjectCreationRequest createRequest{};
		createRequest.projectId = request.projectId;
		createRequest.displayName = request.displayName;
		createRequest.destinationRoot = StringUtility::ToPath(request.destinationRoot);
		createRequest.startSceneId = request.startSceneId;
		createRequest.templateSourceRoot = projectRegistry_->GetTemplateSourceRoot();
		succeeded = projectCreationService_->Start(createRequest, errorMessage);
		if (succeeded) {
			pendingCreateOpenSolution_ = request.openSolutionAfterCreate;
			pendingCreateVisualStudioInstanceId_ = request.visualStudioInstanceId;
		}
		break;
	}
	case ProjectLauncherRequestOperation::Import: {
		ProjectDescriptor descriptor{};
		const std::filesystem::path descriptorPath = StringUtility::ToPath(request.descriptorPath);
		succeeded = descriptor.Load(descriptorPath, errorMessage);
		if (succeeded) {
			ProjectRegistryEntry entry{};
			entry.descriptorPath = descriptor.GetDescriptorPath();
			succeeded = projectRegistry_->RegisterProject(entry, errorMessage) && projectRegistry_->Save(errorMessage);
		}
		break;
	}
	case ProjectLauncherRequestOperation::Remove:
		succeeded = projectRegistry_->RemoveProject(StringUtility::ToPath(request.descriptorPath));
		if (succeeded) {
			succeeded = projectRegistry_->Save(errorMessage);
		} else {
			errorMessage = "Project is not registered.";
		}
		break;
	case ProjectLauncherRequestOperation::RetryFinalize:
		succeeded = projectCreationService_->RetryFinalize(request.operationId, errorMessage);
		break;
	case ProjectLauncherRequestOperation::SetPinned:
		for (const ProjectRegistryEntry& entry : projectRegistry_->GetProjects()) {
			if (StringUtility::ToUtf8(entry.descriptorPath) != request.descriptorPath) {
				continue;
			}
			ProjectRegistryEntry updated = entry;
			updated.pinned = request.pinned;
			succeeded = projectRegistry_->RegisterProject(updated, errorMessage) && projectRegistry_->Save(errorMessage);
			break;
		}
		if (!succeeded && errorMessage.empty()) {
			errorMessage = "Project is not registered.";
		}
		break;
	case ProjectLauncherRequestOperation::OpenFolder:
		succeeded = LaunchProjectFolder(StringUtility::ToPath(request.descriptorPath), errorMessage);
		break;
	case ProjectLauncherRequestOperation::OpenSolution:
		succeeded = LaunchProjectSolution(StringUtility::ToPath(request.descriptorPath), request.visualStudioInstanceId, errorMessage);
		break;
	case ProjectLauncherRequestOperation::GenerateSolutionPreview:
	case ProjectLauncherRequestOperation::RetrySolutionPreview: {
		ProjectSolutionPreviewSnapshot preview{};
		succeeded = projectSolutionGenerationService_->CreatePreview(StringUtility::ToPath(request.descriptorPath), preview, errorMessage);
		if (succeeded && preview.state != ProjectSolutionPreviewState::PreviewReady) {
			errorMessage = preview.detail;
			succeeded = false;
		}
		break;
	}
	case ProjectLauncherRequestOperation::OpenSolutionPreview: {
		// 表示後の設定・生成物変更を再検証し、別のPreviewへ勝手に切り替えて開かない。
		bool matchesCurrentInput = false;
		const ProjectSolutionPreviewSnapshot* preview = projectSolutionGenerationService_->FindPreferredPreview(
			StringUtility::ToPath(request.descriptorPath), matchesCurrentInput, errorMessage);
		if (!preview || preview->operationId != request.operationId) {
			if (preview || errorMessage.empty()) errorMessage = "Selected Preview is unavailable or has changed. Refresh Project Manager.";
			break;
		}
		// 表示後に入力が変わった場合も起動しない。古いPreviewは保存だけ維持する。
		if (!matchesCurrentInput) { errorMessage = "Project has changed. Generate Preview again."; break; }
		errorMessage.clear();
		succeeded = LaunchPreviewSolution(preview->solutionPath, request.visualStudioInstanceId, errorMessage);
		if (!succeeded && errorMessage.empty()) errorMessage = "Preview Solution could not be opened.";
		break;
	}
	case ProjectLauncherRequestOperation::Refresh: {
		// 競合で拒否されたローカル編集を捨て、利用者の明示操作で最新の登録内容へ戻す。
		succeeded = projectRegistry_->Load(errorMessage);
		if (succeeded && projectRegistry_->GetTemplateSourceRoot().empty()) {
			// 起動時だけ補っていた未保存の既定Templateも、再読込後に同じ規則で補う。
			const auto sourceRoot = EditableResourcePath::FindProjectRoot();
			const auto templateRoot = sourceRoot.empty() ? std::filesystem::path{} : sourceRoot.parent_path();
			std::error_code filesystemError;
			if (!templateRoot.empty() && std::filesystem::is_regular_file(templateRoot / L"project/tools/New-GameProject.ps1", filesystemError) && !filesystemError)
				projectRegistry_->SetTemplateSourceRoot(templateRoot);
		}
		break;
	}
	case ProjectLauncherRequestOperation::AdoptSolutionPreview:
		succeeded = projectSolutionGenerationService_->AdoptPreview(StringUtility::ToPath(request.descriptorPath), request.operationId, errorMessage);
		break;
	case ProjectLauncherRequestOperation::RegenerateSolutionPreview:
		succeeded = projectSolutionGenerationService_->RegeneratePreview(StringUtility::ToPath(request.descriptorPath), request.operationId, errorMessage);
		break;
	case ProjectLauncherRequestOperation::AdoptGroupedSolutionLayout:
		succeeded = projectSolutionGenerationService_->MigrateOutputLayout(StringUtility::ToPath(request.descriptorPath), request.operationId, errorMessage);
		break;
	case ProjectLauncherRequestOperation::CommitStagedSolutionGeneration:
		succeeded = projectSolutionGenerationService_->CommitStaged(request.operationId, errorMessage);
		break;
	case ProjectLauncherRequestOperation::ResumeSolutionGenerationCommit:
		succeeded = projectSolutionGenerationService_->ResumeCommit(request.operationId, errorMessage);
		break;
	case ProjectLauncherRequestOperation::RecheckSolutionGenerationRecovery:
		succeeded = projectSolutionGenerationService_->RecheckRecovery(request.operationId, errorMessage);
		break;
	case ProjectLauncherRequestOperation::RestorePreviousSolutionGeneration:
		succeeded = projectSolutionGenerationService_->RestorePrevious(request.operationId, errorMessage);
		break;
	case ProjectLauncherRequestOperation::OpenEditor:
		succeeded = LaunchProjectEditor(StringUtility::ToPath(request.descriptorPath), false, errorMessage);
		break;
	case ProjectLauncherRequestOperation::SwitchEditor:
		if ((editorSession_ && (!editorSession_->IsEditing() || editorSession_->GetEditDocument().IsDirty())) || imguiManager_->HasUnsavedPrefabChanges() || projectCreationService_->GetSnapshot().state == ProjectCreationServiceState::GeneratorRunning) {
			errorMessage = "Switch Editor is blocked by Play mode, unsaved Scene or Prefab changes, or Project creation.";
			break;
		}
		succeeded = LaunchProjectEditor(StringUtility::ToPath(request.descriptorPath), true, errorMessage);
		break;
	default:
		errorMessage = "Unknown Project Launcher request.";
		break;
	}
	projectLauncherStatusMessage_ = succeeded ? "Project Launcher request completed." : errorMessage;
	if (!succeeded && GetProjectAccessIssue() != ProjectAccessIssue::None)
		projectLauncherStatusMessage_ = imguiManager_->NotifyProjectAccessFailure(GetProjectAccessIssue(), errorMessage);
	return true;
}

bool Game::LaunchProjectSolution(
	const std::filesystem::path& descriptorPath,
	const std::string& requestedInstanceId,
	std::string& errorMessage
) {
	if (!visualStudioLocator_) {
		errorMessage = "Visual Studio service is unavailable.";
		return false;
	}
	ProjectDescriptor descriptor{};
	if (!descriptor.Load(descriptorPath, errorMessage)) {
		return false;
	}
	ProjectCompatibilityProbe probe;
	const ProjectCompatibilityReport report = probe.Probe(descriptor);
	if (report.solution != ProjectCompatibilityFileState::Available) {
		errorMessage = report.errors.empty() ? "Solution is unavailable." : report.errors.front();
		return false;
	}
	std::optional<VisualStudioInstance> instance;
	for (const VisualStudioInstance& candidate : visualStudioLocator_->GetInstances()) {
		if (candidate.instanceId == requestedInstanceId && candidate.openable) {
			instance = candidate;
			break;
		}
	}
	if (!instance.has_value()) {
		instance = visualStudioLocator_->SelectPreferred(
			VisualStudioLocator::MakeSelectionRequest({}, false, report)
		);
	}
	if (!instance.has_value()) {
		errorMessage = "No openable Visual Studio instance was found.";
		return false;
	}
	WindowsProcess process;
	return visualStudioLocator_->LaunchSolution(
		*instance,
		descriptor.ResolveProjectPath(descriptor.GetPaths().solution),
		process,
		errorMessage
	);
}

bool Game::LaunchPreviewSolution(
	const std::filesystem::path& solutionPath,
	const std::string& requestedInstanceId,
	std::string& errorMessage
) {
	if (!visualStudioLocator_) {
		errorMessage = "Visual Studio service is unavailable.";
		return false;
	}
	std::error_code filesystemError;
	if (!std::filesystem::is_regular_file(solutionPath, filesystemError) || filesystemError) {
		errorMessage = "Preview Solution is unavailable.";
		return false;
	}
	std::optional<VisualStudioInstance> instance;
	for (const VisualStudioInstance& candidate : visualStudioLocator_->GetInstances()) {
		if (candidate.instanceId == requestedInstanceId && candidate.openable) {
			instance = candidate;
			break;
		}
	}
	if (!instance.has_value()) {
		instance = visualStudioLocator_->SelectPreferred({ {}, false, "v143", "10.0.26100.0" });
	}
	if (!instance.has_value()) {
		errorMessage = "No openable Visual Studio instance was found.";
		return false;
	}
	WindowsProcess process;
	return visualStudioLocator_->LaunchSolution(*instance, solutionPath, process, errorMessage);
}

bool Game::LaunchProjectFolder(
	const std::filesystem::path& descriptorPath,
	std::string& errorMessage
) {
	ProjectDescriptor descriptor{};
	if (!descriptor.Load(descriptorPath, errorMessage)) {
		return false;
	}
	wchar_t windowsDirectory[MAX_PATH]{};
	const UINT length = GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
	if (length == 0 || length >= MAX_PATH) {
		errorMessage = "Windows directory could not be resolved.";
		return false;
	}
	WindowsProcess process;
	WindowsProcessRequest request{};
	request.applicationPath = std::filesystem::path(windowsDirectory) / L"explorer.exe";
	request.arguments = { descriptor.GetProjectRoot().native() };
	request.workingDirectory = descriptor.GetProjectRoot();
	return process.Start(request, errorMessage);
}

bool Game::LaunchProjectEditor(
	const std::filesystem::path& descriptorPath,
	bool switchCurrentEditor,
	std::string& errorMessage
) {
	ProjectDescriptor descriptor{};
	if (!descriptor.Load(descriptorPath, errorMessage)) {
		return false;
	}
	const std::filesystem::path executable = descriptor.ResolveProjectPath(
		descriptor.GetPaths().developmentExecutable
	);
	std::error_code filesystemError;
	if (!std::filesystem::is_regular_file(executable, filesystemError) || filesystemError) {
		errorMessage = "Target Development executable is unavailable.";
		return false;
	}
	WindowsProcess process;
	WindowsProcessRequest request{};
	request.applicationPath = executable;
	request.workingDirectory = descriptor.GetProjectRoot() / L"project";
	if (!process.Start(request, errorMessage)) {
		return false;
	}
	if (switchCurrentEditor) {
		CancelPresentedTransition();
		endRequest_ = true;
	}
	return true;
}

bool Game::OpenRegisteredEditScene(
	const std::string& sceneId,
	bool discardUnsavedChanges,
	std::string& errorMessage
) {
	CancelPresentedTransition();
	const SceneDescriptor* scene = sceneCatalog_
		? sceneCatalog_->Find(sceneId)
		: nullptr;
	if (!scene) {
		errorMessage = "Scene is not registered: " + sceneId;
		return false;
	}
	if (!editorSession_ || !editorSession_->OpenEditScene(
		scene->id,
		scene->displayName,
		scene->filePath,
		discardUnsavedChanges
	)) {
		errorMessage = "Edit Scene could not be opened: " + scene->filePath;
		return false;
	}
	if (imguiManager_) {
		imguiManager_->NotifyEditSceneOpened();
	}
	errorMessage.clear();
	return true;
}

bool Game::IsPresentedTransitionDrawable() const {
	if (!winApp_ || IsIconic(winApp_->GetHwnd())) return false;
	uint32_t width = dxCommon_->GetClientWidth(), height = dxCommon_->GetClientHeight();
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editorSession_ && imguiManager_) {
		if (!imguiManager_->IsSceneViewRenderable()) return false;
		width = imguiManager_->GetSceneViewWidth(); height = imguiManager_->GetSceneViewHeight();
	}
#endif
	return width > 0 && height > 0 && width <= SwordShatterRenderer::MaxImageDimension && height <= SwordShatterRenderer::MaxImageDimension;
}

void Game::CancelPresentedTransition(const std::string& reason) {
	CancelCurrentTextureUpload();
	if (!presentedTransition_ || !sceneManager_ || !audio_) return;
	auto& transition = *presentedTransition_;
	bool committedFailure = false;
#if defined(_DEBUG) || defined(DEVELOPMENT)
	const bool linkedEditorFailure = transition.linked && editorSession_ && imguiManager_ && !reason.empty();
#endif
	if (transition.linked) {
		transition.linked->timing.Report("cancel", transition.linked->request, transition.linked->token.instanceId);
		const auto& linked = *transition.linked;
		if (linked.token.instanceId != kInvalidSceneInstanceId && !linked.committed) {
			sceneManager_->CancelScenePreparation(linked.token);
		}
		if (linked.lastRetirableSerial != 0)
			sceneManager_->NotifyDestinationFrameRetired(linked.token, linked.lastRetirableSerial);
		if (linked.committed) {
			transition.uiSource = {};
			committedFailure = !reason.empty();
			// Commit直後の取消でも、次のmain提出で旧Sceneの退役を進める。
			if (linked.lastRetirableSerial <= linked.proof.recordingSerial)
				transition.linkedRetirement = linked.token;
		}
		transition.linked.reset();
	}
	// 新Scene確定後はManagerが旧Sourceを拒否するため、消滅したMenuを復活させない。
	if (transition.uiSource.requestSerial) {
		sceneManager_->ReportUiTransitionOutcome(transition.uiSource, SceneUiTransitionStatus::Failed, reason.empty() ? "MenuRuntime:TransitionCancelled" : reason);
		transition.uiSource = {};
	}
	transition.controller.Cancel(*sceneManager_,*audio_,reason);
	transition.renderer.reset();
	transition.drawFailed = false; transition.renderedTextureUsed = false;
	if (!reason.empty()) {
		transition.issue = reason;
		Logger::Log("SceneTransition: "+reason+"\n");
	}
	if (committedFailure && runtimeBootstrap_ && winApp_) {
		// standaloneの非致命通知。hold解放後に表示し、閉じると切替済みSceneへ戻る。
		const auto detail = StringUtility::ConvertString(reason);
		MessageBoxW(winApp_->GetHwnd(), detail.c_str(), L"Scene transition failed", MB_OK | MB_ICONWARNING);
	}
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (linkedEditorFailure) imguiManager_->ReportMainDockTransitionIssue(
		ImGuiManager::MainDockTransitionIssue::PresentationFailed, reason);
	else if (imguiManager_) imguiManager_->SetPresentedTransitionStatus(transition.issue);
#endif
}

bool Game::BeginLoadLinkedTransition(const PresentedTransitionRequest& request,
	std::shared_ptr<const SceneTransitionAsset::LoadLinkedAsset> settings,
	const std::filesystem::path& resourcesRoot, std::string& issue) {
	auto& t = *presentedTransition_;
	if (t.linkedRetirement) {
		issue = "The previous scene is still retiring.";
		return false;
	}
	bool playable = runtimeBootstrap_ != nullptr;
	uint32_t width = dxCommon_->GetClientWidth(), height = dxCommon_->GetClientHeight();
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editorSession_ && imguiManager_) {
		playable = editorSession_->IsPlaying() && !editorSession_->IsPaused() &&
			winApp_ && IsWindowVisible(winApp_->GetHwnd()) && !IsIconic(winApp_->GetHwnd()) &&
			imguiManager_->GetMainDockGameViewState() == ImGuiManager::MainDockGameViewState::Ready;
		width = imguiManager_->GetSceneViewWidth();
		height = imguiManager_->GetSceneViewHeight();
	}
#endif
	if (!playable || !settings || settings->loadingText.enabled) {
		issue = "Load-linked transitions require Play in a main dock Game View or standalone Runtime, with loading text disabled.";
		return false;
	}
	try {
		if (!t.preparedResourcesRoot.empty() && t.preparedResourcesRoot != resourcesRoot) {
			issue = "The running project resources root changed.";
			return false;
		}
		if (t.linkedRequestId == (std::numeric_limits<uint64_t>::max)() ||
			!sceneManager_->SetScenePreparationProjectEpoch(PresentedTransitionState::preparedProjectEpoch)) {
			issue = "The scene preparation epoch or transition request ID is unavailable.";
			return false;
		}
		auto linked = std::make_unique<PresentedTransitionState::LinkedRequest>();
		linked->request = request;
		linked->projectEpoch = PresentedTransitionState::preparedProjectEpoch;
		linked->context = executionContext_;
		auto& input = linked->input;
		input.requestId = ++t.linkedRequestId;
		input.settings = std::move(settings);
		input.captureWidth = width;
		input.captureHeight = height;
		const auto* random = std::get_if<SceneTransitionAsset::LoadLinkedRandomSource>(&input.settings->cutSource);
		if ((random && !t.linkedCutSeeds.Resolve(random->seed, input.cutSeed)) ||
			!t.linkedMotionSeeds.Resolve(input.settings->motionSeed, input.motionSeed)) {
			issue = "The transition seed sequence is exhausted.";
			return false;
		}
		SceneTransitionAsset::LoadLinkedGenerationFailure failure;
		if (!linked->generator.TryBegin(input, failure)) {
			issue = !failure.validationDiagnostics.empty() ? failure.validationDiagnostics.front().detail :
				!failure.diagnostics.empty() ? failure.diagnostics.front().message : "Transition generation was rejected.";
			return false;
		}
		linked->load.identity = {input.requestId, input.requestId, input.requestId, input.settingsRevision};
		t.preparedResourcesRoot = resourcesRoot;
		t.linked = std::move(linked);
		t.issue.clear();
		return true;
	} catch (const std::exception& error) {
		issue = error.what();
		return false;
	}
}

void Game::UpdateLoadLinkedTransitionBeforeScene(double delta, bool paused) {
	using Controller = SceneTransitionController;
	using GenerationState = SceneTransitionAsset::LoadLinkedGenerationState;
	auto& t = *presentedTransition_;
	auto& linked = *t.linked;
	bool drawable = IsPresentedTransitionDrawable() && IsWindowVisible(winApp_->GetHwnd());
	auto width = dxCommon_->GetClientWidth(), height = dxCommon_->GetClientHeight();
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editorSession_ && imguiManager_) {
		drawable = drawable && imguiManager_->GetMainDockGameViewState() == ImGuiManager::MainDockGameViewState::Ready;
		width = imguiManager_->GetSceneViewWidth();
		height = imguiManager_->GetSceneViewHeight();
	}
#endif
	const bool suspended = paused || !drawable;
	const auto fail = [this](const std::string& reason) { CancelPresentedTransition(reason); };
	try {
		if (linked.context != executionContext_) {
			fail("The running project context changed.");
			return;
		}
		if (!t.controller.IsActive()) {
			if (suspended) return;
			if (sceneManager_->GetActiveSceneInstanceId() != linked.request.sourceInstanceId ||
				(linked.request.uiSource.requestSerial && !sceneManager_->IsUiTransitionSourceValid(linked.request.uiSource))) {
				fail("The transition source is no longer current.");
				return;
			}
			if (width != linked.input.captureWidth || height != linked.input.captureHeight) {
				// Capture前の生成は同じ解決seedで作り直す。要求受付を解放しない。
				if (t.linkedRequestId == (std::numeric_limits<uint64_t>::max)()) { fail("Transition request ID exhausted."); return; }
				linked.generator.Cancel();
				linked.input.requestId = ++t.linkedRequestId;
				linked.input.captureWidth = width;
				linked.input.captureHeight = height;
				linked.load.identity = {linked.input.requestId, linked.input.requestId, linked.input.requestId, linked.input.settingsRevision};
				SceneTransitionAsset::LoadLinkedGenerationFailure failure;
				if (!linked.generator.TryBegin(linked.input, failure)) { fail("Transition generation could not restart after resize."); return; }
			}
			// Scene準備と同じ2msを初期ソフト予算にする。単一Step自体の時間上限は未保証。
			// 係数1件ごとに描画を待たず、順序と所有を保ったまま有限回だけ進める。
			constexpr auto generationBudget = std::chrono::microseconds{2000};
			constexpr uint32_t maxGenerationSteps = 256;
			const auto generationStart = std::chrono::steady_clock::now();
			auto state = linked.generator.GetStatus().state;
			for (uint32_t step = 0; step < maxGenerationSteps; ++step) {
				state = linked.generator.Step();
				if ((state != GenerationState::GeneratingCuts && state != GenerationState::GeneratingMotion) ||
					std::chrono::steady_clock::now() - generationStart >= generationBudget) break;
			}
			if (state == GenerationState::Failed || state == GenerationState::Cancelled) {
				const auto& failure = linked.generator.GetFailure();
				fail(failure.diagnostics.empty() ? "Transition generation failed." : failure.diagnostics.front().message);
				return;
			}
			if (state != GenerationState::Completed) return;
			std::shared_ptr<const SceneTransitionAsset::LoadLinkedGenerated> generated;
			SceneTransitionAsset::Diagnostics renderDiagnostics;
			SceneTransitionAsset::LoadLinkedDiagnostics diagnostics;
			auto renderer = std::make_unique<SwordShatterRenderer>();
			if (!linked.generator.TryTakeResult(linked.input.requestId, generated) ||
				!renderer->TryInitialize(*dxCommon_, *srvManager_, renderDiagnostics)) {
				fail(renderDiagnostics.empty() ? "Transition renderer initialization failed." : renderDiagnostics.front().message);
				return;
			}
			renderer->BeginFrame(t.frameSerial);
			if (!renderer->TryPrepareLoadLinked(generated, width, height, renderDiagnostics) ||
				!t.controller.TryBeginLoadLinked({linked.request, generated, linked.load.identity.ownerId, linked.load.identity.generation},
					*sceneManager_, diagnostics)) {
				fail(!diagnostics.empty() ? diagnostics.front().detail :
					!renderDiagnostics.empty() ? renderDiagnostics.front().message : "Transition presentation preparation failed.");
				return;
			}
			t.width = width;
			t.height = height;
			t.renderer = std::move(renderer);
			if (t.uiSource.requestSerial) sceneManager_->ReportUiTransitionOutcome(t.uiSource, SceneUiTransitionStatus::Accepted);
			return;
		}

		SceneTransitionAsset::LoadLinkedDiagnostics diagnostics;
		const auto expected = linked.committed ? linked.token.instanceId : linked.request.sourceInstanceId;
		if (sceneManager_->GetActiveSceneInstanceId() != expected) {
			fail("The active scene no longer matches the transition.");
			return;
		}
		if (replayDrawSerial_ == (std::numeric_limits<uint64_t>::max)()) {
			fail("Transition recording serial exhausted.");
			return;
		}
		if (suspended || width != t.width || height != t.height) {
			linked.completed.reset();
			t.controller.DiscardLoadLinkedPresentation();
		}
		if (!suspended && (width != t.width || height != t.height)) {
			SceneTransitionAsset::Diagnostics renderDiagnostics;
			if (!t.renderer->HasCapture() || linked.outputGeneration == (std::numeric_limits<uint64_t>::max)() ||
				!t.renderer->TryResizeOutput(width, height, renderDiagnostics)) {
				fail(renderDiagnostics.empty() ? "Transition capture is unavailable after resize." : renderDiagnostics.front().message);
				return;
			}
			++linked.outputGeneration;
			t.width = width;
			t.height = height;
		}
		if (linked.completed) {
			const auto ticket = *linked.completed;
			linked.completed.reset();
			if (ticket.outputGeneration == linked.outputGeneration &&
				ticket.rendererGeneration == t.renderer->GetGeneration()) {
				t.controller.NotifyLoadLinkedPresented(ticket.presented);
			}
		}
		if (!t.controller.UpdateLoadLinkedBeforeScene(delta, suspended, linked.load, diagnostics)) {
			fail(diagnostics.empty() ? "Transition update failed." : diagnostics.front().detail);
			return;
		}
		if (suspended) return;

		Controller::LoadLinkedPrepareRequest prepare;
		Controller::LoadLinkedFrozen frozen;
		Controller::LoadLinkedCommitRequest commit;
		Controller::LoadLinkedFinished finished;
		const auto epoch = linked.projectEpoch;
		if (t.controller.TryTakeLoadLinkedPrepareRequest(prepare)) {
			linked.timing.Observe(PresentedTransitionState::LoadTiming::Document);
			if (!sceneManager_->BeginScenePreparation({prepare.request.sourceInstanceId, epoch, prepare.request.destinationSceneId}, linked.token))
				fail("Scene preparation was rejected.");
		} else if (t.controller.TryTakeLoadLinkedFrozen(frozen)) {
			SceneTransitionAsset::Diagnostics renderDiagnostics;
			if (!t.renderer->TrySelectLoadLinkedPrefix(frozen.count, renderDiagnostics) ||
				!t.controller.TryBindLoadLinkedDestination({frozen.identity, linked.token.instanceId, linked.token.generation}, diagnostics) ||
				!sceneManager_->BeginPresentDestination(linked.token)) {
				fail(!renderDiagnostics.empty() ? renderDiagnostics.front().message :
					!diagnostics.empty() ? diagnostics.front().detail : "Candidate scene presentation was rejected.");
			}
		} else if (t.controller.TryTakeLoadLinkedCommitRequest(commit)) {
			if (commit.binding.tokenId != linked.token.instanceId || commit.binding.tokenGeneration != linked.token.generation ||
				linked.proof.outputGeneration != linked.outputGeneration ||
				!sceneManager_->CommitScenePreparation(linked.token, linked.proof)) {
				fail("Candidate scene commit was rejected.");
				return;
			}
			linked.committed = true;
			linked.timing.Milestone("commit",linked.token.instanceId);
		} else if (t.controller.TryTakeLoadLinkedFinished(finished)) {
			if (sceneManager_->GetActiveSceneInstanceId() != finished.destination) { fail("The committed destination is no longer active."); return; }
			// Finishedを発行した提出のGPU完了は通知受付前に確認済み。
			sceneManager_->NotifyDestinationFrameRetired(linked.token, finished.finalRecordingSerial);
			t.renderer.reset();
			if (!t.controller.CompleteLoadLinked(finished, *sceneManager_, diagnostics)) {
				fail(diagnostics.empty() ? "Transition completion failed." : diagnostics.front().detail);
				return;
			}
			t.uiSource = {};
			linked.timing.Milestone("presentation_finished",linked.token.instanceId);
			linked.timing.Report("finished", linked.request, linked.token.instanceId);
			t.linked.reset();
		}
	} catch (const std::exception& error) {
		fail(error.what());
	}
}

void Game::UpdateLoadLinkedTransitionAfterScene() {
	using Controller = SceneTransitionController;
	auto& t = *presentedTransition_;
	auto& linked = *t.linked;
	if (linked.token.instanceId == kInvalidSceneInstanceId) return;
	ScenePrepareStatus status;
	if (!sceneManager_->PollScenePreparation(linked.token, status) ||
		status.token.instanceId != linked.token.instanceId || status.token.generation != linked.token.generation ||
		status.projectEpoch != linked.projectEpoch) {
		CancelPresentedTransition("Scene preparation status no longer matches the transition.");
		return;
	}
	using LoadState = Controller::LoadLinkedLoadState;
	linked.timing.ObserveScene(status.phase);
	if (status.phase == ScenePreparePhase::Ready && linked.timing.readyUs < 0) {
		linked.timing.readyUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-linked.timing.start).count();
		linked.timing.Milestone("runtime_ready",linked.token.instanceId);
	}
	using DestinationState = Controller::LoadLinkedDestinationState;
	const auto view = t.controller.GetLoadLinkedView();
	const bool failed = status.phase == ScenePreparePhase::Failed;
	const bool cancelled = status.phase == ScenePreparePhase::Cancelling || status.phase == ScenePreparePhase::Cancelled;
	const bool bound = view && (view->stage == Controller::LoadLinkedStage::AwaitDestination ||
		view->stage == Controller::LoadLinkedStage::AwaitCommit || view->stage == Controller::LoadLinkedStage::Collapse ||
		view->stage == Controller::LoadLinkedStage::FinishPresented);
	if (bound) {
		linked.committed = linked.committed || status.commitSucceeded;
		const auto destination = linked.committed ? sceneManager_->GetActiveSceneInstanceId() :
			sceneManager_->GetPreparedDestinationRenderInstance();
		const auto state = failed ? DestinationState::Failed : cancelled ? DestinationState::Cancelled :
			linked.committed ? DestinationState::Committed :
			destination != kInvalidSceneInstanceId ? DestinationState::Presentable : DestinationState::Pending;
		if (state != linked.destinationState || destination != linked.observedDestination) {
			++linked.destinationRevision; // この列は一件の単調なScene phaseに由来する。
			linked.destinationState = state;
			linked.observedDestination = destination;
		}
		SceneTransitionAsset::LoadLinkedDiagnostics diagnostics;
		if (!t.controller.UpdateLoadLinkedAfterScene({{linked.load.identity, linked.token.instanceId, linked.token.generation},
			state, destination, linked.destinationRevision, std::move(status.diagnostic)}, diagnostics)) {
			CancelPresentedTransition(diagnostics.empty() ? "Candidate scene presentation failed." : diagnostics.front().detail);
		}
	} else {
		const auto state = failed ? LoadState::Failed : cancelled ? LoadState::Cancelled :
			status.phase == ScenePreparePhase::Ready ? LoadState::Ready : LoadState::Preparing;
		if (state != linked.load.state) {
			++linked.load.statusRevision;
			linked.load.state = state;
		}
		linked.load.diagnostic = std::move(status.diagnostic);
	}
}

void Game::UpdatePresentedTransitionBeforeScene(float delta, bool paused) {
	if (!presentedTransition_ || !sceneManager_ || !audio_) return;
	auto& t = *presentedTransition_;
	if (executionContext_ && executionContext_->IsEditing()) { CancelPresentedTransition(); return; }
	if (t.linked) { UpdateLoadLinkedTransitionBeforeScene(delta, paused); return; }
	if (!t.controller.IsActive()) return;
	const bool drawable = IsPresentedTransitionDrawable();
	if (drawable && t.renderer) {
		uint32_t width = dxCommon_->GetClientWidth(), height = dxCommon_->GetClientHeight();
#if defined(_DEBUG) || defined(DEVELOPMENT)
		if (editorSession_ && imguiManager_) { width = imguiManager_->GetSceneViewWidth(); height = imguiManager_->GetSceneViewHeight(); }
#endif
		if (t.width != width || t.height != height) {
			SceneTransitionAsset::Diagnostics diagnostics;
			// Capture前のResizeは画像一組を再準備。Capture後は旧画像を維持する。
			const bool ready = t.renderer->HasCapture()
				? t.renderer->TryResizeOutput(width,height,diagnostics)
				: t.renderer->TryPrepare(t.controller.GetAsset(),width,height,diagnostics);
			t.controller.DiscardDrawReceipt();
			if (!ready) {
				t.issue = diagnostics.empty() ? "Transition resize failed." : diagnostics.front().message;
				t.drawFailed = true; return; // このFrameは旧画像を描き、次safe Updateで取消。
			}
			t.width = width; t.height = height;
		}
	}
	t.controller.UpdateBeforeScene(*sceneManager_,*audio_,delta,paused || !drawable,t.completedSerial);
	if (!t.controller.IsActive()) {
		const auto issue = t.controller.GetDiagnostic();
		CancelPresentedTransition(issue);
	}
}

void Game::UpdatePresentedTransitionAfterScene() {
	if (!presentedTransition_ || !sceneManager_ || !sceneCatalog_ || !audio_) return;
	auto& t = *presentedTransition_;
	if (t.linked) {
		try { UpdateLoadLinkedTransitionAfterScene(); }
		catch (const std::exception& error) { CancelPresentedTransition(error.what()); }
#if defined(_DEBUG) || defined(DEVELOPMENT)
		if (editorSession_ && imguiManager_ && t.linked && t.controller.IsActive())
			imguiManager_->SetPresentedTransitionStatus({}, true, true);
#endif
		return;
	}
	if (t.controller.IsActive()) {
		t.controller.UpdateAfterScene(*sceneManager_,*audio_);
		if (!t.controller.IsActive()) { const auto issue=t.controller.GetDiagnostic(); CancelPresentedTransition(issue); }
	} else {
		// 初回UI計測前/0寸法中は一件の要求を保持し、triggerOnceを失敗で消費しない。
		if (!IsPresentedTransitionDrawable()) return;
		PresentedTransitionRequest request;
		if (!sceneManager_->ConsumePresentedTransitionRequest(request)) return;
		const auto requestReceived = std::chrono::steady_clock::now();
#if defined(_DEBUG) || defined(DEVELOPMENT)
		// 通知の重複抑制を要求ごとに区切り、準備で同じ原因の失敗が続いても再通知する。
		if (imguiManager_) imguiManager_->SetPresentedTransitionStatus({});
#endif
		t.uiSource = request.uiSource;
		SceneTransitionAsset::Diagnostics diagnostics;
		bool ready = IsPresentedTransitionDrawable() && (!executionContext_ || !executionContext_->IsEditing()) &&
			(!request.uiSource.requestSerial || (sceneManager_->IsUiTransitionSourceValid(request.uiSource) &&
				(!executionContext_ || executionContext_->IsPlaying()))) &&
			sceneCatalog_->Find(request.destinationSceneId);
		if (ready) {
			SceneTransitionAsset::DecodedTransitionAsset decoded;
			SceneTransitionAsset::LoadLinkedDiagnostics readDiagnostics;
			std::filesystem::path resourcesRoot;
			try {
				// 従来のresource解決でrootを捕捉し、同じ読取結果からversionを選ぶ。
				resourcesRoot = std::filesystem::canonical(EditableResourcePath::ResolveResource(std::filesystem::path(L"resources")));
				ready = SceneTransitionAsset::TryReadTransitionFile(resourcesRoot, request.transitionAssetPath, decoded, readDiagnostics);
			} catch (const std::exception& error) {
				ready = false;
				diagnostics.push_back({"asset", error.what()});
			}
			if (!ready && !readDiagnostics.empty()) {
				const auto& failure = readDiagnostics.front();
				diagnostics.push_back({failure.field, failure.detail});
			}
			if (ready) {
				using V1Asset = std::shared_ptr<const SceneTransitionAsset::CompiledTransition>;
				if (auto* legacy = std::get_if<V1Asset>(&decoded)) {
					ready = t.controller.TryPrepareDecodedRequest(request, std::move(*legacy), *sceneCatalog_, *audio_, diagnostics);
				} else {
					std::string issue;
					const auto& settings = std::get<std::shared_ptr<const SceneTransitionAsset::LoadLinkedAsset>>(decoded);
					if (!BeginLoadLinkedTransition(request, settings, resourcesRoot, issue)) {
						CancelPresentedTransition(request.transitionAssetPath + ": " + issue);
#if defined(_DEBUG) || defined(DEVELOPMENT)
						if (editorSession_ && imguiManager_) {
							using Issue = ImGuiManager::MainDockTransitionIssue;
							using View = ImGuiManager::MainDockGameViewState;
							const auto state = imguiManager_->GetMainDockGameViewState();
							const auto cause = !editorSession_->IsPlaying() || editorSession_->IsPaused() ? Issue::NotPlaying :
								state == View::NotDockedInMain ? Issue::DockRequired : state != View::Ready ? Issue::ViewUnavailable :
								settings && settings->loadingText.enabled ? Issue::TextUnsupported :
								!t.preparedResourcesRoot.empty() && t.preparedResourcesRoot != resourcesRoot ?
									Issue::ContextChanged : Issue::PresentationFailed;
							imguiManager_->ReportMainDockTransitionIssue(cause, issue);
						}
#endif
					} else {
						auto& timing = t.linked->timing;
						timing.start = timing.last = timing.lastReport = requestReceived;
						timing.previousUpdate = requestReceived;
						timing.Report("start", request, t.linked->token.instanceId);
						timing.Milestone("request_received",t.linked->token.instanceId);
					}
					return;
				}
			}
		}
		if (ready) {
			t.renderer = std::make_unique<SwordShatterRenderer>();
			t.width = dxCommon_->GetClientWidth(); t.height = dxCommon_->GetClientHeight();
#if defined(_DEBUG) || defined(DEVELOPMENT)
			if (editorSession_ && imguiManager_) { t.width=imguiManager_->GetSceneViewWidth(); t.height=imguiManager_->GetSceneViewHeight(); }
#endif
			ready = t.renderer->TryInitialize(*dxCommon_,*srvManager_,diagnostics);
			if (ready) {
				t.renderer->BeginFrame(t.frameSerial);
				ready = t.renderer->TryPrepare(t.controller.GetAsset(),t.width,t.height,diagnostics) &&
					t.controller.BeginPrepared(*sceneManager_,diagnostics);
			}
		}
		if (!ready) {
			CancelPresentedTransition(request.transitionAssetPath+": "+
				(diagnostics.empty() ? "Scene/display is unavailable for transition." : diagnostics.front().field+": "+diagnostics.front().message));
			return;
		}
		t.issue.clear();
		if (t.uiSource.requestSerial) sceneManager_->ReportUiTransitionOutcome(t.uiSource, SceneUiTransitionStatus::Accepted);
	}
	if (t.controller.IsActive() && !t.controller.GetDiagnostic().empty() && t.issue != t.controller.GetDiagnostic()) {
		t.issue = t.controller.GetDiagnostic();
		Logger::Log("SceneTransition: "+t.issue+"\n");
	}
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (imguiManager_) imguiManager_->SetPresentedTransitionStatus(t.issue.empty() ?
		(t.controller.IsActive() ? "Scene transition is active" : "") : t.issue,t.controller.IsActive());
#endif
}

D3D12_GPU_DESCRIPTOR_HANDLE Game::DrawPresentedTransition(D3D12_GPU_DESCRIPTOR_HANDLE source, uint32_t width, uint32_t height) {
	if (!presentedTransition_) return source;
	auto& t = *presentedTransition_; t.renderedTextureUsed = false;
	if (!t.controller.IsActive() || !t.renderer) return source;
	if (t.linked) {
		using Controller = SceneTransitionController;
		auto& linked = *t.linked;
		linked.recorded.reset();
		const auto view = t.controller.GetLoadLinkedView();
		if (!view || t.drawFailed || !IsPresentedTransitionDrawable() ||
			!IsWindowVisible(winApp_->GetHwnd()) || (executionContext_ && executionContext_->IsPaused()) ||
			width != t.width || height != t.height) return source;
		const SwordShatterRenderer::TextureView background{source, width, height, RenderFormats::kDisplayFormat};
		SceneTransitionAsset::Diagnostics diagnostics;
		SwordShatterRenderer::TextureView output;
		Controller::LoadLinkedPresented presented;
		presented.identity = view->identity;
		// SceneManager::BeginRenderFrameもこのserialを記録し、候補のCommit proofと照合する。
		presented.recordingSerial = replayDrawSerial_;
		presented.drawnInstance = sceneManager_->GetActiveSceneInstanceId();
		bool ready = false;
		if (view->stage == Controller::LoadLinkedStage::AwaitCapture) {
			SwordShatterRenderer::CaptureReceipt capture;
			ready = t.renderer->HasCapture() || t.renderer->TryCapture(background, capture, diagnostics);
			presented.kind = Controller::LoadLinkedPresentationKind::Capture;
			if (ready) output = t.renderer->GetCaptureView();
		} else if (view->sample) {
			if (view->stage == Controller::LoadLinkedStage::AwaitDestination) {
				presented.kind = Controller::LoadLinkedPresentationKind::Destination;
				presented.drawnInstance = sceneManager_->GetPreparedDestinationRenderInstance();
			}
			presented.phase = view->sample->phase;
			presented.completedCuts = view->sample->completedCuts;
			presented.prefixCount = view->sample->prefixCount;
			presented.sampleRevision = view->sample->sampleRevision;
			ready = t.renderer->TryComposeLoadLinked(background, *view->sample, diagnostics);
			if (ready) output = t.renderer->GetOutputView();
		}
		if (!ready || !output.srv.ptr) {
			t.drawFailed = true;
			t.issue = diagnostics.empty() ? "Load-linked transition rendering failed." : diagnostics.front().message;
			// 旧合成を使う失敗Frameにはticketを付けない。
			output = t.renderer->GetOutputView();
			if (!output.srv.ptr) output = t.renderer->GetCaptureView();
		} else {
			linked.recorded = PresentedTransitionState::LinkedRequest::Ticket{
				presented, output.srv.ptr, t.renderer->GetGeneration(), linked.outputGeneration};
		}
		t.renderedTextureUsed = output.srv.ptr != 0;
		return t.renderedTextureUsed ? output.srv : source;
	}
	if (!t.drawFailed && IsPresentedTransitionDrawable() && width == t.width && height == t.height) {
		SceneTransitionAsset::Diagnostics diagnostics;
		const SwordShatterRenderer::TextureView background{source,width,height,RenderFormats::kDisplayFormat};
		SwordShatterRenderer::CaptureReceipt capture;
		bool ready = t.renderer->HasCapture() || t.renderer->TryCapture(background,capture,diagnostics);
		if (ready) ready = t.renderer->TryCompose(background,t.controller.GetAsset(),t.controller.GetSample(),diagnostics);
		if (ready) t.controller.NotifyDrawRecorded(t.frameSerial,sceneManager_->GetActiveSceneInstanceId());
		else {
			t.drawFailed = true;
			t.issue = diagnostics.empty() ? "Transition render failed." : diagnostics.front().field+": "+diagnostics.front().message;
			t.controller.DiscardDrawReceipt();
		}
	} else t.controller.DiscardDrawReceipt();
	auto output = t.renderer->GetOutputView();
	if (!output.srv.ptr) output = t.renderer->GetCaptureView();
	if (output.srv.ptr) { t.renderedTextureUsed = true; return output.srv; }
	return source;
}

void Game::ResolveLoadLinkedFrame(uint64_t adoptedSrv, const DirectFrameResult& frame) {
	if (!presentedTransition_) return;
	auto& t = *presentedTransition_;
	const auto fenceSentinel = (std::numeric_limits<uint64_t>::max)();
	const bool gpuComplete = frame.CanContinue() && frame.signaledFence != 0 &&
		frame.completedFence >= frame.signaledFence && frame.completedFence < fenceSentinel;
	if (gpuComplete && t.linkedRetirement) {
		sceneManager_->NotifyDestinationFrameRetired(*t.linkedRetirement, replayDrawSerial_);
		t.linkedRetirement.reset();
	}
	if (!t.linked) return;
	auto& linked = *t.linked;
	const auto recorded = std::exchange(linked.recorded, std::nullopt);
	const bool drewDestination = linked.token.instanceId != kInvalidSceneInstanceId &&
		(linked.committed || sceneManager_->GetPreparedDestinationRenderInstance() == linked.token.instanceId);
	if (recorded && recorded->presented.kind == SceneTransitionController::LoadLinkedPresentationKind::Destination &&
		recorded->presented.drawnInstance == linked.token.instanceId && recorded->srv == adoptedSrv &&
		frame.signaledFence && frame.signaledFence < fenceSentinel && !linked.timing.firstDrawSubmitted) {
		// PostDrawの返した提出証跡の観測時刻。キュー内部のExecute時刻とは区別する。
		linked.timing.firstDrawSubmitted = true;
		linked.timing.Milestone("destination_first_draw_submitted",linked.token.instanceId,recorded->presented.recordingSerial,frame.signaledFence);
	}
	if (gpuComplete && drewDestination) {
		linked.lastRetirableSerial = replayDrawSerial_;
		if (!linked.committed) sceneManager_->NotifyDestinationFrameRetired(linked.token, replayDrawSerial_);
	}
	if (!recorded || !gpuComplete || recorded->srv != adoptedSrv) return;
	if (FAILED(frame.presentResult)) {
		t.drawFailed = true;
		t.issue = "Present failed during the load-linked transition.";
		return;
	}
	if (frame.presentResult != S_OK || !IsPresentedTransitionDrawable() || !IsWindowVisible(winApp_->GetHwnd()) ||
		(executionContext_ && executionContext_->IsPaused())) return;
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editorSession_) {
		ImGuiManager::MainDockGameImageEvidence evidence;
		if (!imguiManager_ || !imguiManager_->TryGetMainDockGameImageEvidence(
			recorded->presented.recordingSerial, recorded->srv, evidence) ||
			evidence.width != t.width || evidence.height != t.height) {
			t.controller.DiscardLoadLinkedPresentation();
			return;
		}
	}
#endif
	if (recorded->presented.kind == SceneTransitionController::LoadLinkedPresentationKind::Destination &&
		recorded->presented.drawnInstance == linked.token.instanceId) {
		linked.proof = {linked.token, linked.projectEpoch, linked.token.instanceId,
			recorded->presented.recordingSerial, recorded->outputGeneration, frame.signaledFence, frame.completedFence};
		if (linked.timing.firstDrawUs < 0) {
			linked.timing.firstDrawUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-linked.timing.start).count();
			linked.timing.Milestone("destination_first_draw_fence_completed",linked.token.instanceId,recorded->presented.recordingSerial,frame.signaledFence);
		}
	}
	linked.completed = recorded;
}

void Game::Draw() {
	if (startupErrorScreen_) {
		dxCommon_->PreDraw();
#if defined(_DEBUG) || defined(DEVELOPMENT)
		srvManager_->PreDraw();
		imguiManager_->EndFrame();
#endif
		if (!HandleDirectFrameResult(dxCommon_->PostDraw())) endRequest_ = true;
		return;
	}

	const float gameViewTime = GetGameViewEffectTime();
	if (sceneManager_) {
		const uint64_t serial = ++replayDrawSerial_;
		sceneManager_->BeginRenderFrame(serial);
		sceneManager_->CaptureReplayBeforeDraw({serial, gameViewTime});
	}

	// 影描画でもスキニングパレットSRVを使うので先に必要
	srvManager_->PreDraw();


	uint32_t renderWidth = dxCommon_->GetClientWidth();
	uint32_t renderHeight = dxCommon_->GetClientHeight();
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editorSession_ && imguiManager_) {
		renderWidth = imguiManager_->GetSceneViewWidth();
		renderHeight = imguiManager_->GetSceneViewHeight();
	}
#endif
    Camera* const normalEditorCamera=Object3dCommon::GetInstance()->GetDefaultCamera();
    // V2の表示証拠は通常Scene描画に限り、Replayの背景や構図で置き換えない。
    const bool linkedPresentation = presentedTransition_ && presentedTransition_->linked &&
        presentedTransition_->controller.IsActive();
    bool replayFramingPreview=false;
#if defined(_DEBUG) || defined(DEVELOPMENT)
    SceneReplayFramingRequest framing;
    if (!linkedPresentation && imguiManager_ && imguiManager_->GetReplayFramingRequest(false,framing)) {
        SceneReplayFramingBasis basis; SceneReplayViewSettings manual;
        const bool success=normalEditorCamera && renderWidth && renderHeight && sceneManager_->TryGetReplayFraming(framing.owner,basis) &&
            ResolveReplayCamera(framing.view,basis.world,normalEditorCamera->GetWorldMatrix(),basis.min,basis.max,basis.min,basis.max,
                static_cast<float>(renderWidth)/renderHeight,replayEditorCamera_,&manual);
        imguiManager_->SetReplayFramingResult(framing,success,manual);
        replayFramingPreview=success && framing.preview;
    }
#endif
    SceneReplayGameFrame replayFrame;
    bool replayActive = !linkedPresentation && renderWidth && renderHeight && sceneManager_->PrepareReplayGameFrame(replayDrawSerial_,
        static_cast<float>(renderWidth)/renderHeight,replayFrame);
    if (replayActive != replayWasActive_ || (replayActive && replayFrame.finishedHold != replayWasFinished_) || (replayActive &&
        (replayFrame.token.instance != lastReplayToken_.instance || replayFrame.token.generation != lastReplayToken_.generation ||
         replayFrame.token.deathSerial != lastReplayToken_.deathSerial || replayFrame.token.cut != lastReplayToken_.cut)))
        InvalidateMotionBlurHistory();
    if (replayActive && (!replayWasActive_ || replayFrame.token.instance != lastReplayToken_.instance ||
        replayFrame.token.generation != lastReplayToken_.generation || replayFrame.token.deathSerial != lastReplayToken_.deathSerial)) {
        try { replayWaterStates_ = CaptureWaterPostEffectStates(true); }
        catch (const std::bad_alloc&) { sceneManager_->RejectReplayGameFrame(replayFrame.token); replayActive=false; InvalidateMotionBlurHistory(); }
        catch (const std::length_error&) { sceneManager_->RejectReplayGameFrame(replayFrame.token); replayActive=false; InvalidateMotionBlurHistory(); }
    }
    if (!replayActive) replayWaterStates_.clear();
    replayWasActive_=replayActive; replayWasFinished_=replayActive && replayFrame.finishedHold; lastReplayToken_=replayFrame.token;
    sceneManager_->DrawShadow();
    sceneManager_->DrawOffscreenViews();
    if (replayActive) replayActive=sceneManager_->DrawReplayGameShadow(replayFrame.token);
	sceneRenderTarget_->Resize(renderWidth, renderHeight);
	if (
		motionBlurHistoryRenderTarget_->GetWidth() != renderWidth ||
		motionBlurHistoryRenderTarget_->GetHeight() != renderHeight
	) {
		motionBlurHistoryRenderTarget_->Resize(renderWidth, renderHeight);
		InvalidateMotionBlurHistory();
	}
	for (SceneRenderTarget* renderTarget : postProcessRenderTargets_) {
		renderTarget->Resize(renderWidth, renderHeight);
	}
	textOverlayRenderTarget_->Resize(renderWidth, renderHeight);
	foregroundComposeRenderTarget_->Resize(
		renderWidth,
		renderHeight
	);

	const bool deferForegroundEffects = waterRefractionEnabled_;
	sceneManager_->SetDeferForegroundEffects(deferForegroundEffects);
	sceneRenderTarget_->Begin();
	srvManager_->PreDraw();
	Object3dCommon::GetInstance()->SetCommonRenderState();
    if (replayActive && !sceneManager_->DrawReplayGameWorld(replayFrame.token)) {
        replayActive=false; InvalidateMotionBlurHistory();
    }
    if (!replayActive) sceneManager_->Draw(replayFramingPreview ? &replayEditorCamera_ : nullptr);
#if defined(_DEBUG) || defined(DEVELOPMENT)
    if (!replayActive) {
	DrawSceneSpatialPreview();
	if (
		editorSession_ &&
		editorSession_->IsEditing() &&
		imguiManager_ &&
		imguiManager_->IsSceneGridVisible()
	) {
		EditorGridRenderer::AddGrid(*DebugRenderer::GetInstance());
	}
	DebugRenderer::GetInstance()->Draw(
		Object3dCommon::GetInstance()->GetDefaultCamera()
	);
    }
#endif
	sceneRenderTarget_->End();

    // Scene描画のApplyRenderCameraが指定した確認Cameraを独立Viewへ持ち越さない。
    Object3dCommon::GetInstance()->SetDefaultCamera(normalEditorCamera);
	bool drawUIEditor = false;
	uint32_t uiWidth = 1, uiHeight = 1;
	SceneUIEditorView uiView;
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editorSession_ && imguiManager_ && uiEditorSceneTarget_ && uiEditorCopy_) {
		if (const auto* view = sceneManager_->GetEditorUIView()) uiView = *view;
		drawUIEditor = uiView.generation && imguiManager_->IsUIEditorViewRenderable();
		if (drawUIEditor) {
			uiWidth = imguiManager_->GetUIEditorViewWidth(); uiHeight = imguiManager_->GetUIEditorViewHeight();
			uiEditorSceneTarget_->Resize(uiWidth, uiHeight);
			uiEditorForegroundTarget_->Resize(uiWidth, uiHeight);
			for (auto* target : uiEditorPostTargets_) target->Resize(uiWidth, uiHeight);
			uiEditorOverlayTarget_->Resize(uiWidth, uiHeight);
			uiEditorCopy_->BeginFrame();
			uiEditorSceneTarget_->Begin();
			srvManager_->PreDraw();
			Object3dCommon::GetInstance()->SetCommonRenderState();
			sceneManager_->DrawEditorUIView("Scene2D", uiWidth, uiHeight);
			uiEditorSceneTarget_->End();
		}
	}
	DrawModelPreview();
	DrawPrefabPreview();
#endif

    // 各Viewの設定/Camera/定数bufferを明示入力にする。Gameメンバーを書換えて戻さない。
    auto processView = [&](SceneRenderTarget* sceneRenderTarget_, SceneRenderTarget** postProcessRenderTargets_,
        SceneRenderTarget* foregroundComposeRenderTarget_, FullscreenCopy* fullscreenCopy_, BloomRenderer* bloomRenderer_,
        const ScenePostProcessSettings& settings, Camera* renderCamera, float gameViewTime, bool ui, bool replay,
        int& outputPass) {
        const bool deferForegroundEffects = settings.waterRefractionEnabled;
        auto drawForeground = [&] {
            if (ui) sceneManager_->DrawEditorUIView("Foreground",uiWidth,uiHeight);
            else if (!replay) sceneManager_->DrawForegroundEffects(replayFramingPreview ? &replayEditorCamera_ : nullptr);
        };
        const float radialBlurCenter_[] = {settings.radialBlurCenter.x, settings.radialBlurCenter.y};
        const float chromaticAberrationCenter_[] = {settings.chromaticAberrationCenter.x, settings.chromaticAberrationCenter.y};
        const float dissolveEdgeColor_[] = {settings.dissolveEdgeColor.x, settings.dissolveEdgeColor.y, settings.dissolveEdgeColor.z, settings.dissolveEdgeColor.w};
        const float outlineColor_[] = {settings.outlineColor.x, settings.outlineColor.y, settings.outlineColor.z, settings.outlineColor.w};
        const float underwaterTintColor_[] = {settings.underwaterTintColor.x, settings.underwaterTintColor.y, settings.underwaterTintColor.z, settings.underwaterTintColor.w};
        const float waterRefractionTintColor_[] = {settings.waterRefractionTintColor.x, settings.waterRefractionTintColor.y, settings.waterRefractionTintColor.z, settings.waterRefractionTintColor.w};
        auto bloomParameters_ = this->bloomParameters_;
        bloomParameters_.enabled = settings.bloomEnabled;
        bloomParameters_.toneMapMode = settings.toneMapMode;
        bloomParameters_.threshold = settings.bloomThreshold;
        bloomParameters_.softKnee = settings.bloomSoftKnee;
        bloomParameters_.intensity = settings.bloomIntensity;
        bloomParameters_.blurIterations = settings.bloomBlurIterations;
        bloomParameters_.downsampleScale = settings.bloomDownsampleScale;
        bloomParameters_.blurRadius = settings.bloomBlurRadius;
        if (replay) bloomParameters_.exposure = settings.baseExposure;
	fullscreenCopy_->BeginFrame();
	const D3D12_GPU_DESCRIPTOR_HANDLE depthHandle =
		sceneRenderTarget_->GetDepthSrvGpuHandle();
	const char* dissolveMaskPath =
		settings.dissolveMaskIndex == 1
			? "resources/noise1.png"
			: "resources/noise0.png";
	const D3D12_GPU_DESCRIPTOR_HANDLE maskHandle =
		TextureManager::GetInstance()->GetSrvHandleGPU(dissolveMaskPath);

	const WaterPostEffectState waterState =
		ResolveWaterPostEffectState(renderCamera, replay ? &replayWaterStates_ : nullptr);
	auto fillWaterParameters = [&](
		FullscreenCopy::Parameters& parameters
	) {
		if (renderCamera) {
			parameters.cameraNear = renderCamera->GetNearClip();
			parameters.cameraFar = renderCamera->GetFarClip();
			const Matrix4x4& cameraWorld =
				renderCamera->GetWorldMatrix();
			parameters.cameraPositionFovY[0] = cameraWorld.m[3][0];
			parameters.cameraPositionFovY[1] = cameraWorld.m[3][1];
			parameters.cameraPositionFovY[2] = cameraWorld.m[3][2];
			parameters.cameraPositionFovY[3] = renderCamera->GetFovY();
			parameters.cameraRightAspect[0] = cameraWorld.m[0][0];
			parameters.cameraRightAspect[1] = cameraWorld.m[0][1];
			parameters.cameraRightAspect[2] = cameraWorld.m[0][2];
			parameters.cameraRightAspect[3] =
				renderCamera->GetAspectRatio();
			parameters.cameraUpTime[0] = cameraWorld.m[1][0];
			parameters.cameraUpTime[1] = cameraWorld.m[1][1];
			parameters.cameraUpTime[2] = cameraWorld.m[1][2];
			parameters.cameraForwardActive[0] = cameraWorld.m[2][0];
			parameters.cameraForwardActive[1] = cameraWorld.m[2][1];
			parameters.cameraForwardActive[2] = cameraWorld.m[2][2];
		}
		parameters.cameraUpTime[3] = gameViewTime;
		parameters.cameraForwardActive[3] =
			waterState.hasVolume ? 1.0f : 0.0f;
		parameters.underwaterParams[0] =
			waterState.cameraInside ? settings.underwaterIntensity : 0.0f;
		parameters.underwaterParams[1] = settings.underwaterFogDensity;
		parameters.underwaterParams[2] = settings.underwaterDistortion;
		for (uint32_t index = 0; index < 4; ++index) {
			parameters.underwaterTintColor[index] =
				underwaterTintColor_[index];
		}
		parameters.waterVolumeCenterActive[0] = waterState.center.x;
		parameters.waterVolumeCenterActive[1] = waterState.center.y;
		parameters.waterVolumeCenterActive[2] = waterState.center.z;
		parameters.waterVolumeCenterActive[3] =
			waterState.hasVolume ? 1.0f : 0.0f;
		parameters.waterVolumeHalfSizeEdge[0] = waterState.halfSize.x;
		parameters.waterVolumeHalfSizeEdge[1] = waterState.halfSize.y;
		parameters.waterVolumeHalfSizeEdge[2] = waterState.halfSize.z;
		parameters.waterVolumeHalfSizeEdge[3] =
			settings.waterRefractionEdgeSoftness;
		for (uint32_t index = 0; index < 4; ++index) {
			parameters.waterRefractionTintColor[index] =
				waterRefractionTintColor_[index];
		}
		parameters.waterRefractionParams[0] =
			settings.waterRefractionStrength;
		parameters.waterRefractionParams[1] =
			settings.waterRefractionTintStrength;
		parameters.waterLightColorIntensity[0] =
			waterState.lightColor.x;
		parameters.waterLightColorIntensity[1] =
			waterState.lightColor.y;
		parameters.waterLightColorIntensity[2] =
			waterState.lightColor.z;
		parameters.waterLightColorIntensity[3] =
			waterState.lightShaftEnabled ? waterState.lightIntensity : 0.0f;
		parameters.waterLightDirectionDensity[0] =
			waterState.lightDirection.x;
		parameters.waterLightDirectionDensity[1] =
			waterState.lightDirection.y;
		parameters.waterLightDirectionDensity[2] =
			waterState.lightDirection.z;
		parameters.waterLightDirectionDensity[3] =
			waterState.lightDensity;
		parameters.waterLightParams[0] =
			waterState.causticsIntensity;
		parameters.waterLightParams[1] =
			waterState.causticsScale;
		parameters.waterLightParams[2] =
			waterState.causticsSpeed;
		parameters.waterLightParams[3] =
			static_cast<float>(waterState.lightSampleCount);
		parameters.waterLightNoiseParams[0] =
			waterState.lightBreakupStrength;
		parameters.waterLightNoiseParams[1] =
			waterState.lightWarpStrength;
		parameters.waterLightNoiseParams[2] =
			waterState.lightNoiseScale;
	};


	bool waterRefractionAppliedBeforeForeground = false;
	if (deferForegroundEffects) {
		if (
			settings.waterRefractionEnabled &&
			waterState.hasVolume &&
			!waterState.cameraInside &&
			renderCamera
		) {
			FullscreenCopy::Parameters parameters{};
			fillWaterParameters(parameters);
			SceneRenderTarget* destination = foregroundComposeRenderTarget_;
			destination->Begin();
			srvManager_->PreDraw();
			fullscreenCopy_->SetParameters(parameters);
			fullscreenCopy_->Draw(
				sceneRenderTarget_->GetSrvGpuHandle(),
				depthHandle,
				maskHandle,
				FullscreenCopy::Effect::kWaterRefraction,
				FullscreenCopy::OutputFormat::kSceneHdr
			);
			destination->End();

			sceneRenderTarget_->Begin(false, false);
			srvManager_->PreDraw();
			fullscreenCopy_->SetParameters(FullscreenCopy::Parameters{});
			fullscreenCopy_->Draw(
				destination->GetSrvGpuHandle(),
				maskHandle,
				maskHandle,
				FullscreenCopy::Effect::kCopy,
				FullscreenCopy::OutputFormat::kSceneHdr
			);
			drawForeground();
			sceneRenderTarget_->End();
			waterRefractionAppliedBeforeForeground = true;
		} else {
			sceneRenderTarget_->Begin(false, false);
			srvManager_->PreDraw();
			drawForeground();
			sceneRenderTarget_->End();
		}
	}


	auto applySceneHdrEffect = [&](
		FullscreenCopy::Effect effect,
		const FullscreenCopy::Parameters& parameters
	) {
		SceneRenderTarget* destination = foregroundComposeRenderTarget_;
		destination->Begin();
		srvManager_->PreDraw();
		fullscreenCopy_->SetParameters(parameters);
		fullscreenCopy_->Draw(
			sceneRenderTarget_->GetSrvGpuHandle(),
			depthHandle,
			maskHandle,
			effect,
			FullscreenCopy::OutputFormat::kSceneHdr
		);
		destination->End();

		sceneRenderTarget_->Begin(false, false);
		srvManager_->PreDraw();
		fullscreenCopy_->SetParameters(FullscreenCopy::Parameters{});
		fullscreenCopy_->Draw(
			destination->GetSrvGpuHandle(),
			maskHandle,
			maskHandle,
			FullscreenCopy::Effect::kCopy,
			FullscreenCopy::OutputFormat::kSceneHdr
		);
		sceneRenderTarget_->End();

	};

	if (
		waterState.hasVolume &&
		waterState.lightShaftEnabled &&
		waterState.lightIntensity > 0.0f &&
		renderCamera
	) {
		FullscreenCopy::Parameters parameters{};
		fillWaterParameters(parameters);
		applySceneHdrEffect(
			FullscreenCopy::Effect::kWaterLightShafts,
			parameters
		);
	}

	if (bloomRenderer_) {
		bloomRenderer_->BeginFrame();
		bloomRenderer_->SetParameters(bloomParameters_);
		bloomRenderer_->Apply(
			sceneRenderTarget_->GetSrvGpuHandle(),
			postProcessRenderTargets_[0]
		);
	} else {
		postProcessRenderTargets_[0]->Begin();
		srvManager_->PreDraw();
		fullscreenCopy_->SetParameters({});
		fullscreenCopy_->Draw(
			sceneRenderTarget_->GetSrvGpuHandle(),
			depthHandle,
			maskHandle,
			FullscreenCopy::Effect::kCopy
		);
		postProcessRenderTargets_[0]->End();
	}
	D3D12_GPU_DESCRIPTOR_HANDLE sourceHandle =
		postProcessRenderTargets_[0]->GetSrvGpuHandle();
	int passIndex = 1;
	auto applyEffect = [&](
		FullscreenCopy::Effect effect,
		const FullscreenCopy::Parameters& parameters
	) {
		SceneRenderTarget* destination =
			postProcessRenderTargets_[passIndex % 2];
		destination->Begin();
		srvManager_->PreDraw();
		fullscreenCopy_->SetParameters(parameters);
		fullscreenCopy_->Draw(
			sourceHandle,
			depthHandle,
			maskHandle,
			effect
		);
		destination->End();
		sourceHandle = destination->GetSrvGpuHandle();
		++passIndex;

	};

	if (settings.grayscaleEnabled) {
		applyEffect(
			FullscreenCopy::Effect::kGrayscale,
			FullscreenCopy::Parameters{}
		);
	}
	if (settings.vignetteEnabled) {
		FullscreenCopy::Parameters parameters{};
		parameters.vignetteScale = settings.vignetteScale;
		parameters.vignettePower = settings.vignettePower;
		parameters.vignetteIntensity = settings.vignetteIntensity;
		applyEffect(FullscreenCopy::Effect::kVignette, parameters);
	}
	if (settings.boxBlurEnabled) {
		FullscreenCopy::Parameters parameters{};
		parameters.blurStrength = settings.boxBlurStrength;
		parameters.blurRadius = settings.boxBlurKernelSize == 5 ? 2u : 1u;
		applyEffect(FullscreenCopy::Effect::kBoxBlur, parameters);
	}
	if (settings.gaussianBlurEnabled) {
		FullscreenCopy::Parameters parameters{};
		parameters.blurStrength = settings.gaussianBlurStrength;
		parameters.blurRadius =
			settings.gaussianBlurKernelSize == 5 ? 2u : 1u;
		parameters.gaussianSigma = settings.gaussianBlurSigma;
		applyEffect(FullscreenCopy::Effect::kGaussianBlur, parameters);
	}
	if (settings.depthOfFieldEnabled) {
		FullscreenCopy::Parameters parameters{};
		parameters.blurStrength = settings.depthOfFieldBlurStrength;
		parameters.dofFocusDistance = settings.depthOfFieldFocusDistance;
		parameters.dofFocusRange = settings.depthOfFieldFocusRange;
		parameters.dofNearStrength = settings.depthOfFieldNearStrength;
		parameters.dofFarStrength = settings.depthOfFieldFarStrength;
		parameters.dofMaxRadius = settings.depthOfFieldMaxRadius;
		Camera* camera =
			renderCamera;
		if (camera) {
			parameters.cameraNear = camera->GetNearClip();
			parameters.cameraFar = camera->GetFarClip();
		}
		applyEffect(FullscreenCopy::Effect::kDepthOfField, parameters);
	}
	if (!ui) {
	const bool motionBlurPlaying =
		executionContext_ && executionContext_->IsPlaying();
	const SceneInstanceId motionBlurSceneInstanceId = sceneManager_
		? sceneManager_->GetActiveSceneInstanceId()
		: kInvalidSceneInstanceId;
	if (
		motionBlurPlaying != motionBlurLastPlaying_ ||
		motionBlurSceneInstanceId != motionBlurLastSceneInstanceId_
	) {
		InvalidateMotionBlurHistory();
	}
	motionBlurLastPlaying_ = motionBlurPlaying;
	motionBlurLastSceneInstanceId_ = motionBlurSceneInstanceId;

	if (!settings.motionBlurEnabled) {
		motionBlurWasEnabled_ = false;
		InvalidateMotionBlurHistory();
	} else {
		Camera* motionBlurCamera =
			renderCamera;
		if (!motionBlurCamera) {
			InvalidateMotionBlurHistory();
		} else {
			const D3D12_GPU_DESCRIPTOR_HANDLE preBlurSourceHandle = sourceHandle;
			const Matrix4x4 currentViewProjection =
				motionBlurCamera->GetViewProjectionMatrix();
			auto storeMotionBlurHistory = [&] {
				motionBlurHistoryRenderTarget_->Begin();
				srvManager_->PreDraw();
				fullscreenCopy_->SetParameters(FullscreenCopy::Parameters{});
				fullscreenCopy_->Draw(
					preBlurSourceHandle,
					depthHandle,
					maskHandle,
					FullscreenCopy::Effect::kCopy
				);
				motionBlurHistoryRenderTarget_->End();
			};

			if (!motionBlurWasEnabled_ || !motionBlurHistoryValid_) {
				storeMotionBlurHistory();
				previousMotionBlurViewProjection_ = currentViewProjection;
				motionBlurHistoryValid_ = true;
			} else {
				FullscreenCopy::Parameters parameters{};
				parameters.motionBlurInverseCurrentViewProjection =
					Inverse(currentViewProjection);
				parameters.motionBlurPreviousViewProjection =
					previousMotionBlurViewProjection_;
				parameters.motionBlurParams[0] = settings.motionBlurStrength;
				parameters.motionBlurParams[1] =
					static_cast<float>(settings.motionBlurSamples);
				parameters.motionBlurParams[2] = settings.motionBlurMaxRadius;
				parameters.motionBlurTextureSizeHistoryValid[0] =
					static_cast<float>(postProcessRenderTargets_[0]->GetWidth());
				parameters.motionBlurTextureSizeHistoryValid[1] =
					static_cast<float>(postProcessRenderTargets_[0]->GetHeight());
				parameters.motionBlurTextureSizeHistoryValid[2] = 1.0f;

				SceneRenderTarget* destination =
					postProcessRenderTargets_[passIndex % 2];
				destination->Begin();
				srvManager_->PreDraw();
				fullscreenCopy_->SetParameters(parameters);
				fullscreenCopy_->Draw(
					preBlurSourceHandle,
					depthHandle,
					maskHandle,
					motionBlurHistoryRenderTarget_->GetSrvGpuHandle(),
					FullscreenCopy::Effect::kMotionBlur
				);
				destination->End();
				sourceHandle = destination->GetSrvGpuHandle();
				++passIndex;

				storeMotionBlurHistory();
				previousMotionBlurViewProjection_ = currentViewProjection;
			}
			motionBlurWasEnabled_ = true;
		}
	}
	}
	if (settings.radialBlurEnabled) {
		FullscreenCopy::Parameters parameters{};
		parameters.radialBlurCenter[0] = radialBlurCenter_[0];
		parameters.radialBlurCenter[1] = radialBlurCenter_[1];
		parameters.radialBlurWidth = settings.radialBlurWidth;
		parameters.radialBlurSamples =
			static_cast<uint32_t>(settings.radialBlurSamples);
		applyEffect(FullscreenCopy::Effect::kRadialBlur, parameters);
	}
	if (settings.noiseEnabled) {
		FullscreenCopy::Parameters parameters{};
		parameters.noiseTime = gameViewTime;
		parameters.noiseAmount = settings.noiseAmount;
		parameters.noiseScale = settings.noiseScale;
		parameters.noiseSeed = settings.noiseSeed;
		applyEffect(FullscreenCopy::Effect::kNoise, parameters);
	}
	if (settings.dissolveEnabled) {
		FullscreenCopy::Parameters parameters{};
		parameters.dissolveThreshold = settings.dissolveThreshold;
		parameters.dissolveEdgeWidth = settings.dissolveEdgeWidth;
		for (uint32_t index = 0; index < 4; ++index) {
			parameters.dissolveEdgeColor[index] =
				dissolveEdgeColor_[index];
		}
		applyEffect(FullscreenCopy::Effect::kDissolve, parameters);
	}
	if (settings.outlineEnabled) {
		FullscreenCopy::Parameters parameters{};
		parameters.outlineLuminanceWeight = settings.outlineLuminanceWeight;
		parameters.outlineDepthWeight = settings.outlineDepthWeight;
		parameters.outlineThreshold = settings.outlineThreshold;
		parameters.outlineSoftness = settings.outlineSoftness;
		parameters.outlineThickness = settings.outlineThickness;
		Camera* camera =
			renderCamera;
		if (camera) {
			parameters.cameraNear = camera->GetNearClip();
			parameters.cameraFar = camera->GetFarClip();
		}
		parameters.outlineFlags =
			(settings.outlineLuminanceEnabled ? 1u : 0u) |
			(settings.outlineDepthEnabled ? 2u : 0u);
		for (uint32_t index = 0; index < 4; ++index) {
			parameters.outlineColor[index] = outlineColor_[index];
		}
		applyEffect(FullscreenCopy::Effect::kOutline, parameters);
	}
	if (
		settings.waterRefractionEnabled &&
		!waterRefractionAppliedBeforeForeground &&
		waterState.hasVolume &&
		!waterState.cameraInside &&
		renderCamera
	) {
		FullscreenCopy::Parameters parameters{};
		fillWaterParameters(parameters);
		applyEffect(FullscreenCopy::Effect::kWaterRefraction, parameters);
	}
	if (settings.underwaterEnabled && waterState.cameraInside && renderCamera) {
		FullscreenCopy::Parameters parameters{};
		fillWaterParameters(parameters);
		applyEffect(FullscreenCopy::Effect::kUnderwater, parameters);
	}
	if (settings.pixelationEnabled) {
		FullscreenCopy::Parameters parameters{};
		parameters.pixelationParams[0] = static_cast<float>(settings.pixelationBlockSize);
		parameters.pixelationParams[1] = static_cast<float>(postProcessRenderTargets_[0]->GetWidth());
		parameters.pixelationParams[2] = static_cast<float>(postProcessRenderTargets_[0]->GetHeight());
		applyEffect(FullscreenCopy::Effect::kPixelation, parameters);
	}
	if (settings.chromaticAberrationEnabled) {
		FullscreenCopy::Parameters parameters{};
		parameters.chromaticCenterIntensity[0] = chromaticAberrationCenter_[0];
		parameters.chromaticCenterIntensity[1] = chromaticAberrationCenter_[1];
		parameters.chromaticCenterIntensity[2] = settings.chromaticAberrationIntensity;
		parameters.chromaticParams[0] = settings.chromaticAberrationFalloff;
		applyEffect(FullscreenCopy::Effect::kChromaticAberration, parameters);
	}
        outputPass = passIndex;
        return sourceHandle;
    };
    auto liveSettings = CapturePostProcessSettings();
    Camera* liveCamera = normalEditorCamera;
    const auto preparedInstance = sceneManager_->GetPreparedDestinationRenderInstance();
    if (linkedPresentation && preparedInstance != kInvalidSceneInstanceId) {
        SceneInstanceId settingsInstance = kInvalidSceneInstanceId;
        uint64_t settingsGeneration = 0;
        if (!sceneManager_->TryGetRenderRuntimePostProcessSettings(liveSettings, settingsGeneration, settingsInstance) ||
            settingsInstance != preparedInstance) {
            presentedTransition_->issue = "Candidate scene post-process settings are unavailable.";
            presentedTransition_->drawFailed = true;
        }
        // 候補のworld描画はSceneManagerが候補cameraで行う。旧Scene cameraを画面効果に借用しない。
        liveCamera = nullptr;
    }
    int passIndex = 1;
    auto sourceHandle = processView(sceneRenderTarget_,postProcessRenderTargets_,foregroundComposeRenderTarget_,fullscreenCopy_,bloomRenderer_,
        replayActive ? replayFrame.postProcess : liveSettings, replayActive ? &replayFrame.camera : (replayFramingPreview ? &replayEditorCamera_ : liveCamera),
        replayActive ? replayFrame.effectTime : gameViewTime,false,replayActive,passIndex);
    D3D12_GPU_DESCRIPTOR_HANDLE uiSourceHandle{};
    if (drawUIEditor) {
        int unusedPass = 1;
        uiSourceHandle = processView(uiEditorSceneTarget_,uiEditorPostTargets_,uiEditorForegroundTarget_,uiEditorCopy_,uiEditorBloom_,
            liveSettings,liveCamera,noiseTime_,true,false,unusedPass);
    }
    sceneManager_->SetDeferForegroundEffects(false);
    const auto depthHandle = sceneRenderTarget_->GetDepthSrvGpuHandle();
    const auto maskHandle = TextureManager::GetInstance()->GetSrvHandleGPU("resources/noise0.png");
    SceneMenuSource replayMenuDrawn{};
	if (sceneManager_ && ((!replayActive && sceneManager_->HasScreenOverlay()) || (replayActive && replayFrame.finishedHold))) {
		textOverlayRenderTarget_->Begin(false, false);
		srvManager_->PreDraw();
		fullscreenCopy_->SetParameters(FullscreenCopy::Parameters{});
		fullscreenCopy_->Draw(
			sourceHandle,
			depthHandle,
			maskHandle,
			FullscreenCopy::Effect::kCopy
		);
        if (replayActive) sceneManager_->DrawReplayMenu(replayFrame.token,replayMenuDrawn);
        else sceneManager_->DrawScreenOverlay(renderWidth, renderHeight);
		textOverlayRenderTarget_->End();
		sourceHandle = textOverlayRenderTarget_->GetSrvGpuHandle();
	}
	if (drawUIEditor) {
		// ScreenOverlayだけはPostEffectの後。同じCanvas/Hierarchy順で一度だけ描く。
		uiEditorOverlayTarget_->Begin(); srvManager_->PreDraw(); uiEditorCopy_->SetParameters({});
		uiEditorCopy_->Draw(uiSourceHandle, uiEditorSceneTarget_->GetDepthSrvGpuHandle(), maskHandle, FullscreenCopy::Effect::kCopy);
		sceneManager_->DrawEditorUIView("ScreenOverlay", uiWidth, uiHeight);
		uiEditorOverlayTarget_->End();
	}
	sourceHandle = DrawPresentedTransition(sourceHandle,sceneRenderTarget_->GetWidth(),sceneRenderTarget_->GetHeight());

#if defined(_DEBUG) || defined(DEVELOPMENT)
	SceneRenderTarget* const postProcessOutputTarget =
		postProcessRenderTargets_[(passIndex - 1) % 2];
	if (editorSession_) {
		const bool linkedImage = presentedTransition_ && presentedTransition_->linked &&
			presentedTransition_->renderedTextureUsed;
		if (linkedImage) imguiManager_->RequireMainDockGameImage(replayDrawSerial_, sourceHandle.ptr);
		imguiManager_->SetUIEditorViewTexture(
			drawUIEditor ? uiEditorOverlayTarget_->GetSrvGpuHandle() : D3D12_GPU_DESCRIPTOR_HANDLE{},
			drawUIEditor ? uiView : SceneUIEditorView{},
			drawUIEditor ? uiEditorOverlayTarget_->GetWidth() : 0,
			drawUIEditor ? uiEditorOverlayTarget_->GetHeight() : 0);
        if (replayFramingPreview) Object3dCommon::GetInstance()->SetDefaultCamera(&replayEditorCamera_);
		imguiManager_->DrawEditorWorkspace(
			sourceHandle,
			linkedImage ? presentedTransition_->width : postProcessOutputTarget->GetWidth(),
			linkedImage ? presentedTransition_->height : postProcessOutputTarget->GetHeight(),
			sceneManager_->GetCurrentSceneName().c_str()
		);
        Object3dCommon::GetInstance()->SetDefaultCamera(normalEditorCamera);
		if (presentedTransition_ && !IsPresentedTransitionDrawable()) presentedTransition_->controller.DiscardDrawReceipt();
	}
#endif

	dxCommon_->PreDraw();
	srvManager_->PreDraw();
	fullscreenCopy_->Draw(sourceHandle, depthHandle, maskHandle);
#if defined(_DEBUG) || defined(DEVELOPMENT)
	imguiManager_->EndFrame(replayActive || drawUIEditor || (presentedTransition_ && presentedTransition_->renderedTextureUsed));
#endif

	uint64_t textureRecordingSerial = 0;
	int64_t textureRecordObservedUs = 0; // TEMP: barrier→PostDraw間は値だけ採取し、診断確保/ログを挟まない。
	if (textureDirect_ && textureDirect_->phase == TextureDirectState::Phase::AwaitRecord) {
		auto& state = *textureDirect_;
		constexpr auto invalidSerial = (std::numeric_limits<uint64_t>::max)();
		if (!state.texture.resource || state.serialCounter >= invalidSerial-1) {
			state.phase = TextureDirectState::Phase::Failed; endRequest_ = true; return;
		}
		const bool gpu = state.texture.mipPolicy == SceneTextureMipPolicy::GpuSrgbBox4096;
		bool ready = false;
		if (gpu) {
			try {
				std::vector<Microsoft::WRL::ComPtr<IUnknown>> refs;
				ready = gpuMipGenerator_ && gpuMipGenerator_->Prepare(*state.texture.resource.Get(),refs,state.mipDiagnostic) &&
					dxCommon_->TryRetainObjectsForNextPostDraw(refs);
			} catch (...) { ready = false; }
			// まだGPU未記録。通常frameは提出し、次Updateで安全なCPU retryを返す。
			if (!ready) state.phase = TextureDirectState::Phase::RetryCpuMips;
		} else {
			ready = dxCommon_->TryRetainResourceForNextPostDraw(state.texture.resource.Get());
			if (!ready) { state.phase = TextureDirectState::Phase::Failed; endRequest_ = true; return; }
		}
		if (ready) {
			textureRecordingSerial = ++state.serialCounter;
			state.texture.recordingSerial = textureRecordingSerial;
			if (gpu) {
				state.mipRecordedAt = std::chrono::steady_clock::now();
				gpuMipGenerator_->Record(*dxCommon_->GetCommandList(),*state.texture.resource.Get());
				srvManager_->PreDraw();
				state.mipRecordCpuUs = std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now()-state.mipRecordedAt).count();
			} else {
				D3D12_RESOURCE_BARRIER barrier{};
				barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barrier.Transition = {state.texture.resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
					D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_GENERIC_READ};
				dxCommon_->GetCommandList()->ResourceBarrier(1,&barrier);
			}
			state.phase = TextureDirectState::Phase::Recorded;
			textureRecordObservedUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
	}
	// バリアからここまで別提出/割当/取消を挟まない。同じPostDrawの証跡だけを関連付ける。
	const DirectFrameResult directFrame = dxCommon_->PostDraw();
	const auto textureFenceObservedUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	if (textureRecordingSerial != 0 && textureDirect_ &&
		textureDirect_->phase == TextureDirectState::Phase::Recorded &&
		textureDirect_->texture.recordingSerial == textureRecordingSerial) {
		auto& state = *textureDirect_;
		state.frameResult = directFrame;
		TraceSceneUpload(sceneManager_,sceneTextureUploadTicket_,"direct_record",0,0,textureRecordingSerial,textureRecordObservedUs);
		constexpr auto invalidFence = (std::numeric_limits<uint64_t>::max)();
		if (directFrame.CanContinue() && !state.cancelled && directFrame.signaledFence != 0 &&
			directFrame.signaledFence < invalidFence &&
			directFrame.completedFence >= directFrame.signaledFence && directFrame.completedFence < invalidFence) {
			state.receiptSerial = textureRecordingSerial;
			state.texture.signaledFence = directFrame.signaledFence;
			state.texture.completedFence = directFrame.completedFence;
			state.phase = TextureDirectState::Phase::ReceiptReady;
			TraceSceneUpload(sceneManager_,sceneTextureUploadTicket_,"direct_done",directFrame.signaledFence,directFrame.completedFence,
				textureRecordingSerial,textureFenceObservedUs);
			if (state.texture.mipPolicy == SceneTextureMipPolicy::GpuSrgbBox4096) {
				// TEMP: 比較完了後に撤去。wallは描画/待機込みでありGPU純時間ではない。
				try {
					const auto desc = state.texture.resource->GetDesc();
					const auto allocation = dxCommon_->GetDevice()->GetResourceAllocationInfo(0,1,&desc);
					const auto token = sceneTextureUploadTicket_ ? sceneTextureUploadTicket_->key.token : ScenePrepareToken{};
					Logger::Log(std::format("[LoadTiming] gpu_mip_complete token={} generation={} upload={} key=\"{}\" target_mips={} "
						"record_cpu_us={} direct_completion_wall_us={} gpu_allocation_bytes={} signaled_fence={} completed_fence={}\n",
						token.instanceId,token.generation,state.texture.jobGeneration,state.texture.textureKey,kSceneGpuMipCount,state.mipRecordCpuUs,
						std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-state.mipRecordedAt).count(),
						allocation.SizeInBytes,state.texture.signaledFence,state.texture.completedFence));
				} catch (...) { /* 完了計測はreceiptへ影響させない。 */ }
			}
		} else {
			state.phase = TextureDirectState::Phase::Failed;
		}
	}
	if (!HandleDirectFrameResult(directFrame)) {
		endRequest_ = true;
		return;
	}
	ResolveLoadLinkedFrame(sourceHandle.ptr, directFrame);
#if defined(_DEBUG) || defined(DEVELOPMENT)
	// UI編集/演出のRTをmain送信後にplatformへ渡し、最後の利用まで既存Fenceで待つ。
	if (imguiManager_->RenderDeferredPlatformWindows()) dxCommon_->ExecuteCommandListAndWait();
#endif
	if (sceneManager_) sceneManager_->NotifyReplayGpuCompleted();
    if (replayActive && IsPresentedTransitionDrawable() && IsWindowVisible(winApp_->GetHwnd()) && !sceneManager_->IsTransitionHeld() &&
        !(presentedTransition_ && presentedTransition_->controller.IsActive())) {
        sceneManager_->AcceptReplayPresentationReceipt(replayFrame.token);
        if (replayMenuDrawn.panelId) sceneManager_->AcceptReplayMenuReceipt(replayFrame.token,replayMenuDrawn);
    }
	if (presentedTransition_) presentedTransition_->completedSerial = presentedTransition_->frameSerial;
}

Game::CameraSnapshot Game::CaptureCameraSnapshot() const {
	CameraSnapshot snapshot{};
	const Object3dCommon* object3dCommon = Object3dCommon::GetInstance();
	const Camera* camera = object3dCommon
		? object3dCommon->GetDefaultCamera()
		: nullptr;
	if (!camera) {
		return snapshot;
	}

	snapshot.valid = true;
	snapshot.orbitMode = camera->IsOrbitMode();
	snapshot.translate = camera->GetTranslate();
	snapshot.rotate = camera->GetRotate();
	snapshot.orbitTarget = camera->GetOrbitTarget();
	snapshot.orbitDistance = camera->GetOrbitDistance();
	snapshot.orbitYaw = camera->GetOrbitYaw();
	snapshot.orbitPitch = camera->GetOrbitPitch();
	snapshot.fovY = camera->GetFovY();
	snapshot.aspectRatio = camera->GetAspectRatio();
	snapshot.nearClip = camera->GetNearClip();
	snapshot.farClip = camera->GetFarClip();
	return snapshot;
}

void Game::RestoreCameraSnapshot(const CameraSnapshot& snapshot) const {
	if (!snapshot.valid) {
		return;
	}
	Object3dCommon* object3dCommon = Object3dCommon::GetInstance();
	Camera* camera = object3dCommon
		? object3dCommon->GetDefaultCamera()
		: nullptr;
	if (!camera) {
		return;
	}

	camera->SetOrbitMode(snapshot.orbitMode);
	camera->SetTranslate(snapshot.translate);
	camera->SetRotate(snapshot.rotate);
	camera->SetOrbitTarget(snapshot.orbitTarget);
	camera->SetOrbitDistance(snapshot.orbitDistance);
	camera->SetOrbitAngle(snapshot.orbitYaw, snapshot.orbitPitch);
	camera->SetFovY(snapshot.fovY);
	camera->SetAspectRatio(snapshot.aspectRatio);
	camera->SetNearClip(snapshot.nearClip);
	camera->SetFarClip(snapshot.farClip);
	camera->UpdatePreviewMatrices();
}

void Game::BeginPauseDebugCamera() {
	pauseMainCameraSnapshot_ = CaptureCameraSnapshot();
}

void Game::EndPauseDebugCamera() {
	pauseMainCameraSnapshot_ = {};
}

std::vector<Game::WaterPostEffectState> Game::CaptureWaterPostEffectStates(bool activeOnly) const {
    std::vector<WaterPostEffectState> result;
    if (!sceneManager_) return result;

	std::vector<const SceneDocument*> documents;
	const SceneDocument* activeDocument =
		sceneManager_->GetActiveSceneDocument();
	if (activeDocument) {
		documents.push_back(activeDocument);
	}
	if (!activeOnly) for (const SceneInstance* instance :
		sceneManager_->GetLoadedSceneInstances()) {
		const SceneDocument* document = instance
			? instance->GetDocument()
			: nullptr;
		if (
			document &&
			std::find(documents.begin(), documents.end(), document) ==
				documents.end()
		) {
			documents.push_back(document);
		}
	}
	if (documents.empty()) {
		return result;
	}


	for (const SceneDocument* document : documents) {
	for (const SceneEntity& entity : document->GetEntities()) {
		if (!IsEntityActiveInHierarchy(*document, entity)) {
			continue;
		}
		const SceneComponent* waterVolume =
			FindEnabledComponent(entity, "WaterVolume");
		if (!waterVolume) {
			continue;
		}

		const Transform transform =
			ResolveScene3DTransform(*document, entity);
		WaterPostEffectState candidate{};
		candidate.hasVolume = true;
		candidate.center = {
			transform.translate.x + waterVolume->waterOffset.x,
			transform.translate.y + waterVolume->waterOffset.y,
			transform.translate.z + waterVolume->waterOffset.z
		};
		candidate.halfSize = {
			(std::max)(waterVolume->waterHalfSize.x, 0.001f),
			(std::max)(waterVolume->waterHalfSize.y, 0.001f),
			(std::max)(waterVolume->waterHalfSize.z, 0.001f)
		};
		candidate.lightShaftEnabled =
			waterVolume->waterLightShaftEnabled;
		candidate.lightColor = waterVolume->waterLightColor;
		candidate.lightDirection = waterVolume->waterLightDirection;
		candidate.lightIntensity =
			(std::max)(waterVolume->waterLightIntensity, 0.0f);
		candidate.lightDensity =
			(std::max)(waterVolume->waterLightDensity, 0.0f);
		candidate.causticsIntensity =
			(std::max)(waterVolume->waterLightCausticsIntensity, 0.0f);
		candidate.causticsScale =
			(std::max)(waterVolume->waterLightCausticsScale, 0.001f);
		candidate.causticsSpeed = waterVolume->waterLightCausticsSpeed;
		candidate.lightBreakupStrength = std::clamp(
			waterVolume->waterLightBreakupStrength,
			0.0f,
			3.0f
		);
		candidate.lightWarpStrength = std::clamp(
			waterVolume->waterLightWarpStrength,
			0.0f,
			3.0f
		);
		candidate.lightNoiseScale =
			(std::max)(waterVolume->waterLightNoiseScale, 0.001f);
		candidate.lightSampleCount = std::clamp(
			waterVolume->waterLightSampleCount,
			4,
			32
		);
		result.push_back(candidate);
	}
	}

	return result;
}

Game::WaterPostEffectState Game::ResolveWaterPostEffectState(const Camera* camera,
    const std::vector<WaterPostEffectState>* frozen) const {
    if (!camera) return {};
    const auto live = frozen ? std::vector<WaterPostEffectState>{} : CaptureWaterPostEffectStates(false);
    const auto& volumes = frozen ? *frozen : live;
    WaterPostEffectState first{};
    for (auto candidate : volumes) {
        candidate.cameraInside=IsPointInsideAabb(camera->GetTranslate(),candidate.center,candidate.halfSize);
        if (!first.hasVolume) first=candidate;
        if (candidate.cameraInside) return candidate;
    }
    return first;
}

ScenePostProcessSettings Game::CapturePostProcessSettings() const {
	ScenePostProcessSettings settings{};
	settings.bloomEnabled = bloomParameters_.enabled != 0;
	settings.baseExposure = baseExposure_;
	settings.toneMapMode = bloomParameters_.toneMapMode;
	settings.bloomThreshold = bloomParameters_.threshold;
	settings.bloomSoftKnee = bloomParameters_.softKnee;
	settings.bloomIntensity = bloomParameters_.intensity;
	settings.bloomBlurIterations = bloomParameters_.blurIterations;
	settings.bloomDownsampleScale = bloomParameters_.downsampleScale;
	settings.bloomBlurRadius = bloomParameters_.blurRadius;
	settings.grayscaleEnabled = grayscaleEnabled_;
	settings.vignetteEnabled = vignetteEnabled_;
	settings.boxBlurEnabled = boxBlurEnabled_;
	settings.gaussianBlurEnabled = gaussianBlurEnabled_;
	settings.depthOfFieldEnabled = depthOfFieldEnabled_;
	settings.motionBlurEnabled = motionBlurEnabled_;
	settings.motionBlurStrength = motionBlurStrength_;
	settings.motionBlurSamples = motionBlurSamples_;
	settings.motionBlurMaxRadius = motionBlurMaxRadius_;
	settings.radialBlurEnabled = radialBlurEnabled_;
	settings.noiseEnabled = noiseEnabled_;
	settings.dissolveEnabled = dissolveEnabled_;
	settings.outlineEnabled = outlineEnabled_;
	settings.underwaterEnabled = underwaterEnabled_;
	settings.waterRefractionEnabled = waterRefractionEnabled_;
	settings.pixelationEnabled = pixelationEnabled_;
	settings.pixelationBlockSize = pixelationBlockSize_;
	settings.chromaticAberrationEnabled = chromaticAberrationEnabled_;
	settings.chromaticAberrationCenter = { chromaticAberrationCenter_[0], chromaticAberrationCenter_[1] };
	settings.chromaticAberrationIntensity = chromaticAberrationIntensity_;
	settings.chromaticAberrationFalloff = chromaticAberrationFalloff_;
	settings.vignetteScale = vignetteScale_;
	settings.vignettePower = vignettePower_;
	settings.vignetteIntensity = vignetteIntensity_;
	settings.boxBlurKernelSize = boxBlurKernelSize_;
	settings.boxBlurStrength = boxBlurStrength_;
	settings.gaussianBlurKernelSize = gaussianBlurKernelSize_;
	settings.gaussianBlurSigma = gaussianBlurSigma_;
	settings.gaussianBlurStrength = gaussianBlurStrength_;
	settings.depthOfFieldFocusDistance = dofFocusDistance_;
	settings.depthOfFieldFocusRange = dofFocusRange_;
	settings.depthOfFieldBlurStrength = dofBlurStrength_;
	settings.depthOfFieldNearStrength = dofNearStrength_;
	settings.depthOfFieldFarStrength = dofFarStrength_;
	settings.depthOfFieldMaxRadius = dofMaxRadius_;
	settings.radialBlurCenter = {
		radialBlurCenter_[0],
		radialBlurCenter_[1]
	};
	settings.radialBlurWidth = radialBlurWidth_;
	settings.radialBlurSamples = radialBlurSamples_;
	settings.noiseAnimate = noiseAnimate_;
	settings.noiseAmount = noiseAmount_;
	settings.noiseScale = noiseScale_;
	settings.noiseSpeed = noiseSpeed_;
	settings.noiseSeed = noiseSeed_;
	settings.dissolveMaskIndex = dissolveMaskIndex_;
	settings.dissolveThreshold = dissolveThreshold_;
	settings.dissolveEdgeWidth = dissolveEdgeWidth_;
	settings.dissolveEdgeColor = {
		dissolveEdgeColor_[0],
		dissolveEdgeColor_[1],
		dissolveEdgeColor_[2],
		dissolveEdgeColor_[3]
	};
	settings.outlineLuminanceEnabled = outlineLuminanceEnabled_;
	settings.outlineDepthEnabled = outlineDepthEnabled_;
	settings.outlineLuminanceWeight = outlineLuminanceWeight_;
	settings.outlineDepthWeight = outlineDepthWeight_;
	settings.outlineThreshold = outlineThreshold_;
	settings.outlineSoftness = outlineSoftness_;
	settings.outlineThickness = outlineThickness_;
	settings.outlineColor = {
		outlineColor_[0],
		outlineColor_[1],
		outlineColor_[2],
		outlineColor_[3]
	};
	settings.underwaterTintColor = {
		underwaterTintColor_[0],
		underwaterTintColor_[1],
		underwaterTintColor_[2],
		underwaterTintColor_[3]
	};
	settings.underwaterIntensity = underwaterIntensity_;
	settings.underwaterFogDensity = underwaterFogDensity_;
	settings.underwaterDistortion = underwaterDistortion_;
	settings.waterRefractionTintColor = {
		waterRefractionTintColor_[0],
		waterRefractionTintColor_[1],
		waterRefractionTintColor_[2],
		waterRefractionTintColor_[3]
	};
	settings.waterRefractionStrength = waterRefractionStrength_;
	settings.waterRefractionEdgeSoftness = waterRefractionEdgeSoftness_;
	settings.waterRefractionTintStrength = waterRefractionTintStrength_;
	return settings;
}

void Game::ApplyPostProcessSettings(
	const ScenePostProcessSettings& settings
) {
	bloomParameters_.enabled = settings.bloomEnabled ? 1 : 0;
	baseExposure_ = settings.baseExposure;
	currentExposure_ = baseExposure_;
	bloomParameters_.exposure = baseExposure_;
	bloomParameters_.toneMapMode = settings.toneMapMode;
	bloomParameters_.threshold = settings.bloomThreshold;
	bloomParameters_.softKnee = settings.bloomSoftKnee;
	bloomParameters_.intensity = settings.bloomIntensity;
	bloomParameters_.blurIterations = settings.bloomBlurIterations;
	bloomParameters_.downsampleScale = settings.bloomDownsampleScale;
	bloomParameters_.blurRadius = settings.bloomBlurRadius;
	grayscaleEnabled_ = settings.grayscaleEnabled;
	vignetteEnabled_ = settings.vignetteEnabled;
	boxBlurEnabled_ = settings.boxBlurEnabled;
	gaussianBlurEnabled_ = settings.gaussianBlurEnabled;
	depthOfFieldEnabled_ = settings.depthOfFieldEnabled;
	motionBlurEnabled_ = settings.motionBlurEnabled;
	motionBlurStrength_ = settings.motionBlurStrength;
	motionBlurSamples_ = settings.motionBlurSamples;
	motionBlurMaxRadius_ = settings.motionBlurMaxRadius;
	radialBlurEnabled_ = settings.radialBlurEnabled;
	noiseEnabled_ = settings.noiseEnabled;
	dissolveEnabled_ = settings.dissolveEnabled;
	outlineEnabled_ = settings.outlineEnabled;
	underwaterEnabled_ = settings.underwaterEnabled;
	waterRefractionEnabled_ = settings.waterRefractionEnabled;
	pixelationEnabled_ = settings.pixelationEnabled;
	pixelationBlockSize_ = settings.pixelationBlockSize;
	chromaticAberrationEnabled_ = settings.chromaticAberrationEnabled;
	chromaticAberrationCenter_[0] = settings.chromaticAberrationCenter.x;
	chromaticAberrationCenter_[1] = settings.chromaticAberrationCenter.y;
	chromaticAberrationIntensity_ = settings.chromaticAberrationIntensity;
	chromaticAberrationFalloff_ = settings.chromaticAberrationFalloff;
	vignetteScale_ = settings.vignetteScale;
	vignettePower_ = settings.vignettePower;
	vignetteIntensity_ = settings.vignetteIntensity;
	boxBlurKernelSize_ = settings.boxBlurKernelSize;
	boxBlurStrength_ = settings.boxBlurStrength;
	gaussianBlurKernelSize_ = settings.gaussianBlurKernelSize;
	gaussianBlurSigma_ = settings.gaussianBlurSigma;
	gaussianBlurStrength_ = settings.gaussianBlurStrength;
	dofFocusDistance_ = settings.depthOfFieldFocusDistance;
	dofFocusRange_ = settings.depthOfFieldFocusRange;
	dofBlurStrength_ = settings.depthOfFieldBlurStrength;
	dofNearStrength_ = settings.depthOfFieldNearStrength;
	dofFarStrength_ = settings.depthOfFieldFarStrength;
	dofMaxRadius_ = settings.depthOfFieldMaxRadius;
	radialBlurCenter_[0] = settings.radialBlurCenter.x;
	radialBlurCenter_[1] = settings.radialBlurCenter.y;
	radialBlurWidth_ = settings.radialBlurWidth;
	radialBlurSamples_ = settings.radialBlurSamples;
	noiseAnimate_ = settings.noiseAnimate;
	noiseAmount_ = settings.noiseAmount;
	noiseScale_ = settings.noiseScale;
	noiseSpeed_ = settings.noiseSpeed;
	noiseSeed_ = settings.noiseSeed;
	dissolveMaskIndex_ = settings.dissolveMaskIndex;
	dissolveThreshold_ = settings.dissolveThreshold;
	dissolveEdgeWidth_ = settings.dissolveEdgeWidth;
	dissolveEdgeColor_[0] = settings.dissolveEdgeColor.x;
	dissolveEdgeColor_[1] = settings.dissolveEdgeColor.y;
	dissolveEdgeColor_[2] = settings.dissolveEdgeColor.z;
	dissolveEdgeColor_[3] = settings.dissolveEdgeColor.w;
	outlineLuminanceEnabled_ = settings.outlineLuminanceEnabled;
	outlineDepthEnabled_ = settings.outlineDepthEnabled;
	outlineLuminanceWeight_ = settings.outlineLuminanceWeight;
	outlineDepthWeight_ = settings.outlineDepthWeight;
	outlineThreshold_ = settings.outlineThreshold;
	outlineSoftness_ = settings.outlineSoftness;
	outlineThickness_ = settings.outlineThickness;
	outlineColor_[0] = settings.outlineColor.x;
	outlineColor_[1] = settings.outlineColor.y;
	outlineColor_[2] = settings.outlineColor.z;
	outlineColor_[3] = settings.outlineColor.w;
	underwaterTintColor_[0] = settings.underwaterTintColor.x;
	underwaterTintColor_[1] = settings.underwaterTintColor.y;
	underwaterTintColor_[2] = settings.underwaterTintColor.z;
	underwaterTintColor_[3] = settings.underwaterTintColor.w;
	underwaterIntensity_ = settings.underwaterIntensity;
	underwaterFogDensity_ = settings.underwaterFogDensity;
	underwaterDistortion_ = settings.underwaterDistortion;
	waterRefractionTintColor_[0] = settings.waterRefractionTintColor.x;
	waterRefractionTintColor_[1] = settings.waterRefractionTintColor.y;
	waterRefractionTintColor_[2] = settings.waterRefractionTintColor.z;
	waterRefractionTintColor_[3] = settings.waterRefractionTintColor.w;
	waterRefractionStrength_ = settings.waterRefractionStrength;
	waterRefractionEdgeSoftness_ = settings.waterRefractionEdgeSoftness;
	waterRefractionTintStrength_ = settings.waterRefractionTintStrength;
}

void Game::UpdateViewEffectClock() {
	const bool resetRequested = resetViewEffectTimeRequested_;
	resetViewEffectTimeRequested_ = false;
	if (resetRequested) gameEffectTime_ = 0.0;
	SceneViewEffectClock clock{};
	SceneInstanceId instance = kInvalidSceneInstanceId;
	const bool runtimeView = !executionContext_ || !executionContext_->IsEditing();
	if (!runtimeView || !sceneManager_ || !sceneManager_->TryGetActiveViewEffectClock(clock, instance)) {
		viewEffectClockBound_ = false;
		viewEffectClockInstance_ = kInvalidSceneInstanceId;
		viewEffectClockGeneration_ = 0;
		lastObservedWorldSeconds_ = gameEffectTime_ = 0.0;
		return;
	}
	const bool sameBinding = viewEffectClockBound_ && instance == viewEffectClockInstance_ &&
		clock.contentGeneration == viewEffectClockGeneration_;
	if (!sameBinding) viewEffectClockBound_ = false;
	const double elapsed = clock.deathWorldElapsedSeconds;
	if (!std::isfinite(elapsed) || elapsed < 0.0 ||
		(sameBinding && elapsed < lastObservedWorldSeconds_)) return;
	if (!sameBinding) {
		viewEffectClockInstance_ = instance;
		viewEffectClockGeneration_ = clock.contentGeneration;
		// 初表示時より前の累積をまとめて足さず、その画面の現在位相を引き継ぐ。
		gameEffectTime_ = resetRequested ? 0.0 : (std::isfinite(noiseTime_) ? noiseTime_ : 0.0);
		lastObservedWorldSeconds_ = elapsed;
		viewEffectClockBound_ = true;
		return;
	}
	const double delta = elapsed - lastObservedWorldSeconds_;
	// Animate off/不正な倍率/Resetでもcursorを進め、再有効化時の追い付き加算を防ぐ。
	lastObservedWorldSeconds_ = elapsed;
	if (resetRequested || !noiseAnimate_ || !std::isfinite(noiseSpeed_)) return;
	const double next = gameEffectTime_ + delta * static_cast<double>(noiseSpeed_);
	if (std::isfinite(next) && std::abs(next) <= static_cast<double>((std::numeric_limits<float>::max)())) {
		gameEffectTime_ = next;
	}
}

float Game::GetGameViewEffectTime() const {
	SceneViewEffectClock clock{};
	SceneInstanceId instance = kInvalidSceneInstanceId;
	if (viewEffectClockBound_ && (!executionContext_ || !executionContext_->IsEditing()) && sceneManager_ &&
		sceneManager_->TryGetActiveViewEffectClock(clock, instance) && instance == viewEffectClockInstance_ &&
		clock.contentGeneration == viewEffectClockGeneration_) {
		return static_cast<float>(gameEffectTime_);
	}
	return noiseTime_;
}

void Game::ApplyRuntimePostProcessSettings() {
	if (!sceneManager_ || !executionContext_ || !executionContext_->IsPlaying()) {
		appliedRuntimePostProcessGeneration_ = static_cast<uint64_t>(-1);
		appliedRuntimePostProcessInstanceId_ = kInvalidSceneInstanceId;
		return;
	}
	ScenePostProcessSettings settings{};
	uint64_t generation = 0;
	if (!sceneManager_->TryGetActiveRuntimePostProcessSettings(
		settings, generation
	)) {
		return;
	}
	const SceneInstanceId instanceId = sceneManager_->GetActiveSceneInstanceId();
	if (
		instanceId == appliedRuntimePostProcessInstanceId_ &&
		generation == appliedRuntimePostProcessGeneration_
	) {
		return;
	}
	ApplyPostProcessSettings(settings);
	appliedRuntimePostProcessInstanceId_ = instanceId;
	appliedRuntimePostProcessGeneration_ = generation;
}

void Game::StorePostProcessSettingsToDocument() {
	if (!editorSession_ || !editorSession_->IsEditing()) {
		return;
	}

	SceneDocument& document = editorSession_->GetEditDocument();
	const std::string sourceKey = GetActivePostProcessSourceKey();
	if (sourceKey != appliedPostProcessSourceKey_) {
		ApplyPostProcessSettings(document.GetPostProcessSettings());
		appliedPostProcessRevision_ = document.GetRevision();
		appliedPostProcessSourceKey_ = sourceKey;
		return;
	}
	const ScenePostProcessSettings settings = CapturePostProcessSettings();
	if (EqualPostProcessSettings(settings, document.GetPostProcessSettings())) {
		appliedPostProcessRevision_ = document.GetRevision();
		appliedPostProcessSourceKey_ = sourceKey;
		return;
	}

	document.SetPostProcessSettings(settings);
	appliedPostProcessRevision_ = document.GetRevision();
	appliedPostProcessSourceKey_ = sourceKey;
}

std::string Game::GetActivePostProcessSourceKey() const {
	if (!executionContext_) {
		return {};
	}
	if (executionContext_->IsEditing()) {
		return "edit:" + executionContext_->GetActiveSceneId();
	}
	if (sceneManager_) {
		const SceneInstanceId instanceId =
			sceneManager_->GetActiveSceneInstanceId();
		if (instanceId != kInvalidSceneInstanceId) {
			return "runtime-instance:" + std::to_string(instanceId);
		}
	}
	return "runtime:" + executionContext_->GetActiveSceneId();
}

void Game::DrawModelPreview() {
	if (
		!imguiManager_ ||
		!modelPreviewRenderTarget_ ||
		!modelPreviewCamera_ ||
		!modelPreviewObject_
	) {
		return;
	}
	if (editorSession_ && !editorSession_->IsEditing()) {
		return;
	}

	std::string modelPath;
	float yaw = 0.0f;
	float pitch = 0.0f;
	float zoom = 1.0f;
	if (!imguiManager_->GetModelPreviewRequest(modelPath, yaw, pitch, zoom)) {
		return;
	}

	if (modelPath != modelPreviewPath_) {
		ModelManager* modelManager = ModelManager::GetInstance();
		modelManager->LoadModel(modelPath);
		Model* model = modelManager->FindModel(modelPath);
		if (!model) {
			return;
		}

		modelPreviewObject_->SetModel(model);
		modelPreviewObject_->SetScale({ 1.0f, 1.0f, 1.0f });
		Vector3 boundsMin{};
		Vector3 boundsMax{};
		if (model->GetLocalBounds(boundsMin, boundsMax)) {
			const Vector3 center = {
				(boundsMin.x + boundsMax.x) * 0.5f,
				(boundsMin.y + boundsMax.y) * 0.5f,
				(boundsMin.z + boundsMax.z) * 0.5f
			};
			const Vector3 halfExtent = {
				(boundsMax.x - boundsMin.x) * 0.5f,
				(boundsMax.y - boundsMin.y) * 0.5f,
				(boundsMax.z - boundsMin.z) * 0.5f
			};
			const float radius = (std::max)(
				std::sqrt(
					halfExtent.x * halfExtent.x +
					halfExtent.y * halfExtent.y +
					halfExtent.z * halfExtent.z
				),
				0.01f
			);
			modelPreviewObject_->SetTranslate({
				-center.x,
				-center.y,
				-center.z
			});
			modelPreviewFitDistance_ =
				radius / std::tan(modelPreviewCamera_->GetFovY() * 0.5f) * 1.25f;
		} else {
			modelPreviewObject_->SetTranslate({ 0.0f, 0.0f, 0.0f });
			modelPreviewFitDistance_ = 5.0f;
		}
		modelPreviewPath_ = modelPath;
	}

	modelPreviewCamera_->SetOrbitTarget({ 0.0f, 0.0f, 0.0f });
	modelPreviewCamera_->SetOrbitAngle(yaw, pitch);
	modelPreviewCamera_->SetOrbitDistance(
		(std::max)(modelPreviewFitDistance_ * zoom, 0.02f)
	);
	modelPreviewCamera_->UpdatePreviewMatrices();
	modelPreviewObject_->Update();

	modelPreviewRenderTarget_->Begin();
	srvManager_->PreDraw();
	Object3dCommon::GetInstance()->SetCommonRenderState();
	modelPreviewObject_->Draw();
	modelPreviewRenderTarget_->End();

	imguiManager_->SetModelPreviewTexture(
		modelPath,
		modelPreviewRenderTarget_->GetSrvGpuHandle(),
		modelPreviewRenderTarget_->GetWidth(),
		modelPreviewRenderTarget_->GetHeight()
	);
}

void Game::DrawPrefabPreview() {
	if (!imguiManager_ || !prefabPreviewRenderer_) {
		return;
	}
	if (editorSession_ && !editorSession_->IsEditing()) {
		return;
	}

	PrefabPreviewRequest request{};
	if (
		!imguiManager_->GetPrefabPreviewRequest(request) ||
		!request.document
	) {
        SceneReplayFramingRequest framing;
        if (imguiManager_->GetReplayFramingRequest(true,framing)) imguiManager_->SetReplayFramingResult(framing,false,{});
		return;
	}

	PrefabPreviewRenderer::OverlayOptions overlayOptions{};
	overlayOptions.selectedEntityId = request.selectedEntityId;
	overlayOptions.showSkeleton = request.showSkeleton;
	overlayOptions.showJointAxes = request.showJointAxes;
	overlayOptions.showColliders = request.showColliders;
	overlayOptions.showCombatVolumes = request.showCombatVolumes;
	overlayOptions.isolateSelectedCollider = request.isolateSelectedCollider;
	overlayOptions.showGrid = request.showGrid; overlayOptions.editVolume=request.attackView.volume;
    SceneReplayFramingRequest framing;
    const bool hasFraming=imguiManager_->GetReplayFramingRequest(true,framing);
    // Animation Previewは同じEntity IDを保つ別世代の表示用Document。元Prefabとの対応を先に照合する。
    const bool matchingFraming=hasFraming && request.assetPath==framing.sourcePath;
    auto displayFraming=framing;
    displayFraming.generation=request.document->GetContentGeneration();
	prefabPreviewRenderer_->Render(
		request.assetPath,
		*request.document,
		request.ghostDocument,
		request.width,
		request.height,
		request.yaw,
		request.pitch,
		request.zoom,
		request.framingSerial,
		overlayOptions,
        matchingFraming ? &displayFraming : nullptr, request.attackView.enabled ? &request.attackView : nullptr,
        request.animationGhost
	);
    if(request.attackView.enabled) imguiManager_->SetAttackPreviewViewResult(prefabPreviewRenderer_->GetAttackViewResult());
    if (hasFraming) {
        Camera camera; SceneReplayViewSettings manual;
        imguiManager_->SetReplayFramingResult(framing,matchingFraming && prefabPreviewRenderer_->GetReplayFramingResult(camera,manual),manual);
    }
	imguiManager_->SetPrefabPreviewTexture(
		request.assetPath,
		request.revision,
		prefabPreviewRenderer_->GetTexture(),
		prefabPreviewRenderer_->GetWidth(),
		prefabPreviewRenderer_->GetHeight(),
		prefabPreviewRenderer_->GetViewMatrix(),
		prefabPreviewRenderer_->GetProjectionMatrix(),
		prefabPreviewRenderer_->GetAnimationGhostResult()
	);
}

void Game::ClearSceneSpatialPreview() {
    // 前FrameのPostDraw完了後のみ。各poseの生GPU addressより先にDraw記録を破棄する。
    spatialPreviewModels_.clear(); spatialPreviewLines_.clear(); spatialPreviewMaterials_.clear();
    spatialPreviewBuffer_.Reset(); spatialPreviewMapped_=nullptr; spatialPreviewCapacity_=0;
    spatialPreviewGeneration_=spatialPreviewRevision_=spatialPreviewSerial_=0; spatialPreviewIssue_.clear();
}

void Game::PrepareSceneSpatialPreview() {
#if defined(_DEBUG) || defined(DEVELOPMENT)
    if (!imguiManager_ || !editorSession_ || !editorSession_->IsEditing()) { ClearSceneSpatialPreview(); return; }
    const auto& frame=imguiManager_->GetSceneSpatialPreviewFrame();
    const auto& source=editorSession_->GetEditDocument();
    if (frame.sourceGeneration==0 || frame.sourceGeneration!=source.GetContentGeneration() || frame.sourceRevision!=source.GetRevision()) {
        ClearSceneSpatialPreview(); return;
    }
    spatialPreviewGeneration_=frame.sourceGeneration; spatialPreviewRevision_=frame.sourceRevision; spatialPreviewSerial_=frame.serial;
    spatialPreviewModels_.clear(); spatialPreviewLines_.clear(); spatialPreviewIssue_.clear();
    for (const auto& line:frame.lines) spatialPreviewLines_.push_back({line.start,line.end,line.color});
    auto report=[&]() { imguiManager_->SetSceneSpatialPreviewRenderIssue(spatialPreviewIssue_,frame.serial,spatialPreviewModels_.size(),0); };
    auto addIssue=[&](const std::string& text) {
        if (spatialPreviewIssue_.find(text)!=std::string::npos) return;
        if (!spatialPreviewIssue_.empty()) spatialPreviewIssue_+='\n';
        spatialPreviewIssue_+=text;
    };
    auto* common=Object3dCommon::GetInstance();
    auto* camera=common->GetDefaultCamera();
    if (!camera || !SpatialGhostFinite(camera->GetViewProjectionMatrix()) ||
        !std::isfinite(camera->GetTranslate().x) || !std::isfinite(camera->GetTranslate().y) || !std::isfinite(camera->GetTranslate().z)) {
        addIssue("Previewカメラがありません、または不正です / Missing or invalid preview camera"); report(); return;
    }
    if (frame.meshes.size()>8200 || frame.meshes.size()+1>(std::numeric_limits<size_t>::max)()/spatialCbStride) {
        addIssue("虚像の必要数が入力上限を超えています / Ghost count exceeds the admitted input bounds"); report(); return;
    }
    if (!frame.meshes.empty() && spatialPreviewCapacity_<frame.meshes.size()) {
        // 新BufferがMapできるまで旧Bufferを保持する。失敗してもmarker/axisは残す。
        Microsoft::WRL::ComPtr<ID3D12Resource> next;
        void* mapped=nullptr; std::string issue;
        if (!CreateSpatialGhostBuffer(dxCommon_->GetDevice(),(frame.meshes.size()+1)*spatialCbStride,next,&mapped,issue)) {
            addIssue(issue); report(); return;
        }
        spatialPreviewBuffer_=std::move(next); spatialPreviewMapped_=static_cast<unsigned char*>(mapped);
        spatialPreviewCapacity_=frame.meshes.size();
    }
    if (!frame.meshes.empty()) {
        const SpatialGhostCamera cameraData{camera->GetTranslate(),0};
        std::memcpy(spatialPreviewMapped_,&cameraData,sizeof(cameraData));
        TextureManager::GetInstance()->LoadTexture("resources/rostock_laage_airport_4k.dds");
    }
    for (auto& group:spatialPreviewMaterials_) group.used=false;
    std::unordered_map<std::string,Model*> resolved;
    for (const auto& mesh:frame.meshes) {
        if (!SpatialGhostFinite(mesh.world)) { addIssue("虚像の行列が不正です / Invalid ghost matrix"); continue; }
        auto found=resolved.find(mesh.modelPath);
        if (found==resolved.end()) {
            ModelManager::GetInstance()->LoadModel(mesh.modelPath);
            found=resolved.emplace(mesh.modelPath,ModelManager::GetInstance()->FindModel(mesh.modelPath)).first;
        }
        Model* model=found->second;
        if (!model || model->HasSkinning() || model->GetMaterialSlots().empty()) {
            addIssue("Previewモデルを表示できません（static mesh/materialが必要） / Static mesh/material unavailable: "+mesh.modelPath); continue;
        }
        const auto world=Multiply(model->GetRootNodeLocalMatrix(),mesh.world);
        const SpatialGhostMatrix matrices{Multiply(world,camera->GetViewProjectionMatrix()),world};
        if (!SpatialGhostFinite(matrices.world) || !SpatialGhostFinite(matrices.wvp)) {
            addIssue("虚像のWorld/WVPが不正です / Invalid ghost World/WVP: "+mesh.modelPath); continue;
        }
        auto groupIt=std::find_if(spatialPreviewMaterials_.begin(),spatialPreviewMaterials_.end(),[&](const auto& group) {
            return group.model==model && group.tint.x==mesh.tint.x && group.tint.y==mesh.tint.y && group.tint.z==mesh.tint.z && group.tint.w==mesh.tint.w;
        });
        if (groupIt==spatialPreviewMaterials_.end()) {
            SpatialPreviewMaterials group; group.model=model; group.tint=mesh.tint;
            spatialPreviewMaterials_.push_back(std::move(group)); groupIt=std::prev(spatialPreviewMaterials_.end());
        }
        auto& group=*groupIt;
        if (!group.used) {
            group.used=true; group.valid=true;
            const auto& materials=model->GetMaterialSlots();
            if (group.owned.size()!=materials.size()) { group.owned.clear(); group.resources.clear(); }
            for (size_t i=0;i<materials.size();++i) {
                void* mapped=nullptr; std::string issue;
                if (i>=group.owned.size()) {
                    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
                    if (!CreateSpatialGhostBuffer(dxCommon_->GetDevice(),spatialCbStride,resource,&mapped,issue)) {
                        addIssue(issue); group.valid=false; break;
                    }
                    group.resources.push_back(resource.Get()); group.owned.push_back(std::move(resource));
                } else {
                    D3D12_RANGE noRead{0,0};
                    if (FAILED(group.owned[i]->Map(0,&noRead,&mapped))) {
                        addIssue("虚像材質をMapできません / Cannot map ghost material"); group.valid=false; break;
                    }
                }
                SpatialGhostMaterial data;
                const auto& color=materials[i].baseColor;
                data.color={color.x*mesh.tint.x,color.y*mesh.tint.y,color.z*mesh.tint.z,color.w*mesh.tint.w};
                std::memcpy(mapped,&data,sizeof(data)); group.owned[i]->Unmap(0,nullptr);
            }
        }
        if (!group.valid) continue;
        const size_t offset=(spatialPreviewModels_.size()+1)*spatialCbStride;
        std::memcpy(spatialPreviewMapped_+offset,&matrices,sizeof(matrices));
        SpatialPreviewModel draw;
        draw.model=model; draw.materialGroup=static_cast<size_t>(groupIt-spatialPreviewMaterials_.begin());
        draw.matrixAddress=spatialPreviewBuffer_->GetGPUVirtualAddress()+offset;
        draw.poseId=mesh.poseId; draw.moving=mesh.moving; draw.entityId=mesh.entityId; draw.cullMode=mesh.meshCullMode;
        draw.worldPosition={mesh.world.m[3][0],mesh.world.m[3][1],mesh.world.m[3][2]};
        spatialPreviewModels_.push_back(std::move(draw));
    }
    // 未使用の色/モデル資源もFence完了後に解放し、Drawのgroup indexを同時に再対応する。
    std::vector<size_t> remap(spatialPreviewMaterials_.size());
    std::vector<SpatialPreviewMaterials> retained;
    for (size_t i=0;i<spatialPreviewMaterials_.size();++i) if (spatialPreviewMaterials_[i].used) {
        remap[i]=retained.size(); retained.push_back(std::move(spatialPreviewMaterials_[i]));
    }
    for (auto& draw:spatialPreviewModels_) draw.materialGroup=remap[draw.materialGroup];
    spatialPreviewMaterials_=std::move(retained); report();
#endif
}

void Game::DrawSceneSpatialPreview() {
#if defined(_DEBUG) || defined(DEVELOPMENT)
    if (!editorSession_ || !editorSession_->IsEditing() || spatialPreviewGeneration_==0 ||
        spatialPreviewGeneration_!=editorSession_->GetEditDocument().GetContentGeneration() ||
        spatialPreviewRevision_!=editorSession_->GetEditDocument().GetRevision() ||
        imguiManager_->GetSceneSpatialPreviewFrame().serial!=spatialPreviewSerial_) return;
    const auto* camera=Object3dCommon::GetInstance()->GetDefaultCamera();
    if (!camera || !SpatialGhostFinite(camera->GetViewProjectionMatrix())) return;
    const Vector3 cameraPosition=camera->GetTranslate();
    if (!std::isfinite(cameraPosition.x) || !std::isfinite(cameraPosition.y) || !std::isfinite(cameraPosition.z)) return;
    std::vector<const SpatialPreviewModel*> sorted;
    for (const auto& draw:spatialPreviewModels_) sorted.push_back(&draw);
    auto distance=[&](const auto* draw) {
        const double x=static_cast<double>(draw->worldPosition.x)-cameraPosition.x;
        const double y=static_cast<double>(draw->worldPosition.y)-cameraPosition.y;
        const double z=static_cast<double>(draw->worldPosition.z)-cameraPosition.z; return x*x+y*y+z*z;
    };
    std::sort(sorted.begin(),sorted.end(),[&](const auto* a,const auto* b) {
        const double da=distance(a), db=distance(b);
        if (da!=db) return da>db;
        if (a->moving!=b->moving) return a->moving<b->moving;
        return std::pair<uint32_t,uint64_t>{a->poseId,a->entityId}<std::pair<uint32_t,uint64_t>{b->poseId,b->entityId};
    });
    auto* common=Object3dCommon::GetInstance();
    auto* commandList=dxCommon_->GetCommandList();
    for (const auto* draw:sorted) {
        common->SetEditorGhostRenderState(draw->cullMode=="None" ? Object3dCommon::CullMode::kNone :
            draw->cullMode=="Front" ? Object3dCommon::CullMode::kFront : Object3dCommon::CullMode::kBack);
        commandList->SetGraphicsRootConstantBufferView(1,draw->matrixAddress);
        commandList->SetGraphicsRootConstantBufferView(4,spatialPreviewBuffer_->GetGPUVirtualAddress());
        commandList->SetGraphicsRootDescriptorTable(7,TextureManager::GetInstance()->GetSrvHandleGPU("resources/rostock_laage_airport_4k.dds"));
        draw->model->DrawWithMaterialSlots(spatialPreviewMaterials_[draw->materialGroup].resources,{});
    }
    for (const auto& line:spatialPreviewLines_) DebugRenderer::GetInstance()->AddDepthTestedLine(line.start,line.end,line.color);
    imguiManager_->SetSceneSpatialPreviewRenderIssue(spatialPreviewIssue_,spatialPreviewSerial_,spatialPreviewModels_.size(),sorted.size());
#endif
}

void Game::Finalize() {
	if (sceneManager_ && presentedTransition_ && presentedTransition_->linked) {
		const auto& linked = *presentedTransition_->linked;
		if (linked.token.instanceId != kInvalidSceneInstanceId && !linked.committed)
			sceneManager_->CancelScenePreparation(linked.token);
	}
	if (sceneManager_ && sceneTextureUploadTicket_) {
		sceneManager_->CancelScenePreparation(sceneTextureUploadTicket_->key.token);
	}
	if (sceneManager_ && materialCopyLookaheadTicket_)
		sceneManager_->CancelScenePreparation(materialCopyLookaheadTicket_->key.token);
	DrainTextureUploadForShutdown();
	if (sceneManager_ && sceneTextureUploadTicket_) {
		sceneManager_->RetireSceneTextureUploadForShutdown(*sceneTextureUploadTicket_);
		sceneTextureUploadTicket_.reset();
	}
	if (sceneManager_ && materialCopyLookaheadTicket_) {
		sceneManager_->RetireMaterialCopyLookahead(*materialCopyLookaheadTicket_);
		materialCopyLookaheadTicket_.reset();
	}
	CancelTextureDirect();
	textureDirect_.reset();
	gpuMipGenerator_.reset();
	CancelPresentedTransition();
	presentedTransition_.reset();
	ClearSceneSpatialPreview();
	if (Input* input = Input::GetInstance()) {
		input->SetCursorCapture(false);
	}
	delete startupErrorScreen_;
	startupErrorScreen_ = nullptr;

#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (imguiManager_) {
		imguiManager_->SetEditorSession(nullptr);
		imguiManager_->SetSceneCatalog(nullptr);
		imguiManager_->SetSceneManager(nullptr);
		imguiManager_->SetSceneTemplateRegistry(nullptr);
	}
#endif
	delete visualStudioLocator_;
	visualStudioLocator_ = nullptr;
	delete projectCreationService_;
	projectCreationService_ = nullptr;
	delete projectSolutionGenerationService_;
	projectSolutionGenerationService_ = nullptr;
	delete projectRegistry_;
	projectRegistry_ = nullptr;
	projectLauncherInitialized_ = false;
	pendingCreateOpenSolution_ = false;
	pendingCreateVisualStudioInstanceId_.clear();
	delete modelPreviewObject_;
	modelPreviewObject_ = nullptr;
	delete prefabPreviewRenderer_;
	prefabPreviewRenderer_ = nullptr;
	delete modelPreviewCamera_;
	modelPreviewCamera_ = nullptr;
	delete modelPreviewRenderTarget_;
	modelPreviewRenderTarget_ = nullptr;

	// 既存Frame/Platform WindowのFence待機後、Game所有のView資源を破棄する。
	delete uiEditorBloom_; uiEditorBloom_ = nullptr;
	delete uiEditorCopy_; uiEditorCopy_ = nullptr;
	delete uiEditorSceneTarget_; uiEditorSceneTarget_ = nullptr;
	delete uiEditorForegroundTarget_; uiEditorForegroundTarget_ = nullptr;
	delete uiEditorOverlayTarget_; uiEditorOverlayTarget_ = nullptr;
	for (auto*& target : uiEditorPostTargets_) { delete target; target = nullptr; }
	delete bloomRenderer_;
	bloomRenderer_ = nullptr;

	delete fullscreenCopy_;
	fullscreenCopy_ = nullptr;

	delete sceneRenderTarget_;
	sceneRenderTarget_ = nullptr;

	delete motionBlurHistoryRenderTarget_;
	motionBlurHistoryRenderTarget_ = nullptr;

	for (SceneRenderTarget*& renderTarget : postProcessRenderTargets_) {
		delete renderTarget;
		renderTarget = nullptr;
	}
	delete textOverlayRenderTarget_;
	textOverlayRenderTarget_ = nullptr;
	delete foregroundComposeRenderTarget_;
	foregroundComposeRenderTarget_ = nullptr;

	sceneManager_ = nullptr;
	executionContext_ = nullptr;
	editorSession_ = nullptr;
	sceneAssetService_ = nullptr;
	sceneTemplateRegistry_ = nullptr;
	delete editorBootstrap_;
	editorBootstrap_ = nullptr;
	delete runtimeBootstrap_;
	runtimeBootstrap_ = nullptr;
	delete sceneCatalog_;
	sceneCatalog_ = nullptr;

	// 基底クラスの終了処理
	Framework::Finalize();
}

int Game::GetEnabledPostEffectCount() const {
	return
		static_cast<int>(grayscaleEnabled_) +
		static_cast<int>(vignetteEnabled_) +
		static_cast<int>(boxBlurEnabled_) +
		static_cast<int>(gaussianBlurEnabled_) +
		static_cast<int>(depthOfFieldEnabled_) +
		static_cast<int>(radialBlurEnabled_) +
		static_cast<int>(noiseEnabled_) +
		static_cast<int>(dissolveEnabled_) +
		static_cast<int>(outlineEnabled_) +
		static_cast<int>(underwaterEnabled_) +
		static_cast<int>(waterRefractionEnabled_) +
		static_cast<int>(pixelationEnabled_) +
		static_cast<int>(chromaticAberrationEnabled_) +
		static_cast<int>(motionBlurEnabled_);
}

void Game::InvalidateMotionBlurHistory() {
	motionBlurHistoryValid_ = false;
}
