// 役割: アプリケーションのメッセージをログファイルへ出力する。
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// ログ出力
namespace Logger
{
	// ログファイルの初期化
	void Initialize();

	// ログ出力
	void Log(const std::string& message);
	void Log(const std::wstring& message);

	// 比較ログ専用。main限定で一行を値コピーし、満杯/停止時は欠測として記録する。
	bool TryEnqueueUploadTrace(std::string_view line) noexcept;
	void NoteUploadTraceDrop() noexcept;
	std::uint64_t UploadTraceDroppedTotal() noexcept;
	bool IsUploadTraceHealthy() noexcept;

	// ログファイルの終了処理
	void Finalize();
}
