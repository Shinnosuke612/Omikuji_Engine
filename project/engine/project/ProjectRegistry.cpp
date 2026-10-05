// 役割: Project登録と生成・作成journalの保存互換と検証を管理する。
#include "ProjectRegistry.h"
#include "../utility/StringUtility.h"

#include "../../externals/nlohmann/json.hpp"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <set>
#include <vector>

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

	bool WriteUtf8Atomically(const std::filesystem::path& target, const std::string& content, std::string& errorMessage) {
		std::error_code error;
		if (!target.parent_path().empty()) {
			std::filesystem::create_directories(target.parent_path(), error);
			if (error) {
				errorMessage = "Project Registry directory could not be created.";
				return false;
			}
		}
		std::filesystem::path temporary = target;
		temporary += L".tmp";
		{
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			if (!output.is_open()) {
				errorMessage = "Project Registry temporary file could not be opened.";
				return false;
			}
			output.write(content.data(), static_cast<std::streamsize>(content.size()));
			output.flush();
			if (!output.good()) {
				output.close();
				std::filesystem::remove(temporary, error);
				errorMessage = "Project Registry temporary file could not be written.";
				return false;
			}
		}
		if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			std::filesystem::remove(temporary, error);
			errorMessage = "Project Registry atomic replace failed.";
			return false;
		}
		return true;
	}
}

bool ProjectRegistry::TryGetDefaultRegistryPath(std::filesystem::path& path, std::string& errorMessage) {
	const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
	if (required == 0) {
		errorMessage = "LOCALAPPDATA is unavailable.";
		return false;
	}
	std::vector<wchar_t> buffer(required);
	if (GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), required) == 0) {
		errorMessage = "LOCALAPPDATA could not be read.";
		return false;
	}
	path = std::filesystem::path(buffer.data()) / L"CG3Engine" / L"ProjectLauncher" / L"registry.json";
	return true;
}

bool ProjectRegistry::Load(std::string& errorMessage) {
	std::filesystem::path path;
	if (!TryGetDefaultRegistryPath(path, errorMessage)) {
		return false;
	}
	return Load(path, errorMessage);
}

