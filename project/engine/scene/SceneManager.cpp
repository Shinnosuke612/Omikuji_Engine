// 役割: シーン生成、更新、描画、遷移の実行を実装する。
#include "SceneManager.h"

#include <cassert>
#include <algorithm>
#include <memory>
#include <utility>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <thread>
#include <filesystem>
#include <limits>
#include <string_view>
#include <stdexcept>
#include <unordered_set>
#include <cstring>
#include "ScenePreparation.h"
#include "SceneRuntimeDocumentState.h"
#include "SceneEntityQuery.h"
#include "../2d/SceneTexturePrepare.h"
#include "../3d/ModelManager.h"
#include "../utility/EditableResourcePath.h"
#include <objbase.h>
#include "SceneDocumentRead.h"

#include "AbstractSceneFactory.h"
#include "BaseScene.h"
#include "SceneCatalog.h"
#include "SceneDocument.h"
#include "SceneExecutionContext.h"
#include "../particle/ParticleManager.h"
#include "../utility/Logger.h"
#include "../utility/StringUtility.h"

// shellはworker完了までmainが保持。workerはManager/Context/Documentへアクセスしない。
struct SceneManager::DocumentReadJob {
	SceneInstanceId instanceId = 0;
	uint64_t generation = 0, projectEpoch = 0;
	std::string sceneId, primaryPath, primaryError, diagnostic;
	std::atomic<bool> cancelled{false}, done{false};
	std::mutex mutex;
	std::thread worker;
	std::unique_ptr<SceneDocumentRead> candidate;
	std::unique_ptr<SceneDocument> document;
	bool readFailed = false, backup = false, terminal = false, failed = false;

	void Start(const std::string& path) {
		done.store(false, std::memory_order_relaxed);
		worker = std::thread([this, path] {
			std::unique_ptr<SceneDocumentRead> result;
			std::string error;
			bool failure = true;
			try {
				if (!cancelled.load(std::memory_order_acquire)) failure = !TryReadSceneDocument(path, result, error);
			} catch (...) {
				// 診断確保自体に失敗しても、scalar失敗値と完了は必ず公開する。
				failure = true;
			}
			try {
				std::lock_guard<std::mutex> lock(mutex);
				if (!cancelled.load(std::memory_order_acquire)) {
					candidate = std::move(result);
					diagnostic.swap(error);
					readFailed = failure;
				}
			} catch (...) {
				readFailed = true;
				candidate.reset();
			}
			done.store(true, std::memory_order_release);
		});
	}
	~DocumentReadJob() {
		cancelled.store(true, std::memory_order_release);
		if (worker.joinable()) worker.join();
	}
};

namespace {
bool SameToken(ScenePrepareToken a, ScenePrepareToken b) noexcept {
	return a.instanceId == b.instanceId && a.generation == b.generation;
}

bool SameTransfer(const SceneTextureTransferKey& a, const SceneTextureTransferKey& b) noexcept {
	return SameToken(a.token, b.token) && a.sourceInstanceId == b.sourceInstanceId &&
		a.projectEpoch == b.projectEpoch && a.entryId == b.entryId;
}

bool SameTicket(const SceneTextureUploadTicket& a, const SceneTextureUploadTicket& b) noexcept {
	return SameTransfer(a.key, b.key) && a.uploadId == b.uploadId;
}

bool SamePresentation(const SceneDestinationPresentation& a, const SceneDestinationPresentation& b) noexcept {
	return SameToken(a.token, b.token) && a.projectEpoch == b.projectEpoch &&
		a.drawnInstanceId == b.drawnInstanceId && a.recordingSerial == b.recordingSerial &&
		a.outputGeneration == b.outputGeneration && a.signaledFence == b.signaledFence &&
		a.completedFence == b.completedFence;
}
}

struct SceneManager::PreparationTransaction {
	// TEMP: 512 MiB設定でも残る待ちの切り分け用。比較完了後に撤去する。
	// worker時間はGPU搬送やmain処理と重なるため、遷移の経過時間には加算しない。
	using TimingClock = std::chrono::steady_clock;
	std::atomic<int64_t> imageWorkerUs{0}, modelWorkerUs{0};
	uint64_t imageStarts = 0, modelStarts = 0, runtimeSteps = 0, modelBuildSteps = 0;
	struct FileImage {
		std::string key;
		SceneTextureCpuSource source;
		bool requireCube = false;
		bool allowMaterialFallback = false;
	};
	ScenePrepareStatus status;
	SceneInstanceId sourceId = kInvalidSceneInstanceId;
	uint64_t sourceContentGeneration = 0;
	std::unique_ptr<SceneRuntimeDocumentState> nextDocument;
	std::unique_ptr<SceneRuntimeDocumentState> previousDocument;
	std::unique_ptr<SceneInstance> candidate;
	std::vector<std::unique_ptr<SceneInstance>> retiring;
	std::string activeName;
	SceneDestinationPresentation commitProof;
	uint64_t lastRenderSerial = 0, retiredSerial = 0;
	bool finalFrameRetired = false;
	bool transferEnabled = false;
	SceneTextureTransferKey imageEntry;
	std::string imageKey;
	SceneTextureMipPolicy imageMipPolicy = SceneTextureMipPolicy::CpuComplete;
	bool allowGpuMips = false;
	std::unordered_set<std::string> forcedCpuMipKeys;
	uint64_t gpuEligibleJobs = 0, gpuRequestedJobs = 0, gpuCompletedImages = 0, gpuCpuRetries = 0, logicalPublished = 0;
	std::optional<TextureManager::DecodedTexture> image;
	std::optional<SceneTextureUploadTicket> ticket;
	// Bは通常の依存カーソルが到達するまで公開先頭とは別に保持する。
	std::optional<SceneTextureUploadTicket> lookaheadTicket;
	std::string lookaheadKey;
	uint64_t lookaheadBytes = 0, lookaheadCpuJob = 0;
	int64_t lookaheadReadyUs = -1;
	SceneTextureMipPolicy lookaheadMipPolicy = SceneTextureMipPolicy::CpuComplete;
	bool discardLookahead = false;
	std::optional<SceneTextureUploadCompletion> completedImage;
	uint64_t nextImageEntry = 0;
	uint64_t textEntity = 0;
	std::string textSignature;
	std::vector<FileImage> fileImages;
	std::vector<std::string> modelKeys;
	size_t entityCursor = 0, imageCursor = 0, modelCursor = 0;
	bool environmentFound = false;
	std::thread modelWorker;
	std::atomic<bool> modelDone{false};
	std::optional<Model::PreparedModelData> parsedModel;
	std::unique_ptr<ModelBuild> modelBuild;
	std::vector<std::string> modelDiagnostics;
	size_t embeddedBindingCursor = 0, materialCursor = 0;
	std::unordered_set<std::string> failedMaterialImages;
	// mainが全Jobの予約/公開順を所有し、workerは安定アドレスのJobだけへ書く。
	// completed/搬送中もfinal保持枠へ計上する。WIC等の内部heapとencoded sourceは予約対象外。
	static constexpr size_t kImageConcurrency = 4; // 比較用設定はここだけ: 1 / 2 / 3 / 4。
	static constexpr uint64_t kImageWorkLimit = 512ull * 1024 * 1024;
	static_assert(kImageConcurrency >= 1 && kImageConcurrency <= 4);
	struct ImageEstimate {
		uint64_t width = 0, height = 0, base = 0, final = 0, peak = 0;
	};
	struct ImageJob {
		FileImage entry;
		ImageEstimate estimate;
		SceneTextureCpuLimits limits;
		SceneTextureCpuResult result;
		SceneTextureCpuTiming timing;
		std::string diagnostic;
		std::thread worker;
		std::atomic<bool> done{false}, cancelled{false};
		SceneTextureCpuStatus outcome = SceneTextureCpuStatus::Failed;
		const char* outcomeName = "not_started";
		TimingClock::time_point launch, started, ended;
		uint64_t id = 0, holdReservation = 0, workReservation = 0;
		uint64_t initialHold = 0, initialWork = 0;
		bool prefetch = false, exclusive = false, reported = false, discard = false;
		// 通常のeraseはdone確認後だけ。未完了joinを許すのはTransaction終了時だけ。
		~ImageJob() { if (worker.joinable()) worker.join(); }
	};
	std::vector<std::unique_ptr<ImageJob>> imageJobs;
	std::unordered_set<std::string> sequentialImageKeys;
	uint64_t activeDecodedBytes = 0, decodedLimit = 0;
	size_t prefetchFileCursor = 0, prefetchBindingCursor = 0;
	bool prefetchScanning = false;

