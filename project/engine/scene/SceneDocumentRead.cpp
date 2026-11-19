// 役割: Sceneの実ファイルIOとJSON解析だけを行い、意味処理から分離する。
#include "SceneDocumentRead.h"
#include "../utility/StringUtility.h"
#include <fstream>
#include <exception>

struct SceneDocumentRead::Data {
	std::string path;
	nlohmann::json root;
};
SceneDocumentRead::SceneDocumentRead() : data_(std::make_unique<Data>()) {}
SceneDocumentRead::~SceneDocumentRead() = default;
const std::string& SceneDocumentRead::SourcePath() const noexcept { return data_->path; }
nlohmann::json SceneDocumentRead::CopyRoot() const { return data_->root; }

bool TryReadSceneDocument(const std::string& path,
	std::unique_ptr<SceneDocumentRead>& out, std::string& diagnostic) {
	bool invalidJson = false;
	return SceneDocumentRead::Read(path, out, diagnostic, invalidJson);
}

bool SceneDocumentRead::Read(const std::string& path,
	std::unique_ptr<SceneDocumentRead>& out, std::string& diagnostic, bool& invalidJson) {
	invalidJson = false;
	try {
		std::ifstream input(StringUtility::ToPath(path), std::ios::binary);
		if (path.empty() || !input.is_open()) {
			diagnostic = "Scene file could not be opened: " + path;
			return false;
		}
		auto candidate = std::unique_ptr<SceneDocumentRead>(new SceneDocumentRead());
		candidate->data_->path = path;
		candidate->data_->root = nlohmann::json::parse(input);
		diagnostic.clear();
		out.swap(candidate);
		return true;
	} catch (const nlohmann::json::exception& exception) {
		invalidJson = true;
		diagnostic = "Scene JSON is invalid: ";
		diagnostic += exception.what();
		return false;
	} catch (const std::exception& exception) {
		diagnostic = "Scene JSON is invalid: ";
		diagnostic += exception.what();
		return false;
	}
}