bool ProjectRegistry::Load(const std::filesystem::path& registryPath, std::string& errorMessage) {
	registryPath_ = NormalizeAbsolutePath(registryPath);
	if (registryPath_.empty()) {
		errorMessage = "Project Registry path must be absolute.";
		return false;
	}
	std::error_code filesystemError;
	if (!std::filesystem::exists(registryPath_, filesystemError)) {
		if (filesystemError) {
			errorMessage = "Project Registry path could not be inspected.";
			return false;
		}
		templateSourceRoot_.clear();
		preferredVisualStudioInstanceId_.clear();
		projects_.clear();
		workspaceRoots_.clear();
		dependencyOverrides_.clear();
		creationOperations_.clear();
		solutionGenerationOperations_.clear();
		return true;
	}
	std::string text;
	if (!ReadUtf8File(registryPath_, text)) {
		errorMessage = "Project Registry could not be read.";
		return false;
	}
	try {
		const json root = json::parse(text);
		if (!HasOnlyFields(root, { "dependencyOverrides", "operations", "preferredVisualStudioInstanceId", "projects", "schemaVersion", "solutionGenerationOperations", "templateSourceRoot", "workspaceRoots" }) ||
			!root.contains("schemaVersion") || !root.at("schemaVersion").is_number_integer() ||
			!root.contains("projects") || !root.at("projects").is_array() ||
			!root.contains("workspaceRoots") || !root.at("workspaceRoots").is_array() ||
			!root.contains("dependencyOverrides") || !root.at("dependencyOverrides").is_array() ||
			!root.contains("operations") || !root.at("operations").is_array() ||
			!root.contains("templateSourceRoot") || !root.at("templateSourceRoot").is_string() ||
			!ReadRequiredString(root, "preferredVisualStudioInstanceId", preferredVisualStudioInstanceId_)) {
			errorMessage = "Project Registry schema is invalid.";
			return false;
		}
		const int schemaVersion = root.at("schemaVersion").get<int>();
		if (schemaVersion != 1 && schemaVersion != 2 && schemaVersion != kSchemaVersion) {
			errorMessage = "Project Registry schema is invalid.";
			return false;
		}
		// filesystem::path has no UTF-8 string reference; parse this field separately.
		templateSourceRoot_ = NormalizeAbsolutePath(StringUtility::ToPath(root.at("templateSourceRoot").get<std::string>()));
		projects_.clear();
		workspaceRoots_.clear();
		dependencyOverrides_.clear();
		creationOperations_.clear();
		solutionGenerationOperations_.clear();
		for (const json& value : root.at("projects")) {
			ProjectRegistryEntry entry{};
			std::string path;
			if (!HasOnlyFields(value, { "descriptorPath", "lastOpenedUtc", "pinned" }) || !ReadRequiredString(value, "descriptorPath", path) ||
				!ReadRequiredString(value, "lastOpenedUtc", entry.lastOpenedUtc) || !value.contains("pinned") || !value.at("pinned").is_boolean()) {
				errorMessage = "Project Registry project entry is invalid.";
				return false;
			}
			entry.descriptorPath = NormalizeAbsolutePath(StringUtility::ToPath(path));
			entry.pinned = value.at("pinned").get<bool>();
			if (entry.descriptorPath.empty() || std::any_of(projects_.begin(), projects_.end(), [&entry](const ProjectRegistryEntry& current) { return AreSamePath(current.descriptorPath, entry.descriptorPath); })) {
				errorMessage = "Project Registry contains duplicate or invalid descriptor paths.";
				return false;
			}
			projects_.push_back(std::move(entry));
		}
		for (const json& value : root.at("workspaceRoots")) {
			if (!value.is_string()) {
				errorMessage = "Project Registry workspace root is invalid.";
				return false;
			}
			const std::filesystem::path path = NormalizeAbsolutePath(StringUtility::ToPath(value.get<std::string>()));
			if (path.empty() || std::any_of(workspaceRoots_.begin(), workspaceRoots_.end(), [&path](const std::filesystem::path& current) { return AreSamePath(current, path); })) {
				errorMessage = "Project Registry contains duplicate or invalid workspace roots.";
				return false;
			}
			workspaceRoots_.push_back(path);
		}
		for (const json& value : root.at("dependencyOverrides")) {
			ProjectDependencyOverride overrideValue{};
			std::string checkoutPath;
			if (!HasOnlyFields(value, { "checkoutPath", "dependencyId", "projectId" }) ||
				!ReadRequiredString(value, "projectId", overrideValue.projectId) || !ReadRequiredString(value, "dependencyId", overrideValue.dependencyId) ||
				!ReadRequiredString(value, "checkoutPath", checkoutPath)) {
				errorMessage = "Project Registry dependency override is invalid.";
				return false;
			}
			overrideValue.checkoutPath = NormalizeAbsolutePath(StringUtility::ToPath(checkoutPath));
			if (overrideValue.projectId.empty() || overrideValue.dependencyId.empty() || overrideValue.checkoutPath.empty()) {
				errorMessage = "Project Registry dependency override path is invalid.";
				return false;
			}
			dependencyOverrides_.push_back(std::move(overrideValue));
		}
		for (const json& value : root.at("operations")) {
			ProjectCreationOperation operation{};
			std::string destinationRoot;
			std::string finalProjectRoot;
			std::string templateSourceRoot;
			std::string generatorPath;
			std::string logPath;
			std::string state;
			if (!HasOnlyFields(value, { "destinationRoot", "displayName", "exitCode", "finalProjectRoot", "generatorPath", "logPath", "operationId", "processId", "processStartTimeFileTime", "projectId", "startSceneId", "state", "templateSourceRoot", "outputLayout" }) ||
				!ReadRequiredString(value, "operationId", operation.operationId) || !ReadRequiredString(value, "projectId", operation.projectId) ||
				!ReadRequiredString(value, "displayName", operation.displayName) || !ReadRequiredString(value, "startSceneId", operation.startSceneId) ||
				!ReadRequiredString(value, "destinationRoot", destinationRoot) || !ReadRequiredString(value, "finalProjectRoot", finalProjectRoot) ||
				!ReadRequiredString(value, "templateSourceRoot", templateSourceRoot) || !ReadRequiredString(value, "generatorPath", generatorPath) ||
				!ReadRequiredString(value, "logPath", logPath) || !ReadRequiredString(value, "state", state) ||
				!value.contains("processId") || !value.at("processId").is_number_unsigned() ||
				!value.contains("processStartTimeFileTime") || !value.at("processStartTimeFileTime").is_number_unsigned() ||
				!value.contains("exitCode") || !value.at("exitCode").is_number_integer() ||
				!TryParseCreationOperationState(state, operation.state)) {
				errorMessage = "Project Registry creation operation is invalid.";
				return false;
			}
			if (value.contains("outputLayout")) {
				if (schemaVersion != 3 || !value.at("outputLayout").is_string()) { errorMessage = "Creation output layout is invalid."; return false; }
				operation.outputLayout = value.at("outputLayout").get<std::string>();
				if (!operation.outputLayout.empty() && operation.outputLayout != "GroupedV1" && operation.outputLayout != "ProjectFilesV2") { errorMessage = "Creation output layout is unsupported."; return false; }
			}
			operation.destinationRoot = NormalizeAbsolutePath(StringUtility::ToPath(destinationRoot));
			operation.finalProjectRoot = NormalizeAbsolutePath(StringUtility::ToPath(finalProjectRoot));
			operation.templateSourceRoot = NormalizeAbsolutePath(StringUtility::ToPath(templateSourceRoot));
			operation.generatorPath = NormalizeAbsolutePath(StringUtility::ToPath(generatorPath));
			operation.logPath = NormalizeAbsolutePath(StringUtility::ToPath(logPath));
			operation.processId = value.at("processId").get<uint32_t>();
			operation.processStartTimeFileTime = value.at("processStartTimeFileTime").get<uint64_t>();
			operation.exitCode = value.at("exitCode").get<int32_t>();
			if (operation.operationId.empty() || operation.projectId.empty() || operation.destinationRoot.empty() ||
				operation.finalProjectRoot.empty() || operation.templateSourceRoot.empty() || operation.generatorPath.empty() || operation.logPath.empty() ||
				std::any_of(creationOperations_.begin(), creationOperations_.end(), [&operation](const ProjectCreationOperation& current) { return current.operationId == operation.operationId; })) {
				errorMessage = "Project Registry creation operation has invalid paths or duplicate ID.";
				return false;
			}
			creationOperations_.push_back(std::move(operation));
		}
		if (root.contains("solutionGenerationOperations")) {
			if (!root.at("solutionGenerationOperations").is_array()) {
				errorMessage = "Project Registry solution generation operations are invalid.";
				return false;
			}
			for (const json& value : root.at("solutionGenerationOperations")) {
				SolutionGenerationOperation operation{};
				std::string projectRoot;
				std::string stagingRoot;
				std::string rollbackRoot;
				std::string state;
				if (!HasOnlyFields(value, { "files", "operationId", "projectId", "projectRoot", "rollbackRoot", "stagingRoot", "state" }) ||
					!ReadRequiredString(value, "operationId", operation.operationId) || !ReadRequiredString(value, "projectId", operation.projectId) ||
					!ReadRequiredString(value, "projectRoot", projectRoot) || !ReadRequiredString(value, "stagingRoot", stagingRoot) ||
					!ReadRequiredString(value, "rollbackRoot", rollbackRoot) || !ReadRequiredString(value, "state", state) ||
					!value.contains("files") || !value.at("files").is_array() || !TryParseSolutionGenerationOperationState(state, operation.state)) {
					errorMessage = "Project Registry solution generation operation is invalid.";
					return false;
				}
				operation.projectRoot = NormalizeAbsolutePath(StringUtility::ToPath(projectRoot));
				operation.stagingRoot = NormalizeAbsolutePath(StringUtility::ToPath(stagingRoot));
				operation.rollbackRoot = NormalizeAbsolutePath(StringUtility::ToPath(rollbackRoot));
				bool hasDescriptor = false;
				bool hasManifest = false;
				for (const json& fileValue : value.at("files")) {
					SolutionGenerationOperationFile file{};
					std::string relativePath;
					if (schemaVersion == 1) {
						if (!HasOnlyFields(fileValue, { "nextContentHash", "path", "previousContentHash", "previouslyExisted" }) ||
							!ReadRequiredString(fileValue, "path", relativePath) || !ReadRequiredString(fileValue, "previousContentHash", file.previousContentHash) ||
							!ReadRequiredString(fileValue, "nextContentHash", file.nextContentHash) || !fileValue.contains("previouslyExisted") || !fileValue.at("previouslyExisted").is_boolean()) {
							errorMessage = "Project Registry solution generation file is invalid.";
							return false;
						}
						file.previousExists = fileValue.at("previouslyExisted").get<bool>();
						file.nextExists = true;
					} else {
						std::string kind;
						if (!HasOnlyFields(fileValue, { "kind", "nextContentHash", "nextExists", "path", "previousContentHash", "previousExists" }) ||
							!ReadRequiredString(fileValue, "kind", kind) || !ReadRequiredString(fileValue, "path", relativePath) ||
							!ReadRequiredString(fileValue, "previousContentHash", file.previousContentHash) ||
							!ReadRequiredString(fileValue, "nextContentHash", file.nextContentHash) ||
							!fileValue.contains("previousExists") || !fileValue.at("previousExists").is_boolean() ||
							!fileValue.contains("nextExists") || !fileValue.at("nextExists").is_boolean() ||
							!TryParseSolutionGenerationOperationFileKind(kind, file.kind)) {
							errorMessage = "Project Registry solution generation file is invalid.";
							return false;
						}
						file.previousExists = fileValue.at("previousExists").get<bool>();
						file.nextExists = fileValue.at("nextExists").get<bool>();
					}
					file.relativePath = StringUtility::ToPath(relativePath).lexically_normal();
					if (schemaVersion < 3 && file.kind == SolutionGenerationOperationFileKind::Manifest && file.relativePath != std::filesystem::path(L"project/build/generated/solution-generation.json")) { errorMessage = "New manifest layout requires Registry schema 3."; return false; }
					if (schemaVersion == 1 && file.relativePath == std::filesystem::path(L"project/build/generated/solution-generation.json")) {
						file.kind = SolutionGenerationOperationFileKind::Manifest;
					}
					if (file.kind == SolutionGenerationOperationFileKind::Descriptor) {
						hasDescriptor = true;
					}
					if (file.kind == SolutionGenerationOperationFileKind::Manifest) {
						hasManifest = true;
					}
					operation.files.push_back(std::move(file));
				}
				if (!hasManifest) {
					errorMessage = "Project Registry solution generation operation lacks a manifest file.";
					return false;
				}
				if (hasDescriptor && std::count_if(operation.files.begin(), operation.files.end(), [](const SolutionGenerationOperationFile& file) {
					return file.kind == SolutionGenerationOperationFileKind::Descriptor;
				}) != 1) {
					errorMessage = "Project Registry solution generation operation has multiple descriptor files.";
					return false;
				}
				solutionGenerationOperations_.push_back(std::move(operation));
			}
		}
		return Validate(errorMessage);
	} catch (const json::exception&) {
		errorMessage = "Project Registry JSON could not be parsed.";
		return false;
	}
}

