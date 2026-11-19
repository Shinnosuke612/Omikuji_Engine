// 役割: Scene画像の値入力と同期CPU準備の契約を定義する。
#pragma once

#include "TextureManager.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>

enum class SceneTextureCpuStatus { Prepared, Failed, UnsupportedRecipe, ReservationExceeded, Cancelled };

// パス/画像bytesは値所有。呼び出し中の入力は変更・消費しない。
struct SceneTextureCpuSource {
	enum class Recipe { OrdinaryFile, OrdinaryMemory, ModelGrayscaleBaseColor, HdrEnvironment };
	Recipe recipe = Recipe::OrdinaryFile;
	std::filesystem::path resolvedPath;
	std::vector<uint8_t> bytes;
	std::string formatHint;
	TextureManager::TextureColorSpace colorSpace = TextureManager::TextureColorSpace::Automatic;
};

struct SceneTextureCpuResult {
	TextureManager::DecodedTexture texture;
	bool mipFallbackUsed = false;
	// GPU要求時もtextureは実CPU payloadのmip1。下位段の完成はDIRECT receiptが保証する。
	SceneTextureMipPolicy mipPolicy = SceneTextureMipPolicy::CpuComplete;
};

// TEMP: ロード改善の比較が済んだら撤去するCPU専用計測。bytesはScratchImageの全subresource合計。
// 未観測はobserved=false。HDRのdecode直後は既存一体APIの外から観測できない。
struct SceneTextureCpuImageSize {
	bool observed = false;
	uint64_t bytes = 0, width = 0, height = 0, depth = 0, arraySize = 0, mipLevels = 0;
};
struct SceneTextureCpuTiming {
	int64_t decodeUs = 0, mipUs = 0, hdrCombinedUs = 0;
	std::chrono::steady_clock::time_point decodeStarted, decodeEnded, mipStarted, mipEnded, hdrStarted, hdrEnded;
	const char* decodeState = "not_started";
	const char* mipState = "not_started";
	SceneTextureCpuImageSize decoded, final;
	bool gpuEligible = false;
	const char* gpuDecision = "not_evaluated";
};

// Job完了まで借用。decodedBytes=0は検査なし。finalBytes=0ならdecodedBytesを最終上限にも使う。
// finalBytes指定時は見積もり済みPNG/JPEGのwidth/height/配列/mip数と生成先上界をmip確保前にも照合する。
// 取消はCPU段階間で観測し、実行中のdecoder自体を中断しない。worker内で予算を待たない。
struct SceneTextureCpuLimits {
	uint64_t width = 0, height = 0, decodedBytes = 0, finalBytes = 0;
	const std::atomic<bool>* cancelled = nullptr;
	bool allowGpuMips = false, forceCpuMips = false;
};

// Fileは非空の絶対pathと空bytes、Memoryは空pathと非空bytesを要求する。
// formatHintはMemoryだけで使用し、空なら既存decodeのWIC選択を維持する。
// 呼出元がパス解決と実行threadのWIC用COM初期化を担当する。GPU/cache/worker操作なし。
// 成功時だけoutを置換。mip失敗は元画像でPreparedを返し、fallbackフラグを立てる。
// 灰色recipeは空path/非空bytes、HDR環境は絶対path/空bytes。どちらもAutomatic限定。
// 灰色は選別済みbase color用。formatHintを無視し、必要なら元PNGを再decodeする。
// HDR環境は変換済みmipを保持し、失敗を元画像で代用しない（fallbackフラグfalse）。
// 未知enum/無効入力はFailed。UnsupportedRecipe値は既存API互換のため保持する。
// diagnosticは今回結果に更新（内部診断用）。確保例外は伝播し、outは維持する。
// PreparedはCPU準備成功だけを示し、COPY対応formatやScene完了を保証しない。
// timingは任意の借用出力で、失敗/例外時も試行済みの区間だけ更新する。outの成功時置換は不変。
// 時間はsteady_clockのmicroseconds。HDR合算はdecode/mipの各時間へ重複計上しない。
// limitsの予約超過はReservationExceeded、段階間の取消はCancelled。どちらもoutを保持する。
SceneTextureCpuStatus TryPrepareSceneTexture(
	const SceneTextureCpuSource& source, SceneTextureCpuResult& out, std::string& diagnostic,
	SceneTextureCpuTiming* timing = nullptr, const SceneTextureCpuLimits* limits = nullptr);