	// TEMP: 負荷比較終了後に撤去。各時刻は同一steady_clock、ログはmainだけが出す。
	enum class ImageIdle { NotEvaluated, NoKnownCandidate, Capacity, EstimateUnknown, UnsupportedSource,
		CachedOrFailed, GateClosed, ReadyNotStarted, ResultHeld, StartFailed, Disabled, Count };
	ImageIdle lastImageIdle = ImageIdle::NotEvaluated;
	TimingClock::time_point imageWindowStart = TimingClock::now();
	bool imageWindowReported = false, imageHistoryComplete = true;
	uint64_t peakImageHold = 0, peakImageWork = 0, peakObservedReady = 0;
	struct ImageInterval {
		TimingClock::time_point started, ended, mipStarted, mipEnded;
		bool fourK = false;
	};
	std::vector<ImageInterval> imageHistory;
	bool runtimeStarted = false, assetsPublished = false, preferRuntime = false;
	uint64_t assetRevision = 1;
	// TEMP: 並行化比較後に撤去。main区間は互いに重ならず、worker区間はunionして交差を測る。
	std::vector<std::pair<TimingClock::time_point, TimingClock::time_point>> runtimeHistory, modelHistory;
	TimingClock::time_point modelRunStarted{}, modelRunEnded{};
	bool runtimeHistoryComplete = true, firstLocalReported = false, allLocalReported = false;
	int64_t runtimeMainUs = 0, earlyRuntimeUs = 0, ticketRuntimeUs = 0, maxRuntimeStepUs = 0;
	uint64_t runtimeDeadlineOverruns = 0, localReadyAtAssets = 0;
	void RuntimeMilestone(const char* event, std::string_view key = {}) const noexcept {
		try { Logger::Log(std::format("[LoadTiming] runtime_milestone token={} generation={} event={} steady_us={} local_ready={} bound={} key=\"{}\"\n",
			status.token.instanceId,status.token.generation,event,
			std::chrono::duration_cast<std::chrono::microseconds>(TimingClock::now().time_since_epoch()).count(),
			status.runtime.localReady,status.runtime.bound,key)); } catch (...) {}
	}
	void RetainModelInterval() noexcept {
		if (modelRunEnded > modelRunStarted) try { modelHistory.push_back({modelRunStarted,modelRunEnded}); }
		catch (...) { runtimeHistoryComplete = false; }
		modelRunStarted = modelRunEnded = {};
	}
	void RecordRuntimeWork(TimingClock::time_point begin, TimingClock::time_point end,
		const SceneRuntimePrepareBudget& budget) noexcept {
		const auto us = ImageUs(end-begin);
		runtimeMainUs += us; maxRuntimeStepUs = (std::max)(maxRuntimeStepUs,us);
		if (!assetsPublished) earlyRuntimeUs += us;
		if (ticket) ticketRuntimeUs += us;
		if (end > budget.deadline) ++runtimeDeadlineOverruns;
		if (runtimeHistoryComplete) try { runtimeHistory.push_back({begin,end}); } catch (...) { runtimeHistoryComplete = false; }
		if (!firstLocalReported && status.runtime.localReady) { firstLocalReported = true; RuntimeMilestone("first_local_ready"); }
		if (!allLocalReported && status.runtime.localReady == status.runtime.entities &&
			(status.runtime.entities || std::string_view(status.runtime.action) == "objects_local_ready")) {
			allLocalReported = true; RuntimeMilestone("objects_local_ready");
		}
		if (std::string_view(status.runtime.action) == "ordered_bindings_done") RuntimeMilestone("ordered_bindings_done");
		if (std::string_view(status.runtime.action) != "idle" && std::string_view(status.runtime.action) != "objects_local_ready") try {
			Logger::Log(std::format("[LoadTiming] runtime_entity token={} generation={} action={} entity={} index={} local_started={} local_ready={} bound={} work_us={}\n",
				status.token.instanceId,status.token.generation,status.runtime.action,status.runtime.lastEntity,status.runtime.lastIndex,
				status.runtime.localStarted,status.runtime.localReady,status.runtime.bound,us));
		} catch (...) {}
	}
	void ReportRuntimeOverlap() noexcept {
		try {
			auto cpu = modelHistory;
			for (const auto& span : imageHistory) cpu.push_back({span.started,span.ended});
			std::sort(cpu.begin(),cpu.end());
			std::vector<std::pair<TimingClock::time_point, TimingClock::time_point>> united;
			for (const auto& span : cpu) {
				if (united.empty() || span.first > united.back().second) united.push_back(span);
				else united.back().second = (std::max)(united.back().second,span.second);
			}
			int64_t overlap = 0;
			for (const auto& runtime : runtimeHistory) for (const auto& span : united) {
				const auto a = (std::max)(runtime.first,span.first), b = (std::min)(runtime.second,span.second);
				if (b > a) overlap += ImageUs(b-a);
			}
			const auto& m = status.runtime;
			Logger::Log(std::format("[LoadTiming] runtime_overlap token={} generation={} local_started={} local_ready={} bound={} entities={} "
				"local_ready_at_assets={} skipped_ahead={} runtime_main_work_us={} early_runtime_main_us={} cpu_runtime_overlap_us={} "
				"upload_ticket_runtime_overlap_us={} history_complete={} max_runtime_step_us={} deadline_overruns={} wait_model_probes={} wait_texture_probes={} wait_environment_probes={}\n",
				status.token.instanceId,status.token.generation,m.localStarted,m.localReady,m.bound,m.entities,localReadyAtAssets,m.skippedAhead,
				runtimeMainUs,earlyRuntimeUs,runtimeHistoryComplete && imageHistoryComplete ? overlap : -1,ticketRuntimeUs,
				runtimeHistoryComplete && imageHistoryComplete,maxRuntimeStepUs,runtimeDeadlineOverruns,m.waitModel,m.waitTexture,m.waitEnvironment));
		} catch (...) { /* 任意の計測失敗で候補の成否を変えない。 */ }
	}
	static int64_t ImageUs(TimingClock::duration value) noexcept {
		return std::chrono::duration_cast<std::chrono::microseconds>(value).count();
	}
	int64_t ImageStamp(TimingClock::time_point value) const noexcept {
		return value == TimingClock::time_point{} ? -1 : ImageUs(value - imageWindowStart);
	}
	void SetImageIdle(ImageIdle reason) noexcept { lastImageIdle = reason; }
	uint64_t ImageHold() const noexcept {
		uint64_t total = activeDecodedBytes + lookaheadBytes;
		for (const auto& work : imageJobs) total += work->holdReservation;
		return total;
	}
	uint64_t ImageWork() const noexcept {
		uint64_t total = activeDecodedBytes + lookaheadBytes;
		for (const auto& work : imageJobs) total += work->workReservation;
		return total;
	}
	void UpdateImagePeaks() noexcept {
		peakImageHold = (std::max)(peakImageHold, ImageHold());
		peakImageWork = (std::max)(peakImageWork, ImageWork());
		uint64_t observed = activeDecodedBytes + lookaheadBytes;
		for (const auto& work : imageJobs) {
			if (work->reported) observed += work->result.texture.image.GetPixelsSize();
		}
		peakObservedReady = (std::max)(peakObservedReady, observed);
	}
	static bool SameImage(const FileImage& a, const FileImage& b) {
		return a.key == b.key && a.source.recipe == b.source.recipe &&
			a.source.resolvedPath == b.source.resolvedPath && a.source.bytes == b.source.bytes &&
			a.source.formatHint == b.source.formatHint && a.source.colorSpace == b.source.colorSpace;
	}
	ImageJob* FindImageJob(const FileImage& entry) {
		for (const auto& work : imageJobs) {
			if (work->entry.key != entry.key) continue;
			if (!SameImage(work->entry, entry)) throw std::runtime_error("Conflicting image job source: " + entry.key);
			return work.get();
		}
		return nullptr;
	}
	bool HasImageKey(const std::string& key) const noexcept {
		if (lookaheadTicket && lookaheadKey == key) return true;
		return std::any_of(imageJobs.begin(), imageJobs.end(), [&](const auto& work) { return work->entry.key == key; });
	}
	static ImageEstimate EstimatePng(const std::vector<uint8_t>& bytes,
		SceneTextureCpuSource::Recipe recipe, const std::string& hint) noexcept {
		using Recipe = SceneTextureCpuSource::Recipe;
		ImageEstimate value;
		if (recipe != Recipe::OrdinaryMemory && recipe != Recipe::ModelGrayscaleBaseColor) return value;
		if (recipe == Recipe::OrdinaryMemory && !hint.empty() && hint != "png" && hint != ".png") return value;
		constexpr uint8_t signature[] = {137,80,78,71,13,10,26,10,0,0,0,13,'I','H','D','R'};
		if (bytes.size() < 33 || std::memcmp(bytes.data(), signature, sizeof(signature))) return value;
		if ((bytes[24] != 1 && bytes[24] != 2 && bytes[24] != 4 && bytes[24] != 8) ||
			(bytes[25] != 0 && bytes[25] != 2 && bytes[25] != 3 && bytes[25] != 4 && bytes[25] != 6) ||
			bytes[26] || bytes[27] || bytes[28] > 1) return value;
		const auto read32 = [&](size_t o) { return (uint64_t(bytes[o]) << 24) | (uint64_t(bytes[o+1]) << 16) |
			(uint64_t(bytes[o+2]) << 8) | bytes[o+3]; };
		value.width = read32(16); value.height = read32(20);
		if (!value.width || !value.height || value.width > UINT32_MAX || value.height > UINT32_MAX) return {};
		if (value.width > UINT64_MAX / 4 / value.height) return {};
		// PNG <=8bitはWICの最大RGBA8で見積もる。CPU pitchでありGPUの256整列ではない。
		for (size_t w = static_cast<size_t>(value.width), h = static_cast<size_t>(value.height);;) {
			size_t row = 0, slice = 0;
			if (FAILED(DirectX::ComputePitch(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, row, slice)) ||
				slice > UINT64_MAX - value.final) return {};
			if (!value.base) value.base = slice;
			value.final += slice;
			if (w == 1 && h == 1) break;
			w = (std::max)(size_t{1}, w / 2); h = (std::max)(size_t{1}, h / 2);
		}
		if (value.base > UINT64_MAX - value.final) return {};
		// source+全mipが同時生存するpeak。gray->RGBA中の2画像もこの上界に含まれる。
		value.peak = value.base + value.final;
		return value;
	}
	// mainの予約見積もり専用。限定形式を証明できなければ全ゼロで返す。WIC/GPUを呼ばない。
	static ImageEstimate EstimateJpeg(const std::vector<uint8_t>& bytes,
		SceneTextureCpuSource::Recipe recipe, const std::string& hint) noexcept {
		using Recipe = SceneTextureCpuSource::Recipe;
		if (recipe != Recipe::OrdinaryMemory || (!hint.empty() && hint != "jpg" && hint != ".jpg" && hint != "jpeg" && hint != ".jpeg") ||
			bytes.size() < 4 || bytes[0] != 0xff || bytes[1] != 0xd8) return {};
		ImageEstimate value;
		size_t position = 2;
		bool frameFound = false;
		// 8bit/3成分のbaseline一走査だけを予約対象にする。entropyは既存decoderへ渡す。
		// 未知marker/CMYK/progressive/破損は予約不明へ戻し、従来のexclusive経路を保つ。
		// SOF0のlength=8+3*N、SOSのlength=6+2*N（ここではN=3）。
		while (position < bytes.size()) {
			if (bytes[position++] != 0xff) return {};
			while (position < bytes.size() && bytes[position] == 0xff) ++position;
			if (position == bytes.size()) return {};
			const uint8_t marker = bytes[position++];
			if (bytes.size()-position < 2) return {};
			const size_t length = (size_t(bytes[position]) << 8) | bytes[position+1];
			if (length < 2 || length > bytes.size()-position) return {};
			if (marker == 0xc0) {
				if (frameFound || length != 17 || bytes[position+2] != 8 || bytes[position+7] != 3) return {};
				value.height = (uint64_t(bytes[position+3]) << 8) | bytes[position+4];
				value.width = (uint64_t(bytes[position+5]) << 8) | bytes[position+6];
				if (!value.width || !value.height) return {};
				frameFound = true;
			} else if (marker == 0xda) {
				if (!frameFound || length != 12 || bytes[position+2] != 3 ||
					bytes[position+9] != 0 || bytes[position+10] != 63 || bytes[position+11] != 0) return {};
				// WICの8bit RGB/YCbCr展開の最大RGBA8。GPU無効/一回CPU retryでも全mipを予約する。
				for (size_t w=static_cast<size_t>(value.width), h=static_cast<size_t>(value.height);;) {
					size_t row = 0, slice = 0;
					if (FAILED(DirectX::ComputePitch(DXGI_FORMAT_R8G8B8A8_UNORM,w,h,row,slice)) || slice > UINT64_MAX-value.final) return {};
					if (!value.base) value.base = slice;
					value.final += slice;
					if (w == 1 && h == 1) break;
					w = (std::max)(size_t{1},w/2); h = (std::max)(size_t{1},h/2);
				}
				if (value.base > UINT64_MAX-value.final) return {};
				value.peak = value.base+value.final;
				return value;
			} else if (!((marker >= 0xe0 && marker <= 0xef) || marker == 0xdb || marker == 0xc4 || marker == 0xdd || marker == 0xfe)) return {};
			position += length;
		}
		return {};
	}
	static ImageEstimate EstimateMemoryImage(const std::vector<uint8_t>& bytes,
		SceneTextureCpuSource::Recipe recipe, const std::string& hint) noexcept {
		const auto png = EstimatePng(bytes,recipe,hint);
		return png.final ? png : EstimateJpeg(bytes,recipe,hint);
	}
	// TEMP: 搬送待ちの比較後に撤去。全状態はmain限定で、素材/キューの制御には参照しない。
	struct DiagText {
		uint64_t entity = 0;
		std::string key;
		int64_t payloadUs = -1;
		bool skipped = false;
	};
	struct DiagOwner {
		uint64_t upload = 0, order = 0, entity = 0;
		std::string key;
		int64_t copyUs = -1, directUs = -1, publishUs = -1;
		bool text = false, terminal = false;
	};
	struct DiagSnapshot {
		uint64_t owner = 0, order = 0, cpuJob = 0;
		std::string key;
		const char* kind = "unknown";
		const char* state = "next_not_known";
		const char* source = "none";
		int64_t readyUs = -1, copyUs = -1, directUs = -1, publishUs = -1;
		bool ready = false, handoff = false, ownerTerminal = false, sourceLive = false;
		bool operator==(const DiagSnapshot&) const = default;
	};
	DiagOwner diagOwner;
	DiagSnapshot diagLast;
	std::vector<DiagText> diagTexts;
	bool diagHealthy = true, diagSealed = false, diagHasSample = false, diagSourceValid = true;
	uint64_t diagNo = 0, diagOrder = 0, diagMaterialCount = 0, diagTextCount = 0;
	uint64_t diagDropsAtBegin = 0;
	uint64_t diagPendingCpuJob = 0;
	int64_t diagPendingReadyUs = -1, diagServiceStart = -1, diagDeadline = -1;
	bool diagCanPrepare = false, diagForeignOwner = false;
	static int64_t DiagNow() noexcept {
		return std::chrono::duration_cast<std::chrono::microseconds>(TimingClock::now().time_since_epoch()).count();
	}
	template<class... Args>
	void Diag(const char* event, int64_t at, std::format_string<Args...> format, Args&&... args) noexcept {
		if (diagSealed) return;
		const auto traceNo = ++diagNo;
		try {
			const auto detail = std::format(format,std::forward<Args>(args)...);
			if (!Logger::TryEnqueueUploadTrace(std::format("[UploadTrace] token={} generation={} trace_no={} event={} at_us={} {}\n",
				status.token.instanceId,status.token.generation,traceNo,event,at,detail))) diagHealthy = false;
		} catch (...) {
			diagHealthy = false;
			Logger::NoteUploadTraceDrop();
		}
	}
	void DiagPlanTexts() noexcept {
		try {
			for (const auto& entity : nextDocument->document->GetEntities()) {
				if (!SceneEntityQuery::FindEnabledComponent(entity,"TextRenderer")) continue;
				DiagText text;
				text.entity = entity.id;
				text.key = "__runtime_text_"+activeName+"_"+std::to_string(status.token.instanceId)+"_"+std::to_string(entity.id);
				diagTexts.push_back(std::move(text));
				Diag("text_plan",DiagNow(),"entity={} key=\"{}\" content_time=not_independently_observed",entity.id,diagTexts.back().key);
			}
		} catch (...) { diagHealthy = false; }
	}
	void DiagTextEvent(const ScenePreparedTextObservation& observation) noexcept {
		try {
			auto found = std::find_if(diagTexts.begin(),diagTexts.end(),[&](const auto& text){return text.entity == observation.entity;});
			if (found == diagTexts.end()) { diagHealthy = false; return; }
			if (!observation.key.empty() && observation.key != found->key) diagHealthy = false;
			if (std::string_view(observation.event) == "payload_ready") found->payloadUs = observation.atUs;
			if (std::string_view(observation.event) == "layout_skipped" || std::string_view(observation.event) == "raster_skipped") found->skipped = true;
			Diag(observation.event,observation.atUs,"entity={} key=\"{}\" kind=text bytes={} content_time=settings_resolved_proxy",
				observation.entity,found->key,observation.bytes);
		} catch (...) { diagHealthy = false; }
	}
	void DiagCpuDone(const ImageJob& work) noexcept {
		Diag("cpu_done",DiagNow(),"key=\"{}\" kind=material cpu_job={} worker_done_us={} prepared={} discarded={} cancelled={} cached={} prefetch={} bytes={} result={}",
			work.entry.key,work.id,std::chrono::duration_cast<std::chrono::microseconds>(work.ended.time_since_epoch()).count(),
			work.outcome == SceneTextureCpuStatus::Prepared,work.discard,work.cancelled.load(std::memory_order_relaxed),
			TextureManager::GetInstance()->HasTexture(work.entry.key),work.prefetch,work.result.texture.image.GetPixelsSize(),work.outcomeName);
	}
	DiagSnapshot DiagCandidate() const {
		DiagSnapshot value;
		value.owner = diagOwner.upload; value.order = diagOwner.order;
		value.copyUs = diagOwner.copyUs; value.directUs = diagOwner.directUs; value.publishUs = diagOwner.publishUs;
		value.ownerTerminal = diagOwner.terminal;
		value.sourceLive = diagSourceValid && status.phase != ScenePreparePhase::Failed && status.phase != ScenePreparePhase::Cancelling && status.phase != ScenePreparePhase::Cancelled;
		if (image && !ticket && !completedImage) {
			value.key = imageKey; value.kind = textEntity ? "text" : "material"; value.source = "owned_payload";
			if (TextureManager::GetInstance()->HasTexture(imageKey)) { value.state = "cache_excluded"; return value; }
			value.ready = image->image.GetPixelsSize() != 0 && image->image.GetPixelsSize() <= decodedLimit;
			value.handoff = transferEnabled && value.ready;
			value.state = value.handoff ? "eligible" : "payload_or_admission_gate";
			value.cpuJob = diagPendingCpuJob; value.readyUs = diagPendingReadyUs;
			if (textEntity) for (const auto& text : diagTexts) if (text.entity == textEntity) value.readyUs = text.payloadUs;
			return value;
		}
		const FileImage* entry = nullptr;
		for (size_t i=imageCursor;i<fileImages.size();++i) {
			if (fileImages[i].key == diagOwner.key || failedMaterialImages.contains(fileImages[i].key) || TextureManager::GetInstance()->HasTexture(fileImages[i].key)) continue;
			entry = &fileImages[i]; value.key = entry->key; value.source = "file_queue"; break;
		}
		if (!entry && !modelWorker.joinable() && parsedModel) {
			for (size_t i=embeddedBindingCursor;i<parsedModel->embeddedBindings.size();++i) {
				const auto index = parsedModel->embeddedBindings[i].sourceIndex;
				if (index >= parsedModel->embeddedTextures.size()) { value.state = "binding_unknown"; return value; }
				const auto& key = parsedModel->embeddedTextures[index].key;
				if (key == diagOwner.key || failedMaterialImages.contains(key) || TextureManager::GetInstance()->HasTexture(key)) continue;
				value.key = key; value.source = "embedded_binding"; break;
			}
		}
		if (!value.key.empty()) {
			value.kind = "material"; value.state = "cpu_not_started";
			for (const auto& work : imageJobs) if (work->entry.key == value.key) {
				value.cpuJob = work->id;
				// workerがended/resultを公開した後だけ読む。追加join/Pollや進行はしない。
				if (!work->done.load(std::memory_order_acquire)) { value.state = "cpu_running"; return value; }
				value.readyUs = std::chrono::duration_cast<std::chrono::microseconds>(work->ended.time_since_epoch()).count();
				if (work->discard || work->cancelled.load(std::memory_order_relaxed)) { value.state = "cpu_discarded"; return value; }
				if (work->outcome != SceneTextureCpuStatus::Prepared) { value.state = "cpu_failed"; return value; }
				const auto bytes = work->result.texture.image.GetPixelsSize();
				value.ready = bytes && bytes <= decodedLimit && (!entry || !entry->requireCube || work->result.texture.metadata.IsCubemap());
				value.handoff = entry && value.ready;
				value.state = !value.ready ? "payload_or_shape_gate" : value.handoff ? "eligible" : "order_not_materialized";
				return value;
			}
			return value;
		}
		if (diagOwner.text || assetsPublished) {
			bool after = !diagOwner.text;
			for (const auto& text : diagTexts) {
				if (!after) { if (text.entity == diagOwner.entity) after = true; continue; }
				if (text.skipped) continue;
				value.key = text.key; value.kind = "text"; value.source = "text_plan";
				value.readyUs = text.payloadUs; value.ready = text.payloadUs >= 0;
				value.state = value.ready ? "order_not_materialized" : "not_generated";
				return value;
			}
			value.state = "no_successor";
		}
		return value;
	}
	void DiagSample(int64_t at) noexcept {
		if (diagSealed) return;
		try {
			const auto value = DiagCandidate();
			if (diagHasSample && value == diagLast) return;
			diagLast = value; diagHasSample = true;
			// probe後の時刻を使い、観測中に終わったworkerを過去へreadyとして遡及させない。
			Diag("sample",(std::max)(at,DiagNow()),"owner={} owner_seq={} key=\"{}\" kind={} state={} source={} cpu_job={} ready_us={} cpu_ready={} handoff={} copy_us={} direct_us={} publish_us={} owner_terminal={} source_live={}",
				value.owner,value.order,value.key,value.kind,value.state,value.source,value.cpuJob,value.readyUs,value.ready,value.handoff,
				value.copyUs,value.directUs,value.publishUs,value.ownerTerminal,value.sourceLive);
		} catch (...) { diagHealthy = false; }
	}
	void DiagStage(const SceneTextureTransferObservation& value) noexcept {
		if (diagSealed) return;
		const int64_t at = value.atUs ? value.atUs : DiagNow();
		const std::string_view event = value.event;
		if (event == "service_begin") {
			diagServiceStart = at; diagDeadline = value.deadlineUs;
			diagCanPrepare = value.canPrepare; diagForeignOwner = value.foreignGpuOwner;
		} else if (event == "service_end") {
			if (diagServiceStart >= 0) Diag("service_window",at,"begin_us={} end_us={} deadline_us={} can_prepare={} foreign_owner={}",
				diagServiceStart,at,diagDeadline,diagCanPrepare,diagForeignOwner || value.foreignGpuOwner);
			diagServiceStart = -1;
		} else if (event != "sample" && value.uploadId == diagOwner.upload && value.uploadId) {
			if (event == "copy_all_done" && value.signaledFence && value.completedFence >= value.signaledFence &&
				value.completedFence != (std::numeric_limits<uint64_t>::max)()) diagOwner.copyUs = at;
			if (event == "direct_done" && value.signaledFence && value.completedFence >= value.signaledFence &&
				value.completedFence != (std::numeric_limits<uint64_t>::max)()) diagOwner.directUs = at;
			if (event == "result_taken" && value.outcome != 0) diagOwner.terminal = true;
			Diag(value.event,at,"upload={} order={} key=\"{}\" kind={} signaled_fence={} completed_fence={} recording_serial={} outcome={}",
				value.uploadId,diagOwner.order,diagOwner.key,diagOwner.text ? "text" : "material",value.signaledFence,value.completedFence,value.recordingSerial,value.outcome);
		}
		// DIRECT記録の通知はPostDraw後からの遡及イベント。現在の候補状態を記録時へ戻さない。
		if (event != "direct_record") DiagSample(DiagNow());
	}
	void DiagAdmission(const SceneTextureUploadTicket& value) noexcept {
		try {
			const uint64_t previous = diagOwner.upload;
			const int64_t previousPublish = diagOwner.publishUs;
			diagOwner = {}; diagOwner.upload = value.uploadId; diagOwner.order = ++diagOrder;
			diagOwner.key = imageKey; diagOwner.entity = textEntity; diagOwner.text = textEntity != 0;
			if (diagOwner.text) ++diagTextCount; else ++diagMaterialCount;
			Diag("admit",DiagNow(),"upload={} order={} previous_upload={} previous_publish_us={} key=\"{}\" kind={} entity={} cpu_job={} ready_us={}",
				value.uploadId,diagOrder,previous,previousPublish,imageKey,diagOwner.text ? "text" : "material",textEntity,diagPendingCpuJob,diagPendingReadyUs);
			diagPendingCpuJob = 0; diagPendingReadyUs = -1;
			DiagSample(DiagNow());
		} catch (...) { diagHealthy = false; }
	}
	void DiagPublished(int64_t observedUs) noexcept {
		diagOwner.publishUs = observedUs;
		Diag("published",diagOwner.publishUs,"upload={} order={} key=\"{}\" kind={}",diagOwner.upload,diagOwner.order,diagOwner.key,diagOwner.text ? "text" : "material");
		DiagSample(diagOwner.publishUs);
	}
	void DiagFinish(const char* reason) noexcept {
		if (diagSealed) return;
		SceneTextureTransferObservation end; end.event = "service_end"; end.atUs = DiagNow();
		DiagStage(end);
		const auto drops = Logger::UploadTraceDroppedTotal();
		Diag("end",DiagNow(),"reason={} healthy={} material_admitted={} text_admitted={} text_planned={} event_count={} sink_dropped_total={} precision=host_observed_service_clipped",
			reason,diagHealthy && Logger::IsUploadTraceHealthy() && drops == diagDropsAtBegin,
			diagMaterialCount,diagTextCount,diagTexts.size(),diagNo+1,drops);
		diagSealed = true;
	}