bool ProjectRegistry::Save(std::string& errorMessage) const {
	if (!Validate(errorMessage)) {
		return false;
	}
	json projects = json::array();
	for (const ProjectRegistryEntry& entry : projects_) {
		projects.push_back({ { "descriptorPath", StringUtility::ToUtf8(entry.descriptorPath) }, { "lastOpenedUtc", entry.lastOpenedUtc }, { "pinned", entry.pinned } });
	}
	json workspaceRoots = json::array();
	for (const std::filesystem::path& root : workspaceRoots_) {
		workspaceRoots.push_back(StringUtility::ToUtf8(root));
	}
	json dependencyOverrides = json::array();
	for (const ProjectDependencyOverride& value : dependencyOverrides_) {
		dependencyOverrides.push_back({ { "checkoutPath", StringUtility::ToUtf8(value.checkoutPath) }, { "dependencyId", value.dependencyId }, { "projectId", value.projectId } });
	}
	json operations = json::array();
	for (const ProjectCreationOperation& operation : creationOperations_) {
		operations.push_back({
			{ "destinationRoot", StringUtility::ToUtf8(operation.destinationRoot) },
			{ "displayName", operation.displayName },
			{ "exitCode", operation.exitCode },
			{ "finalProjectRoot", StringUtility::ToUtf8(operation.finalProjectRoot) },
			{ "generatorPath", StringUtility::ToUtf8(operation.generatorPath) },
			{ "logPath", StringUtility::ToUtf8(operation.logPath) },
			{ "operationId", operation.operationId },
			{ "outputLayout", operation.outputLayout },
			{ "processId", operation.processId },
			{ "processStartTimeFileTime", operation.processStartTimeFileTime },
			{ "projectId", operation.projectId },
			{ "startSceneId", operation.startSceneId },
			{ "state", CreationOperationStateToString(operation.state) },
			{ "templateSourceRoot", StringUtility::ToUtf8(operation.templateSourceRoot) }
		});
	}
	json solutionGenerationOperations = json::array();
	for (const SolutionGenerationOperation& operation : solutionGenerationOperations_) {
		json files = json::array();
		for (const SolutionGenerationOperationFile& file : operation.files) {
			files.push_back({
				{ "kind", SolutionGenerationOperationFileKindToString(file.kind) },
				{ "nextContentHash", file.nextContentHash },
				{ "nextExists", file.nextExists },
				{ "path", StringUtility::ToUtf8(file.relativePath) },
				{ "previousContentHash", file.previousContentHash },
				{ "previousExists", file.previousExists }
			});
		}
		solutionGenerationOperations.push_back({
			{ "files", files }, { "operationId", operation.operationId }, { "projectId", operation.projectId },
			{ "projectRoot", StringUtility::ToUtf8(operation.projectRoot) }, { "rollbackRoot", StringUtility::ToUtf8(operation.rollbackRoot) },
			{ "stagingRoot", StringUtility::ToUtf8(operation.stagingRoot) }, { "state", SolutionGenerationOperationStateToString(operation.state) }
		});
	}
	const json root = {
		{ "dependencyOverrides", dependencyOverrides },
		{ "operations", operations },
		{ "preferredVisualStudioInstanceId", preferredVisualStudioInstanceId_ },
		{ "projects", projects },
		{ "schemaVersion", kSchemaVersion },
		{ "solutionGenerationOperations", solutionGenerationOperations },
		{ "templateSourceRoot", StringUtility::ToUtf8(templateSourceRoot_) },
		{ "workspaceRoots", workspaceRoots }
	};
	return WriteUtf8Atomically(registryPath_, root.dump(2) + "\n", errorMessage);
}

