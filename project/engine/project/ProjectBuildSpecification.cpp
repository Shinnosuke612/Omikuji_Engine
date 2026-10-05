// 役割: Game Projectのビルド仕様の保存先を解決し、JSONの形式と対応する設定値を検証する。
#include "ProjectBuildSpecification.h"

#include "ProjectDescriptor.h"
#include "../../externals/nlohmann/json.hpp"

#include <fstream>
#include <initializer_list>
#include <iterator>
#include <set>

namespace {
	using json = nlohmann::json;

	bool ReadUtf8File(const std::filesystem::path& path, std::string& output) {
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open()) {
			return false;
		}
		output.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		return !input.bad();
	}

	bool HasOnlyFields(const json& object, std::initializer_list<const char*> fields) {
		if (!object.is_object()) {
			return false;
		}
		const std::set<std::string> allowed(fields.begin(), fields.end());
		for (const auto& entry : object.items()) {
			if (allowed.find(entry.key()) == allowed.end()) {
				return false;
			}
		}
		return true;
	}

	bool ReadRequiredString(const json& object, const char* key, std::string& output) {
		if (!object.contains(key) || !object.at(key).is_string()) {
			return false;
		}
		output = object.at(key).get<std::string>();
		return true;
	}

	bool ReadStringArray(const json& object, const char* key, std::vector<std::string>& output) {
		if (!object.contains(key) || !object.at(key).is_array()) {
			return false;
		}
		output.clear();
		for (const auto& value : object.at(key)) {
			if (!value.is_string()) {
				return false;
			}
			output.push_back(value.get<std::string>());
		}
		return true;
	}

	bool ReadSourceSet(const json& object, const char* name, ProjectBuildSourceSet& output, bool rootRequired) {
		if (!object.contains(name) || !object.at(name).is_object()) {
			return false;
		}
		const json& sourceSet = object.at(name);
		if (rootRequired) {
			if (!HasOnlyFields(sourceSet, { "include", "root" }) ||
				!ReadRequiredString(sourceSet, "root", output.root) ||
				!ReadStringArray(sourceSet, "include", output.include)) {
				return false;
			}
			output.files.clear();
		} else {
			if (!HasOnlyFields(sourceSet, { "files" }) ||
				!ReadStringArray(sourceSet, "files", output.files)) {
				return false;
			}
			output.root.clear();
			output.include.clear();
		}
		output.name = name;
		return true;
	}
}

bool ProjectBuildSpecification::ResolvePath(const std::filesystem::path& projectRoot, std::filesystem::path& specificationPath, std::string& errorMessage) {
	specificationPath.clear();
	errorMessage.clear();
	std::error_code error;
	const std::filesystem::path root = std::filesystem::absolute(projectRoot, error).lexically_normal();
	if (error) {
		errorMessage = "Build specification Project root could not be resolved: " + error.message();
		return false;
	}
	const std::filesystem::path currentPath = root / L"config" / L"project.build.json";
	const std::filesystem::path legacyPath = root / L"project" / L"build" / L"project.build.json";
	const bool currentExists = std::filesystem::exists(currentPath, error);
	if (error) {
		errorMessage = "Current build specification path could not be inspected: " + error.message();
		return false;
	}
	const bool legacyExists = std::filesystem::exists(legacyPath, error);
	if (error) {
		errorMessage = "Legacy build specification path could not be inspected: " + error.message();
		return false;
	}
	if (currentExists && legacyExists) {
		errorMessage = "Build specification is ambiguous: both config/project.build.json and project/build/project.build.json exist.";
		return false;
	}
	const std::filesystem::path selected = currentExists ? currentPath : legacyPath;
	if (currentExists || legacyExists) {
		if (!std::filesystem::is_regular_file(selected, error) || error) {
			errorMessage = "Build specification path is not a readable regular file.";
			return false;
		}
		std::string content;
		if (!ReadUtf8File(selected, content)) {
			errorMessage = "Build specification could not be read.";
			return false;
		}
	}
	specificationPath = selected;
	return true;
}