	void LogImageJob(const ImageJob& work) noexcept {
		DiagCpuDone(work);
		const auto& t = work.timing;
		try {
			Logger::Log(std::format("[LoadTiming] image_cpu target={} token={} generation={} job={} key=\"{}\" mode={} recipe={} result={} "
				"launch_us={} run_start_us={} run_end_us={} worker_us={} observed_us={} decode_start_us={} decode_end_us={} decode_state={} decode_us={} "
				"mip_start_us={} mip_end_us={} mip_state={} mip_call_us={} hdr_combined_us={} "
				"decoded_observed={} decoded_bytes={} decoded_width={} decoded_height={} decoded_depth={} decoded_array={} decoded_mips={} "
				"final_observed={} final_bytes={} final_width={} final_height={} final_depth={} final_array={} final_mips={} "
				"configured_limit={} initial_hold_reservation={} initial_work_reservation={} exclusive={} discarded={}\n",
				activeName, status.token.instanceId, status.token.generation, work.id, work.entry.key,
				work.prefetch ? "prefetch" : "normal", static_cast<int>(work.entry.source.recipe), work.outcomeName,
				ImageStamp(work.launch), ImageStamp(work.started), ImageStamp(work.ended), ImageUs(work.ended-work.started),
				ImageStamp(TimingClock::now()),
				ImageStamp(t.decodeStarted), ImageStamp(t.decodeEnded), t.decodeState, t.decodeUs,
				ImageStamp(t.mipStarted), ImageStamp(t.mipEnded), t.mipState, t.mipUs, t.hdrCombinedUs,
				t.decoded.observed, t.decoded.bytes, t.decoded.width, t.decoded.height, t.decoded.depth, t.decoded.arraySize, t.decoded.mipLevels,
				t.final.observed, t.final.bytes, t.final.width, t.final.height, t.final.depth, t.final.arraySize, t.final.mipLevels,
				kImageConcurrency, work.initialHold, work.initialWork, work.exclusive, work.discard));
		} catch (...) { /* 計測はJobの結果を変更しない。 */ }
	}
	void ObserveFinishedImage() noexcept {
		// result/timingを触る前にacquire。個別失敗も通常cursorに選ばれるまで保持する。
		for (auto it = imageJobs.begin(); it != imageJobs.end();) {
			auto& work = **it;
			if (!work.done.load(std::memory_order_acquire)) { ++it; continue; }
			if (work.worker.joinable()) work.worker.join();
			if (!work.reported) {
				work.reported = true;
				imageWorkerUs.fetch_add(ImageUs(work.ended-work.started), std::memory_order_relaxed);
				if (work.timing.gpuEligible) ++gpuEligibleJobs;
				const bool gpu = work.result.mipPolicy == SceneTextureMipPolicy::GpuSrgbBox4096;
				if (gpu) ++gpuRequestedJobs;
				// TEMP: GPU mip比較完了後にplan/summary/retryのLoadTiming計測を撤去する。
				try {
					uint64_t targetBytes = 0;
					if (gpu) for (uint64_t w=kSceneGpuMipSize;w;w/=2) targetBytes += w*w*4;
					Logger::Log(std::format("[LoadTiming] image_mip_plan target={} token={} generation={} job={} key=\"{}\" eligible={} decision={} "
						"mip_location={} cpu_payload_bytes={} cpu_prepared_mips={} target_mips={} target_gpu_logical_bytes={} cpu_payload_format={} cpu_payload_alpha_mode={}\n",
						activeName,status.token.instanceId,status.token.generation,work.id,work.entry.key,work.timing.gpuEligible,work.timing.gpuDecision,
						gpu ? "gpu_pending" : "cpu",work.result.texture.image.GetPixelsSize(),work.result.texture.metadata.mipLevels,
						gpu ? size_t{kSceneGpuMipCount} : work.result.texture.metadata.mipLevels,targetBytes,
						static_cast<uint32_t>(work.result.texture.metadata.format),static_cast<uint32_t>(work.result.texture.metadata.GetAlphaMode())));
				} catch (...) { /* 計測はworkerの結果を変更しない。 */ }
				const uint64_t bytes = work.result.texture.image.GetPixelsSize();
				work.holdReservation = bytes; work.workReservation = bytes;
				try { imageHistory.push_back({work.started, work.ended, work.timing.mipStarted, work.timing.mipEnded,
					work.timing.decoded.width >= 4096 && work.timing.decoded.height >= 4096}); }
				catch (...) { imageHistoryComplete = false; }
				LogImageJob(work);
			}
			if (work.discard) it = imageJobs.erase(it); else ++it;
		}
		UpdateImagePeaks();
	}
	void EraseImageJob(ImageJob* work) noexcept {
		const auto found = std::find_if(imageJobs.begin(), imageJobs.end(), [&](const auto& value) { return value.get() == work; });
		assert(found != imageJobs.end() && (*found)->done.load(std::memory_order_acquire));
		imageJobs.erase(found);
	}
	void CancelImageJobs() noexcept {
		for (auto& work : imageJobs) { work->discard = true; work->cancelled.store(true, std::memory_order_relaxed); }
	}
	void PruneCachedImageJobs() noexcept {
		// cacheを読むのはmainだけ。通常bindingが画像を要求しなくなったBも回収する。
		for (auto& work : imageJobs) if (TextureManager::GetInstance()->HasTexture(work->entry.key)) {
			if (!work->discard) Diag("cpu_excluded",DiagNow(),"key=\"{}\" cpu_job={} reason=cache",work->entry.key,work->id);
			work->discard = true; work->cancelled.store(true,std::memory_order_relaxed);
		}
		ObserveFinishedImage();
	}
	bool DrainImageJobs() noexcept {
		CancelImageJobs(); ObserveFinishedImage();
		return imageJobs.empty();
	}
	void ReportImageWindow(const char* end) noexcept {
		ObserveFinishedImage();
		if (imageWindowReported || std::any_of(imageJobs.begin(), imageJobs.end(),
			[](const auto& work) { return !work->reported; })) return;
		imageWindowReported = true;
		DiagFinish(end);
		ReportRuntimeOverlap();
		uint64_t runPeak = 0, mipPeak = 0;
		int64_t fourKRunOverlap = 0, fourKMipOverlap = 0;
		const auto overlap = [](auto a, auto b, auto c, auto d) {
			if (a == TimingClock::time_point{} || c == TimingClock::time_point{}) return int64_t{0};
			const auto first = (std::max)(a,c), last = (std::min)(b,d);
			return last > first ? ImageUs(last-first) : int64_t{0};
		};
		for (size_t i = 0; i < imageHistory.size(); ++i) {
			const auto& a = imageHistory[i];
			uint64_t running = 0, mips = 0;
			for (const auto& b : imageHistory) {
				if (b.started <= a.started && a.started < b.ended) ++running;
				if (a.mipStarted != TimingClock::time_point{} && b.mipStarted != TimingClock::time_point{} &&
					b.mipStarted <= a.mipStarted && a.mipStarted < b.mipEnded) ++mips;
			}
			runPeak = (std::max)(runPeak,running); mipPeak = (std::max)(mipPeak,mips);
			for (size_t j = i+1; a.fourK && j < imageHistory.size(); ++j) if (imageHistory[j].fourK) {
				const auto& b = imageHistory[j];
				fourKRunOverlap += overlap(a.started,a.ended,b.started,b.ended);
				fourKMipOverlap += overlap(a.mipStarted,a.mipEnded,b.mipStarted,b.mipEnded);
			}
		}
		// 区間の和集合だけを実行wallとして数える。旧単workerの理由別idle総和とは別の指標。
		const auto windowEnd = TimingClock::now();
		int64_t runningUnion = 0;
		for (auto cursor = imageWindowStart; cursor < windowEnd;) {
			auto next = windowEnd;
			bool running = false;
			for (const auto& interval : imageHistory) {
				if (interval.started <= cursor && cursor < interval.ended) running = true;
				if (interval.started > cursor) next = (std::min)(next,interval.started);
				if (interval.ended > cursor) next = (std::min)(next,interval.ended);
			}
			if (running) runningUnion += ImageUs(next-cursor);
			cursor = next;
		}
		const auto windowUs = ImageUs(windowEnd-imageWindowStart);
		const auto workerUs = imageWorkerUs.load(std::memory_order_relaxed);
		const double utilization = windowUs > 0 ? double(workerUs)/(double(windowUs)*kImageConcurrency) : 0.0;
		try {
			Logger::Log(std::format("[LoadTiming] image_worker_window target={} token={} generation={} end={} window=transaction_created_to_end "
				"window_us={} configured_limit={} run_peak={} mip_peak={} history_complete={} four_k_run_pair_overlap_us={} four_k_mip_pair_overlap_us={} "
				"hold_limit={} work_limit={} peak_hold_reservation={} peak_work_reservation={} peak_observed_ready_bytes={} last_idle_reason={} "
				"worker_total_us={} running_union_us={} all_workers_idle_us={} configured_slot_wall_utilization={}\n",
				activeName,status.token.instanceId,status.token.generation,end,windowUs,
				kImageConcurrency,imageHistoryComplete ? static_cast<int64_t>(runPeak) : -1,
				imageHistoryComplete ? static_cast<int64_t>(mipPeak) : -1,imageHistoryComplete,
				imageHistoryComplete ? fourKRunOverlap : -1,imageHistoryComplete ? fourKMipOverlap : -1,decodedLimit,kImageWorkLimit,
				peakImageHold,peakImageWork,peakObservedReady,static_cast<int>(lastImageIdle),
				workerUs,imageHistoryComplete ? runningUnion : -1,imageHistoryComplete ? windowUs-runningUnion : -1,utilization));
			Logger::Log(std::format("[LoadTiming] gpu_mip_summary target={} token={} generation={} eligible_jobs={} requested_jobs={} "
				"gpu_completed={} cpu_retry_count={} logical_published_entries={}\n",activeName,status.token.instanceId,status.token.generation,
				gpuEligibleJobs,gpuRequestedJobs,gpuCompletedImages,gpuCpuRetries,logicalPublished));
		} catch (...) { /* 計測は終了/公開に影響させない。 */ }
	}
	bool StartImage(const FileImage& entry, bool prefetch = false) {
		if (HasImageKey(entry.key)) return false;
		const auto estimate = sequentialImageKeys.contains(entry.key) || !entry.source.resolvedPath.empty() ? ImageEstimate{} :
			EstimateMemoryImage(entry.source.bytes, entry.source.recipe, entry.source.formatHint);
		const bool exclusive = !estimate.final || estimate.final > decodedLimit || estimate.peak > kImageWorkLimit;
		const auto hold = exclusive ? decodedLimit : estimate.final;
		const auto peak = exclusive ? kImageWorkLimit : estimate.peak;
		// holdはworkの内数。二つの上限を同時に照合し、worker側に予約待ちを作らない。
		if (imageJobs.size() >= kImageConcurrency || ImageHold() > decodedLimit || hold > decodedLimit-ImageHold() ||
			ImageWork() > kImageWorkLimit || peak > kImageWorkLimit-ImageWork() ||
			(exclusive && (!imageJobs.empty() || activeDecodedBytes || lookaheadBytes)) ||
			std::any_of(imageJobs.begin(),imageJobs.end(),[](const auto& job){return job->exclusive;})) {
			SetImageIdle(ImageIdle::Capacity);
			// 未対応の通常Aを未来Bが塞ぐ場合はBを取消し、通常順の単独経路を優先する。
			if (!prefetch) CancelImageJobs();
			return false;
		}
		auto job = std::make_unique<ImageJob>();
		job->entry = entry; job->estimate = estimate; job->exclusive = exclusive; job->prefetch = prefetch;
		job->holdReservation = job->initialHold = hold; job->workReservation = job->initialWork = peak;
		job->limits.cancelled = &job->cancelled;
		if (exclusive) job->limits.decodedBytes = decodedLimit;
		if (!exclusive) job->limits = {estimate.width,estimate.height,estimate.base,estimate.final,&job->cancelled};
		job->limits.allowGpuMips = allowGpuMips;
		job->limits.forceCpuMips = forcedCpuMipKeys.contains(entry.key);
		auto* work = job.get();
		imageJobs.push_back(std::move(job));
		work->launch = TimingClock::now(); work->id = imageStarts+1;
		UpdateImagePeaks();
		try {
			work->worker = std::thread([work] {
				work->started = TimingClock::now();
				const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
				try {
					if (FAILED(com)) work->outcomeName = "com_failed";
					else {
						work->outcome = TryPrepareSceneTexture(work->entry.source,work->result,work->diagnostic,&work->timing,&work->limits);
						work->outcomeName = work->outcome == SceneTextureCpuStatus::Prepared ? "prepared" :
							work->outcome == SceneTextureCpuStatus::ReservationExceeded ? "reservation_exceeded" :
							work->outcome == SceneTextureCpuStatus::Cancelled ? "cancelled" : "failed";
					}
				} catch (...) { work->outcomeName = "exception"; }
				if (SUCCEEDED(com)) CoUninitialize();
				work->ended = TimingClock::now();
				work->done.store(true,std::memory_order_release);
			});
		} catch (...) { imageJobs.pop_back(); SetImageIdle(ImageIdle::StartFailed); throw; }
		++imageStarts;
		try {
			const auto effective = exclusive ? uint64_t{1} : (std::min)({uint64_t{kImageConcurrency},
				(decodedLimit-activeDecodedBytes)/hold,(kImageWorkLimit-activeDecodedBytes)/peak});
			Logger::Log(std::format("[LoadTiming] image_admit target={} token={} generation={} job={} key=\"{}\" launch_us={} "
				"configured_limit={} same_size_limit_with_transfer={} job_slots={} hold_reserved={} work_reserved={} hold_limit={} work_limit={} "
				"transfer_hold_bytes={} exclusive={} reservation_bound_known={} reservation_base_bytes={} reservation_final_bytes={} reservation_peak_bytes={} scope=owned_image_buffers_internal_heap_excluded\n",
				activeName,status.token.instanceId,status.token.generation,work->id,entry.key,ImageStamp(work->launch),kImageConcurrency,
				effective,imageJobs.size(),ImageHold(),ImageWork(),decodedLimit,kImageWorkLimit,activeDecodedBytes,exclusive,!exclusive,
				estimate.base,estimate.final,estimate.peak));
		} catch (...) { /* Admission計測の失敗は開始済みJobを変更しない。 */ }
		if (!prefetch) {
			prefetchFileCursor = imageCursor+1; prefetchBindingCursor = embeddedBindingCursor+1; prefetchScanning = true;
		}
		return true;
	}
	void PumpPrefetch() {
		if (!prefetchScanning || textEntity || imageCursor >= fileImages.size() ||
			(!activeDecodedBytes && !HasImageKey(fileImages[imageCursor].key))) return;
		if (std::any_of(imageJobs.begin(),imageJobs.end(),[](const auto& work){return work->exclusive;})) return;
		while (imageJobs.size() < kImageConcurrency) {
			const FileImage* file = nullptr;
			const Model::EmbeddedTextureSource* embedded = nullptr;
			if (prefetchFileCursor < fileImages.size()) file = &fileImages[prefetchFileCursor++];
			else if (parsedModel && prefetchBindingCursor < parsedModel->embeddedBindings.size()) {
				const auto& binding = parsedModel->embeddedBindings[prefetchBindingCursor++];
				if (binding.sourceIndex >= parsedModel->embeddedTextures.size()) continue;
				embedded = &parsedModel->embeddedTextures[binding.sourceIndex];
			} else { prefetchScanning = false; SetImageIdle(ImageIdle::NoKnownCandidate); return; }
			const auto& key = file ? file->key : embedded->key;
			if (HasImageKey(key) || sequentialImageKeys.contains(key) || failedMaterialImages.contains(key) ||
				TextureManager::GetInstance()->HasTexture(key)) continue;
			const auto recipe = file ? file->source.recipe : embedded->expandGrayscaleBaseColor ?
				SceneTextureCpuSource::Recipe::ModelGrayscaleBaseColor : SceneTextureCpuSource::Recipe::OrdinaryMemory;
			if (file && !file->source.resolvedPath.empty()) { SetImageIdle(ImageIdle::UnsupportedSource); continue; }
			const auto estimate = EstimateMemoryImage(file ? file->source.bytes : embedded->bytes, recipe,
				file ? file->source.formatHint : embedded->formatHint);
			if (!estimate.final) { SetImageIdle(ImageIdle::EstimateUnknown); continue; }
			if (estimate.final > decodedLimit || estimate.peak > kImageWorkLimit || ImageHold() > decodedLimit-estimate.final ||
				ImageWork() > kImageWorkLimit-estimate.peak) { SetImageIdle(ImageIdle::Capacity); continue; }
			FileImage candidateImage;
			if (file) candidateImage = *file;
			else {
				candidateImage.key = key; candidateImage.source.recipe = recipe;
				candidateImage.source.bytes = embedded->bytes; candidateImage.source.formatHint = embedded->formatHint;
				candidateImage.allowMaterialFallback = true;
			}
			StartImage(candidateImage,true);
		}
	}
	void TryPumpPrefetch() noexcept {
		try { PumpPrefetch(); } catch (...) { prefetchScanning = false; SetImageIdle(ImageIdle::StartFailed); }
	}
	~PreparationTransaction() {
		CancelImageJobs();
		for (auto& work : imageJobs) if (work->worker.joinable()) work->worker.join();
		if (modelWorker.joinable()) modelWorker.join();
		RetainModelInterval();
		ReportImageWindow("destructor_cleanup");
	}

	void AddFileImage(const std::string& path, bool environment, bool materialFallback = false) {
		auto resolved = std::filesystem::absolute(
			EditableResourcePath::ResolveResource(StringUtility::ToPath(path))).lexically_normal();
		const bool hdr = environment && CompareStringOrdinal(
			resolved.extension().c_str(), -1, L".hdr", -1, TRUE) == CSTR_EQUAL;
		const auto key = hdr ? TextureManager::MakeEnvironmentTextureKey(path) : path;
		const auto recipe = hdr ? SceneTextureCpuSource::Recipe::HdrEnvironment : SceneTextureCpuSource::Recipe::OrdinaryFile;
		for (auto& entry : fileImages) {
			if (entry.key != key) continue;
			if (entry.source.recipe != recipe || entry.source.resolvedPath != resolved) {
				throw std::runtime_error("Conflicting Scene texture dependency: " + key);
			}
			entry.requireCube = entry.requireCube || environment;
			entry.allowMaterialFallback = entry.allowMaterialFallback && materialFallback;
			return;
		}
		FileImage entry;
		entry.key = key;
		entry.source.recipe = recipe;
		entry.source.resolvedPath = std::move(resolved);
		entry.requireCube = environment;
		entry.allowMaterialFallback = materialFallback;
		fileImages.push_back(std::move(entry));
	}

	void AddEmbeddedImage(const Model::EmbeddedTextureSource& source) {
		SceneTextureCpuSource input;
		input.recipe = source.expandGrayscaleBaseColor ? SceneTextureCpuSource::Recipe::ModelGrayscaleBaseColor :
			SceneTextureCpuSource::Recipe::OrdinaryMemory;
		input.bytes = source.bytes;
		input.formatHint = source.formatHint;
		for (const auto& entry : fileImages) {
			if (entry.key != source.key) continue;
			if (entry.source.recipe != input.recipe || entry.source.bytes != input.bytes ||
				entry.source.formatHint != input.formatHint || !entry.source.resolvedPath.empty()) {
				throw std::runtime_error("Conflicting embedded Scene texture dependency: " + source.key);
			}
			return;
		}
		FileImage entry;
		entry.key = source.key;
		entry.source = std::move(input);
		entry.allowMaterialFallback = true;
		fileImages.push_back(std::move(entry));
	}

	void SkipFailedMaterialImage() {
		failedMaterialImages.insert(fileImages.at(imageCursor).key);
		// 失敗したAの搬送が終端になった時点で、その保持creditだけ解放する。
		activeDecodedBytes = 0;
		prefetchScanning = false;
		++imageCursor;
		status.phase = ScenePreparePhase::PreparingCpu;
	}