bool ProjectRegistry::Validate(std::string& errorMessage) const {
	if (registryPath_.empty() || (!templateSourceRoot_.empty() && !templateSourceRoot_.is_absolute())) {
		errorMessage = "Project Registry path is invalid.";
		return false;
	}
	for (size_t index = 0; index < projects_.size(); ++index) {
		if (!projects_[index].descriptorPath.is_absolute()) {
			errorMessage = "Project Registry descriptor path is not absolute.";
			return false;
		}
		for (size_t other = index + 1; other < projects_.size(); ++other) {
			if (AreSamePath(projects_[index].descriptorPath, projects_[other].descriptorPath)) {
				errorMessage = "Project Registry descriptor path is duplicated.";
				return false;
			}
		}
	}
	for (size_t index = 0; index < workspaceRoots_.size(); ++index) {
		if (!workspaceRoots_[index].is_absolute()) {
			errorMessage = "Project Registry workspace root is not absolute.";
			return false;
		}
		for (size_t other = index + 1; other < workspaceRoots_.size(); ++other) {
			if (AreSamePath(workspaceRoots_[index], workspaceRoots_[other])) {
				errorMessage = "Project Registry workspace root is duplicated.";
				return false;
			}
		}
	}
	for (const ProjectDependencyOverride& value : dependencyOverrides_) {
		if (value.projectId.empty() || value.dependencyId.empty() || !value.checkoutPath.is_absolute()) {
			errorMessage = "Project Registry dependency override is invalid.";
			return false;
		}
	}
	for (size_t index = 0; index < dependencyOverrides_.size(); ++index) {
		for (size_t other = index + 1; other < dependencyOverrides_.size(); ++other) {
			if (dependencyOverrides_[index].projectId == dependencyOverrides_[other].projectId &&
				dependencyOverrides_[index].dependencyId == dependencyOverrides_[other].dependencyId) {
				errorMessage = "Project Registry dependency override is duplicated.";
				return false;
			}
		}
	}
	for (size_t index = 0; index < creationOperations_.size(); ++index) {
		const ProjectCreationOperation& operation = creationOperations_[index];
		if (operation.operationId.empty() || operation.projectId.empty() || operation.destinationRoot.empty() ||
			operation.finalProjectRoot.empty() || operation.templateSourceRoot.empty() || operation.generatorPath.empty() || operation.logPath.empty() ||
			!operation.destinationRoot.is_absolute() || !operation.finalProjectRoot.is_absolute() ||
			!operation.templateSourceRoot.is_absolute() || !operation.generatorPath.is_absolute() || !operation.logPath.is_absolute() ||
			(!operation.outputLayout.empty() && operation.outputLayout != "GroupedV1" && operation.outputLayout != "ProjectFilesV2")) {
			errorMessage = "Project Registry creation operation is invalid.";
			return false;
		}
		for (size_t other = index + 1; other < creationOperations_.size(); ++other) {
			if (operation.operationId == creationOperations_[other].operationId) {
				errorMessage = "Project Registry creation operation ID is duplicated.";
				return false;
			}
		}
	}
	for (size_t index = 0; index < solutionGenerationOperations_.size(); ++index) {
		const SolutionGenerationOperation& operation = solutionGenerationOperations_[index];
		if (operation.operationId.empty() || operation.projectId.empty() || !operation.projectRoot.is_absolute() ||
			!operation.stagingRoot.is_absolute() || !operation.rollbackRoot.is_absolute() || operation.files.empty()) {
			errorMessage = "Project Registry solution generation operation is invalid.";
			return false;
		}
		for (size_t other = index + 1; other < solutionGenerationOperations_.size(); ++other) {
			if (operation.operationId == solutionGenerationOperations_[other].operationId) {
				errorMessage = "Project Registry solution generation operation ID is duplicated.";
				return false;
			}
		}
		for (size_t fileIndex = 0; fileIndex < operation.files.size(); ++fileIndex) {
			const SolutionGenerationOperationFile& file = operation.files[fileIndex];
			if (file.relativePath.empty() || file.relativePath.is_absolute() || file.relativePath.has_root_name() || file.relativePath.has_root_directory() ||
				(file.previousExists ? file.previousContentHash.empty() : !file.previousContentHash.empty()) ||
				(file.nextExists ? file.nextContentHash.empty() : !file.nextContentHash.empty()) ||
				(!file.previousExists && !file.nextExists)) {
				errorMessage = "Project Registry solution generation file is invalid.";
				return false;
			}
			for (const std::filesystem::path& part : file.relativePath) {
				if (part == L"..") {
					errorMessage = "Project Registry solution generation file path is unsafe.";
					return false;
				}
			}
			if (file.kind == SolutionGenerationOperationFileKind::Artifact &&
				(CompareStringOrdinal(file.relativePath.lexically_normal().c_str(), -1, L"project\\build\\generated\\solution-generation.json", -1, TRUE) == CSTR_EQUAL ||
				 CompareStringOrdinal(file.relativePath.lexically_normal().c_str(), -1, L"intermediate\\project-files\\solution-generation.json", -1, TRUE) == CSTR_EQUAL)) { errorMessage = "Manifest cannot be an artifact."; return false; }
			if (file.kind == SolutionGenerationOperationFileKind::Descriptor && file.relativePath != std::filesystem::path(L"game.project.json")) {
				errorMessage = "Project Registry solution generation descriptor path is invalid.";
				return false;
			}
			if (file.kind == SolutionGenerationOperationFileKind::Manifest && file.relativePath != std::filesystem::path(L"project/build/generated/solution-generation.json") && file.relativePath != std::filesystem::path(L"intermediate/project-files/solution-generation.json")) {
				errorMessage = "Project Registry solution generation manifest path is invalid.";
				return false;
			}
			for (size_t other = fileIndex + 1; other < operation.files.size(); ++other) {
				if (CompareStringOrdinal(operation.files[fileIndex].relativePath.c_str(), -1, operation.files[other].relativePath.c_str(), -1, TRUE) == CSTR_EQUAL) {
					errorMessage = "Project Registry solution generation file path is duplicated.";
					return false;
				}
			}
		}
		const size_t descriptorCount = static_cast<size_t>(std::count_if(operation.files.begin(), operation.files.end(), [](const SolutionGenerationOperationFile& file) {
			return file.kind == SolutionGenerationOperationFileKind::Descriptor;
		}));
		const size_t manifestCount = static_cast<size_t>(std::count_if(operation.files.begin(), operation.files.end(), [](const SolutionGenerationOperationFile& file) {
			return file.kind == SolutionGenerationOperationFileKind::Manifest;
		}));
		if (manifestCount == 1 && std::any_of(operation.files.begin(), operation.files.end(), [](const SolutionGenerationOperationFile& file) { return file.kind == SolutionGenerationOperationFileKind::Manifest && !file.nextExists; })) { errorMessage = "Single manifest must describe the next generation."; return false; }
		if (descriptorCount > 1 || (manifestCount != 1 && !(manifestCount == 2 && descriptorCount == 1 &&
			std::any_of(operation.files.begin(), operation.files.end(), [](const SolutionGenerationOperationFile& file) {
				return file.kind == SolutionGenerationOperationFileKind::Descriptor && file.previousExists && file.nextExists;
			}) &&
			std::count_if(operation.files.begin(), operation.files.end(), [](const SolutionGenerationOperationFile& file) {
				return file.kind == SolutionGenerationOperationFileKind::Manifest && file.relativePath == std::filesystem::path(L"project/build/generated/solution-generation.json") && file.previousExists && !file.nextExists;
			}) == 1 &&
			std::count_if(operation.files.begin(), operation.files.end(), [](const SolutionGenerationOperationFile& file) {
				return file.kind == SolutionGenerationOperationFileKind::Manifest && file.relativePath == std::filesystem::path(L"intermediate/project-files/solution-generation.json") && !file.previousExists && file.nextExists;
			}) == 1))) {
			errorMessage = "Project Registry solution generation file kind set is invalid.";
			return false;
		}
	}
	return true;
}

