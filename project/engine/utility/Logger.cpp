// 役割: ログファイルの初期化とメッセージ出力を実装する。
#include "Logger.h"
#include <Windows.h>
#include "StringUtility.h"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <format>
#include <cassert>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>

namespace {
	std::ofstream gLogStream;

	// TEMP: 先行COPYの比較計測終了後に、発行箇所・補助ファイルと合わせて撤去する。
	class UploadTraceSink {
	public:
		~UploadTraceSink() { Stop(); }

		void Start(std::string path) noexcept {
			try {
				worker_ = std::jthread([this, path = std::move(path)](std::stop_token stop) {
					try { Write(path, stop); }
					catch (...) { writeFailed_.store(true, std::memory_order_release); }
				});
				started_ = true;
			} catch (...) {
				writeFailed_.store(true, std::memory_order_release);
			}
		}

		bool Enqueue(std::string_view line) noexcept {
			const auto head = head_.load(std::memory_order_relaxed);
			const auto used = head - tail_.load(std::memory_order_acquire);
			if (!started_ || writeFailed_.load(std::memory_order_acquire) || used >= slots_.size() ||
				line.empty() || line.size() > kLineBytes || line.back() != '\n') {
				Drop();
				return false;
			}
			auto& slot = slots_[head % slots_.size()];
			std::memcpy(slot.bytes.data(), line.data(), line.size());
			slot.size = line.size();
			if (used + 1 > highWater_.load(std::memory_order_relaxed))
				highWater_.store(used + 1, std::memory_order_relaxed);
			head_.store(head + 1, std::memory_order_release);
			return true;
		}

		void Drop() noexcept { dropped_.fetch_add(1, std::memory_order_relaxed); }
		uint64_t DroppedTotal() const noexcept { return dropped_.load(std::memory_order_relaxed); }

		bool Healthy() const noexcept {
			return started_ && !WriteFailed();
		}

		bool WriteFailed() const noexcept { return writeFailed_.load(std::memory_order_acquire); }
		void MarkWriteFailed() noexcept { writeFailed_.store(true, std::memory_order_release); }

		void Stop() noexcept {
			started_ = false;
			if (worker_.joinable()) {
				worker_.request_stop();
				worker_.join();
			}
		}

	private:
		static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);
		static constexpr size_t kLineBytes = 2048;
		struct Slot {
			std::array<char, kLineBytes> bytes{};
			size_t size = 0;
		};

		void Write(const std::string& path, std::stop_token stop) {
			using namespace std::chrono_literals;
			using Clock = std::chrono::steady_clock;
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			constexpr std::string_view header = "[UploadTraceSink] version=1 state=started\n";
			stream.write(header.data(), header.size());
			stream.flush();
			uint64_t reportedDrops = 0;
			bool dirty = false;
			auto lastFlush = Clock::now();

			const auto checkpoint = [&](bool final) {
				const auto drops = dropped_.load(std::memory_order_relaxed);
				const auto line = std::format(
					"[UploadTraceSink] checkpoint=true final={} dropped_total={} high_water={} complete=true\n",
					final, drops, highWater_.load(std::memory_order_relaxed));
				stream.write(line.data(), line.size());
				stream.flush();
				reportedDrops = drops;
				dirty = false;
				lastFlush = Clock::now();
			};

			while (stream) {
				const bool stopping = stop.stop_requested();
				// バッチの終端を固定。末尾のcheckpointが、解析側で読める範囲を示す。
				const auto end = head_.load(std::memory_order_acquire);
				bool hasTraceEnd = false;
				for (auto tail = tail_.load(std::memory_order_relaxed); tail != end && stream; ) {
					const auto& slot = slots_[tail % slots_.size()];
					const std::string_view line(slot.bytes.data(), slot.size);
					stream.write(line.data(), line.size());
					dirty = true;
					hasTraceEnd |= line.starts_with("[UploadTrace] ") &&
						line.find(" event=end ") != std::string_view::npos;
					tail_.store(++tail, std::memory_order_release);
				}
				if (!stream) break;
				if (stopping) {
					checkpoint(true);
					break;
				}
				if (hasTraceEnd || ((dirty || reportedDrops != DroppedTotal()) &&
					Clock::now() - lastFlush >= 100ms)) {
					checkpoint(false);
				}
				std::this_thread::sleep_for(20ms);
			}
			stream.close();
			if (!stream) writeFailed_.store(true, std::memory_order_release);
		}

		// mainだけがheadを進め、writerだけがtailを進める。joinは終了時だけ。
		std::array<Slot, 1024> slots_{};
		std::atomic<uint64_t> head_{0}, tail_{0}, dropped_{0}, highWater_{0};
		std::atomic<bool> writeFailed_{false};
		bool started_ = false;
		std::jthread worker_;
	};
	UploadTraceSink gUploadTrace;
}

namespace Logger {

	void Initialize() {
		std::filesystem::create_directories("Logs");

		// 現在時刻を取得
		std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
		std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds>
			nowSeconds = std::chrono::time_point_cast<std::chrono::seconds>(now);

		// ローカル時間へ変換
		std::chrono::zoned_time localTime{ std::chrono::current_zone(), nowSeconds };

		// ファイル名生成
		std::string dateString = std::format("{:%Y%m%d_%H%M%S}", localTime);
		std::string logFilePath = std::string("Logs/") + dateString + ".log";

		// ファイルオープン
		gLogStream.open(logFilePath, std::ios::out);
		assert(gLogStream.is_open());

		try {
			const auto traceFile = dateString + ".upload-trace.log";
			Log("[UploadTraceSink] sidecar=" + traceFile + "\n");
			gUploadTrace.Start("Logs/" + traceFile);
		} catch (...) {
			gUploadTrace.MarkWriteFailed();
		}
	}

	void Log(const std::string& message) {
		OutputDebugStringA(message.c_str());

		if (gLogStream.is_open()) {
			gLogStream << message;
			gLogStream.flush();
		}
	}

	void Log(const std::wstring& message) {
		OutputDebugStringW(message.c_str());

		if (gLogStream.is_open()) {
			gLogStream << StringUtility::ConvertString(message);
			gLogStream.flush();
		}
	}

	void Finalize() {
		gUploadTrace.Stop();
		if (gUploadTrace.WriteFailed()) Log("[UploadTraceSink] status=write_failed\n");
		if (gLogStream.is_open()) {
			gLogStream.close();
		}
	}

	bool TryEnqueueUploadTrace(std::string_view line) noexcept { return gUploadTrace.Enqueue(line); }
	void NoteUploadTraceDrop() noexcept { gUploadTrace.Drop(); }
	std::uint64_t UploadTraceDroppedTotal() noexcept { return gUploadTrace.DroppedTotal(); }
	bool IsUploadTraceHealthy() noexcept { return gUploadTrace.Healthy(); }
}