	void StartModel(const std::string& key) {
		auto requested = StringUtility::ToPath(key);
		if (requested.is_absolute()) {
			requested = EditableResourcePath::ToProjectRelative(EditableResourcePath::ResolveResource(requested));
		}
		auto normalized = StringUtility::ToUtf8(requested.lexically_normal());
		if (normalized.starts_with("resources/")) normalized.erase(0, sizeof("resources/") - 1);
		const auto path = std::filesystem::absolute(
			EditableResourcePath::ResolveResource(StringUtility::ToPath(normalized))).lexically_normal();
		const auto alleyRoot = std::filesystem::absolute(
			EditableResourcePath::ResolveResource("alley")).lexically_normal();
		const auto relative = path.lexically_relative(alleyRoot);
		const bool alley = !relative.empty() && *relative.begin() != ".." && relative != ".";
		modelDiagnostics.clear();
		modelDone.store(false, std::memory_order_relaxed);
		modelWorker = std::thread([this, path, alley] {
			const auto started = TimingClock::now();
			modelRunStarted = started;
			try {
				Model::PreparedModelData candidateModel;
				if (Model::TryPrepareModelData(path, alley, candidateModel, modelDiagnostics)) {
					parsedModel.emplace(std::move(candidateModel));
				}
			} catch (...) {}
			modelRunEnded = TimingClock::now();
			modelWorkerUs.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(modelRunEnded - started).count(), std::memory_order_relaxed);
			modelDone.store(true, std::memory_order_release);
		});
		++modelStarts;
	}
};

SceneManager::SceneManager() = default;

bool SceneManager::IsPreparationSourceLive() const noexcept {
	if (!preparation_ || preparation_->status.projectEpoch != preparationEpoch_ ||
		!executionContext_ || executionContext_->IsEditing() ||
		preparation_->sourceId != activeSceneInstanceId_) return false;
	const auto* doc = GetActiveSceneDocument();
	return doc && doc->GetContentGeneration() == preparation_->sourceContentGeneration;
}

bool SceneManager::SetScenePreparationProjectEpoch(uint64_t epoch) {
	if (!epoch || epoch < preparationEpoch_) return false;
	if (epoch != preparationEpoch_ && preparation_) {
		CancelScenePreparation(preparation_->status.token);
	}
	preparationEpoch_ = epoch;
	return true;
}

bool SceneManager::BeginScenePreparation(const ScenePrepareRequest& request, ScenePrepareToken& out) {
	if (!request.projectEpoch || request.projectEpoch != preparationEpoch_ ||
		!request.sourceInstanceId || request.sourceInstanceId != activeSceneInstanceId_ ||
		!executionContext_ || !executionContext_->IsPlaying() || !sceneFactory_ || !sceneCatalog_ ||
		IsGameExitPending() || pendingSceneInstance_ || !pendingUnloadSceneInstanceIds_.empty() ||
		documentReadJob_) return false;
	if (preparation_ && (preparation_->candidate || !preparation_->retiring.empty() ||
		preparation_->ticket || preparation_->lookaheadTicket || preparation_->previousDocument)) return false;
	const auto* source = GetActiveSceneDocument();
	const auto* descriptor = sceneCatalog_->Find(request.targetSceneId);
	if (!source || !descriptor || descriptor->runtimeProfile != "RUNTIME" ||
		nextPreparationGeneration_ == (std::numeric_limits<uint64_t>::max)() ||
		!nextSceneInstanceId_ || nextSceneInstanceId_ == (std::numeric_limits<SceneInstanceId>::max)()) return false;

	try {
		auto job = std::make_unique<PreparationTransaction>();
		auto scene = std::unique_ptr<BaseScene>(sceneFactory_->CreateScene(descriptor->runtimeProfile));
		if (!scene || !scene->SupportsTransitionHold()) return false;
		job->status.token = {nextSceneInstanceId_, nextPreparationGeneration_ + 1};
		job->status.projectEpoch = request.projectEpoch;
		job->sourceId = request.sourceInstanceId;
		job->sourceContentGeneration = source->GetContentGeneration();
		job->activeName = descriptor->id;
		job->nextDocument = std::make_unique<SceneRuntimeDocumentState>();
		job->nextDocument->sceneId = descriptor->id;
		job->nextDocument->filePath = descriptor->filePath;
		const auto path = StringUtility::ToUtf8(std::filesystem::absolute(
			StringUtility::ToPath(descriptor->filePath)).lexically_normal());
		auto key = descriptor->id;
		if (FindSceneInstanceByKey(key)) key += "#" + std::to_string(job->status.token.instanceId);
		while (FindSceneInstanceByKey(key)) key += "#";
		job->candidate.reset(new SceneInstance(job->status.token.instanceId, descriptor->id, std::move(key), std::move(scene)));
		const auto token = job->status.token;
		if (!TryStartDocumentRead(token.instanceId, token.generation, request.projectEpoch, descriptor->id, path)) return false;
		++nextSceneInstanceId_;
		nextPreparationGeneration_ = token.generation;
		preparation_ = std::move(job);
		preparation_->diagDropsAtBegin = Logger::UploadTraceDroppedTotal();
		preparation_->Diag("begin",PreparationTransaction::DiagNow(),"precision=host_observed no_extra_gpu_poll=true sink_dropped_total={}",
			preparation_->diagDropsAtBegin);
		out = token;
		return true;
	} catch (...) {
		return false;
	}
}

bool SceneManager::PollScenePreparation(ScenePrepareToken token, ScenePrepareStatus& out) const {
	if (!preparation_ || !SameToken(token, preparation_->status.token)) return false;
	auto snapshot = preparation_->status;
	out = std::move(snapshot);
	return true;
}

void SceneManager::FailPreparation(std::string_view reason) noexcept {
	auto& job = *preparation_;
	job.status.phase = ScenePreparePhase::Failed;
	job.transferEnabled = false;
	try { job.status.diagnostic.assign(reason); } catch (...) { job.status.diagnostic.clear(); }
	CancelDocumentRead();
}

void SceneManager::CancelScenePreparation(ScenePrepareToken token) noexcept {
	if (!preparation_ || !SameToken(token, preparation_->status.token) ||
		preparation_->status.commitSucceeded || preparation_->status.phase == ScenePreparePhase::Cancelled ||
		preparation_->status.phase == ScenePreparePhase::Failed) return;
	preparation_->status.phase = ScenePreparePhase::Cancelling;
	preparation_->transferEnabled = false;
	CancelDocumentRead();
}

bool SceneManager::BeginPresentDestination(ScenePrepareToken token) {
	if (!IsPreparationSourceLive() || !SameToken(token, preparation_->status.token) ||
		preparation_->status.phase != ScenePreparePhase::Ready || !transitionHeld_) return false;
	preparation_->status.phase = ScenePreparePhase::PresentingDestination;
	return true;
}

BaseScene* SceneManager::GetPreparedRenderScene() const noexcept {
	if (!preparation_ || preparation_->status.phase != ScenePreparePhase::PresentingDestination ||
		!IsPreparationSourceLive() || !preparation_->candidate) return nullptr;
	return preparation_->candidate->GetScene();
}

SceneInstanceId SceneManager::GetPreparedDestinationRenderInstance() const noexcept {
	return GetPreparedRenderScene() ? preparation_->status.token.instanceId : kInvalidSceneInstanceId;
}

bool SceneManager::TryGetRenderRuntimePostProcessSettings(ScenePostProcessSettings& settings,
	uint64_t& generation, SceneInstanceId& instanceId) const {
	const auto* candidate = GetPreparedRenderScene();
	const auto* scene = candidate ? candidate : GetActiveScene();
	if (!scene || !scene->TryGetRuntimePostProcessSettings(settings, generation)) return false;
	instanceId = candidate ? preparation_->status.token.instanceId : activeSceneInstanceId_;
	return true;
}

bool SceneManager::CommitScenePreparation(ScenePrepareToken token, const SceneDestinationPresentation& proof) {
	if (!preparation_ || !SameToken(token, preparation_->status.token)) return false;
	auto& job = *preparation_;
	if (job.status.commitSucceeded) return SamePresentation(job.commitProof, proof);
	if (!IsPreparationSourceLive() || !transitionHeld_ ||
		job.status.phase != ScenePreparePhase::PresentingDestination || !job.candidate || !job.nextDocument ||
		!SameToken(token, proof.token) || proof.projectEpoch != preparationEpoch_ ||
		proof.drawnInstanceId != token.instanceId || !proof.outputGeneration ||
		!proof.recordingSerial || proof.recordingSerial != job.lastRenderSerial ||
		!proof.signaledFence || proof.completedFence < proof.signaledFence ||
		proof.completedFence == (std::numeric_limits<uint64_t>::max)()) return false;
	const auto* oldDocument = &executionContext_->GetActiveDocument();
	for (const auto& instance : sceneInstances_) {
		if (instance->IsPersistent() && instance->GetDocument() == oldDocument) return false;
	}
	try {
		job.retiring.reserve(sceneInstances_.size());
		sceneInstances_.reserve(sceneInstances_.size() + 1);
	} catch (...) {
		return false;
	}
	// 以下は無割当。旧Instanceとその借用Documentを同じ退役枠へ移す。
	job.status.phase = ScenePreparePhase::AwaitingCommit;
	executionContext_->ExchangePreparedRuntime(job.nextDocument);
	job.previousDocument = std::move(job.nextDocument);
	for (auto it = sceneInstances_.begin(); it != sceneInstances_.end();) {
		if ((*it)->IsPersistent()) { ++it; continue; }
		job.retiring.push_back(std::move(*it));
		it = sceneInstances_.erase(it);
	}
	sceneInstances_.push_back(std::move(job.candidate));
	activeSceneInstanceId_ = token.instanceId;
	sceneInstances_.back()->GetScene()->ActivatePreparedRuntime();
	currentSceneName_.swap(job.activeName);
	job.commitProof = proof;
	job.status.commitSucceeded = true;
	job.status.phase = ScenePreparePhase::RetiringPrevious;
	lastLoadOutcome_ = SceneLoadOutcome{token.instanceId, SceneLoadStatus::Activated, {}};
	return true;
}

void SceneManager::NotifyDestinationFrameRetired(ScenePrepareToken token, uint64_t recordingSerial) noexcept {
	if (!preparation_ || !SameToken(token, preparation_->status.token) ||
		!recordingSerial || recordingSerial != preparation_->lastRenderSerial) return;
	preparation_->retiredSerial = recordingSerial;
	if (preparation_->status.commitSucceeded && recordingSerial > preparation_->commitProof.recordingSerial) {
		preparation_->finalFrameRetired = true;
	}
}

bool SceneManager::TryBorrowSceneTextureUpload(SceneTextureUploadLoan& out) noexcept {
	if (!IsPreparationSourceLive()) return false;
	auto& job = *preparation_;
	if (!job.transferEnabled || !job.image || job.ticket || job.completedImage) return false;
	out = {job.imageEntry, &job.imageKey, &*job.image, job.imageMipPolicy};
	return true;
}

bool SceneManager::TryBorrowMaterialCopyLookahead(const SceneTextureUploadTicket& head, SceneTextureUploadLoan& out) noexcept {
	if (!IsPreparationSourceLive()) return false;
	auto& job = *preparation_;
	if (!job.transferEnabled || job.status.phase != ScenePreparePhase::Uploading || job.textEntity ||
		job.lookaheadTicket || !job.ticket || !SameTicket(*job.ticket, head) ||
		job.imageMipPolicy != SceneTextureMipPolicy::CpuComplete || job.imageCursor >= job.fileImages.size() ||
		job.nextImageEntry == (std::numeric_limits<uint64_t>::max)()) return false;
	try {
		const auto* textures = TextureManager::GetInstance();
		const auto nextFile = job.imageCursor + 1;
		PreparationTransaction::ImageJob* cpu = nullptr;
		bool requireCube = false;
		if (nextFile < job.fileImages.size()) {
			const auto& entry = job.fileImages[nextFile];
			cpu = job.FindImageJob(entry);
			requireCube = entry.requireCube;
		} else if (!job.modelWorker.joinable() && job.parsedModel) {
			// 現在のbindingがAと一致する場合だけ、同じModelの次の有効bindingを証明できる。
			const auto& model = *job.parsedModel;
			const size_t current = job.embeddedBindingCursor;
			if (current >= model.embeddedBindings.size()) return false;
			const auto sourceAt = [&](size_t i) -> const Model::EmbeddedTextureSource* {
				const auto& binding = model.embeddedBindings[i];
				if (binding.sourceIndex >= model.embeddedTextures.size() ||
					binding.materialIndex >= model.model.materials.size()) return nullptr;
				return &model.embeddedTextures[binding.sourceIndex];
			};
			const auto* a = sourceAt(current);
			if (!a || a->key != job.imageKey) return false;
			for (size_t i = current + 1; i < model.embeddedBindings.size(); ++i) {
				const auto* b = sourceAt(i);
				if (!b) return false;
				if (b->key == a->key || job.failedMaterialImages.contains(b->key) || textures->HasTexture(b->key)) continue;
				const auto recipe = b->expandGrayscaleBaseColor ? SceneTextureCpuSource::Recipe::ModelGrayscaleBaseColor :
					SceneTextureCpuSource::Recipe::OrdinaryMemory;
				for (const auto& work : job.imageJobs) {
					const auto& source = work->entry.source;
					if (work->entry.key == b->key && source.recipe == recipe && source.resolvedPath.empty() &&
						source.formatHint == b->formatHint && source.bytes == b->bytes) {
						cpu = work.get();
						break;
					}
				}
				break; // 次が未準備でも、それより先を追い越さない。
			}
		}
		// 通常Pumpが取り込んだ結果だけ使用し、この位置ではworkerを待たない。
		if (!cpu || !cpu->reported || cpu->discard || cpu->cancelled.load(std::memory_order_relaxed) ||
			cpu->outcome != SceneTextureCpuStatus::Prepared || textures->HasTexture(cpu->entry.key) ||
			job.failedMaterialImages.contains(cpu->entry.key)) return false;
		const auto bytes = cpu->result.texture.image.GetPixelsSize();
		// workerの既存予約をBに移す。Aと他worker込みで同じ上限を使う。
		if (!bytes || bytes > cpu->holdReservation || bytes > cpu->workReservation ||
			job.ImageHold() > job.decodedLimit || job.ImageWork() > job.kImageWorkLimit ||
			(requireCube && !cpu->result.texture.metadata.IsCubemap())) return false;
		job.lookaheadKey = cpu->entry.key;
		job.lookaheadCpuJob = cpu->id;
		job.lookaheadReadyUs = std::chrono::duration_cast<std::chrono::microseconds>(cpu->ended.time_since_epoch()).count();
		job.lookaheadMipPolicy = cpu->result.mipPolicy;
		out = {{head.key.token, head.key.sourceInstanceId, head.key.projectEpoch, ++job.nextImageEntry},
			&job.lookaheadKey, &cpu->result.texture, job.lookaheadMipPolicy};
		return true;
	} catch (...) { return false; } // 任意の先行受付だけを見送る。
}

void SceneManager::CommitMaterialCopyLookahead(const SceneTextureUploadTicket& ticket, uint64_t decodedBytes) noexcept {
	auto& job = *preparation_;
	const auto cpu = std::find_if(job.imageJobs.begin(), job.imageJobs.end(),
		[&](const auto& work) { return work->id == job.lookaheadCpuJob; });
	assert(cpu != job.imageJobs.end() && !job.lookaheadTicket && decodedBytes);
	job.lookaheadTicket = ticket;
	job.lookaheadBytes = decodedBytes;
	job.discardLookahead = false;
	job.EraseImageJob(cpu->get());
	job.UpdateImagePeaks();
}

bool SceneManager::IsSceneTextureUploadHead(const SceneTextureUploadTicket& ticket) const noexcept {
	return preparation_ && preparation_->ticket && SameTicket(*preparation_->ticket, ticket);
}

void SceneManager::ObserveSceneTextureTransfer(const SceneTextureTransferObservation& value) noexcept {
	if (!preparation_ || !SameToken(value.token,preparation_->status.token)) return;
	preparation_->diagSourceValid = IsPreparationSourceLive();
	preparation_->DiagStage(value);
}

void SceneManager::CommitSceneTextureUpload(const SceneTextureUploadTicket& ticket) noexcept {
	assert(preparation_ && preparation_->image && !preparation_->ticket &&
		ticket.uploadId && SameTransfer(preparation_->imageEntry, ticket.key));
	preparation_->ticket = ticket;
	preparation_->image.reset();
	preparation_->DiagAdmission(ticket);
	if (!preparation_->textEntity) {
		preparation_->prefetchFileCursor = preparation_->imageCursor + 1;
		preparation_->prefetchBindingCursor = preparation_->embeddedBindingCursor + 1;
		preparation_->prefetchScanning = true;
	}
}

void SceneManager::RejectSceneTextureUpload(const SceneTextureTransferKey& key, std::string_view reason) noexcept {
	if (!IsPreparationSourceLive() || !preparation_->image ||
		!SameTransfer(preparation_->imageEntry, key)) return;
	FailPreparation(reason);
}

bool SceneManager::ShouldCancelSceneTextureUpload(const SceneTextureUploadTicket& ticket) const noexcept {
	if (!preparation_ || !IsPreparationSourceLive() || preparation_->status.phase == ScenePreparePhase::Cancelling ||
		preparation_->status.phase == ScenePreparePhase::Failed) return true;
	if (IsSceneTextureUploadHead(ticket)) return false;
	return !preparation_->lookaheadTicket || !SameTicket(*preparation_->lookaheadTicket, ticket) ||
		preparation_->discardLookahead;
}