bool ProjectRegistry::RegisterProject(const ProjectRegistryEntry& source, std::string& errorMessage) {
	ProjectRegistryEntry entry = source;
	entry.descriptorPath = NormalizeAbsolutePath(entry.descriptorPath);
	if (entry.descriptorPath.empty()) {
		errorMessage = "Project descriptor path must be absolute.";
		return false;
	}
	for (ProjectRegistryEntry& current : projects_) {
		if (AreSamePath(current.descriptorPath, entry.descriptorPath)) {
			current = std::move(entry);
			return true;
		}
	}
	projects_.push_back(std::move(entry));
	return true;
}

bool ProjectRegistry::RemoveProject(const std::filesystem::path& descriptorPath) {
	const std::filesystem::path normalized = NormalizeAbsolutePath(descriptorPath);
	const auto result = std::remove_if(projects_.begin(), projects_.end(), [&normalized](const ProjectRegistryEntry& entry) { return AreSamePath(entry.descriptorPath, normalized); });
	const bool removed = result != projects_.end();
	projects_.erase(result, projects_.end());
	return removed;
}

bool ProjectRegistry::AddWorkspaceRoot(const std::filesystem::path& root, std::string& errorMessage) {
	const std::filesystem::path normalized = NormalizeAbsolutePath(root);
	if (normalized.empty()) {
		errorMessage = "Workspace root must be absolute.";
		return false;
	}
	if (std::none_of(workspaceRoots_.begin(), workspaceRoots_.end(), [&normalized](const std::filesystem::path& current) { return AreSamePath(current, normalized); })) {
		workspaceRoots_.push_back(normalized);
	}
	return true;
}

