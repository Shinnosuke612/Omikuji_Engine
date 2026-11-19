// 役割: SceneDocumentの構造とCatalogをまたぐ参照を一括検証する。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class SceneCatalog;
class SceneDocument;
struct SceneUpgradePoolEntry;

enum class SceneValidationSeverity {
	Warning,
	Error
};

struct SceneValidationIssue {
	SceneValidationSeverity severity = SceneValidationSeverity::Error;
	std::string sceneId;
	std::string filePath;
	uint64_t entityId = 0;
	std::string message;
};

class SceneValidator final {
public:
	// Editorと選択Runtimeが同じ参照/Writer/候補pool制約を使用。空文字は正常。
	static std::string UpgradeSelectionIssue(const SceneDocument& document, uint64_t panelId,
		const std::vector<SceneUpgradePoolEntry>* pool = nullptr);
	static bool ValidateDocument(
		const SceneDocument& document,
		const SceneCatalog* catalog,
		const std::string& sceneId,
		const std::string& filePath,
		std::vector<SceneValidationIssue>& issues
	);
	static bool ValidateCatalog(
		const SceneCatalog& catalog,
		std::vector<SceneValidationIssue>& issues
	);
	static std::string FormatIssues(
		const std::vector<SceneValidationIssue>& issues,
		size_t maxIssueCount = 12
	);
};