void SceneManager::AcceptSceneTextureUpload(SceneTextureUploadCompletion&& completion) noexcept {
	if (!preparation_ || !preparation_->ticket || !SameTicket(*preparation_->ticket, completion.ticket)) {
		assert(false && "Scene texture completion does not match the in-flight ticket");
		return;
	}
	auto& job = *preparation_;
	const bool discard = ShouldCancelSceneTextureUpload(completion.ticket);
	job.ticket.reset();
	if (discard) { job.activeDecodedBytes = 0; return; }
	if (completion.outcome == SceneTextureUploadOutcome::RetryCpuMips) {
		try {
			if (job.textEntity || job.imageMipPolicy != SceneTextureMipPolicy::GpuSrgbBox4096 ||
				!job.forcedCpuMipKeys.insert(job.imageKey).second) {
				FailPreparation("Invalid or repeated GPU mip CPU retry."); return;
			}
			++job.gpuCpuRetries;
			job.activeDecodedBytes = 0;
			job.imageMipPolicy = SceneTextureMipPolicy::CpuComplete;
			job.status.phase = ScenePreparePhase::PreparingCpu;
		} catch (...) { FailPreparation("GPU mip CPU retry allocation failed."); }
		try {
			Logger::Log(std::format("[LoadTiming] gpu_mip_cpu_retry token={} generation={} key=\"{}\" reason={}\n",
				job.status.token.instanceId,job.status.token.generation,job.imageKey,completion.diagnostic));
		} catch (...) { /* 計測はretry可否を変更しない。 */ }
		return;
	}
	if (completion.outcome == SceneTextureUploadOutcome::Failed && !job.textEntity &&
		job.imageCursor < job.fileImages.size() && job.fileImages[job.imageCursor].allowMaterialFallback) {
		try { job.SkipFailedMaterialImage(); }
		catch (...) { FailPreparation("Failed to retain the material texture fallback result."); }
		return;
	}
	if (completion.outcome != SceneTextureUploadOutcome::Ready || !completion.texture.resource ||
		completion.texture.textureKey != job.imageKey || !completion.recordingSerial || !completion.signaledFence ||
		completion.completedFence < completion.signaledFence ||
		completion.completedFence == (std::numeric_limits<uint64_t>::max)()) {
		FailPreparation(completion.diagnostic.empty() ? std::string_view("Scene texture upload failed.") :
			std::string_view(completion.diagnostic));
		return;
	}
	job.completedImage.emplace(std::move(completion));
	if (job.completedImage->texture.gpuSrgbMipsPrepared) ++job.gpuCompletedImages;
}

void SceneManager::RetireSceneTextureUploadForShutdown(const SceneTextureUploadTicket& ticket) noexcept {
	if (!preparation_) return;
	if (IsSceneTextureUploadHead(ticket)) {
		preparation_->ticket.reset();
		preparation_->activeDecodedBytes = 0;
	}
}

void SceneManager::RetireMaterialCopyLookahead(const SceneTextureUploadTicket& ticket) noexcept {
	if (preparation_ && preparation_->lookaheadTicket && SameTicket(*preparation_->lookaheadTicket, ticket)) {
		preparation_->lookaheadTicket.reset();
		preparation_->lookaheadBytes = 0;
	}
}

ScenePreparePumpResult SceneManager::PumpScenePreparation(const ScenePrepareBudget& budget) {
	if (!preparation_) return ScenePreparePumpResult::Terminal;
	auto& job = *preparation_;
	// Stop後も次の要求までは同じ取引を照会できる。回収済み候補の構築は再開しない。
	if (job.status.phase == ScenePreparePhase::Cancelled || job.status.phase == ScenePreparePhase::Completed)
		return ScenePreparePumpResult::Terminal;
	job.ObserveFinishedImage();
	if (!job.status.commitSucceeded && !IsPreparationSourceLive()) CancelScenePreparation(job.status.token);
	job.transferEnabled = false;

	if (job.status.phase == ScenePreparePhase::Cancelling || job.status.phase == ScenePreparePhase::Failed) {
		job.SetImageIdle(PreparationTransaction::ImageIdle::Disabled);
		PollDocumentRead();
		if (!job.DrainImageJobs()) return ScenePreparePumpResult::Yield;
		if (job.modelWorker.joinable()) {
			if (!job.modelDone.load(std::memory_order_acquire)) return ScenePreparePumpResult::Yield;
			job.modelWorker.join();
			job.RetainModelInterval();
		}
		job.prefetchScanning = false;
		job.parsedModel.reset();
		job.modelBuild.reset();
		job.image.reset();
		job.completedImage.reset();
		if (!job.ticket) job.activeDecodedBytes = 0;
		if (!documentReadJob_ && !job.ticket && !job.lookaheadTicket && job.retiredSerial >= job.lastRenderSerial) {
			job.candidate.reset();
			job.nextDocument.reset();
			if (job.status.phase == ScenePreparePhase::Cancelling) job.status.phase = ScenePreparePhase::Cancelled;
			job.ReportImageWindow(job.status.phase == ScenePreparePhase::Cancelled ? "cancelled_cleanup" : "failed_cleanup");
			// worker・搬送・診断の参照が終わった依存一覧は、失敗・取消後に保持しない。
			decltype(job.fileImages){}.swap(job.fileImages);
		}
		return job.candidate || documentReadJob_ || job.ticket || job.lookaheadTicket ? ScenePreparePumpResult::Yield : ScenePreparePumpResult::Terminal;
	}
	if (job.status.phase == ScenePreparePhase::RetiringPrevious) {
		// Commit時の証跡には終端scatterの利用は含まれない。
		if (!job.finalFrameRetired) return ScenePreparePumpResult::Yield;
		if (!job.retiring.empty()) {
			auto& instance = job.retiring.back();
			const auto sceneId = instance->GetSceneId();
			instance->GetScene()->PrepareReplayForFinalize();
			instance.reset();
			job.retiring.pop_back();
			// Particle共有枠は同Sceneの旧候補が最後に退役した後だけ解放する。
			if (std::none_of(job.retiring.begin(), job.retiring.end(), [&](const auto& remaining) {
				return remaining->GetSceneId() == sceneId;
			})) ReleaseSceneParticlesIfUnused(sceneId);
			return ScenePreparePumpResult::Yield;
		}
		job.previousDocument.reset();
		job.status.phase = ScenePreparePhase::Completed;
		return ScenePreparePumpResult::Terminal;
	}
	const bool enabled = budget.runtime.maxItems && budget.runtime.maxResourceCreates &&
		budget.runtime.maxCopyBytes && budget.model.maxCopyBytes && budget.model.maxResources &&
		budget.maxTextureStagingBytes && budget.maxDecodedImageBytes && budget.softCpuMicroseconds;
	if (!enabled) {
		job.SetImageIdle(PreparationTransaction::ImageIdle::Disabled);
		return ScenePreparePumpResult::Yield;
	}
	job.decodedLimit = budget.maxDecodedImageBytes;
	job.allowGpuMips = budget.allowGpuMips;
	job.transferEnabled = true;
	job.PruneCachedImageJobs();
	if (job.lookaheadTicket && (job.discardLookahead || TextureManager::GetInstance()->HasTexture(job.lookaheadKey))) {
		// 別経路で公開された場合も、Bを回収してから通常のcache/cube照合に戻る。
		job.discardLookahead = true;
		if (!job.ticket && !job.completedImage) return ScenePreparePumpResult::Yield;
	}
	if (job.status.phase == ScenePreparePhase::PreparingCpu || job.status.phase == ScenePreparePhase::Uploading) job.TryPumpPrefetch();
	// 素材laneとは別の候補内進捗。末尾の確定は全素材公開barrier後だけ許す。
	const auto pumpRuntime = [&]() -> ScenePreparePumpResult {
		if (!job.runtimeStarted || (job.assetsPublished && (job.image || job.ticket))) return ScenePreparePumpResult::Yield;
		auto runtimeBudget = budget.runtime;
		runtimeBudget.allowRuntimeFinalize = job.assetsPublished;
		runtimeBudget.assetRevision = job.assetRevision;
		runtimeBudget.metrics = &job.status.runtime;
		runtimeBudget.textObservationContext = &job;
		runtimeBudget.textObservation = [](void* context, const ScenePreparedTextObservation& value) noexcept {
			static_cast<PreparationTransaction*>(context)->DiagTextEvent(value);
		};
		if (PreparationTransaction::TimingClock::now() >= runtimeBudget.deadline) return ScenePreparePumpResult::Yield;
		job.status.runtime.action = "idle";
		std::string diagnostic;
		const auto began = PreparationTransaction::TimingClock::now();
		RuntimePrepareStep step;
		try {
			++job.runtimeSteps;
			step = job.candidate->StepPreparedRuntime(runtimeBudget, diagnostic);
		} catch (const std::exception& error) {
			job.RecordRuntimeWork(began,PreparationTransaction::TimingClock::now(),runtimeBudget);
			FailPreparation(error.what()); return ScenePreparePumpResult::Yield;
		} catch (...) {
			job.RecordRuntimeWork(began,PreparationTransaction::TimingClock::now(),runtimeBudget);
			FailPreparation("Destination Runtime preparation failed."); return ScenePreparePumpResult::Yield;
		}
		job.RecordRuntimeWork(began,PreparationTransaction::TimingClock::now(),runtimeBudget);
		if (step == RuntimePrepareStep::Ready) {
			if (!job.assetsPublished) { FailPreparation("Runtime became ready before assets."); return ScenePreparePumpResult::Yield; }
			job.status.phase = ScenePreparePhase::Ready;
			job.RuntimeMilestone("runtime_ready");
			job.ReportImageWindow("ready");
			try {
				Logger::Log(std::format("[LoadTiming] workers target={} token={} image_jobs={} image_decode_mip_ms={} "
					"model_jobs={} model_parse_ms={} model_build_steps={} runtime_steps={}\n",
					job.activeName,job.status.token.instanceId,job.imageStarts,job.imageWorkerUs.load(std::memory_order_relaxed)/1000,
					job.modelStarts,job.modelWorkerUs.load(std::memory_order_relaxed)/1000,job.modelBuildSteps,job.runtimeSteps));
			} catch (...) {}
			return ScenePreparePumpResult::Yield;
		}
		if (step == RuntimePrepareStep::Progress) return ScenePreparePumpResult::ContinueNow;
		if (step == RuntimePrepareStep::WaitingForAssets && job.assetsPublished) {
			FailPreparation("Runtime dependencies remained unavailable after all required assets were published.");
			return ScenePreparePumpResult::Yield;
		}
		if (step != RuntimePrepareStep::Pending && step != RuntimePrepareStep::WaitingForAssets) {
			FailPreparation(diagnostic); return ScenePreparePumpResult::Yield;
		}
		if (job.assetsPublished) try {
			PreparedSceneTextImage image;
			if (job.candidate->GetScene()->TryTakePreparedTextImage(image)) {
				const auto size = image.texture.image.GetPixelsSize();
				if (!size || size > budget.maxDecodedImageBytes || job.nextImageEntry == (std::numeric_limits<uint64_t>::max)()) {
					FailPreparation("Destination text image exceeded the preparation budget."); return ScenePreparePumpResult::Yield;
				}
				job.imageKey = std::move(image.textureKey);
				job.diagPendingCpuJob = 0;
				job.diagPendingReadyUs = PreparationTransaction::DiagNow(); // 真のpayload時刻はText callbackに記録。
				job.textEntity = image.entityId; job.textSignature = std::move(image.contentSignature);
				job.imageEntry = {job.status.token,job.sourceId,job.status.projectEpoch,++job.nextImageEntry};
				job.image.emplace(std::move(image.texture)); job.imageMipPolicy = SceneTextureMipPolicy::CpuComplete;
				job.activeDecodedBytes = size; job.UpdateImagePeaks();
			}
		} catch (...) { FailPreparation("Destination text preparation failed."); }
		return ScenePreparePumpResult::Yield;
	};

	const auto pumpAssets = [&]() -> ScenePreparePumpResult {
		if (job.status.phase == ScenePreparePhase::Uploading && job.ticket && !job.textEntity) return ScenePreparePumpResult::Yield;
		if (job.status.phase == ScenePreparePhase::ReadingDocument) {
			const auto token = job.status.token;
			bool failed = false;
			if (!TryTakeDocumentRead(token.instanceId, token.generation, job.status.projectEpoch, job.nextDocument->document, failed)) return ScenePreparePumpResult::Yield;
			if (failed || !job.nextDocument->document) {
				FailPreparation("Destination SceneDocument could not be prepared.");
				return ScenePreparePumpResult::Yield;
			}
			job.nextDocument->document->MarkClean();
			job.candidate->BindDocument(job.nextDocument->document.get());
		job.DiagPlanTexts();
			job.RuntimeMilestone("document_bound");
			try {
				if (!job.candidate->BeginPreparedRuntime(this)) {
					FailPreparation("Destination Runtime preparation could not start."); return ScenePreparePumpResult::Yield;
				}
			} catch (...) { FailPreparation("Destination Runtime Begin failed."); return ScenePreparePumpResult::Yield; }
			job.runtimeStarted = true;
			job.RuntimeMilestone("runtime_begin");
			job.status.phase = ScenePreparePhase::ResolvingDependencies;
			return ScenePreparePumpResult::Yield;
		}
		if (job.completedImage) {
			try {
				std::string diagnostic;
				job.DiagSample(PreparationTransaction::DiagNow());
				const auto publishBeginUs = PreparationTransaction::DiagNow();
				const auto published = TextureManager::GetInstance()->TryPublishCompletedTexture(job.completedImage->texture, diagnostic);
				const auto publishEndUs = PreparationTransaction::DiagNow();
				job.Diag("publish_call",publishEndUs,"upload={} key=\"{}\" begin_us={} end_us={}",
					job.completedImage->ticket.uploadId,job.imageKey,publishBeginUs,publishEndUs);
				if (published == PreparedPublishStatus::Failed) {
					if (!job.textEntity && job.imageCursor < job.fileImages.size() &&
						job.fileImages[job.imageCursor].allowMaterialFallback) job.SkipFailedMaterialImage();
					else FailPreparation(diagnostic);
				} else {
					++job.logicalPublished;
					job.DiagPublished(publishEndUs);
					++job.assetRevision;
					job.RuntimeMilestone("texture_published",job.imageKey);
					if (job.textEntity) {
						if (!job.candidate->GetScene()->AcceptPreparedTextImage(job.textEntity, job.textSignature, diagnostic)) {
							FailPreparation(diagnostic.empty() ? "Destination text image was rejected." : diagnostic);
						}
						job.textEntity = 0;
					} else {
						++job.imageCursor;
						job.status.phase = ScenePreparePhase::PreparingCpu;
					}
				}
			} catch (...) {
				FailPreparation("Scene texture publication failed.");
			}
			job.completedImage.reset();
			job.activeDecodedBytes = 0;
			job.imageMipPolicy = SceneTextureMipPolicy::CpuComplete;
			job.prefetchScanning = false;
			return ScenePreparePumpResult::Yield;
		}
		try {
			if (job.status.phase == ScenePreparePhase::ResolvingDependencies) {
				const auto& document = *job.nextDocument->document;
				if (!job.entityCursor) {
					job.AddFileImage("resources/rostock_laage_airport_4k.dds", true);
				}
				if (job.entityCursor == document.GetEntities().size()) {
					job.status.phase = ScenePreparePhase::PreparingCpu;
					return ScenePreparePumpResult::ContinueNow;
				}
				const auto& entity = document.GetEntities()[job.entityCursor++];
				if (const auto* mesh = SceneEntityQuery::FindEnabledComponent(entity, "MeshRenderer");
					mesh && !mesh->modelPath.empty()) {
					if (std::find(job.modelKeys.begin(), job.modelKeys.end(), mesh->modelPath) == job.modelKeys.end()) {
						job.modelKeys.push_back(mesh->modelPath);
					}
					for (const auto& override : mesh->meshMaterialOverrides) {
						if (override.enabled && !override.texturePath.empty()) job.AddFileImage(override.texturePath, false);
					}
				}
				if (const auto* sprite = SceneEntityQuery::FindEnabledComponent(entity, "SpriteRenderer");
					sprite && !sprite->texturePath.empty()) job.AddFileImage(sprite->texturePath, false);
				if (!job.environmentFound && SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)) {
					if (const auto* environment = SceneEntityQuery::FindEnabledComponent(entity, "Environment")) {
						job.environmentFound = true;
						if (environment->environmentSkyboxEnabled && !environment->environmentSkyboxPath.empty()) {
							const auto path = StringUtility::ToPath(environment->environmentSkyboxPath);
							job.AddFileImage(path.is_absolute() ?
								StringUtility::ToUtf8(EditableResourcePath::ToProjectRelative(path)) :
								environment->environmentSkyboxPath, true);
						}
					}
				}
				return ScenePreparePumpResult::ContinueNow;
			}
			if (job.status.phase == ScenePreparePhase::PreparingCpu) {
				if (job.imageCursor < job.fileImages.size()) {
					const auto& entry = job.fileImages[job.imageCursor];
					auto* textures = TextureManager::GetInstance();
					if (job.lookaheadTicket) {
						if (job.lookaheadKey != entry.key) {
							job.discardLookahead = true;
							return ScenePreparePumpResult::Yield;
						}
						// 正規の依存カーソルがBを要求し、先頭Aの公開/失敗解決が済んだ。
						job.imageKey.swap(job.lookaheadKey);
						job.imageEntry = job.lookaheadTicket->key;
						job.ticket = job.lookaheadTicket;
						job.lookaheadTicket.reset();
						job.activeDecodedBytes = std::exchange(job.lookaheadBytes, 0);
						job.imageMipPolicy = job.lookaheadMipPolicy;
						job.diagPendingCpuJob = job.lookaheadCpuJob;
						job.diagPendingReadyUs = job.lookaheadReadyUs;
						job.status.phase = ScenePreparePhase::Uploading;
						job.DiagAdmission(*job.ticket);
						job.prefetchFileCursor = job.imageCursor + 1;
						job.prefetchBindingCursor = job.embeddedBindingCursor + 1;
						job.prefetchScanning = true;
						return ScenePreparePumpResult::Yield;
					}
					if (textures->HasTexture(entry.key)) {
						if (auto* cpu = job.FindImageJob(entry)) {
							cpu->discard = true; cpu->cancelled.store(true,std::memory_order_relaxed);
						}
						if (entry.requireCube && !textures->GetMetaData(entry.key).IsCubemap()) {
							FailPreparation("Destination environment texture is not a cubemap: " + entry.key);
							return ScenePreparePumpResult::Yield;
						}
						++job.imageCursor;
						return ScenePreparePumpResult::ContinueNow;
					}
					auto* cpu = job.FindImageJob(entry);
					if (!cpu) {
						if (job.StartImage(entry)) job.TryPumpPrefetch();
						return ScenePreparePumpResult::Yield;
					}
					if (cpu->discard) return ScenePreparePumpResult::Yield;
					if (!job.prefetchScanning) {
						job.prefetchFileCursor = job.imageCursor+1;
						job.prefetchBindingCursor = job.embeddedBindingCursor+1;
						job.prefetchScanning = true;
					}
					job.TryPumpPrefetch();
					if (!cpu->done.load(std::memory_order_acquire)) return ScenePreparePumpResult::Yield;
					job.ObserveFinishedImage();
					if (cpu->outcome == SceneTextureCpuStatus::ReservationExceeded && !cpu->exclusive) {
						// 推定不足は材質失敗としない。通常順に来た時だけ他Jobを退け単独再試行する。
						job.sequentialImageKeys.insert(entry.key);
						job.EraseImageJob(cpu);
						return ScenePreparePumpResult::ContinueNow;
					}
					const auto size = cpu->result.texture.image.GetPixelsSize();
					if (cpu->outcome != SceneTextureCpuStatus::Prepared || !size || size > budget.maxDecodedImageBytes ||
						(entry.requireCube && !cpu->result.texture.metadata.IsCubemap())) {
						if (cpu->outcome == SceneTextureCpuStatus::Failed && entry.allowMaterialFallback) {
							job.EraseImageJob(cpu); job.SkipFailedMaterialImage();
							return ScenePreparePumpResult::Yield;
						}
						FailPreparation("Destination texture CPU preparation failed or exceeded budget: " + entry.key + " " + cpu->diagnostic);
						return ScenePreparePumpResult::Yield;
					}
					job.imageKey = entry.key;
					job.imageEntry = {job.status.token,job.sourceId,job.status.projectEpoch,++job.nextImageEntry};
					job.image.emplace(std::move(cpu->result.texture));
					job.diagPendingCpuJob = cpu->id;
					job.diagPendingReadyUs = std::chrono::duration_cast<std::chrono::microseconds>(cpu->ended.time_since_epoch()).count();
					job.imageMipPolicy = cpu->result.mipPolicy;
					job.activeDecodedBytes = size;
					job.EraseImageJob(cpu);
					job.UpdateImagePeaks();
					job.status.phase = ScenePreparePhase::Uploading;
					return ScenePreparePumpResult::Yield;
				}
				if (job.modelCursor < job.modelKeys.size()) {
					const auto& key = job.modelKeys[job.modelCursor];
					auto* manager = ModelManager::GetInstance();
					if (!job.modelWorker.joinable() && !job.parsedModel && !job.modelBuild) {
						if (auto* model = manager->FindModel(key)) {
							if (model->HasSkinning()) FailPreparation("Destination skinned model is unsupported: " + key);
							else {
								++job.modelCursor;
								return ScenePreparePumpResult::ContinueNow;
							}
						} else {
							job.StartModel(key);
						}
						return ScenePreparePumpResult::Yield;
					}
					if (job.modelWorker.joinable()) {
						if (!job.modelDone.load(std::memory_order_acquire)) return ScenePreparePumpResult::Yield;
						job.modelWorker.join();
						job.RetainModelInterval();
						if (!job.parsedModel) {
							FailPreparation("Destination model parse failed: " + key +
								(job.modelDiagnostics.empty() ? std::string{} : " " + job.modelDiagnostics.front()));
							return ScenePreparePumpResult::Yield;
						}
						if (job.parsedModel->model.hasSkinning) {
							FailPreparation("Destination model skinning requires further preparation: " + key);
							return ScenePreparePumpResult::Yield;
						}
						job.embeddedBindingCursor = job.materialCursor = 0;
						return ScenePreparePumpResult::Yield;
					}
					std::string diagnostic;
					if (!job.modelBuild) {
						auto& prepared = *job.parsedModel;
						// 同期Modelのbinding順で処理し、成功した画像だけを材質へ反映する。
						if (job.embeddedBindingCursor < prepared.embeddedBindings.size()) {
							const auto& binding = prepared.embeddedBindings[job.embeddedBindingCursor];
							if (binding.sourceIndex >= prepared.embeddedTextures.size() ||
								binding.materialIndex >= prepared.model.materials.size()) {
								FailPreparation("Invalid embedded material binding: " + key);
								return ScenePreparePumpResult::Yield;
							}
							const auto& source = prepared.embeddedTextures[binding.sourceIndex];
							if (TextureManager::GetInstance()->HasTexture(source.key)) {
								prepared.model.materials[binding.materialIndex].textureFilePath = source.key;
							} else if (!job.failedMaterialImages.contains(source.key)) {
								job.AddEmbeddedImage(source);
								return ScenePreparePumpResult::ContinueNow;
							} else if (source.expandGrayscaleBaseColor) {
								Logger::Log("Failed to expand grayscale model base color: " + source.key + "\n");
							}
							++job.embeddedBindingCursor;
							return ScenePreparePumpResult::ContinueNow;
						}
						if (job.materialCursor < prepared.model.materials.size()) {
							auto& material = prepared.model.materials[job.materialCursor];
							if (!material.textureFilePath.empty() &&
								TextureManager::GetInstance()->HasTexture(material.textureFilePath)) {
								++job.materialCursor;
							} else if (!material.textureFilePath.empty() &&
								!job.failedMaterialImages.contains(material.textureFilePath)) {
								job.AddFileImage(material.textureFilePath, false, true);
							} else {
								material.textureFilePath = material.textureFilePath.empty() ?
									"resources/human/white.png" : "resources/uvChecker.png";
								if (TextureManager::GetInstance()->HasTexture(material.textureFilePath)) ++job.materialCursor;
								else if (job.failedMaterialImages.contains(material.textureFilePath)) {
									FailPreparation("Required model fallback texture is unavailable: " + material.textureFilePath);
								} else job.AddFileImage(material.textureFilePath, false);
							}
							return job.status.phase == ScenePreparePhase::Failed ?
								ScenePreparePumpResult::Yield : ScenePreparePumpResult::ContinueNow;
						}
						if (!manager->TryBeginPrepared(*job.parsedModel, job.modelBuild, diagnostic)) {
							FailPreparation(diagnostic);
						}
						job.parsedModel.reset();
						return ScenePreparePumpResult::Yield;
					}
					++job.modelBuildSteps;
					const auto step = Model::StepPrepared(*job.modelBuild, budget.model, diagnostic);
					if (step == PreparedBuildStep::Pending) return ScenePreparePumpResult::Yield;
					std::unique_ptr<Model> model;
					if (step != PreparedBuildStep::Ready || !Model::TryTakePrepared(*job.modelBuild, model) ||
						manager->TryPublishPreparedModel(key, model, diagnostic) == PreparedPublishStatus::Failed) {
						FailPreparation(diagnostic.empty() ? "Destination model publication failed." : diagnostic);
						return ScenePreparePumpResult::Yield;
					}
					job.modelBuild.reset();
					++job.modelCursor;
					++job.assetRevision;
					job.RuntimeMilestone("model_published",key);
					return ScenePreparePumpResult::Yield;
				}
				if (!job.DrainImageJobs()) return ScenePreparePumpResult::Yield;
				// 素材確定後は再試行や依存照合に使わないため、圧縮bytesと一覧の容量を返す。
				decltype(job.fileImages){}.swap(job.fileImages);
				job.assetsPublished = true;
				job.localReadyAtAssets = job.status.runtime.localReady;
				job.RuntimeMilestone("assets_published");
				job.status.phase = ScenePreparePhase::ConstructingRuntime;
				return ScenePreparePumpResult::ContinueNow;
			}
			if (job.status.phase == ScenePreparePhase::ConstructingRuntime) return pumpRuntime();
		} catch (const std::exception& error) {
			FailPreparation(error.what());
		} catch (...) {
			FailPreparation("Destination preparation failed.");
		}
		return job.status.phase == ScenePreparePhase::Failed ? ScenePreparePumpResult::Yield :
			ScenePreparePumpResult::Terminal;
	};
	// 両laneが実行可能なら交互に進める。素材が待っていても候補仕事を試す。
	if (!job.assetsPublished && job.runtimeStarted && job.preferRuntime) {
		job.preferRuntime = false;
		const auto result = pumpRuntime();
		if (result == ScenePreparePumpResult::ContinueNow || job.status.phase == ScenePreparePhase::Failed) return result;
	}
	const auto result = pumpAssets();
	if (job.status.phase == ScenePreparePhase::Failed || job.status.phase == ScenePreparePhase::Ready || job.assetsPublished) return result;
	job.preferRuntime = true;
	if (result == ScenePreparePumpResult::Yield && job.runtimeStarted) return pumpRuntime();
	return result;
}