bool ProjectRegistry::RemoveWorkspaceRoot(const std::filesystem::path& root) {
	const std::filesystem::path normalized = NormalizeAbsolutePath(root);
	const auto result = std::remove_if(workspaceRoots_.begin(), workspaceRoots_.end(), [&normalized](const std::filesystem::path& current) { return AreSamePath(current, normalized); });
	const bool removed = result != workspaceRoots_.end();
	workspaceRoots_.erase(result, workspaceRoots_.end());
	return removed;
}

bool ProjectRegistry::SetDependencyOverride(const ProjectDependencyOverride& source, std::string& errorMessage) {
	ProjectDependencyOverride value = source;
	value.checkoutPath = NormalizeAbsolutePath(value.checkoutPath);
	if (value.projectId.empty() || value.dependencyId.empty() || value.checkoutPath.empty()) {
		errorMessage = "Dependency override must have project, dependency, and absolute checkout path.";
		return false;
	}
	for (ProjectDependencyOverride& current : dependencyOverrides_) {
		if (current.projectId == value.projectId && current.dependencyId == value.dependencyId) {
			current = std::move(value);
			return true;
		}
	}
	dependencyOverrides_.push_back(std::move(value));
	return true;
}

bool ProjectRegistry::RemoveDependencyOverride(const std::string& projectId, const std::string& dependencyId) {
	const auto result = std::remove_if(dependencyOverrides_.begin(), dependencyOverrides_.end(), [&projectId, &dependencyId](const ProjectDependencyOverride& value) { return value.projectId == projectId && value.dependencyId == dependencyId; });
	const bool removed = result != dependencyOverrides_.end();
	dependencyOverrides_.erase(result, dependencyOverrides_.end());
	return removed;
}