bool ProjectBuildSpecification::Load(const std::filesystem::path& specificationPath, std::string& errorMessage) {
	path_ = std::filesystem::absolute(specificationPath).lexically_normal();
	std::string text;
	if (!ReadUtf8File(path_, text)) {
		errorMessage = "Build specification could not be read.";
		return false;
	}
	try {
		const json root = json::parse(text);
		if (!HasOnlyFields(root, { "configurations", "externals", "platform", "runtime", "schemaVersion", "sourceSets", "toolchain" }) ||
			!root.contains("schemaVersion") || !root.at("schemaVersion").is_number_integer() ||
			root.at("schemaVersion").get<int>() != kSchemaVersion ||
			!ReadStringArray(root, "configurations", configurations_) ||
			!ReadRequiredString(root, "platform", toolchain_.platform) ||
			!root.contains("toolchain") || !HasOnlyFields(root.at("toolchain"), { "platformToolset", "windowsSdk" }) ||
			!ReadRequiredString(root.at("toolchain"), "platformToolset", toolchain_.platformToolset) ||
			!ReadRequiredString(root.at("toolchain"), "windowsSdk", toolchain_.windowsSdk) ||
			!root.contains("externals") || !HasOnlyFields(root.at("externals"), { "assimp", "directXTexProject", "imguiProject" }) ||
			!ReadRequiredString(root.at("externals"), "assimp", externals_.assimp) ||
			!ReadRequiredString(root.at("externals"), "directXTexProject", externals_.directXTexProject) ||
			!ReadRequiredString(root.at("externals"), "imguiProject", externals_.imguiProject) ||
			!root.contains("runtime") || !HasOnlyFields(root.at("runtime"), { "copyResources", "resourceRoot" }) ||
			!root.at("runtime").contains("copyResources") || !root.at("runtime").at("copyResources").is_boolean() ||
			!ReadRequiredString(root.at("runtime"), "resourceRoot", runtime_.resourceRoot) ||
			!root.contains("sourceSets") || !HasOnlyFields(root.at("sourceSets"), { "engine", "entry", "game" }) ||
			!ReadSourceSet(root.at("sourceSets"), "engine", engine_, true) ||
			!ReadSourceSet(root.at("sourceSets"), "game", game_, true) ||
			!ReadSourceSet(root.at("sourceSets"), "entry", entry_, false)) {
			errorMessage = "Unsupported or invalid build specification schema.";
			return false;
		}
		runtime_.copyResources = root.at("runtime").at("copyResources").get<bool>();
		schemaVersion_ = kSchemaVersion;
		return Validate(errorMessage);
	} catch (const json::exception&) {
		errorMessage = "Build specification JSON could not be parsed.";
		return false;
	}
}

bool ProjectBuildSpecification::Validate(std::string& errorMessage) const {
	if (path_.empty() || schemaVersion_ != kSchemaVersion ||
		configurations_ != std::vector<std::string>{ "Debug", "Development", "Release" } ||
		toolchain_.platform != "x64" || toolchain_.platformToolset != "v143" ||
		toolchain_.windowsSdk != "10.0.26100.0" || externals_.assimp != "snapshot-prebuilt" ||
		externals_.directXTexProject != "externals/DirectXTex/DirectXTex_Desktop_2022_Win10.vcxproj" ||
		externals_.imguiProject != "externals/imgui/imgui.vcxproj" || !runtime_.copyResources ||
		runtime_.resourceRoot != "resources" || engine_.name != "engine" || game_.name != "game" || entry_.name != "entry" ||
		engine_.root != "engine" || game_.root != "application" ||
		engine_.include != std::vector<std::string>{ "**/*.cpp", "**/*.h" } ||
		game_.include != std::vector<std::string>{ "**/*.cpp", "**/*.h" } ||
		entry_.files != std::vector<std::string>{ "main.cpp" } ||
		!ProjectDescriptor::IsSafeProjectRelativePath(engine_.root) ||
		!ProjectDescriptor::IsSafeProjectRelativePath(game_.root) ||
		!ProjectDescriptor::IsSafeProjectRelativePath(runtime_.resourceRoot)) {
		errorMessage = "Build specification values are invalid.";
		return false;
	}
	return true;
}

const std::filesystem::path& ProjectBuildSpecification::GetPath() const { return path_; }
const std::vector<std::string>& ProjectBuildSpecification::GetConfigurations() const { return configurations_; }
const ProjectBuildToolchain& ProjectBuildSpecification::GetToolchain() const { return toolchain_; }
const ProjectBuildExternalReferences& ProjectBuildSpecification::GetExternals() const { return externals_; }
const ProjectBuildRuntime& ProjectBuildSpecification::GetRuntime() const { return runtime_; }
const ProjectBuildSourceSet& ProjectBuildSpecification::GetEngineSourceSet() const { return engine_; }
const ProjectBuildSourceSet& ProjectBuildSpecification::GetGameSourceSet() const { return game_; }
const ProjectBuildSourceSet& ProjectBuildSpecification::GetEntrySourceSet() const { return entry_; }