bool SceneManager::TryStartDocumentRead(SceneInstanceId instanceId, uint64_t generation, uint64_t projectEpoch,
	const std::string& sceneId, const std::string& resolvedPath) {
	if (documentReadJob_ || !instanceId || !generation || !projectEpoch || sceneId.empty() ||
		resolvedPath.empty()) return false;
	try {
		if (!StringUtility::ToPath(resolvedPath).is_absolute()) return false;
		auto job = std::make_unique<DocumentReadJob>();
		job->instanceId = instanceId; job->generation = generation; job->projectEpoch = projectEpoch;
		job->sceneId = sceneId; job->primaryPath = resolvedPath;
		job->Start(resolvedPath);
		documentReadJob_ = std::move(job);
		return true;
	} catch (...) { return false; }
}

void SceneManager::CancelDocumentRead() noexcept {
	if (documentReadJob_) documentReadJob_->cancelled.store(true, std::memory_order_release);
}

void SceneManager::PollDocumentRead() {
	if (!documentReadJob_ ||
		!documentReadJob_->done.load(std::memory_order_acquire)) return;
	auto& job = *documentReadJob_;
	// 完了公開後だけjoin。通常Frameで進行中IO/parseを待たない。
	if (job.worker.joinable()) job.worker.join();
	if (job.cancelled.load(std::memory_order_acquire)) { documentReadJob_.reset(); return; }
	if (job.terminal) return;
	try {
		std::unique_ptr<SceneDocumentRead> read;
		{
			std::lock_guard<std::mutex> lock(job.mutex);
			read = std::move(job.candidate);
		}
		auto document = std::make_unique<SceneDocument>();
		if (!job.readFailed && read && document->LoadReadCandidate(*read)) {
			if (job.backup) {
				document->MarkDirty();
				job.diagnostic = "Recovered from backup: " + job.primaryError;
			} else job.diagnostic.clear();
			job.document = std::move(document);
			job.terminal = true;
			return;
		}
		if (!job.readFailed && read) job.diagnostic = document->GetLastLoadError();
		if (!job.backup) {
			job.primaryError = job.diagnostic;
			job.backup = true;
			job.Start(job.primaryPath + ".bak");
			return;
		}
		job.diagnostic = "Primary: " + job.primaryError + " | Backup: " + job.diagnostic;
	} catch (...) {
		// partial Documentはlocalで破棄。追加診断確保に依存せず終端失敗へ進める。
	}
	job.failed = job.terminal = true;
	// backup workerの生成自体が失敗した場合も終端を一回取得できるようにする。
	job.done.store(true, std::memory_order_release);
}

bool SceneManager::TryTakeDocumentRead(SceneInstanceId instanceId, uint64_t generation, uint64_t projectEpoch,
	std::unique_ptr<SceneDocument>& out, bool& failed) {
	if (!documentReadJob_) return false;
	auto& job = *documentReadJob_;
	if (job.instanceId != instanceId || job.generation != generation || job.projectEpoch != projectEpoch) {
		CancelDocumentRead();
		return false;
	}
	PollDocumentRead();
	if (!documentReadJob_ || !job.terminal) return false;
	failed = job.failed;
	if (!job.failed) out = std::move(job.document);
	documentReadJob_.reset();
	return true;
}

bool SceneManager::RequestGameExit(SceneInstanceId sourceInstanceId, uint64_t generation) {
	const auto* document = GetActiveSceneDocument();
	if (!sourceInstanceId || sourceInstanceId != activeSceneInstanceId_ || !document ||
		generation != document->GetContentGeneration() || (executionContext_ && !executionContext_->IsPlaying()) ||
		pendingSceneInstance_ || presentedTransitionBusy_ || transitionHeld_ || !pendingUnloadSceneInstanceIds_.empty()) return false;
	if (IsGameExitPending()) return gameExitSource_ == sourceInstanceId && gameExitGeneration_ == generation;
	gameExitSource_ = sourceInstanceId; gameExitGeneration_ = generation;
	return true;
}

bool SceneManager::ConsumeGameExitRequest() {
	const auto source = std::exchange(gameExitSource_, kInvalidSceneInstanceId);
	const auto generation = std::exchange(gameExitGeneration_, uint64_t{0});
	const auto* document = GetActiveSceneDocument();
	return source && source == activeSceneInstanceId_ && document && generation == document->GetContentGeneration() &&
		(!executionContext_ || executionContext_->IsPlaying()) && !pendingSceneInstance_ && !transitionHeld_ && !presentedTransitionBusy_;
}

bool SceneManager::RequestPresentedTransition(const PresentedTransitionRequest& request) {
	if (request.uiSource.requestSerial && (request.uiSource.instanceId != request.sourceInstanceId ||
		!IsUiTransitionSourceValid(request.uiSource) || (executionContext_ && !executionContext_->IsPlaying()))) return false;
	if (IsGameExitPending() || presentedTransitionBusy_ || pendingSceneInstance_ || transitionHeld_ ||
		!FindSceneInstance(request.sourceInstanceId) || request.destinationSceneId.empty() ||
		request.transitionAssetPath.empty() || !sceneCatalog_ || !sceneCatalog_->Find(request.destinationSceneId)) {
		Logger::Log("Presented SceneTransition rejected (busy or invalid reference), source:"+
			std::to_string(request.sourceInstanceId)+", destination:"+request.destinationSceneId+
			", asset:"+request.transitionAssetPath+"\n");
		return false;
	}
	presentedTransitionRequest_ = request;
	presentedUiTransitionSource_ = request.uiSource;
	presentedTransitionBusy_ = true;
	return true;
}

bool SceneManager::ConsumePresentedTransitionRequest(PresentedTransitionRequest& out) {
	if (!presentedTransitionRequest_) return false;
	out = std::move(*presentedTransitionRequest_);
	presentedTransitionRequest_.reset();
	return true;
}

void SceneManager::FinishPresentedTransitionRequest() {
	presentedUiTransitionSource_ = {};
	presentedTransitionRequest_.reset();
	presentedTransitionBusy_ = false;
}

void SceneManager::ChangeScene(const std::string& sceneId) {
	if (presentedTransitionBusy_) {
		Logger::Log("SceneTransition rejected while a presented transition is occupied\n");
		return;
	}
	if (LoadScene(sceneId, SceneLoadMode::Single) != kInvalidSceneInstanceId) {
		// Bootstrapの初回生成は展開済みContextを使い、実行中の切替だけ再読込する。
		pendingReadSavedScene_ = activeSceneInstanceId_ != kInvalidSceneInstanceId;
	}
}

SceneInstanceId SceneManager::LoadScene(
	const std::string& sceneId,
	SceneLoadMode loadMode,
	const std::string& instanceKey
) {
	assert(sceneFactory_);
	if (IsGameExitPending() || !sceneFactory_ || pendingSceneInstance_) {
		return kInvalidSceneInstanceId;
	}

	const SceneDescriptor* descriptor = sceneCatalog_
		? sceneCatalog_->Find(sceneId)
		: nullptr;
	if (loadMode == SceneLoadMode::Additive) {
		if (!descriptor) {
			Logger::Log("Additive Scene is not registered: " + sceneId + "\n");
			return kInvalidSceneInstanceId;
		}
		if (executionContext_ && executionContext_->IsEditing()) {
			Logger::Log("Additive Scene loading is available in Play/Runtime mode only\n");
			return kInvalidSceneInstanceId;
		}
	}
	const std::string& runtimeProfile = descriptor
		? descriptor->runtimeProfile
		: sceneId;
	BaseScene* newScene = sceneFactory_->CreateScene(runtimeProfile);
	if (!newScene) {
		Logger::Log("Scene runtime profile is not registered: " + runtimeProfile + "\n");
		return kInvalidSceneInstanceId;
	}
	if (transitionHeld_ && !newScene->SupportsTransitionHold()) {
		Logger::Log("Scene runtime profile does not support transition hold: " + runtimeProfile + "\n");
		delete newScene;
		return kInvalidSceneInstanceId;
	}

	if (descriptor && executionContext_) {
		if (executionContext_->IsEditing()) {
			if (executionContext_->GetActiveSceneId() != descriptor->id) {
				Logger::Log(
					"Edit SceneDocument must be opened before changing Scene: " +
					descriptor->id + "\n"
				);
				delete newScene;
				return kInvalidSceneInstanceId;
			}
		}
	}

	// Documentはまだ切り替えず、次のUpdate先頭でSceneInstanceと同時に確定する。
	const SceneInstanceId instanceId = nextSceneInstanceId_++;
	if (nextSceneInstanceId_ == kInvalidSceneInstanceId) {
		++nextSceneInstanceId_;
	}
	std::string resolvedInstanceKey = instanceKey.empty()
		? (descriptor ? descriptor->id : sceneId)
		: instanceKey;
	if (const SceneInstance* existing = FindSceneInstanceByKey(
		resolvedInstanceKey
	)) {
		if (!instanceKey.empty()) {
			Logger::Log(
				"Scene instance key is already loaded: " +
				resolvedInstanceKey + "\n"
			);
			delete newScene;
			return kInvalidSceneInstanceId;
		}
		(void)existing;
		resolvedInstanceKey += "#" + std::to_string(instanceId);
		while (FindSceneInstanceByKey(resolvedInstanceKey)) {
			resolvedInstanceKey += "#";
		}
	}
	pendingSceneInstance_.reset(new SceneInstance(
		instanceId,
		descriptor ? descriptor->id : sceneId,
		std::move(resolvedInstanceKey),
		std::unique_ptr<BaseScene>(newScene)
	));
	pendingSceneLoadMode_ = loadMode;
	pendingReloadSceneInstanceId_ = kInvalidSceneInstanceId;
	pendingRestartScene_ = false;
	pendingReadSavedScene_ = loadMode == SceneLoadMode::Single && presentedTransitionBusy_;
	pendingUiTransitionSource_ = loadMode == SceneLoadMode::Single && presentedTransitionBusy_ ? presentedUiTransitionSource_ : SceneMenuSource{};
	return instanceId;
}

bool SceneManager::CancelPendingSceneLoad(SceneInstanceId instanceId) {
	if (instanceId == kInvalidSceneInstanceId || !pendingSceneInstance_ ||
		pendingSceneInstance_->GetId() != instanceId) {
		return false;
	}
	DiscardPendingScene();
	return true;
}

bool SceneManager::ConsumeLoadOutcome(
	SceneInstanceId instanceId, SceneLoadOutcome& outcome
) {
	if (!lastLoadOutcome_ || lastLoadOutcome_->instanceId != instanceId) {
		return false;
	}
	outcome = std::move(*lastLoadOutcome_);
	lastLoadOutcome_.reset();
	return true;
}