bool ProjectRegistry::UpsertCreationOperation(const ProjectCreationOperation& source, std::string& errorMessage) {
	ProjectCreationOperation operation = source;
	operation.destinationRoot = NormalizeAbsolutePath(operation.destinationRoot);
	operation.finalProjectRoot = NormalizeAbsolutePath(operation.finalProjectRoot);
	operation.templateSourceRoot = NormalizeAbsolutePath(operation.templateSourceRoot);
	operation.generatorPath = NormalizeAbsolutePath(operation.generatorPath);
	operation.logPath = NormalizeAbsolutePath(operation.logPath);
	if (operation.operationId.empty() || operation.projectId.empty() || operation.destinationRoot.empty() ||
		operation.finalProjectRoot.empty() || operation.templateSourceRoot.empty() || operation.generatorPath.empty() || operation.logPath.empty()) {
		errorMessage = "Creation operation requires an ID, Project ID, and absolute paths.";
		return false;
	}
	for (ProjectCreationOperation& current : creationOperations_) {
		if (current.operationId == operation.operationId) {
			current = std::move(operation);
			return true;
		}
	}
	creationOperations_.push_back(std::move(operation));
	return true;
}

const ProjectCreationOperation* ProjectRegistry::FindCreationOperation(const std::string& operationId) const {
	const auto iterator = std::find_if(creationOperations_.begin(), creationOperations_.end(), [&operationId](const ProjectCreationOperation& value) { return value.operationId == operationId; });
	return iterator == creationOperations_.end() ? nullptr : &*iterator;
}

bool ProjectRegistry::UpsertSolutionGenerationOperation(const SolutionGenerationOperation& source, std::string& errorMessage) {
	SolutionGenerationOperation operation = source;
	operation.projectRoot = NormalizeAbsolutePath(operation.projectRoot);
	operation.stagingRoot = NormalizeAbsolutePath(operation.stagingRoot);
	operation.rollbackRoot = NormalizeAbsolutePath(operation.rollbackRoot);
	if (operation.operationId.empty() || operation.projectId.empty() || operation.projectRoot.empty() || operation.stagingRoot.empty() || operation.rollbackRoot.empty() || operation.files.empty()) {
		errorMessage = "Solution generation operation requires IDs, absolute paths, and files.";
		return false;
	}
	for (SolutionGenerationOperation& current : solutionGenerationOperations_) {
		if (current.operationId == operation.operationId) {
			current = std::move(operation);
			return true;
		}
	}
	solutionGenerationOperations_.push_back(std::move(operation));
	return true;
}

