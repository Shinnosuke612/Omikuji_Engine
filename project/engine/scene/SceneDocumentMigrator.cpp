// 役割: 旧Scene JSONへ現在形式の必須コンテナとEntity既定値を補う。
#include "SceneDocumentMigrator.h"

#include "../math/Quaternion.h"

using json = nlohmann::json;

bool SceneDocumentMigrator::Migrate(
	json& root,
	bool& migrated,
	std::string& errorMessage
) {
	migrated = false;
	if (!root.is_object()) {
		errorMessage = "Scene JSON root must be an object";
		return false;
	}

	int sourceVersion = 1;
	if (root.contains("version")) {
		if (!root.at("version").is_number_integer()) {
			errorMessage = "Scene version must be an integer";
			return false;
		}
		sourceVersion = root.at("version").get<int>();
	}
	if (sourceVersion <= 0) {
		errorMessage = "Scene version must be greater than zero";
		return false;
	}
	if (sourceVersion > kCurrentVersion) {
		errorMessage =
			"Scene version " + std::to_string(sourceVersion) +
			" is newer than supported version " +
			std::to_string(kCurrentVersion);
		return false;
	}
	if (sourceVersion == kCurrentVersion) {
		errorMessage.clear();
		return true;
	}

	if (!MigrateLegacyDocument(root, errorMessage)) {
		return false;
	}
	root["version"] = kCurrentVersion;
	migrated = true;
	errorMessage.clear();
	return true;
}

bool SceneDocumentMigrator::MigrateLegacyDocument(
	json& root,
	std::string& errorMessage
) {
	if (!root.contains("entities") || !root.at("entities").is_array()) {
		errorMessage = "Legacy Scene must contain an entities array";
		return false;
	}
	if (!root.contains("postProcess")) {
		root["postProcess"] = json::object();
	}
	if (!root.contains("debug")) {
		root["debug"] = json::object();
	}
	if (!root.contains("lighting")) {
		root["lighting"] = {
			{ "shadowMapSize", 4096 }
		};
	}
	if (!root.contains("teams")) {
		root["teams"] = json::array();
	}

	for (json& entity : root["entities"]) {
		if (!entity.is_object()) {
			errorMessage = "Legacy Scene contains an invalid Entity entry";
			return false;
		}
		if (!entity.contains("parentId")) {
			entity["parentId"] = 0;
		}
		if (!entity.contains("name")) {
			entity["name"] = "Entity";
		}
		if (!entity.contains("folder")) {
			entity["folder"] = false;
		}
		if (!entity.contains("folderTeamEnabled")) {
			entity["folderTeamEnabled"] = false;
		}
		if (!entity.contains("active")) {
			entity["active"] = true;
		}
		if (!entity.contains("locked")) {
			entity["locked"] = false;
		}
		if (!entity.contains("team")) {
			entity["team"] = "";
		}
		if (!entity.contains("transform")) {
			entity["transform"] = {
				{ "scale", { 1.0f, 1.0f, 1.0f } },
				{ "rotate", { 0.0f, 0.0f, 0.0f } },
				{ "translate", { 0.0f, 0.0f, 0.0f } }
			};
		}
		json& transform = entity["transform"];
		if (
			transform.is_object() &&
			!transform.contains("rotation") &&
			transform.contains("rotate") &&
			transform.at("rotate").is_array() &&
			transform.at("rotate").size() == 3
		) {
			const json& rotate = transform.at("rotate");
			const Quaternion quaternion = MakeQuaternionFromEuler({
				rotate[0].get<float>(),
				rotate[1].get<float>(),
				rotate[2].get<float>()
			});
			transform["rotation"] = {
				quaternion.x,
				quaternion.y,
				quaternion.z,
				quaternion.w
			};
			transform.erase("rotate");
		}
		if (!entity.contains("sprite")) {
			entity["sprite"] = json::object();
		}
		if (!entity.contains("components")) {
			entity["components"] = json::array();
		}
		if (!entity.at("components").is_array()) {
			errorMessage = "Legacy Entity components must be an array";
			return false;
		}
		for (json& component : entity["components"]) {
			if (component.is_string()) {
				component = {
					{ "type", component.get<std::string>() },
					{ "enabled", true }
				};
			}
			// 旧Enemyは明示切替まで従来動作。新方式の設定値を制作データへ自動投入しない。
			if (component.is_object() && component.value("type", std::string{}) == "EnemyBehavior" &&
				!component.contains("movementMode")) component["movementMode"] = "Legacy";
		}
	}
	errorMessage.clear();
	return true;
}