bool SceneManager::UnloadScene(SceneInstanceId instanceId) {
	if (IsGameExitPending()) return false;
	if (instanceId == kInvalidSceneInstanceId) {
		return false;
	}
	if (
		pendingSceneInstance_ &&
		pendingSceneInstance_->GetId() == instanceId
	) {
		DiscardPendingScene();
		return true;
	}
	if (!FindSceneInstance(instanceId)) {
		return false;
	}
	if (
		std::find(
			pendingUnloadSceneInstanceIds_.begin(),
			pendingUnloadSceneInstanceIds_.end(),
			instanceId
		) == pendingUnloadSceneInstanceIds_.end()
	) {
		pendingUnloadSceneInstanceIds_.push_back(instanceId);
	}
	return true;
}

bool SceneManager::SetActiveScene(SceneInstanceId instanceId) {
	const SceneInstance* instance = FindSceneInstance(instanceId);
	if (!instance) {
		return false;
	}
	if (activeSceneInstanceId_ != instanceId) { uiTransitionOutcome_.reset(); gameExitSource_ = kInvalidSceneInstanceId; gameExitGeneration_ = 0; }
	activeSceneInstanceId_ = instanceId;
	currentSceneName_ = instance->GetSceneId();
	return true;
}

bool SceneManager::SetScenePersistent(
	SceneInstanceId instanceId,
	bool persistent
) {
	if (
		persistent &&
		executionContext_ &&
		executionContext_->IsEditing()
	) {
		Logger::Log("Persistent Scene is available in Play/Runtime mode only\n");
		return false;
	}
	if (
		pendingSceneInstance_ &&
		pendingSceneInstance_->GetId() == instanceId
	) {
		pendingSceneInstance_->SetPersistent(persistent);
		return true;
	}
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (instance->GetId() == instanceId) {
			instance->SetPersistent(persistent);
			return true;
		}
	}
	return false;
}

void SceneManager::ReloadCurrentScene() {
	gameExitSource_ = kInvalidSceneInstanceId; gameExitGeneration_ = 0;
	// Reload/Editor Stopは演出の取消境界。旧保持を新しい予約へ持ち越さない。
	EndTransitionHold();
	if (pendingSceneInstance_) {
		if (currentSceneName_.empty()) {
			return;
		}
		DiscardPendingScene();
	}
	const std::string targetSceneId =
		executionContext_ && executionContext_->IsEditing()
			? executionContext_->GetActiveSceneId()
			: currentSceneName_;
	if (targetSceneId.empty()) {
		return;
	}

	if (
		LoadScene(targetSceneId, SceneLoadMode::Single) !=
		kInvalidSceneInstanceId
	) {
		// ReloadだけはPersistent指定に関係なく現在Instanceを置き換える。
		pendingReloadSceneInstanceId_ = activeSceneInstanceId_;
		// Play終了後のEdit ModeにはRuntimeで追加したPersistent Instanceを残さない。
		pendingForceUnloadAllSceneInstances_ =
			executionContext_ && executionContext_->IsEditing();
	}
}

SceneInstanceId SceneManager::RequestSceneRestart(SceneInstanceId sourceInstanceId) {
	if (sourceInstanceId == kInvalidSceneInstanceId || sourceInstanceId != activeSceneInstanceId_ ||
		pendingSceneInstance_ || presentedTransitionBusy_ || transitionHeld_ ||
		(executionContext_ && !executionContext_->IsPlaying()) || !sceneCatalog_ ||
		!sceneCatalog_->Find(currentSceneName_)) return kInvalidSceneInstanceId;
	const SceneInstanceId id = LoadScene(currentSceneName_, SceneLoadMode::Single);
	if (id != kInvalidSceneInstanceId) {
		pendingRestartScene_ = true;
		pendingReloadSceneInstanceId_ = sourceInstanceId;
	}
	return id;
}

bool SceneManager::IsUiTransitionSourceValid(const SceneMenuSource& source) const {
	const auto* document = GetActiveSceneDocument();
	return source.instanceId && source.instanceId == activeSceneInstanceId_ && document &&
		source.generation == document->GetContentGeneration() && (!executionContext_ || !executionContext_->IsEditing());
}
SceneInstanceId SceneManager::RequestUiSceneTransition(const SceneMenuSource& source, const std::string& destination) {
	if (!source.requestSerial || !IsUiTransitionSourceValid(source) || (executionContext_ && !executionContext_->IsPlaying()) ||
		pendingSceneInstance_ || transitionHeld_ || presentedTransitionBusy_ || !sceneCatalog_ || !sceneCatalog_->Find(destination)) return kInvalidSceneInstanceId;
	const auto id = LoadScene(destination, SceneLoadMode::Single);
	if (id != kInvalidSceneInstanceId) pendingUiTransitionSource_ = source;
	return id;
}
void SceneManager::ReportUiTransitionOutcome(const SceneMenuSource& source, SceneUiTransitionStatus status, const std::string& diagnostic) {
	if (source.requestSerial && IsUiTransitionSourceValid(source)) uiTransitionOutcome_ = SceneUiTransitionOutcome{source, status, diagnostic};
}
bool SceneManager::ConsumeUiTransitionOutcome(const SceneMenuSource& source, SceneUiTransitionOutcome& outcome) {
	if (!uiTransitionOutcome_ || !IsUiTransitionSourceValid(source) || uiTransitionOutcome_->source.instanceId != source.instanceId ||
		uiTransitionOutcome_->source.generation != source.generation || uiTransitionOutcome_->source.requestSerial != source.requestSerial) return false;
	outcome = *uiTransitionOutcome_; uiTransitionOutcome_.reset(); return true;
}

SceneDocument* SceneManager::GetActiveSceneDocument() {
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (instance->GetId() == activeSceneInstanceId_) {
			if (SceneDocument* document = instance->GetDocument()) {
				return document;
			}
			break;
		}
	}
	return executionContext_ ? &executionContext_->GetActiveDocument() : nullptr;
}

const SceneDocument* SceneManager::GetActiveSceneDocument() const {
	const SceneInstance* instance = GetActiveSceneInstance();
	if (instance && instance->GetDocument()) {
		return instance->GetDocument();
	}
	return executionContext_ ? &executionContext_->GetActiveDocument() : nullptr;
}

const SceneInstance* SceneManager::GetActiveSceneInstance() const {
	return FindSceneInstance(activeSceneInstanceId_);
}

bool SceneManager::TryGetRuntimeStat(const SceneDocument& document, uint64_t entityId,
	const std::string& statId, float& out, float* maxValue) const {
	const SceneInstance* instance = GetActiveSceneInstance();
	if (!instance || instance->GetDocument() != &document) return false;
	const BaseScene* scene = instance->GetScene();
	float value = 0.0f;
	if (!scene || !scene->TryGetRuntimeStat(entityId, statId, value, maxValue)) return false;
	out = value;
	return true;
}

bool SceneManager::TryGetRuntimeFlag(const SceneDocument& document, uint64_t entityId,
	const std::string& flagId, bool& out) const {
	const SceneInstance* instance = GetActiveSceneInstance();
	if (!instance || instance->GetDocument() != &document) return false;
	const BaseScene* scene = instance->GetScene();
	bool value = false;
	if (!scene || !scene->TryGetRuntimeFlag(entityId, flagId, value)) return false;
	out = value;
	return true;
}

bool SceneManager::TryGetEnemyPositioningStatus(const SceneDocument& document, uint64_t entityId,
	SceneEnemyPositioningStatus& out) const {
	const auto* instance = GetActiveSceneInstance();
	if (!instance || instance->GetDocument() != &document || !instance->GetScene()) return false;
	SceneEnemyPositioningStatus value;
	if (!instance->GetScene()->TryGetEnemyPositioningStatus(entityId, value)) return false;
	out = value;
	return true;
}

const SceneInstance* SceneManager::FindSceneInstance(SceneInstanceId instanceId) const {
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (instance->GetId() == instanceId) {
			return instance.get();
		}
	}
	return nullptr;
}

const SceneInstance* SceneManager::FindSceneInstanceByKey(
	const std::string& instanceKey
) const {
	if (instanceKey.empty()) {
		return nullptr;
	}
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (instance->GetInstanceKey() == instanceKey) {
			return instance.get();
		}
	}
	return nullptr;
}

std::vector<const SceneInstance*> SceneManager::GetLoadedSceneInstances() const {
	std::vector<const SceneInstance*> result;
	result.reserve(sceneInstances_.size());
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		result.push_back(instance.get());
	}
	return result;
}

SceneEntity* SceneManager::ResolveEntity(const SceneEntityHandle& handle) {
	if (!handle.IsValid()) {
		return nullptr;
	}
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (instance->GetId() != handle.sceneInstanceId) {
			continue;
		}
		SceneDocument* document = instance->GetDocument();
		return document ? document->FindEntity(handle.entityId) : nullptr;
	}
	return nullptr;
}

const SceneEntity* SceneManager::ResolveEntity(
	const SceneEntityHandle& handle
) const {
	if (!handle.IsValid()) {
		return nullptr;
	}
	const SceneInstance* instance = FindSceneInstance(
		handle.sceneInstanceId
	);
	const SceneDocument* document = instance
		? instance->GetDocument()
		: nullptr;
	return document ? document->FindEntity(handle.entityId) : nullptr;
}

SceneEntityHandle SceneManager::ResolveEntityReference(
	SceneInstanceId sourceInstanceId,
	const SceneEntityReference& reference
) const {
	if (reference.entityId == 0) {
		return {};
	}

	const SceneInstance* targetInstance = nullptr;
	if (reference.sceneId.empty()) {
		if (!reference.instanceKey.empty()) {
			return {};
		}
		targetInstance = FindSceneInstance(sourceInstanceId);
	} else if (!reference.instanceKey.empty()) {
		targetInstance = FindSceneInstanceByKey(reference.instanceKey);
		if (
			targetInstance &&
			targetInstance->GetSceneId() != reference.sceneId
		) {
			targetInstance = nullptr;
		}
	} else {
		for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
			if (instance->GetSceneId() != reference.sceneId) {
				continue;
			}
			if (targetInstance) {
				// 同じSceneアセットが複数ある場合はinstanceKeyが必須。
				return {};
			}
			targetInstance = instance.get();
		}
	}
	if (!targetInstance) {
		return {};
	}

	const SceneEntityHandle handle{
		targetInstance->GetId(),
		reference.entityId
	};
	return ResolveEntity(handle) ? handle : SceneEntityHandle{};
}

BaseScene* SceneManager::GetActiveScene() const {
	const SceneInstance* instance = GetActiveSceneInstance();
	return instance ? instance->GetScene() : nullptr;
}

ScenePreparationDiagnostic SceneManager::GetActivePreparationDiagnostic() const {
	const BaseScene* scene = GetActiveScene();
	return scene ? scene->GetPreparationDiagnostic() : ScenePreparationDiagnostic{};
}

bool SceneManager::TryGetActiveRuntimePostProcessSettings(
	ScenePostProcessSettings& settings,
	uint64_t& generation
) const {
	BaseScene* scene = GetActiveScene();
	return scene && scene->TryGetRuntimePostProcessSettings(settings, generation);
}

bool SceneManager::TryGetActiveViewEffectClock(SceneViewEffectClock& out, SceneInstanceId& instanceId) const {
	const BaseScene* scene = GetActiveScene();
	SceneViewEffectClock value{};
	if (!scene || !scene->TryGetViewEffectClock(value)) return false;
	out = value;
	instanceId = activeSceneInstanceId_;
	return true;
}

void SceneManager::PrepareMenuInput(float deltaTime, bool allowed) {
	const bool running = !executionContext_ || executionContext_->IsPlaying();
	for (const auto& instance : sceneInstances_) {
		if (IsGameExitPending()) break;
		if (auto* scene = instance->GetScene()) scene->PrepareMenuInput(deltaTime, allowed && running && scene == GetActiveScene());
	}
	const auto* active = GetActiveScene();
	const auto control = active ? active->GetUiControlFrame() : SceneUiControlFrame{};
	const auto* document = GetActiveSceneDocument();
	const bool blocked = running && document && control.instanceId == activeSceneInstanceId_ &&
		control.generation == document->GetContentGeneration() && control.blockGameplayInput;
	// Editor Pause中もMenu由来の理由は保持し、Resumeで勝手に解除しない。
	gamePaused_ = (!executionContext_ || !executionContext_->IsEditing()) && document &&
		control.instanceId == activeSceneInstanceId_ && control.generation == document->GetContentGeneration() && control.gamePaused;
	pauseBgm_ = gamePaused_ && control.pauseBgm;
	for (const auto& instance : sceneInstances_) if (auto* scene = instance->GetScene()) scene->SetUiGameplayInputBlocked(blocked);
}

bool SceneManager::CanPauseGame() const {
	if (sceneInstances_.empty()) return false;
	for (const auto& instance : sceneInstances_)
		if (!instance->GetScene() || !instance->GetScene()->SupportsGamePause()) return false;
	return !pendingSceneInstance_ || (pendingSceneInstance_->GetScene() && pendingSceneInstance_->GetScene()->SupportsGamePause());
}
void SceneManager::PrepareGameUpdate(float deltaTime) {
	if (gameUpdatePrepared_ || transitionHeld_ || IsGameExitPending()) return;
	ProcessPendingSceneUnloads();
	ActivatePendingScene();
	PrepareMenuInput(deltaTime, menuInputAllowed_);
	gameUpdatePrepared_ = true;
}

void SceneManager::Update(float deltaTime)
{
	if (transitionHeld_) {
		UpdateTransitionHold();
		return;
	}
	PrepareGameUpdate(deltaTime);
	gameUpdatePrepared_ = false;
	if (IsGameExitPending()) return;

	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			if (gamePaused_) scene->UpdateGamePaused(deltaTime);
			else scene->Update(deltaTime);
			if (IsGameExitPending()) return;
		}
	}
	if (!sceneInstances_.empty()) {
		if (BaseScene* activeScene = GetActiveScene()) {
			ParticleManager::GetInstance()->SetCamera(
				activeScene->GetRenderCamera()
			);
		}
		if (!gamePaused_) ParticleManager::GetInstance()->Update();
	}
}

bool SceneManager::BeginTransitionHold() {
	if (IsGameExitPending()) return false;
	if (executionContext_ && executionContext_->IsEditing()) {
		return false;
	}
	if (sceneInstances_.empty()) {
		return false;
	}
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		const BaseScene* scene = instance->GetScene();
		if (!scene || !scene->SupportsTransitionHold()) {
			return false;
		}
	}
	if (pendingSceneInstance_) {
		const BaseScene* pendingScene = pendingSceneInstance_->GetScene();
		if (!pendingScene || !pendingScene->SupportsTransitionHold()) {
			return false;
		}
	}
	transitionHeld_ = true;
	return true;
}

void SceneManager::EndTransitionHold() {
	transitionHeld_ = false;
}

void SceneManager::UpdateTransitionHold() {
	gameUpdatePrepared_ = false;
	if (!transitionHeld_ || (executionContext_ && executionContext_->IsPaused())) {
		PrepareMenuInput(0.0f, false);
		return;
	}
	ProcessPendingSceneUnloads();
	ActivatePendingScene();
	PrepareMenuInput(0.0f, false);
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			// Commit済みの候補は用意済みの初回描画値を散乱完了まで保持する。
			if (preparation_ && preparation_->status.commitSucceeded &&
				preparation_->status.token.instanceId == instance->GetId()) continue;
			scene->UpdateTransitionHold();
		}
	}
	if (BaseScene* activeScene = GetActiveScene()) {
		ParticleManager::GetInstance()->SetCamera(activeScene->GetRenderCamera());
	}
}

void SceneManager::ActivatePendingScene()
{
	if (!pendingSceneInstance_) {
		return;
	}
	if (pendingUiTransitionSource_.requestSerial && !IsUiTransitionSourceValid(pendingUiTransitionSource_)) {
		DiscardPendingScene("MenuRuntime:InvalidSource"); return;
	}
	// 予約後に実行状態/Activeが変わった場合も、編集用Documentを読み替えない。
	if (pendingRestartScene_ && (transitionHeld_ || presentedTransitionBusy_ ||
		(executionContext_ && !executionContext_->IsPlaying()) ||
		activeSceneInstanceId_ != pendingReloadSceneInstanceId_)) {
		DiscardPendingScene("SceneRestart:Rejected");
		return;
	}
	const SceneDescriptor* descriptor = sceneCatalog_
		? sceneCatalog_->Find(pendingSceneInstance_->GetSceneId())
		: nullptr;
	if ((pendingRestartScene_ || pendingUiTransitionSource_.requestSerial) && !descriptor) {
		DiscardPendingScene(pendingRestartScene_ ? "SceneRestart:InvalidScene" : "MenuRuntime:InvalidScene");
		return;
	}
	// Editor状態が予約後に変わっていても、異なるDocumentとの組み合わせを許可しない。
	if (
		pendingSceneLoadMode_ == SceneLoadMode::Single &&
		descriptor &&
		executionContext_ &&
		executionContext_->IsEditing() &&
		executionContext_->GetActiveSceneId() != descriptor->id
	) {
		Logger::Log(
			"Pending Scene does not match the active Edit SceneDocument: " +
			descriptor->id + "\n"
		);
		DiscardPendingScene(
			"Pending Scene does not match the active Edit SceneDocument: " + descriptor->id
		);
		return;
	}
	// Eventの同Scene遷移も保存状態へ戻す。Play開始/Editor Reloadでは編集内容を維持する。
	const bool readSavedRuntimeDocument =
		pendingSceneLoadMode_ == SceneLoadMode::Single && descriptor && executionContext_ &&
		!executionContext_->IsEditing() &&
		(pendingReadSavedScene_ || pendingRestartScene_ || pendingUiTransitionSource_.requestSerial ||
			executionContext_->GetActiveSceneId() != descriptor->id);
	// 読込成功後の旧cleanupが新Documentへ触れないよう、Contextの旧借用元を隔離する。
	SceneInstance* restartingInstance = nullptr;
	std::unique_ptr<SceneDocument> restartOldDocument;
	if (readSavedRuntimeDocument) {
		for (const auto& instance : sceneInstances_) {
			if (instance->GetDocument() == &executionContext_->GetActiveDocument()) {
				restartingInstance = instance.get();
				restartOldDocument = instance->GetDocument()->CloneForRuntimeDetach();
				break;
			}
		}
	}
	if (
		readSavedRuntimeDocument &&
		!executionContext_->LoadRuntimeScene(
			descriptor->id,
			descriptor->filePath
		)
	) {
		Logger::Log(
			"Runtime SceneDocument could not be loaded: " +
			descriptor->filePath + "\n"
		);
		DiscardPendingScene("Runtime SceneDocument could not be loaded: " + descriptor->filePath);
		return;
	}
	if (restartingInstance) restartingInstance->OwnDocument(std::move(restartOldDocument));
	if (pendingSceneLoadMode_ == SceneLoadMode::Additive) {
		if (!descriptor) {
			DiscardPendingScene("Additive Scene is not registered");
			return;
		}
		auto document = std::make_unique<SceneDocument>();
		if (!document->Load(descriptor->filePath)) {
			Logger::Log(
				"Additive SceneDocument could not be loaded: " +
				descriptor->filePath + "\n" + document->GetLastLoadError() + "\n"
			);
			DiscardPendingScene(
				"Additive SceneDocument could not be loaded: " + descriptor->filePath +
				": " + document->GetLastLoadError()
			);
			return;
		}
		document->MarkClean();
		pendingSceneInstance_->OwnDocument(std::move(document));
	} else if (executionContext_) {
		pendingSceneInstance_->BindDocument(
			&executionContext_->GetActiveDocument()
		);
	} else if (descriptor) {
		auto document = std::make_unique<SceneDocument>();
		if (!document->Load(descriptor->filePath)) {
			Logger::Log(
				"SceneDocument could not be loaded: " +
				descriptor->filePath + "\n" + document->GetLastLoadError() + "\n"
			);
			DiscardPendingScene(
				"SceneDocument could not be loaded: " + descriptor->filePath +
				": " + document->GetLastLoadError()
			);
			return;
		}
		document->MarkClean();
		pendingSceneInstance_->OwnDocument(std::move(document));
	}
	if (pendingSceneInstance_->IsPersistent()) {
		// Context所有Documentを次のSingleロードから隔離する。
		pendingSceneInstance_->SetPersistent(true);
	}

	if (
		pendingSceneLoadMode_ == SceneLoadMode::Single &&
		!sceneInstances_.empty()
	) {
		// 新Documentの検証に成功してから、現在Sceneの所有物を破棄する。
		const bool persistentInstancesRemain =
			UnloadNonPersistentSceneInstances(
				pendingReloadSceneInstanceId_,
				pendingForceUnloadAllSceneInstances_,
				true
			);

		if (!persistentInstancesRemain) {
			// Persistent Sceneがなければ、従来どおり共有演出も完全に切り替える。
			ParticleManager::GetInstance()->ClearActiveParticles();
			ParticleManager::GetInstance()->ClearGpuParticles();
		}
	}

	const SceneLoadMode loadMode = pendingSceneLoadMode_;
	const SceneInstanceId instanceId = pendingSceneInstance_->GetId();
	sceneInstances_.push_back(std::move(pendingSceneInstance_));
	if (
		loadMode == SceneLoadMode::Single ||
		activeSceneInstanceId_ == kInvalidSceneInstanceId
	) {
		SetActiveScene(instanceId);
	}

	sceneInstances_.back()->Initialize(this);
	lastLoadOutcome_ = SceneLoadOutcome{ instanceId, SceneLoadStatus::Activated, {} };
	pendingUiTransitionSource_ = {};
	pendingSceneLoadMode_ = SceneLoadMode::Single;
	pendingReloadSceneInstanceId_ = kInvalidSceneInstanceId;
	pendingForceUnloadAllSceneInstances_ = false;
	pendingRestartScene_ = false;
	pendingReadSavedScene_ = false;
}