const SolutionGenerationOperation* ProjectRegistry::FindSolutionGenerationOperation(const std::string& operationId) const {
	const auto iterator = std::find_if(solutionGenerationOperations_.begin(), solutionGenerationOperations_.end(), [&operationId](const SolutionGenerationOperation& value) { return value.operationId == operationId; });
	return iterator == solutionGenerationOperations_.end() ? nullptr : &*iterator;
}

bool ProjectRegistry::AreSamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
	const std::wstring normalizedLeft = NormalizeAbsolutePath(left).native();
	const std::wstring normalizedRight = NormalizeAbsolutePath(right).native();
	return !normalizedLeft.empty() && !normalizedRight.empty() && CompareStringOrdinal(normalizedLeft.c_str(), -1, normalizedRight.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::filesystem::path ProjectRegistry::NormalizeAbsolutePath(const std::filesystem::path& value) {
	if (value.empty() || !value.is_absolute()) {
		return {};
	}
	return std::filesystem::absolute(value).lexically_normal();
}

const char* ProjectRegistry::CreationOperationStateToString(ProjectCreationOperationState value) {
	switch (value) {
	case ProjectCreationOperationState::InProgress: return "in-progress";
	case ProjectCreationOperationState::Failed: return "failed";
	case ProjectCreationOperationState::RegistrationIncomplete: return "registration-incomplete";
	case ProjectCreationOperationState::Complete: return "complete";
	default: return "failed";
	}
}

bool ProjectRegistry::TryParseCreationOperationState(const std::string& value, ProjectCreationOperationState& state) {
	if (value == "in-progress") { state = ProjectCreationOperationState::InProgress; return true; }
	if (value == "failed") { state = ProjectCreationOperationState::Failed; return true; }
	if (value == "registration-incomplete") { state = ProjectCreationOperationState::RegistrationIncomplete; return true; }
	if (value == "complete") { state = ProjectCreationOperationState::Complete; return true; }
	return false;
}

const char* ProjectRegistry::SolutionGenerationOperationStateToString(SolutionGenerationOperationState value) {
	switch (value) {
	case SolutionGenerationOperationState::Staged: return "staged";
	case SolutionGenerationOperationState::CommitInProgress: return "commit-in-progress";
	case SolutionGenerationOperationState::RollbackInProgress: return "rollback-in-progress";
	case SolutionGenerationOperationState::RolledBack: return "rolled-back";
	case SolutionGenerationOperationState::Complete: return "complete";
	case SolutionGenerationOperationState::RecoveryRequired: return "recovery-required";
	default: return "recovery-required";
	}
}

bool ProjectRegistry::TryParseSolutionGenerationOperationState(const std::string& value, SolutionGenerationOperationState& state) {
	if (value == "staged") { state = SolutionGenerationOperationState::Staged; return true; }
	if (value == "commit-in-progress") { state = SolutionGenerationOperationState::CommitInProgress; return true; }
	if (value == "rollback-in-progress") { state = SolutionGenerationOperationState::RollbackInProgress; return true; }
	if (value == "rolled-back") { state = SolutionGenerationOperationState::RolledBack; return true; }
	if (value == "complete") { state = SolutionGenerationOperationState::Complete; return true; }
	if (value == "recovery-required") { state = SolutionGenerationOperationState::RecoveryRequired; return true; }
	return false;
}

const char* ProjectRegistry::SolutionGenerationOperationFileKindToString(SolutionGenerationOperationFileKind value) {
	switch (value) {
	case SolutionGenerationOperationFileKind::Artifact: return "artifact";
	case SolutionGenerationOperationFileKind::Descriptor: return "descriptor";
	case SolutionGenerationOperationFileKind::Manifest: return "manifest";
	default: return "artifact";
	}
}

bool ProjectRegistry::TryParseSolutionGenerationOperationFileKind(const std::string& value, SolutionGenerationOperationFileKind& kind) {
	if (value == "artifact") { kind = SolutionGenerationOperationFileKind::Artifact; return true; }
	if (value == "descriptor") { kind = SolutionGenerationOperationFileKind::Descriptor; return true; }
	if (value == "manifest") { kind = SolutionGenerationOperationFileKind::Manifest; return true; }
	return false;
}
