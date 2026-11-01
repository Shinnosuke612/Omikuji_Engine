// 役割: 旧Scene JSONを現在のDocument形式へ段階的に更新する。
#pragma once

#include <string>

#include "../../externals/nlohmann/json.hpp"

class SceneDocumentMigrator final {
public:
	static constexpr int kCurrentVersion = 37;

	static bool Migrate(
		nlohmann::json& root,
		bool& migrated,
		std::string& errorMessage
	);

private:
	static bool MigrateLegacyDocument(
		nlohmann::json& root,
		std::string& errorMessage
	);
};