void SceneManager::DiscardPendingScene(const std::string& diagnostic)
{
	pendingUiTransitionSource_ = {};
	if (pendingSceneInstance_) {
		lastLoadOutcome_ = SceneLoadOutcome{
			pendingSceneInstance_->GetId(), SceneLoadStatus::Failed, diagnostic
		};
	}
	pendingSceneInstance_.reset();
	pendingSceneLoadMode_ = SceneLoadMode::Single;
	pendingReloadSceneInstanceId_ = kInvalidSceneInstanceId;
	pendingForceUnloadAllSceneInstances_ = false;
	pendingRestartScene_ = false;
	pendingReadSavedScene_ = false;
}

void SceneManager::ProcessPendingSceneUnloads()
{
	for (SceneInstanceId instanceId : pendingUnloadSceneInstanceIds_) {
		auto instance = std::find_if(
			sceneInstances_.begin(),
			sceneInstances_.end(),
			[instanceId](const std::unique_ptr<SceneInstance>& candidate) {
				return candidate->GetId() == instanceId;
			}
		);
		if (instance == sceneInstances_.end()) {
			continue;
		}
		const std::string sceneId = (*instance)->GetSceneId();
		if ((*instance)->GetScene()) (*instance)->GetScene()->PrepareReplayForFinalize();
		(*instance)->Finalize();
		sceneInstances_.erase(instance);
		ReleaseSceneParticlesIfUnused(sceneId);
	}
	pendingUnloadSceneInstanceIds_.clear();

	if (!FindSceneInstance(activeSceneInstanceId_)) {
		if (sceneInstances_.empty()) {
			activeSceneInstanceId_ = kInvalidSceneInstanceId;
			currentSceneName_.clear();
		} else {
			SetActiveScene(sceneInstances_.front()->GetId());
		}
	}
}

void SceneManager::UpdatePaused()
{
	gameUpdatePrepared_ = false;
	PrepareMenuInput(0.0f, false);
	if (transitionHeld_) {
		return;
	}
	// Pause中は遷移予約も進めず、Resume後の通常Updateで反映する。
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			scene->UpdatePaused();
		}
	}
}

const SceneUIEditorView* SceneManager::GetEditorUIView() const {
	const auto* document = GetActiveSceneDocument();
	return executionContext_ && executionContext_->IsEditing() && document && editorUIView_.Matches(*document)
		? &editorUIView_ : nullptr;
}

void SceneManager::DrawEditorUIView(const char* renderSpace, uint32_t width, uint32_t height) {
	const auto* view = GetEditorUIView();
	auto* scene = GetActiveScene();
	if (!view || !scene || !renderSpace) return;
	// この呼出しの間だけ有効。描画packet保持と描画先の選択を分離する。
	struct ViewScope {
		bool& active;
		~ViewScope() { active = false; }
	} scope{drawingEditorUIView_};
	drawingEditorUIView_ = true;
	const std::string space(renderSpace);
	if (space == "Scene2D") {
		if (view->Isolated()) scene->DrawEditorUI(*view, renderSpace);
		else Draw();
	} else if (space == "Foreground") {
		if (!view->Isolated()) DrawForegroundEffects();
	} else if (space == "ScreenOverlay") {
		if (view->Isolated()) scene->DrawEditorUI(*view, renderSpace);
		else DrawScreenOverlay(width, height);
	}
}

void SceneManager::BeginRenderFrame(uint64_t serial) {
	if (auto* candidate = GetPreparedRenderScene()) {
		preparation_->lastRenderSerial = serial;
		candidate->BeginRenderFrame(serial);
		for (const auto& instance : sceneInstances_) {
			if (instance->IsPersistent()) instance->GetScene()->BeginRenderFrame(serial);
		}
		return;
	}
	// Commit直後のStopでEdit Sceneに戻っても、同じmainキューの次の完了提出で旧Sceneを退役できる。
	if (preparation_ && preparation_->status.phase == ScenePreparePhase::RetiringPrevious &&
		!preparation_->finalFrameRetired) {
		preparation_->lastRenderSerial = serial;
	}
	for (const auto& instance : sceneInstances_) {
		if (auto* scene = instance->GetScene()) scene->BeginRenderFrame(serial);
	}
}

void SceneManager::Draw(Camera* editorCamera)
{
	BaseScene* candidate = GetPreparedRenderScene();
	BaseScene* activeScene = candidate ? candidate : GetActiveScene();
	if (!activeScene) {
		return;
	}
	Camera* renderCamera = !candidate && editorCamera ? editorCamera : activeScene->GetRenderCamera();
	activeScene->DrawEnvironment(renderCamera);
	auto drawSceneContent = [activeScene, renderCamera](BaseScene* scene) {
		scene->PrepareSceneContent(renderCamera);
		// Prepareが設定したRoot SignatureへActive Lightingを再Bindする。
		activeScene->BindLighting();
		scene->DrawPreparedSceneContent(renderCamera);
	};
	drawSceneContent(activeScene);
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (candidate && !instance->IsPersistent()) continue;
		if (BaseScene* scene = instance->GetScene()) {
			if (scene != activeScene) {
				drawSceneContent(scene);
			}
		}
	}
}

void SceneManager::DrawForegroundEffects(Camera* editorCamera)
{
	BaseScene* candidate = GetPreparedRenderScene();
	BaseScene* activeScene = candidate ? candidate : GetActiveScene();
	if (!activeScene) {
		return;
	}
	Camera* renderCamera = !candidate && editorCamera ? editorCamera : activeScene->GetRenderCamera();
	activeScene->DrawForegroundEffectsWithCamera(renderCamera);
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (candidate && !instance->IsPersistent()) continue;
		if (BaseScene* scene = instance->GetScene()) {
			if (scene != activeScene) {
				scene->DrawForegroundEffectsWithCamera(renderCamera);
			}
		}
	}
}

bool SceneManager::HasScreenOverlay() const
{
	const auto* candidate = GetPreparedRenderScene();
	const auto* active = candidate ? candidate : GetActiveScene();
	if (active && (active->HasMenuOverlay() || (candidate && active->HasScreenOverlay()))) return true;
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (candidate && !instance->IsPersistent()) continue;
		if (const BaseScene* scene = instance->GetScene()) {
			if (scene->HasScreenOverlay()) {
				return true;
			}
		}
	}
	return false;
}

void SceneManager::DrawScreenOverlay(uint32_t width, uint32_t height)
{
	BaseScene* candidate = GetPreparedRenderScene();
	BaseScene* activeScene = candidate ? candidate : GetActiveScene();
	if (!activeScene) {
		return;
	}
	activeScene->DrawScreenOverlay(width, height);
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (candidate && !instance->IsPersistent()) continue;
		if (BaseScene* scene = instance->GetScene()) {
			if (scene != activeScene) {
				scene->DrawScreenOverlay(width, height);
			}
		}
	}
	// Additive HUDの後へActive Menuを重ね、後ろのSceneに覆われないようにする。
	activeScene->DrawMenuOverlay();
}

void SceneManager::DrawShadow()
{
	BaseScene* candidate = GetPreparedRenderScene();
	if (BaseScene* activeScene = candidate ? candidate : GetActiveScene()) {
		std::vector<Object3d*> shadowCasters;
		if (candidate) candidate->CollectShadowCasters(shadowCasters);
		for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
			if (candidate && !instance->IsPersistent()) continue;
			if (BaseScene* scene = instance->GetScene()) {
				scene->CollectShadowCasters(shadowCasters);
			}
		}
		// Shadow設定とMapはActive Scene、Casterは全Loaded Sceneから集約する。
		activeScene->RenderShadowCasters(shadowCasters);
	}
}

void SceneManager::SetDeferForegroundEffects(bool defer)
{
	if (auto* candidate = GetPreparedRenderScene()) candidate->SetDeferForegroundEffects(defer);
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			scene->SetDeferForegroundEffects(defer);
		}
	}
}

void SceneManager::DrawOffscreenViews()
{
	BaseScene* candidate = GetPreparedRenderScene();
	if (candidate) candidate->DrawOffscreenViews();
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (candidate && !instance->IsPersistent()) continue;
		if (BaseScene* scene = instance->GetScene()) {
			scene->DrawOffscreenViews();
		}
	}
}

bool SceneManager::UnloadNonPersistentSceneInstances(
	SceneInstanceId forcedUnloadInstanceId,
	bool forceUnloadAll,
	bool prepareForSceneTransition
)
{
	std::vector<std::string> unloadedSceneIds;
	for (auto instance = sceneInstances_.begin();
		instance != sceneInstances_.end();) {
		if (
			!forceUnloadAll &&
			(*instance)->IsPersistent() &&
			(*instance)->GetId() != forcedUnloadInstanceId
		) {
			++instance;
			continue;
		}
		const std::string sceneId = (*instance)->GetSceneId();
		if (prepareForSceneTransition) {
			if (BaseScene* scene = (*instance)->GetScene()) {
				scene->PrepareForSceneTransition();
			}
		}
		if ((*instance)->GetScene()) (*instance)->GetScene()->PrepareReplayForFinalize();
		(*instance)->Finalize();
		instance = sceneInstances_.erase(instance);
		if (
			std::find(
				unloadedSceneIds.begin(),
				unloadedSceneIds.end(),
				sceneId
			) == unloadedSceneIds.end()
		) {
			unloadedSceneIds.push_back(sceneId);
		}
	}
	for (const std::string& sceneId : unloadedSceneIds) {
		ReleaseSceneParticlesIfUnused(sceneId);
	}
	return !sceneInstances_.empty();
}

void SceneManager::ReleaseSceneParticlesIfUnused(const std::string& sceneId)
{
	const bool sameSceneStillLoaded = std::any_of(
		sceneInstances_.begin(),
		sceneInstances_.end(),
		[&sceneId](const std::unique_ptr<SceneInstance>& candidate) {
			return candidate->GetSceneId() == sceneId;
		}
	);
	if (!sameSceneStillLoaded) {
		ParticleManager::GetInstance()->ReleaseSceneParticles(sceneId);
	}
}

void SceneManager::UnloadAllSceneInstances()
{
	gameExitSource_ = kInvalidSceneInstanceId; gameExitGeneration_ = 0;
	gameUpdatePrepared_ = gamePaused_ = pauseBgm_ = false;
	pendingUiTransitionSource_ = {}; presentedUiTransitionSource_ = {};
	uiTransitionOutcome_.reset();
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (instance->GetScene()) instance->GetScene()->PrepareReplayForFinalize();
		instance->Finalize();
	}
	sceneInstances_.clear();
	pendingUnloadSceneInstanceIds_.clear();
	activeSceneInstanceId_ = kInvalidSceneInstanceId;
	currentSceneName_.clear();
}

SceneManager::~SceneManager()
{
	CancelDocumentRead();
	documentReadJob_.reset(); // 終了確定後だけ進行中workerをjoinし、Scene/Context破棄より先に回収。
	preparation_.reset();
	UnloadAllSceneInstances();
	DiscardPendingScene();
}

void SceneManager::CaptureReplayBeforeDraw(const SceneReplayCaptureRequest& request) {
	if (executionContext_ && executionContext_->IsEditing()) return;
	const auto* instance = GetActiveSceneInstance();
	if (!instance || !instance->GetScene() || !instance->GetDocument() ||
		(executionContext_ && instance->GetDocument() != &executionContext_->GetActiveDocument())) return;
	instance->GetScene()->CaptureReplayBeforeDraw(request);
}
bool SceneManager::TryGetReplayHistoryStatus(const SceneDocument& document, uint64_t id, SceneReplayHistoryStatus& out) const {
	const auto* instance = GetActiveSceneInstance();
	if (!instance || instance->GetDocument() != &document || !instance->GetScene()) return false;
	SceneReplayHistoryStatus value;
	if (!instance->GetScene()->TryGetReplayHistoryStatus(id, value)) return false;
	out = std::move(value); return true;
}

bool SceneManager::TryGetDeathReplayStatus(const SceneDocument& document, uint64_t id, SceneDeathReplayStatus& out) const {
	const auto* instance = GetActiveSceneInstance();
	if (!instance || instance->GetDocument() != &document || !instance->GetScene()) return false;
	SceneDeathReplayStatus value;
	if (!instance->GetScene()->TryGetDeathReplayStatus(id, value)) return false;
	out = value;
	return true;
}

int SceneManager::GetDeathResponseStatus(const SceneDocument& document, uint64_t id) const {
	const auto* instance = GetActiveSceneInstance();
	if (!instance || instance->GetDocument() != &document || !instance->GetScene()) return -1;
	return instance->GetScene()->GetDeathResponseStatus(id);
}

void SceneManager::NotifyReplayGpuCompleted() {
    for (const auto& instance : sceneInstances_)
        if (instance->GetScene()) instance->GetScene()->NotifyReplayGpuCompleted();
}

void SceneManager::AdvanceReplayPresentation(float delta, bool allowed) {
    const auto* instance=GetActiveSceneInstance();
    if (instance && instance->GetScene()) instance->GetScene()->AdvanceReplayPresentation(delta,
        allowed && !transitionHeld_ && !(executionContext_ && !executionContext_->IsPlaying()));
}
bool SceneManager::PrepareReplayGameFrame(uint64_t serial, float aspect, SceneReplayGameFrame& out) {
    const auto* instance=GetActiveSceneInstance();
    if (!instance || !instance->GetScene() || (executionContext_ && executionContext_->IsEditing())) return false;
    return instance->GetScene()->PrepareReplayGameFrame(serial,aspect,out);
}
bool SceneManager::DrawReplayGameShadow(const SceneReplayPresentationToken& token) {
    const auto* instance=GetActiveSceneInstance();
    return instance && GetActiveSceneInstanceId()==token.instance && instance->GetScene() && instance->GetScene()->DrawReplayGameShadow(token);
}
bool SceneManager::DrawReplayGameWorld(const SceneReplayPresentationToken& token) {
    const auto* instance=GetActiveSceneInstance();
    return instance && GetActiveSceneInstanceId()==token.instance && instance->GetScene() && instance->GetScene()->DrawReplayGameWorld(token);
}
bool SceneManager::DrawReplayMenu(const SceneReplayPresentationToken& token, SceneMenuSource& source) {
    auto* scene=GetActiveScene();
    return scene && GetActiveSceneInstanceId()==token.instance && scene->DrawReplayMenu(token,source);
}
void SceneManager::AcceptReplayMenuReceipt(const SceneReplayPresentationToken& token, const SceneMenuSource& source) {
    auto* scene=GetActiveScene();
    if (scene && GetActiveSceneInstanceId()==token.instance) scene->AcceptReplayMenuReceipt(token,source);
}
void SceneManager::AcceptReplayPresentationReceipt(const SceneReplayPresentationToken& token) {
    const auto* instance=GetActiveSceneInstance();
    if (instance && GetActiveSceneInstanceId()==token.instance && instance->GetScene()) instance->GetScene()->AcceptReplayPresentationReceipt(token);
}

bool SceneManager::TryGetReplayFraming(uint64_t owner, SceneReplayFramingBasis& out) const {
    const auto* instance=GetActiveSceneInstance();
    return executionContext_ && executionContext_->IsEditing() && instance && instance->GetScene() &&
        instance->GetScene()->TryGetReplayFraming(owner,out);
}

void SceneManager::RejectReplayGameFrame(const SceneReplayPresentationToken& token) {
    const auto* instance=GetActiveSceneInstance();
    if (instance && GetActiveSceneInstanceId()==token.instance && instance->GetScene()) instance->GetScene()->RejectReplayGameFrame(token);
}
