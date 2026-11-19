// 役割: 単一SceneファイルのIO/JSON解析結果を専有する不透明候補を提供する。
#pragma once
#include <memory>
#include <string>
#include "../../externals/nlohmann/json.hpp"

class SceneDocument;
class SceneDocumentRead final {
public:
	~SceneDocumentRead();
	// 候補の寿命内で借用。移動/破棄後に参照を保持しない。
	const std::string& SourcePath() const noexcept;
	SceneDocumentRead(const SceneDocumentRead&) = delete;
	SceneDocumentRead& operator=(const SceneDocumentRead&) = delete;
private:
	struct Data;
	std::unique_ptr<Data> data_;
	SceneDocumentRead();
	// 意味適用時のmigration用コピー。JSONを公開APIには出さない。
	nlohmann::json CopyRoot() const;
	// 同期Loadが従来のJSON例外時cleanupを維持するための内部分類。
	static bool Read(const std::string&, std::unique_ptr<SceneDocumentRead>&, std::string&, bool& invalidJson);
	friend class SceneDocument;
	friend bool TryReadSceneDocument(const std::string&, std::unique_ptr<SceneDocumentRead>&, std::string&);
};

// CPU専用、単一ファイル。Registry/世代/Contextへ触れず、成功時だけoutを交換する。
// 同期callerは相対path可。worker callerはmainで絶対pathを解決して渡す。
// 標準例外を診断へ変換するが診断確保失敗/nonstd例外は伝播し得る。
bool TryReadSceneDocument(const std::string& resolvedFilePath,
	std::unique_ptr<SceneDocumentRead>& out, std::string& diagnostic);
