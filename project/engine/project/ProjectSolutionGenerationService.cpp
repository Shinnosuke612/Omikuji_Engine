// 役割: Snapshot ProjectのBuild可能なSolution previewをPC local workspaceへ組み立てる。
#include "ProjectSolutionGenerationService.h"

#include "../utility/StringUtility.h"
#include "ProjectBuildSpecification.h"
#include "ProjectDescriptor.h"
#include "SolutionGenerationModel.h"
#include "SolutionGenerationTransaction.h"
#include "../../externals/nlohmann/json.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <random>
#include <sstream>
#include <utility>

namespace {
	using json = nlohmann::json;

	bool IsReparsePoint(const std::filesystem::path& path) {
		const DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
	}

	bool HasReparsePointInExistingPath(const std::filesystem::path& path) {
		std::filesystem::path current = path.root_path();
		for (const std::filesystem::path& part : path.relative_path()) {
			current /= part;
			std::error_code error;
			if (!std::filesystem::exists(current, error) || error) break;
			if (IsReparsePoint(current)) return true;
		}
		return false;
	}

	bool IsWithin(const std::filesystem::path& child, const std::filesystem::path& parent) {
		const std::filesystem::path normalizedChild = std::filesystem::absolute(child).lexically_normal();
		const std::filesystem::path normalizedParent = std::filesystem::absolute(parent).lexically_normal();
		auto childPart = normalizedChild.begin();
		for (auto parentPart = normalizedParent.begin(); parentPart != normalizedParent.end(); ++parentPart, ++childPart) {
			if (childPart == normalizedChild.end() || CompareStringOrdinal(childPart->c_str(), -1, parentPart->c_str(), -1, TRUE) != CSTR_EQUAL) return false;
		}
		return true;
	}

	bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
		const std::wstring normalizedLeft = std::filesystem::absolute(left).lexically_normal().native();
		const std::wstring normalizedRight = std::filesystem::absolute(right).lexically_normal().native();
		return !normalizedLeft.empty() && !normalizedRight.empty() && CompareStringOrdinal(normalizedLeft.c_str(), -1, normalizedRight.c_str(), -1, TRUE) == CSTR_EQUAL;
	}

	bool ReadFile(const std::filesystem::path& path, std::string& content) {
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open()) return false;
		content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		return !input.bad();
	}

	std::string HashContent(const std::string& content) {
		std::uint64_t value = 14695981039346656037ull;
		for (const unsigned char character : content) { value ^= character; value *= 1099511628211ull; }
		std::ostringstream stream;
		stream << std::hex << std::uppercase << std::setfill('0') << std::setw(16) << value;
		return stream.str();
	}

	bool MatchesHash(const std::filesystem::path& path, const std::string& expected) {
		std::string content;
		return ReadFile(path, content) && HashContent(content) == expected;
	}

	// 既知版の全文を識別する。Sourceを変更したり未知版を推測で許可しない。
	bool ValidateProjectFilesSource(const ProjectDescriptor& descriptor, std::string& errorMessage) {
		const auto header = descriptor.GetProjectRoot() / L"project/engine/utility/EditableResourcePath.h";
		std::error_code error;
		std::string content;
		if (HasReparsePointInExistingPath(header) || !std::filesystem::is_regular_file(header, error) || error || !ReadFile(header, content)) { errorMessage = "Source root detector is missing or unsafe. Update this Project Source before generating V2."; return false; }
		if (content.compare(0, 3, "\xEF\xBB\xBF") == 0) content.erase(0, 3);
		for (size_t position = 0; (position = content.find("\r\n", position)) != std::string::npos;) content.replace(position, 2, "\n");
		const std::string canonical = "path / \"CG2_2025_04_14.vcxproj\"";
		std::vector<std::string> markers = { canonical, "path / \"" + descriptor.GetProjectId() + ".vcxproj\"", "path / \"build/generated/" + descriptor.GetProjectId() + "/" + descriptor.GetProjectId() + ".vcxproj\"" };
		std::sort(markers.begin(), markers.end()); markers.erase(std::unique(markers.begin(), markers.end()), markers.end());
		size_t count = 0; size_t foundPosition = 0; std::string foundMarker;
		for (const std::string& marker : markers) {
			for (size_t position = 0; (position = content.find(marker, position)) != std::string::npos; position += marker.size()) { ++count; foundPosition = position; foundMarker = marker; }
		}
		if (count != 1) { errorMessage = "Source root detector marker is ambiguous. Update this Project Source."; return false; }
		content.replace(foundPosition, foundMarker.size(), canonical);
		if (HashContent(content) != "1D00E42E62656193") { errorMessage = "Source root detector is not the verified descriptor-aware version. Update this Project Source and verify its Build and Editor assets."; return false; }
		return true;
	}
	std::string MakeOperationId() {
		std::random_device randomDevice;
		std::mt19937 generator(randomDevice());
		std::uniform_int_distribution<unsigned int> distribution(0, 255);
		std::ostringstream output;
		output << std::hex << std::setfill('0');
		for (size_t index = 0; index < 16; ++index) output << std::setw(2) << distribution(generator);
		return output.str();
	}

	bool IsOperationId(const std::string& value) {
		return value.size() == 32 && std::all_of(value.begin(), value.end(), [](unsigned char character) { return std::isxdigit(character) != 0; });
	}

	std::string ToLowerAscii(std::string value) {
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
		return value;
	}

	bool IsExcludedPreviewPath(const std::filesystem::path& relativePath, bool directory) {
		if (relativePath.empty()) return true;
		const std::string first = ToLowerAscii(StringUtility::ToUtf8(*relativePath.begin()));
		if (first == ".vs" || first == "generated" || first == "logs" || first == "cache" || first == ".git") return true;
		const std::filesystem::path normalized = relativePath.lexically_normal();
		// V2はGame root側だが、Source内へ混入した同名生成物もcopy対象にしない。
		if (IsWithin(normalized, std::filesystem::path(L"intermediate/project-files"))) return true;
		auto iterator = normalized.begin();
		if (iterator != normalized.end() && ToLowerAscii(StringUtility::ToUtf8(*iterator)) == "build") {
			++iterator;
			if (iterator != normalized.end() && ToLowerAscii(StringUtility::ToUtf8(*iterator)) == "generated") {
				return true;
			}
			if (!directory && iterator != normalized.end() && ToLowerAscii(StringUtility::ToUtf8(*iterator)) == "project.build.json") {
				++iterator;
				if (iterator == normalized.end()) return true;
			}
		}
		if (directory) return false;
		const std::string extension = ToLowerAscii(StringUtility::ToUtf8(relativePath.extension()));
		return extension == ".pdb" || extension == ".obj" || extension == ".exe" || extension == ".ilk" || extension == ".tmp" || extension == ".bak";
	}

	bool CopyProjectTree(const std::filesystem::path& sourceRoot, const std::filesystem::path& destinationRoot, std::string& errorMessage) {
		std::error_code error;
	if (!std::filesystem::is_directory(sourceRoot, error) || error || HasReparsePointInExistingPath(sourceRoot) || HasReparsePointInExistingPath(destinationRoot)) {
			errorMessage = "Preview Project source root is unsafe.";
			return false;
		}
		std::filesystem::recursive_directory_iterator iterator(sourceRoot, std::filesystem::directory_options::none, error);
		const std::filesystem::recursive_directory_iterator end;
		while (!error && iterator != end) {
			const std::filesystem::directory_entry entry = *iterator;
			const std::filesystem::path source = entry.path();
			const std::filesystem::path relative = std::filesystem::relative(source, sourceRoot, error).lexically_normal();
			const bool directory = entry.is_directory(error);
			if (error || relative.empty() || relative.is_absolute() || !IsWithin(destinationRoot / relative, destinationRoot) || HasReparsePointInExistingPath(source)) {
				errorMessage = "Preview workspace encountered an unsafe source path.";
				return false;
			}
			if (IsExcludedPreviewPath(relative, directory)) {
				if (directory) iterator.disable_recursion_pending();
				iterator.increment(error);
				continue;
			}
			const std::filesystem::path destination = destinationRoot / relative;
			if (directory) {
				std::filesystem::create_directories(destination, error);
			} else if (entry.is_regular_file(error)) {
				std::filesystem::create_directories(destination.parent_path(), error);
				if (!error && !std::filesystem::exists(destination, error)) std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, error);
			} else {
				errorMessage = "Preview workspace source contains a non-regular file.";
				return false;
			}
			if (error) { errorMessage = "Preview workspace copy failed."; return false; }
			iterator.increment(error);
		}
		if (error) { errorMessage = "Preview workspace could not enumerate Project files."; return false; }
		return true;
	}

	// 旧Projectの仕様も新配置へ一本化する。canonical設定は書き換えない。
	bool CopyPreviewBuildSpecification(const std::filesystem::path& source, const std::filesystem::path& projectRoot, const std::filesystem::path& stagingRoot, std::string& errorMessage) {
		const std::filesystem::path destination = (stagingRoot / L"config" / L"project.build.json").lexically_normal();
		std::error_code error;
		if (!IsWithin(source, projectRoot) || !IsWithin(destination, stagingRoot) ||
			HasReparsePointInExistingPath(source) || HasReparsePointInExistingPath(destination) ||
			!std::filesystem::is_regular_file(source, error) || error) {
			errorMessage = "Preview build specification copy path is unsafe.";
			return false;
		}
		std::filesystem::create_directories(destination.parent_path(), error);
		if (error) {
			errorMessage = "Preview configuration directory could not be created: " + error.message();
			return false;
		}
		if (!std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, error) || error) {
			errorMessage = "Preview build specification could not be copied: " + error.message();
			return false;
		}
		return true;
	}

	bool ReadPreviewManifest(const std::filesystem::path& manifestPath, ProjectSolutionPreviewSnapshot& output, std::string& errorMessage) {
		std::string content;
		if (!ReadFile(manifestPath, content)) { errorMessage = "Generation manifest could not be read."; return false; }
		try {
			const json manifest = json::parse(content);
			if (!manifest.is_object() || !manifest.contains("schemaVersion") || !manifest.at("schemaVersion").is_number_integer() || manifest.at("schemaVersion").get<int>() != 1 ||
				!manifest.contains("inputIdentity") || !manifest.at("inputIdentity").is_string() || !manifest.contains("artifacts") || !manifest.at("artifacts").is_array()) {
				errorMessage = "Generation manifest schema is invalid.";
				return false;
			}
			output.inputIdentity = manifest.at("inputIdentity").get<std::string>();
			output.artifacts.clear();
			for (const json& value : manifest.at("artifacts")) {
				if (!value.is_object() || value.size() != 2 || !value.contains("path") || !value.at("path").is_string() || !value.contains("contentHash") || !value.at("contentHash").is_string()) {
					errorMessage = "Generation manifest artifact is invalid.";
					return false;
				}
				const std::filesystem::path relative = StringUtility::ToPath(value.at("path").get<std::string>()).lexically_normal();
				if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory() || std::any_of(relative.begin(), relative.end(), [](const std::filesystem::path& part) { return part == L".."; })) {
					errorMessage = "Generation manifest artifact path is invalid.";
					return false;
				}
				if (std::any_of(output.artifacts.begin(), output.artifacts.end(), [&relative](const SolutionGenerationArtifact& artifact) {
					return CompareStringOrdinal(artifact.relativePath.lexically_normal().c_str(), -1, relative.lexically_normal().c_str(), -1, TRUE) == CSTR_EQUAL;
				})) {
					errorMessage = "Generation manifest artifact path is duplicated.";
					return false;
				}
				output.artifacts.push_back({ relative, value.at("contentHash").get<std::string>() });
			}
		} catch (const json::exception&) {
			errorMessage = "Generation manifest JSON could not be parsed.";
			return false;
		}
		return true;
	}

	std::filesystem::path FixedManifestRelativePath(SolutionGenerationLayout layout = SolutionGenerationLayout::ProjectFilesV2);
	// 保存済みoperation内の既知二候補だけ。二重配置を勝手に選ばない。
	std::filesystem::path PreviewManifestRelativePath(const std::filesystem::path& root) {
		std::error_code error;
		const auto oldPath = FixedManifestRelativePath(SolutionGenerationLayout::GroupedV1);
		const auto newPath = FixedManifestRelativePath();
		const bool oldExists = std::filesystem::exists(root / oldPath, error);
		if (error) return {};
		const bool newExists = std::filesystem::exists(root / newPath, error);
		if (error || oldExists == newExists) return {};
		return newExists ? newPath : oldPath;
	}

	// descriptorのstaging書出先（root直下）と衝突せず、正式artifactにも含めない。
	std::filesystem::path PreviewOriginRelativePath(const std::filesystem::path& root) {
		const auto manifestPath = PreviewManifestRelativePath(root);
		return manifestPath.empty() ? std::filesystem::path{} : manifestPath.parent_path() / L"preview-origin.json";
	}

	bool MatchesPreviewOwner(const ProjectSolutionPreviewSnapshot& preview, const ProjectDescriptor& descriptor) {
		return preview.projectId == descriptor.GetProjectId() && !preview.descriptorPath.empty() &&
			preview.descriptorPath.is_absolute() && SamePath(preview.descriptorPath, descriptor.GetDescriptorPath());
	}

	// 復元時だけ出力pathを採用する。失敗時にSnapshotの所有元を部分更新しない。
	bool ReadPreviewOrigin(const ProjectSolutionPreviewSnapshot& preview, std::filesystem::path& descriptorPath, std::string& errorMessage) {
		const std::filesystem::path path = preview.stagingRoot / PreviewOriginRelativePath(preview.stagingRoot);
		std::error_code error;
		std::string content;
		if (preview.stagingRoot.empty() || !IsWithin(path, preview.stagingRoot) || HasReparsePointInExistingPath(path) ||
			!std::filesystem::is_regular_file(path, error) || error || !ReadFile(path, content)) {
			errorMessage = "Preview origin is missing or unsafe. Generate Preview again.";
			return false;
		}
		try {
			const json origin = json::parse(content);
			if (!origin.is_object() || origin.size() != 5 || !origin.contains("schemaVersion") ||
				!origin.at("schemaVersion").is_number_integer() || origin.at("schemaVersion") != 1) {
				errorMessage = "Preview origin schema is invalid. Generate Preview again.";
				return false;
			}
			for (const char* field : { "projectId", "operationId", "descriptorPath", "inputIdentity" }) {
				if (!origin.contains(field) || !origin.at(field).is_string() || origin.at(field).get<std::string>().empty()) {
					errorMessage = "Preview origin metadata is invalid. Generate Preview again.";
					return false;
				}
			}
			const std::string ownerPath = origin.at("descriptorPath").get<std::string>();
			const std::filesystem::path normalizedOwner = StringUtility::ToPath(ownerPath).lexically_normal();
			if (ownerPath.find('\0') != std::string::npos || !normalizedOwner.is_absolute() ||
				origin.at("projectId").get<std::string>() != preview.projectId ||
				origin.at("operationId").get<std::string>() != preview.operationId || !IsOperationId(preview.operationId) ||
				origin.at("inputIdentity").get<std::string>() != preview.inputIdentity ||
				StringUtility::ToUtf8(preview.stagingRoot.filename()) != preview.operationId ||
				StringUtility::ToUtf8(preview.stagingRoot.parent_path().filename()) != preview.projectId) {
				errorMessage = "Preview origin does not match this operation. Generate Preview again.";
				return false;
			}
			descriptorPath = normalizedOwner;
		} catch (const json::exception&) {
			errorMessage = "Preview origin JSON is invalid. Generate Preview again.";
			return false;
		}
		return true;
	}

	// 完成したcopyの後に一度だけ保存する。既存Previewへ所有元を推定して追記しない。
	bool WritePreviewOrigin(const ProjectSolutionPreviewSnapshot& preview, std::string& errorMessage) {
		const std::filesystem::path path = preview.stagingRoot / PreviewOriginRelativePath(preview.stagingRoot);
		std::error_code error;
		if (preview.descriptorPath.empty() || !preview.descriptorPath.is_absolute() ||
			!IsWithin(path, preview.stagingRoot) || HasReparsePointInExistingPath(path) ||
			std::filesystem::exists(path, error) || error) {
			errorMessage = "Preview origin cannot be written safely.";
			return false;
		}
		const json origin = { { "schemaVersion", 1 }, { "projectId", preview.projectId },
			{ "operationId", preview.operationId }, { "descriptorPath", StringUtility::ToUtf8(preview.descriptorPath) },
			{ "inputIdentity", preview.inputIdentity } };
		const std::string content = origin.dump(2) + "\n";
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if (!output.is_open()) { errorMessage = "Preview origin could not be opened."; return false; }
		output.write(content.data(), static_cast<std::streamsize>(content.size()));
		output.flush();
		const bool written = output.good();
		output.close();
		if (!written || output.fail()) { errorMessage = "Preview origin could not be written."; return false; }
		return true;
	}

	bool VerifyPreview(const ProjectSolutionPreviewSnapshot& preview, std::string& errorMessage) {
		if (preview.stagingRoot.empty() || HasReparsePointInExistingPath(preview.stagingRoot)) { errorMessage = "Preview staging root is unsafe."; return false; }
		// 表示後のmetadata変更も拒否する。空の所有元をProject IDだけで補完しない。
		std::filesystem::path ownerPath;
		if (preview.descriptorPath.empty() || !preview.descriptorPath.is_absolute()) {
			errorMessage = "Preview owner is unavailable. Generate Preview again.";
			return false;
		}
		if (!ReadPreviewOrigin(preview, ownerPath, errorMessage)) return false;
		if (!SamePath(ownerPath, preview.descriptorPath)) {
			errorMessage = "Preview owner has changed. Generate Preview again.";
			return false;
		}
		// IDE生成後の設定copy失敗を再起動後も拒否する。originが有効なら旧設定配置も読める。
		if (HasReparsePointInExistingPath(preview.stagingRoot / L"config" / L"project.build.json") ||
			HasReparsePointInExistingPath(preview.stagingRoot / L"project" / L"build" / L"project.build.json")) {
			errorMessage = "Preview build specification path is unsafe.";
			return false;
		}
		std::filesystem::path specificationPath;
		if (!ProjectBuildSpecification::ResolvePath(preview.stagingRoot, specificationPath, errorMessage)) {
			errorMessage = "Preview build specification is invalid: " + errorMessage;
			return false;
		}
		std::error_code error;
		// ResolvePathは両候補欠落でも旧pathを返すが、完成したPreviewには仕様が必須。
		if (!IsWithin(specificationPath, preview.stagingRoot) || !std::filesystem::is_regular_file(specificationPath, error) || error) {
			errorMessage = "Preview build specification is missing or unreadable.";
			return false;
		}
		ProjectBuildSpecification specification{};
		if (!specification.Load(specificationPath, errorMessage)) {
			errorMessage = "Preview build specification is invalid: " + errorMessage;
			return false;
		}
		for (const SolutionGenerationArtifact& artifact : preview.artifacts) {
			const std::filesystem::path path = preview.stagingRoot / artifact.relativePath;
			if (!IsWithin(path, preview.stagingRoot) || HasReparsePointInExistingPath(path) || !std::filesystem::is_regular_file(path, error) || error || !MatchesHash(path, artifact.contentHash)) { errorMessage = "Preview artifact hash verification failed."; return false; }
		}
		const auto manifestPath = PreviewManifestRelativePath(preview.stagingRoot);
		if (manifestPath.empty() || HasReparsePointInExistingPath(preview.stagingRoot / manifestPath)) { errorMessage = "Preview manifest is missing or ambiguous."; return false; }
		const auto artifactDirectory = manifestPath == FixedManifestRelativePath() ? preview.stagingRoot / L"intermediate/project-files" : preview.stagingRoot / L"project/build/generated" / StringUtility::ToPath(preview.projectId);
		if (preview.artifacts.size() != 8 || std::any_of(preview.artifacts.begin(), preview.artifacts.end(), [&preview, &artifactDirectory](const SolutionGenerationArtifact& artifact) { return !IsWithin(preview.stagingRoot / artifact.relativePath, artifactDirectory); })) { errorMessage = "Preview artifact layout is unsupported."; return false; }
		ProjectSolutionPreviewSnapshot diskManifest{};
		if (!ReadPreviewManifest(preview.stagingRoot / manifestPath, diskManifest, errorMessage) || diskManifest.inputIdentity != preview.inputIdentity || diskManifest.artifacts.size() != preview.artifacts.size()) { if (errorMessage.empty()) errorMessage = "Preview manifest metadata has changed."; return false; }
		for (const auto& artifact : preview.artifacts) {
			if (std::none_of(diskManifest.artifacts.begin(), diskManifest.artifacts.end(), [&artifact](const SolutionGenerationArtifact& other) { return other.relativePath == artifact.relativePath && other.contentHash == artifact.contentHash; })) { errorMessage = "Preview manifest has changed."; return false; }
		}
		if (manifestPath == FixedManifestRelativePath()) {
			ProjectDescriptor local{};
			const auto path = preview.stagingRoot / L"game.project.json";
			if (HasReparsePointInExistingPath(path) || !std::filesystem::is_regular_file(path, error) || error || !local.Load(path, errorMessage) || local.GetProjectId() != preview.projectId ||
				local.GetPaths().solution != "intermediate/project-files/" + preview.projectId + ".sln" || local.GetPaths().msbuildProject != "intermediate/project-files/" + preview.projectId + ".vcxproj" ||
				HasReparsePointInExistingPath(preview.stagingRoot / L"project/resources") || !std::filesystem::is_directory(preview.stagingRoot / L"project/resources", error) || error) { errorMessage = "V2 Preview descriptor or resources are invalid."; return false; }
		}
		return !preview.solutionPath.empty() && std::filesystem::is_regular_file(preview.solutionPath);
	}

	bool WriteFile(const std::filesystem::path& path, const std::string& content, std::string& errorMessage) {
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);
		if (error) {
			errorMessage = "Staged descriptor directory could not be created.";
			return false;
		}
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if (!output.is_open()) {
			errorMessage = "Staged descriptor could not be opened.";
			return false;
		}
		output.write(content.data(), static_cast<std::streamsize>(content.size()));
		output.flush();
		if (!output.good()) {
			errorMessage = "Staged descriptor could not be written.";
			return false;
		}
		return true;
	}

	bool SameRelativePath(const std::filesystem::path& left, const std::filesystem::path& right) {
		return CompareStringOrdinal(left.lexically_normal().c_str(), -1, right.lexically_normal().c_str(), -1, TRUE) == CSTR_EQUAL;
	}

	bool IsSafeRelativePath(const std::filesystem::path& path) {
		return !path.empty() && !path.is_absolute() && !path.has_root_name() && !path.has_root_directory() &&
			std::none_of(path.begin(), path.end(), [](const std::filesystem::path& part) { return part == L".."; });
	}

	std::filesystem::path FixedManifestRelativePath(SolutionGenerationLayout layout) {
		return layout == SolutionGenerationLayout::ProjectFilesV2 ? std::filesystem::path(L"intermediate/project-files/solution-generation.json") : std::filesystem::path(L"project/build/generated/solution-generation.json");
	}

	std::filesystem::path LegacyArtifactDirectory(const ProjectDescriptor& descriptor) {
		return (descriptor.GetProjectRoot() / L"project").lexically_normal();
	}

	std::filesystem::path GroupedArtifactDirectory(const ProjectDescriptor& descriptor) {
		return (descriptor.GetProjectRoot() / L"project" / L"build" / L"generated" / StringUtility::ToPath(descriptor.GetProjectId())).lexically_normal();
	}

	std::filesystem::path ToProjectRelative(const std::filesystem::path& root, const std::filesystem::path& path, std::string& errorMessage) {
		std::error_code error;
		const std::filesystem::path relative = std::filesystem::relative(path, root, error).lexically_normal();
		if (error || !IsSafeRelativePath(relative)) {
			errorMessage = "Project path could not be made relative safely.";
			return {};
		}
		return relative;
	}

	bool IsLegacyRootLayout(const ProjectDescriptor& descriptor) {
		const std::filesystem::path root = LegacyArtifactDirectory(descriptor);
		return SamePath(descriptor.ResolveProjectPath(descriptor.GetPaths().solution), root / StringUtility::ToPath(descriptor.GetProjectId() + ".sln")) &&
			SamePath(descriptor.ResolveProjectPath(descriptor.GetPaths().msbuildProject), root / StringUtility::ToPath(descriptor.GetProjectId() + ".vcxproj"));
	}

	bool IsGroupedLayout(const ProjectDescriptor& descriptor) {
		const std::filesystem::path root = GroupedArtifactDirectory(descriptor);
		return SamePath(descriptor.ResolveProjectPath(descriptor.GetPaths().solution), root / StringUtility::ToPath(descriptor.GetProjectId() + ".sln")) &&
			SamePath(descriptor.ResolveProjectPath(descriptor.GetPaths().msbuildProject), root / StringUtility::ToPath(descriptor.GetProjectId() + ".vcxproj"));
	}

	std::filesystem::path ProjectFilesDirectory(const ProjectDescriptor& descriptor) { return descriptor.GetProjectRoot() / L"intermediate/project-files"; }
	bool IsProjectFilesLayout(const ProjectDescriptor& descriptor) {
		const auto root = ProjectFilesDirectory(descriptor);
		return SamePath(descriptor.ResolveProjectPath(descriptor.GetPaths().solution), root / StringUtility::ToPath(descriptor.GetProjectId() + ".sln")) && SamePath(descriptor.ResolveProjectPath(descriptor.GetPaths().msbuildProject), root / StringUtility::ToPath(descriptor.GetProjectId() + ".vcxproj"));
	}
	std::filesystem::path CanonicalManifestRelativePath(const ProjectDescriptor& descriptor) { return FixedManifestRelativePath(IsProjectFilesLayout(descriptor) ? SolutionGenerationLayout::ProjectFilesV2 : SolutionGenerationLayout::GroupedV1); }

	enum class CanonicalManifestState { Unambiguous, PendingRecovery, Invalid };

	// 診断と操作入口で同じ二重配置判定を使う。復旧自体は保存済みjournalの経路へ委ねる。
	CanonicalManifestState InspectCanonicalManifestSet(const ProjectDescriptor& descriptor, const ProjectRegistry& registry, std::string& errorMessage) {
		const auto root = descriptor.GetProjectRoot();
		std::error_code error;
		const bool oldExists = std::filesystem::exists(root / FixedManifestRelativePath(SolutionGenerationLayout::GroupedV1), error);
		if (error) { errorMessage = "Old generation manifest could not be inspected."; return CanonicalManifestState::Invalid; }
		const bool newExists = std::filesystem::exists(root / FixedManifestRelativePath(), error);
		if (error) { errorMessage = "New generation manifest could not be inspected."; return CanonicalManifestState::Invalid; }
		// V2 descriptorに旧manifestだけ残る状態も、初回採用で二重配置へ進めない。
		const bool unexpectedOldManifest = oldExists && IsProjectFilesLayout(descriptor);
		if (!(oldExists && newExists) && !unexpectedOldManifest) return CanonicalManifestState::Unambiguous;
		const bool matchingRecovery = std::any_of(registry.GetSolutionGenerationOperations().begin(), registry.GetSolutionGenerationOperations().end(), [&root, &descriptor](const SolutionGenerationOperation& operation) {
			return operation.projectId == descriptor.GetProjectId() && SamePath(operation.projectRoot, root) &&
				operation.state != SolutionGenerationOperationState::Complete && operation.state != SolutionGenerationOperationState::RolledBack &&
				std::any_of(operation.files.begin(), operation.files.end(), [](const SolutionGenerationOperationFile& file) {
					return file.kind == SolutionGenerationOperationFileKind::Manifest && file.relativePath == FixedManifestRelativePath(SolutionGenerationLayout::GroupedV1) && file.previousExists && !file.nextExists;
				}) &&
				std::any_of(operation.files.begin(), operation.files.end(), [](const SolutionGenerationOperationFile& file) {
					return file.kind == SolutionGenerationOperationFileKind::Manifest && file.relativePath == FixedManifestRelativePath() && !file.previousExists && file.nextExists;
				});
		});
		errorMessage = matchingRecovery ? "Recover the matching generation journal before starting a new generation operation." :
			newExists ? "Ambiguous old and new manifests without a matching recovery journal." : "Old generation manifest conflicts with the Project Files V2 descriptor without a matching recovery journal.";
		return matchingRecovery ? CanonicalManifestState::PendingRecovery : CanonicalManifestState::Invalid;
	}

	ProjectDescriptor MakeProjectFilesDescriptor(const ProjectDescriptor& descriptor) {
		ProjectDescriptor proposed = descriptor;
		ProjectDescriptorPaths paths = proposed.GetPaths();
		const std::string base = "intermediate/project-files/" + descriptor.GetProjectId();
		paths.solution = base + ".sln";
		paths.msbuildProject = base + ".vcxproj";
		proposed.SetPaths(paths);
		return proposed;
	}

	std::string SerializeDescriptor(const ProjectDescriptor& descriptor) {
		const ProjectDescriptorPaths& paths = descriptor.GetPaths();
		const ProjectDependencyPaths& dependencies = descriptor.GetDependencies();
		const ProjectEngineProvenance& provenance = descriptor.GetEngineProvenance();
		const ProjectDescriptorTemplate& descriptorTemplate = descriptor.GetTemplate();
		const json root = {
			{ "dependencies", { { "lock", dependencies.lock }, { "manifest", dependencies.manifest } } },
			{ "displayName", descriptor.GetDisplayName() },
			{ "engine", {
				{ "mode", descriptor.GetEngineMode() == ProjectEngineMode::Snapshot ? "snapshot" : "managed-source" },
				{ "provenance", { { "repository", provenance.repository }, { "revision", provenance.revision } } }
			} },
			{ "ide", { { "preferredVisualStudioMajor", descriptor.GetPreferredVisualStudioMajor() } } },
			{ "paths", {
				{ "developmentExecutable", paths.developmentExecutable },
				{ "msbuildProject", paths.msbuildProject },
				{ "sceneCatalog", paths.sceneCatalog },
				{ "solution", paths.solution }
			} },
			{ "projectId", descriptor.GetProjectId() },
			{ "schemaVersion", ProjectDescriptor::kSchemaVersion },
			{ "template", { { "id", descriptorTemplate.id }, { "schemaVersion", descriptorTemplate.schemaVersion }, { "sourceKind", descriptorTemplate.sourceKind } } }
		};
		return root.dump(2) + "\n";
	}

	bool ReadManifestFiles(const std::filesystem::path& manifestPath, std::vector<SolutionGenerationOperationFile>& output, std::string& errorMessage) {
		ProjectSolutionPreviewSnapshot snapshot{};
		if (!ReadPreviewManifest(manifestPath, snapshot, errorMessage)) {
			return false;
		}
		output.clear();
		for (const SolutionGenerationArtifact& artifact : snapshot.artifacts) {
			SolutionGenerationOperationFile file{};
			file.kind = SolutionGenerationOperationFileKind::Artifact;
			file.relativePath = artifact.relativePath;
			file.previousContentHash = artifact.contentHash;
			file.previousExists = true;
			file.nextExists = false;
			output.push_back(std::move(file));
		}
		return true;
	}

	bool CaptureOwnedArtifacts(const std::filesystem::path& projectRoot, std::vector<SolutionGenerationOperationFile>& files, uint32_t& modifiedCount, std::string& errorMessage) {
		modifiedCount = 0;
		if (HasReparsePointInExistingPath(projectRoot)) {
			errorMessage = "Canonical artifact root is unsafe.";
			return false;
		}
		for (SolutionGenerationOperationFile& file : files) {
			const std::filesystem::path target = projectRoot / file.relativePath;
			std::error_code filesystemError;
			if (!IsWithin(target, projectRoot) || HasReparsePointInExistingPath(target) || !std::filesystem::is_regular_file(target, filesystemError) || filesystemError) {
				errorMessage = "Canonical owned artifact is missing or unsafe.";
				return false;
			}
			std::string content;
			if (!ReadFile(target, content)) {
				errorMessage = "Canonical owned artifact could not be read.";
				return false;
			}
			const std::string currentHash = HashContent(content);
			if (currentHash != file.previousContentHash) ++modifiedCount;
			file.previousContentHash = currentHash;
		}
		return true;
	}

	bool ContainsArtifact(const std::vector<SolutionGenerationArtifact>& artifacts, const std::filesystem::path& relativePath) {
		return std::any_of(artifacts.begin(), artifacts.end(), [&relativePath](const SolutionGenerationArtifact& artifact) {
			return SameRelativePath(artifact.relativePath, relativePath);
		});
	}

	bool VerifyGroupedDirectory(const ProjectDescriptor& descriptor, std::string& errorMessage) {
		ProjectSolutionPreviewSnapshot manifest{};
		if (!ReadPreviewManifest(descriptor.GetProjectRoot() / CanonicalManifestRelativePath(descriptor), manifest, errorMessage)) return false;
		const std::filesystem::path groupedDirectory = IsProjectFilesLayout(descriptor) ? ProjectFilesDirectory(descriptor) : GroupedArtifactDirectory(descriptor);
		std::error_code error;
		if (!std::filesystem::is_directory(groupedDirectory, error) || error || HasReparsePointInExistingPath(groupedDirectory)) {
			errorMessage = "Grouped artifact directory is unsafe or missing.";
			return false;
		}
		std::filesystem::recursive_directory_iterator iterator(groupedDirectory, std::filesystem::directory_options::none, error);
		const std::filesystem::recursive_directory_iterator end;
		while (!error && iterator != end) {
			const std::filesystem::path path = iterator->path();
			if (IsReparsePoint(path)) {
				errorMessage = "Grouped artifact directory contains a reparse point.";
				return false;
			}
			if (iterator->is_directory(error)) {
				iterator.increment(error);
				continue;
			}
			if (error || !iterator->is_regular_file(error)) {
				errorMessage = "Grouped artifact directory contains an invalid entry.";
				return false;
			}
			const std::filesystem::path relative = std::filesystem::relative(path, descriptor.GetProjectRoot(), error).lexically_normal();
			if (error || (!ContainsArtifact(manifest.artifacts, relative) && relative != CanonicalManifestRelativePath(descriptor))) {
				errorMessage = "Grouped artifact directory contains an unknown file.";
				return false;
			}
			iterator.increment(error);
		}
		if (error) {
			errorMessage = "Grouped artifact directory could not be enumerated.";
			return false;
		}
		return true;
	}

	bool VerifyCanonicalGeneration(const ProjectDescriptor& descriptor, std::string& errorMessage) {
		std::vector<SolutionGenerationOperationFile> owned;
		uint32_t modified = 0;
		if (!ReadManifestFiles(descriptor.GetProjectRoot() / CanonicalManifestRelativePath(descriptor), owned, errorMessage) ||
			!CaptureOwnedArtifacts(descriptor.GetProjectRoot(), owned, modified, errorMessage)) return false;
		if (modified != 0) { errorMessage = "GeneratedFileModified: canonical generation no longer matches its manifest."; return false; }
		return VerifyGroupedDirectory(descriptor, errorMessage);
	}
	bool HasUnknownGroupedTargetFile(const ProjectDescriptor& descriptor, const ProjectSolutionPreviewSnapshot&, std::string& errorMessage) {
		const std::filesystem::path groupedDirectory = ProjectFilesDirectory(descriptor);
		std::error_code error;
		if (!std::filesystem::exists(groupedDirectory, error)) {
			return static_cast<bool>(error);
		}
		if (error || !std::filesystem::is_directory(groupedDirectory, error) || HasReparsePointInExistingPath(groupedDirectory)) {
			errorMessage = "Grouped artifact directory is unsafe.";
			return true;
		}
		std::filesystem::recursive_directory_iterator iterator(groupedDirectory, std::filesystem::directory_options::none, error);
		const std::filesystem::recursive_directory_iterator end;
		while (!error && iterator != end) {
			const std::filesystem::directory_entry entry = *iterator;
			if (IsReparsePoint(entry.path())) {
				errorMessage = "Grouped artifact directory contains a reparse point.";
				return true;
			}
			if (!entry.is_regular_file(error)) {
				iterator.increment(error);
				continue;
			}
			errorMessage = "Grouped artifact directory is not empty.";
			return true;
		}
		if (error) {
			errorMessage = "Grouped artifact directory could not be enumerated.";
			return true;
		}
		return false;
	}
}

ProjectSolutionGenerationService::ProjectSolutionGenerationService(ProjectRegistry& registry) : registry_(registry) {
}

bool ProjectSolutionGenerationService::InspectProject(const std::filesystem::path& descriptorPath, ProjectSolutionPreviewSnapshot& output, std::string& errorMessage) const {
	output = {};
	ProjectDescriptor descriptor{};
	if (!descriptor.Load(descriptorPath, errorMessage)) return false;
	output.projectId = descriptor.GetProjectId();
	output.descriptorPath = descriptor.GetDescriptorPath();
	if (descriptor.GetEngineMode() != ProjectEngineMode::Snapshot) {
		output.detail = "Unsupported Managed Source: dependency active view is not implemented.";
		return true;
	}
	output.legacyArtifactDirectory = descriptor.ResolveProjectPath(descriptor.GetPaths().solution).parent_path();
	output.groupedArtifactDirectory = ProjectFilesDirectory(descriptor);
	output.layoutMigrationRequired = !IsProjectFilesLayout(descriptor);
	std::filesystem::path specificationPath;
	if (!ProjectBuildSpecification::ResolvePath(descriptor.GetProjectRoot(), specificationPath, errorMessage)) return false;
	if (!std::filesystem::is_regular_file(specificationPath)) {
		output.detail = "Legacy Manual Solution: project.build.json is unavailable.";
		return true;
	}
	ProjectBuildSpecification specification{};
	SolutionGenerationModel model{};
	SolutionGenerationModelBuilder builder{};
	if (!ValidateProjectFilesSource(descriptor, errorMessage) || !specification.Load(specificationPath, errorMessage) || !builder.Build(descriptor, specification, model, errorMessage)) return false;
	if (!SamePath(descriptor.GetDescriptorPath(), descriptor.GetProjectRoot() / L"game.project.json") || HasReparsePointInExistingPath(descriptor.GetDescriptorPath())) { errorMessage = "V2 requires the canonical game.project.json descriptor."; return false; }
	const CanonicalManifestState manifestState = InspectCanonicalManifestSet(descriptor, registry_, errorMessage);
	if (manifestState == CanonicalManifestState::Invalid) return false;
	if (manifestState == CanonicalManifestState::PendingRecovery) {
		output.detail = errorMessage;
		errorMessage.clear();
		return true;
	}
	output.state = ProjectSolutionPreviewState::Ready;
	const std::filesystem::path manifestPath = model.projectRoot / CanonicalManifestRelativePath(descriptor);
	if (!std::filesystem::exists(manifestPath)) output.detail = "Preview ready. Canonical generation manifest is unavailable.";
	else {
		std::vector<SolutionGenerationOperationFile> oldFiles;
		if (!ReadManifestFiles(manifestPath, oldFiles, errorMessage) || !CaptureOwnedArtifacts(model.projectRoot, oldFiles, output.modifiedOwnedArtifactCount, errorMessage)) {
			errorMessage.clear();
			output.detail = "Generated File Modified: canonical artifact could not be captured safely.";
		} else {
			output.retiredArtifactCount = output.layoutMigrationRequired ? static_cast<uint32_t>(oldFiles.size()) : 0u;
			if (output.modifiedOwnedArtifactCount != 0) {
				output.detail = "Generated Drift: " + std::to_string(output.modifiedOwnedArtifactCount) + " owned artifacts differ from the manifest.";
			} else {
				output.detail = output.layoutMigrationRequired ? "Generated Clean. Project Files V2 migration requires a verified Preview." : "Generated Clean.";
			}
		}
	}
	return true;
}

bool ProjectSolutionGenerationService::CreatePreview(const std::filesystem::path& descriptorPath, ProjectSolutionPreviewSnapshot& output, std::string& errorMessage) {
	ProjectSolutionPreviewSnapshot inspected{};
	if (!InspectProject(descriptorPath, inspected, errorMessage)) return false;
	if (inspected.state != ProjectSolutionPreviewState::Ready) { output = inspected; return true; }
	ProjectDescriptor descriptor{};
	ProjectBuildSpecification specification{};
	SolutionGenerationModel model{};
	SolutionGenerationModelBuilder builder{};
	if (!descriptor.Load(descriptorPath, errorMessage)) return false;
	std::filesystem::path specificationPath;
	if (!ProjectBuildSpecification::ResolvePath(descriptor.GetProjectRoot(), specificationPath, errorMessage)) return false;
	if (!ValidateProjectFilesSource(descriptor, errorMessage) || !specification.Load(specificationPath, errorMessage) || !builder.Build(descriptor, specification, model, errorMessage)) return false;

	if (InspectCanonicalManifestSet(descriptor, registry_, errorMessage) != CanonicalManifestState::Unambiguous) return false;
	output = inspected;
	output.operationId = MakeOperationId();
	output.stagingRoot = GetPreviewRoot() / StringUtility::ToPath(output.projectId) / StringUtility::ToPath(output.operationId);
	std::error_code filesystemError;
	std::filesystem::create_directories(output.stagingRoot, filesystemError);
	if (filesystemError || HasReparsePointInExistingPath(output.stagingRoot) || !std::filesystem::is_empty(output.stagingRoot, filesystemError) || filesystemError) {
		errorMessage = "Preview staging root could not be created safely.";
		output.state = ProjectSolutionPreviewState::Failed;
		output.detail = errorMessage;
		UpsertPreview(output);
		return false;
	}
	SolutionGenerationEmitter emitter{};
	SolutionGenerationPreview preview{};
	if (!emitter.EmitPreview(model, output.stagingRoot, preview, errorMessage) ||
		!CopyProjectTree(model.sourceDirectory, output.stagingRoot / L"project", errorMessage) ||
		!CopyPreviewBuildSpecification(specification.GetPath(), model.projectRoot, output.stagingRoot, errorMessage) ||
		!WriteFile(output.stagingRoot / L"game.project.json", SerializeDescriptor(MakeProjectFilesDescriptor(descriptor)), errorMessage)) {
		output.state = ProjectSolutionPreviewState::Failed;
		output.detail = errorMessage;
		UpsertPreview(output);
		return false;
	}
	output.stagingRoot = preview.stagingRoot;
	output.inputIdentity = preview.inputIdentity;
	output.artifacts = std::move(preview.artifacts);
	for (const SolutionGenerationArtifact& artifact : output.artifacts) {
		if (artifact.relativePath.extension() == L".sln") {
			output.solutionPath = output.stagingRoot / artifact.relativePath;
			break;
		}
	}
	if (!WritePreviewOrigin(output, errorMessage) || !VerifyPreview(output, errorMessage)) {
		output.state = ProjectSolutionPreviewState::Failed;
		output.detail = errorMessage.empty() ? "Preview Solution is unavailable." : errorMessage;
		UpsertPreview(output);
		return false;
	}
	output.state = ProjectSolutionPreviewState::PreviewReady;
	output.detail = "Preview Ready - Not Built.";
	UpsertPreview(output);
	return true;
}

bool ProjectSolutionGenerationService::CreateInitialGroupedGeneration(
	const ProjectDescriptor& descriptor,
	const std::string& operationId,
	const std::vector<std::filesystem::path>& retiredLegacyArtifacts,
	std::string& errorMessage,
	SolutionGenerationLayout outputLayout
) {
	if (operationId.empty() || descriptor.GetProjectRoot().empty() || (outputLayout == SolutionGenerationLayout::GroupedV1 ? !IsGroupedLayout(descriptor) : !IsProjectFilesLayout(descriptor)) || !descriptor.Validate(errorMessage)) {
		if (errorMessage.empty()) errorMessage = "Initial IDE generation request is invalid.";
		return false;
	}
	if (outputLayout != SolutionGenerationLayout::GroupedV1 && outputLayout != SolutionGenerationLayout::ProjectFilesV2) { errorMessage = "Initial output layout is unsupported."; return false; }
	if (outputLayout == SolutionGenerationLayout::ProjectFilesV2 && !ValidateProjectFilesSource(descriptor, errorMessage)) return false;
	if (retiredLegacyArtifacts.size() != 3) {
		errorMessage = "Initial IDE generation requires exactly three legacy artifacts.";
		return false;
	}
	for (const std::filesystem::path& relativePath : retiredLegacyArtifacts) {
		if (!IsSafeRelativePath(relativePath)) {
			errorMessage = "Initial IDE retirement path is unsafe.";
			return false;
		}
	}
	const std::filesystem::path projectRoot = descriptor.GetProjectRoot().lexically_normal();
	for (const SolutionGenerationOperation& operation : registry_.GetSolutionGenerationOperations()) {
		if (operation.projectRoot == projectRoot && operation.operationId != operationId &&
			operation.state != SolutionGenerationOperationState::Complete && operation.state != SolutionGenerationOperationState::RolledBack) {
			errorMessage = "Project already has a pending solution generation operation.";
			return false;
		}
	}

	SolutionGenerationTransaction transaction{};
	if (const SolutionGenerationOperation* existing = registry_.FindSolutionGenerationOperation(operationId)) {
		if (existing->projectRoot != projectRoot || existing->projectId != descriptor.GetProjectId()) {
			errorMessage = "Initial IDE generation operation does not match the Project.";
			return false;
		}
		// 再開では保存されたlayoutを優先し、別layoutの要求でcommitを始めない。
		if (std::none_of(existing->files.begin(), existing->files.end(), [outputLayout](const SolutionGenerationOperationFile& file) {
			return file.kind == SolutionGenerationOperationFileKind::Manifest && file.nextExists && file.relativePath == FixedManifestRelativePath(outputLayout);
		})) { errorMessage = "Initial generation journal layout does not match the creation request."; return false; }
		if (existing->state == SolutionGenerationOperationState::Complete) {
			// 現在manifestとartifactが同時に変わっていても、保存済みjournalの完成集合を基準にする。
			if (transaction.Recover(operationId, registry_, errorMessage) != SolutionGenerationRecoveryResult::Completed) {
				if (errorMessage.empty()) errorMessage = "Completed initial generation no longer matches its journal.";
				return false;
			}
			if (!VerifyCanonicalGeneration(descriptor, errorMessage)) {
				if (errorMessage.empty()) errorMessage = "Completed IDE generation could not be verified.";
				return false;
			}
			return true;
		}
		if (existing->state == SolutionGenerationOperationState::Staged) {
			if (!transaction.Commit(operationId, registry_, errorMessage)) return false;
			if (!VerifyCanonicalGeneration(descriptor, errorMessage)) {
				if (errorMessage.empty()) errorMessage = "Staged IDE generation could not be verified.";
				return false;
			}
			return true;
		} else if (existing->state == SolutionGenerationOperationState::CommitInProgress ||
			existing->state == SolutionGenerationOperationState::RollbackInProgress ||
			existing->state == SolutionGenerationOperationState::RecoveryRequired) {
			const SolutionGenerationRecoveryResult result = transaction.Recover(operationId, registry_, errorMessage);
			if (result == SolutionGenerationRecoveryResult::RecoveryRequired || result == SolutionGenerationRecoveryResult::NoOperation) {
				if (errorMessage.empty()) errorMessage = "Initial IDE generation requires recovery.";
				return false;
			}
			if (result == SolutionGenerationRecoveryResult::Completed) {
				if (!VerifyCanonicalGeneration(descriptor, errorMessage)) {
					if (errorMessage.empty()) errorMessage = "Recovered IDE generation could not be verified.";
					return false;
				}
				return true;
			}
		}
	}
	const std::filesystem::path stagingRoot = GetPreviewRoot() / L"creation" / StringUtility::ToPath(descriptor.GetProjectId()) /
		StringUtility::ToPath(operationId + "-" + MakeOperationId());
	std::error_code filesystemError;
	std::filesystem::create_directories(stagingRoot, filesystemError);
	if (filesystemError || HasReparsePointInExistingPath(stagingRoot) || !std::filesystem::is_empty(stagingRoot, filesystemError) || filesystemError) {
		errorMessage = "Initial IDE staging root could not be created safely.";
		return false;
	}

	SolutionGenerationModel model{};
	if (!BuildModel(descriptor, model, errorMessage, outputLayout)) return false;
	SolutionGenerationEmitter emitter{};
	SolutionGenerationPreview preview{};
	if (!emitter.EmitPreview(model, stagingRoot, preview, errorMessage) || preview.artifacts.size() != 8) {
		if (errorMessage.empty()) errorMessage = "Initial IDE artifact generation failed.";
		return false;
	}
	ProjectDescriptor stagedDescriptor = descriptor;
	stagedDescriptor.SetDescriptorPath(stagingRoot / L"game.project.json");
	if (!stagedDescriptor.Save(errorMessage)) return false;

	SolutionGenerationTransactionRequest request{};
	request.operationId = operationId;
	request.projectId = descriptor.GetProjectId();
	request.projectRoot = projectRoot;
	request.preview.stagingRoot = stagingRoot;
	request.preview.manifestPath = FixedManifestRelativePath(outputLayout);
	request.preview.inputIdentity = preview.inputIdentity;
	request.preview.artifacts = preview.artifacts;
	for (const SolutionGenerationArtifact& artifact : preview.artifacts) {
		request.files.push_back({ SolutionGenerationOperationFileKind::Artifact, artifact.relativePath, {}, artifact.contentHash, false, true });
	}
	const std::filesystem::path descriptorRelativePath = L"game.project.json";
	std::string descriptorContent;
	if (!ReadFile(stagingRoot / descriptorRelativePath, descriptorContent)) {
		errorMessage = "Initial IDE descriptor could not be read from staging.";
		return false;
	}
	request.files.push_back({ SolutionGenerationOperationFileKind::Descriptor, descriptorRelativePath, {}, HashContent(descriptorContent), false, true });
	for (const std::filesystem::path& relativePath : retiredLegacyArtifacts) {
		const std::filesystem::path target = projectRoot / relativePath;
		std::string content;
		std::error_code fileError;
		if (!std::filesystem::is_regular_file(target, fileError) || fileError || !ReadFile(target, content)) {
			errorMessage = "Initial IDE legacy artifact is missing or unreadable.";
			return false;
		}
		request.files.push_back({ SolutionGenerationOperationFileKind::Artifact, relativePath, HashContent(content), {}, true, false });
	}
	std::string manifestContent;
	if (!ReadFile(stagingRoot / request.preview.manifestPath, manifestContent)) {
		errorMessage = "Initial IDE manifest could not be read from staging.";
		return false;
	}
	request.files.push_back({ SolutionGenerationOperationFileKind::Manifest, request.preview.manifestPath, {}, HashContent(manifestContent), false, true });
	if (!transaction.Stage(request, registry_, errorMessage) || !transaction.Commit(operationId, registry_, errorMessage)) return false;

	if (!VerifyCanonicalGeneration(descriptor, errorMessage)) {
		if (errorMessage.empty()) errorMessage = "Initial IDE generation verification failed.";
		return false;
	}
	for (const std::filesystem::path& relativePath : retiredLegacyArtifacts) {
		if (std::filesystem::exists(projectRoot / relativePath)) {
			errorMessage = "Initial IDE legacy artifact was not retired.";
			return false;
		}
	}
	return true;
}

const ProjectSolutionPreviewSnapshot* ProjectSolutionGenerationService::FindPreferredPreview(
	const std::filesystem::path& descriptorPath, bool& matchesCurrentInput, std::string& errorMessage) const {
	matchesCurrentInput = false;
	errorMessage.clear();
	ProjectDescriptor descriptor{};
	SolutionGenerationModel model{};
	if (!BuildCurrentModel(descriptorPath, descriptor, model, errorMessage)) return nullptr;
	SolutionGenerationEmitter emitter{};
	const std::string currentIdentity = emitter.ComputeInputIdentity(model);
	const ProjectSolutionPreviewSnapshot* stalePreview = nullptr;
	for (auto iterator = previews_.rbegin(); iterator != previews_.rend(); ++iterator) {
		const ProjectSolutionPreviewSnapshot& preview = *iterator;
		if (preview.state != ProjectSolutionPreviewState::PreviewReady || !MatchesPreviewOwner(preview, descriptor)) continue;
		std::string verificationError;
		if (!VerifyPreview(preview, verificationError)) continue;
		if (preview.inputIdentity == currentIdentity) {
			matchesCurrentInput = true;
			return &preview;
		}
		if (!stalePreview) stalePreview = &preview;
	}
	if (stalePreview) errorMessage = "Preview input no longer matches the current Project.";
	return stalePreview;
}

bool ProjectSolutionGenerationService::CanRegeneratePreview(
	const std::filesystem::path& descriptorPath, const std::string& operationId, std::string& errorMessage) const {
	errorMessage.clear();
	ProjectDescriptor descriptor{};
	SolutionGenerationModel model{};
	if (!BuildCurrentModel(descriptorPath, descriptor, model, errorMessage)) return false;
	if (descriptor.GetEngineMode() != ProjectEngineMode::Snapshot || !IsProjectFilesLayout(descriptor)) {
		errorMessage = "Regenerate requires a Snapshot Project using Project Files V2 output layout.";
		return false;
	}
	if (std::none_of(registry_.GetProjects().begin(), registry_.GetProjects().end(), [&descriptor](const ProjectRegistryEntry& entry) {
		return SamePath(entry.descriptorPath, descriptor.GetDescriptorPath());
	})) {
		errorMessage = "Project descriptor is not registered.";
		return false;
	}
	const auto preview = std::find_if(previews_.begin(), previews_.end(), [&operationId](const ProjectSolutionPreviewSnapshot& value) {
		return value.operationId == operationId && value.state == ProjectSolutionPreviewState::PreviewReady;
	});
	if (preview == previews_.end() || !MatchesPreviewOwner(*preview, descriptor) || !VerifyPreview(*preview, errorMessage)) {
		if (errorMessage.empty()) errorMessage = "Verified Preview is unavailable.";
		return false;
	}
	SolutionGenerationEmitter emitter{};
	if (preview->inputIdentity != emitter.ComputeInputIdentity(model)) {
		errorMessage = "Preview input no longer matches the current Project.";
		return false;
	}
	const std::filesystem::path manifestPath = model.projectRoot / CanonicalManifestRelativePath(descriptor);
	std::error_code filesystemError;
	if (HasReparsePointInExistingPath(manifestPath) || !std::filesystem::is_regular_file(manifestPath, filesystemError) || filesystemError) {
		errorMessage = "Canonical generation manifest is missing or unsafe.";
		return false;
	}
	std::vector<SolutionGenerationOperationFile> oldFiles;
	if (!ReadManifestFiles(manifestPath, oldFiles, errorMessage)) return false;
	if (oldFiles.size() != 8 || preview->artifacts.size() != 8 ||
		std::any_of(oldFiles.begin(), oldFiles.end(), [&model, &preview](const SolutionGenerationOperationFile& file) {
			return !IsSafeRelativePath(file.relativePath) || !IsWithin(model.projectRoot / file.relativePath, model.artifactDirectory) ||
				!ContainsArtifact(preview->artifacts, file.relativePath);
		}) || std::any_of(preview->artifacts.begin(), preview->artifacts.end(), [&model](const SolutionGenerationArtifact& artifact) {
			return !IsSafeRelativePath(artifact.relativePath) || !IsWithin(model.projectRoot / artifact.relativePath, model.artifactDirectory);
		})) {
		errorMessage = "Regenerate requires the same eight owned Project Files V2 artifact paths.";
		return false;
	}
	uint32_t modifiedOwnedArtifactCount = 0;
	if (!CaptureOwnedArtifacts(model.projectRoot, oldFiles, modifiedOwnedArtifactCount, errorMessage)) {
		errorMessage = "GeneratedFileModified: " + errorMessage;
		return false;
	}
	if (modifiedOwnedArtifactCount != 0) {
		errorMessage = "GeneratedFileModified: an owned artifact no longer matches its manifest.";
		return false;
	}
	if (registry_.FindSolutionGenerationOperation(operationId)) {
		errorMessage = "Preview operation has already been staged. Generate a new Preview.";
		return false;
	}
	if (std::any_of(registry_.GetSolutionGenerationOperations().begin(), registry_.GetSolutionGenerationOperations().end(), [&descriptor](const SolutionGenerationOperation& operation) {
		return operation.projectId == descriptor.GetProjectId() && operation.state != SolutionGenerationOperationState::Complete && operation.state != SolutionGenerationOperationState::RolledBack;
	})) {
		errorMessage = "Project already has a pending generation operation.";
		return false;
	}
	return registry_.Validate(errorMessage);
}

bool ProjectSolutionGenerationService::RegeneratePreview(
	const std::filesystem::path& descriptorPath, const std::string& operationId, std::string& errorMessage) {
	if (!CanRegeneratePreview(descriptorPath, operationId, errorMessage)) return false;
	ProjectDescriptor descriptor{};
	SolutionGenerationModel model{};
	if (!BuildCurrentModel(descriptorPath, descriptor, model, errorMessage)) return false;
	const auto preview = std::find_if(previews_.begin(), previews_.end(), [&operationId](const ProjectSolutionPreviewSnapshot& value) {
		return value.operationId == operationId && value.state == ProjectSolutionPreviewState::PreviewReady;
	});
	if (preview == previews_.end() || !MatchesPreviewOwner(*preview, descriptor)) { errorMessage = "Verified Preview for this Project is unavailable."; return false; }
	if (!VerifyPreview(*preview, errorMessage) || preview->inputIdentity != SolutionGenerationEmitter{}.ComputeInputIdentity(model)) { if (errorMessage.empty()) errorMessage = "Preview input no longer matches the current Project."; return false; }
	SolutionGenerationPreview transactionPreview{};
	transactionPreview.stagingRoot = preview->stagingRoot;
	transactionPreview.manifestPath = FixedManifestRelativePath();
	transactionPreview.inputIdentity = preview->inputIdentity;
	transactionPreview.artifacts = preview->artifacts;
	SolutionGenerationTransaction transaction{};
	const bool staged = transaction.Stage({ operationId, descriptor.GetProjectId(), model.projectRoot, std::move(transactionPreview) }, registry_, errorMessage);
	const bool committed = staged && transaction.Commit(operationId, registry_, errorMessage);
	RefreshRecoverySnapshots();
	return committed;
}

bool ProjectSolutionGenerationService::CanAdoptPreview(const std::filesystem::path& descriptorPath, const std::string& operationId, std::string& errorMessage) const {
	ProjectDescriptor descriptor{};
	SolutionGenerationModel model{};
	if (!BuildCurrentModel(descriptorPath, descriptor, model, errorMessage)) return false;
	if (!IsProjectFilesLayout(descriptor)) { errorMessage = "Use explicit layout migration for this Project."; return false; }
	if (std::none_of(registry_.GetProjects().begin(), registry_.GetProjects().end(), [&descriptor](const ProjectRegistryEntry& entry) { return SamePath(entry.descriptorPath, descriptor.GetDescriptorPath()); })) {
		errorMessage = "Project descriptor is not registered.";
		return false;
	}
	const auto preview = std::find_if(previews_.begin(), previews_.end(), [&operationId](const ProjectSolutionPreviewSnapshot& value) { return value.operationId == operationId && value.state == ProjectSolutionPreviewState::PreviewReady; });
	if (preview == previews_.end() || !MatchesPreviewOwner(*preview, descriptor) || !VerifyPreview(*preview, errorMessage)) {
		if (errorMessage.empty()) errorMessage = "Verified Preview is unavailable.";
		return false;
	}
	SolutionGenerationEmitter emitter{};
	if (preview->inputIdentity != emitter.ComputeInputIdentity(model)) { errorMessage = "Preview input no longer matches the current Project."; return false; }
	const std::filesystem::path manifestPath = model.projectRoot / CanonicalManifestRelativePath(descriptor);
	if (std::filesystem::exists(manifestPath)) { errorMessage = "Canonical generation already exists; use Regenerate instead."; return false; }
	if (std::any_of(registry_.GetSolutionGenerationOperations().begin(), registry_.GetSolutionGenerationOperations().end(), [&descriptor](const SolutionGenerationOperation& operation) {
		return operation.projectId == descriptor.GetProjectId() && operation.state != SolutionGenerationOperationState::Complete && operation.state != SolutionGenerationOperationState::RolledBack;
	})) { errorMessage = "Project already has a pending generation operation."; return false; }
	for (const SolutionGenerationArtifact& artifact : preview->artifacts) {
		if (!IsWithin(model.projectRoot / artifact.relativePath, model.projectRoot) || std::filesystem::exists(model.projectRoot / artifact.relativePath)) {
			errorMessage = "Canonical target is not empty for initial adoption.";
			return false;
		}
	}
	return true;
}

bool ProjectSolutionGenerationService::AdoptPreview(const std::filesystem::path& descriptorPath, const std::string& operationId, std::string& errorMessage) {
	if (!CanAdoptPreview(descriptorPath, operationId, errorMessage)) return false;
	ProjectDescriptor descriptor{};
	SolutionGenerationModel model{};
	if (!BuildCurrentModel(descriptorPath, descriptor, model, errorMessage)) return false;
	const auto preview = std::find_if(previews_.begin(), previews_.end(), [&operationId](const ProjectSolutionPreviewSnapshot& value) { return value.operationId == operationId; });
	if (preview == previews_.end() || !MatchesPreviewOwner(*preview, descriptor)) { errorMessage = "Verified Preview for this Project is unavailable."; return false; }
	if (!VerifyPreview(*preview, errorMessage) || preview->inputIdentity != SolutionGenerationEmitter{}.ComputeInputIdentity(model)) { if (errorMessage.empty()) errorMessage = "Preview input no longer matches the current Project."; return false; }
	SolutionGenerationPreview transactionPreview{};
	transactionPreview.stagingRoot = preview->stagingRoot;
	transactionPreview.manifestPath = FixedManifestRelativePath();
	transactionPreview.inputIdentity = preview->inputIdentity;
	transactionPreview.artifacts = preview->artifacts;
	SolutionGenerationTransaction transaction{};
	const bool staged = transaction.Stage({ operationId, descriptor.GetProjectId(), model.projectRoot, std::move(transactionPreview) }, registry_, errorMessage);
	const bool committed = staged && transaction.Commit(operationId, registry_, errorMessage);
	RefreshRecoverySnapshots();
	return committed;
}

bool ProjectSolutionGenerationService::CanMigrateOutputLayout(const std::filesystem::path& descriptorPath, const std::string& operationId, std::string& errorMessage) const {
	ProjectDescriptor descriptor{};
	SolutionGenerationModel model{};
	if (!BuildCurrentModel(descriptorPath, descriptor, model, errorMessage)) return false;
	if (!IsLegacyRootLayout(descriptor) && !IsGroupedLayout(descriptor)) {
		errorMessage = IsProjectFilesLayout(descriptor) ? "Project already uses Project Files V2 output layout." : "Project descriptor Solution layout is unsupported.";
		return false;
	}
	if (std::none_of(registry_.GetProjects().begin(), registry_.GetProjects().end(), [&descriptor](const ProjectRegistryEntry& entry) { return SamePath(entry.descriptorPath, descriptor.GetDescriptorPath()); })) {
		errorMessage = "Project descriptor is not registered.";
		return false;
	}
	if (std::any_of(registry_.GetSolutionGenerationOperations().begin(), registry_.GetSolutionGenerationOperations().end(), [&descriptor](const SolutionGenerationOperation& operation) {
		return operation.projectId == descriptor.GetProjectId() && operation.state != SolutionGenerationOperationState::Complete && operation.state != SolutionGenerationOperationState::RolledBack;
	})) {
		errorMessage = "Project already has a pending generation operation.";
		return false;
	}
	const auto preview = std::find_if(previews_.begin(), previews_.end(), [&operationId](const ProjectSolutionPreviewSnapshot& value) {
		return value.operationId == operationId && value.state == ProjectSolutionPreviewState::PreviewReady;
	});
	if (preview == previews_.end() || !MatchesPreviewOwner(*preview, descriptor) || !VerifyPreview(*preview, errorMessage)) {
		if (errorMessage.empty()) errorMessage = "Verified Project Files V2 Preview is unavailable.";
		return false;
	}
	SolutionGenerationEmitter emitter{};
	if (preview->inputIdentity != emitter.ComputeInputIdentity(model)) {
		errorMessage = "Preview input no longer matches the current Project.";
		return false;
	}
	for (const SolutionGenerationArtifact& artifact : preview->artifacts) {
		if (!IsWithin(model.projectRoot / artifact.relativePath, model.artifactDirectory)) {
			errorMessage = "Preview does not use the Project Files V2 artifact directory.";
			return false;
		}
	}
	const std::filesystem::path manifestPath = model.projectRoot / CanonicalManifestRelativePath(descriptor);
	std::vector<SolutionGenerationOperationFile> oldFiles;
	uint32_t modifiedOwnedArtifactCount = 0;
	if (!std::filesystem::is_regular_file(manifestPath) || !ReadManifestFiles(manifestPath, oldFiles, errorMessage) ||
		!CaptureOwnedArtifacts(model.projectRoot, oldFiles, modifiedOwnedArtifactCount, errorMessage)) {
		return false;
	}
	if (IsGroupedLayout(descriptor)) {
		if (oldFiles.size() != 8 || std::any_of(oldFiles.begin(), oldFiles.end(), [&descriptor](const SolutionGenerationOperationFile& file) { return !IsWithin(descriptor.GetProjectRoot() / file.relativePath, GroupedArtifactDirectory(descriptor)); })) { errorMessage = "V1 migration requires its eight owned IDE artifacts."; return false; }
	} else {
		const auto base = std::filesystem::path(L"project") / StringUtility::ToPath(descriptor.GetProjectId());
		const std::vector<std::filesystem::path> expected = { StringUtility::ToPath(StringUtility::ToUtf8(base) + ".sln"), StringUtility::ToPath(StringUtility::ToUtf8(base) + ".vcxproj"), StringUtility::ToPath(StringUtility::ToUtf8(base) + ".vcxproj.filters") };
		if (oldFiles.size() != expected.size() || std::any_of(oldFiles.begin(), oldFiles.end(), [&expected](const SolutionGenerationOperationFile& file) { return std::none_of(expected.begin(), expected.end(), [&file](const std::filesystem::path& path) { return SameRelativePath(path, file.relativePath); }); })) { errorMessage = "Legacy migration requires exactly its three owned IDE artifacts."; return false; }
	}
	if (modifiedOwnedArtifactCount != 0) { errorMessage = "GeneratedFileModified: migration requires unchanged owned artifacts."; return false; }
	if (HasUnknownGroupedTargetFile(descriptor, *preview, errorMessage)) {
		if (errorMessage.empty()) errorMessage = "Grouped artifact directory contains an unknown file.";
		return false;
	}
	ProjectDescriptor proposed = MakeProjectFilesDescriptor(descriptor);
	std::string validationError;
	if (!proposed.Validate(validationError)) {
		errorMessage = "Project Files V2 descriptor is invalid: " + validationError;
		return false;
	}
	const ProjectDescriptorPaths& currentPaths = descriptor.GetPaths();
	const ProjectDescriptorPaths& proposedPaths = proposed.GetPaths();
	if (currentPaths.sceneCatalog != proposedPaths.sceneCatalog ||
		currentPaths.developmentExecutable != proposedPaths.developmentExecutable ||
		descriptor.GetProjectId() != proposed.GetProjectId() ||
		descriptor.GetDisplayName() != proposed.GetDisplayName() ||
		descriptor.GetPreferredVisualStudioMajor() != proposed.GetPreferredVisualStudioMajor() ||
		descriptor.GetEngineMode() != proposed.GetEngineMode() ||
		descriptor.GetEngineProvenance().repository != proposed.GetEngineProvenance().repository ||
		descriptor.GetEngineProvenance().revision != proposed.GetEngineProvenance().revision ||
		descriptor.GetDependencies().manifest != proposed.GetDependencies().manifest ||
		descriptor.GetDependencies().lock != proposed.GetDependencies().lock ||
		descriptor.GetTemplate().id != proposed.GetTemplate().id ||
		descriptor.GetTemplate().schemaVersion != proposed.GetTemplate().schemaVersion ||
		descriptor.GetTemplate().sourceKind != proposed.GetTemplate().sourceKind) {
		errorMessage = "Project Files V2 descriptor changes fields other than generated Solution paths.";
		return false;
	}
	return registry_.Validate(errorMessage);
}

bool ProjectSolutionGenerationService::MigrateOutputLayout(const std::filesystem::path& descriptorPath, const std::string& operationId, std::string& errorMessage) {
	if (!CanMigrateOutputLayout(descriptorPath, operationId, errorMessage)) return false;
	ProjectDescriptor descriptor{};
	SolutionGenerationModel model{};
	if (!BuildCurrentModel(descriptorPath, descriptor, model, errorMessage)) return false;
	const auto preview = std::find_if(previews_.begin(), previews_.end(), [&operationId](const ProjectSolutionPreviewSnapshot& value) {
		return value.operationId == operationId && value.state == ProjectSolutionPreviewState::PreviewReady;
	});
	if (preview == previews_.end() || !MatchesPreviewOwner(*preview, descriptor)) {
		errorMessage = "Verified Project Files V2 Preview for this Project is unavailable.";
		return false;
	}
	if (!VerifyPreview(*preview, errorMessage) || preview->inputIdentity != SolutionGenerationEmitter{}.ComputeInputIdentity(model)) { if (errorMessage.empty()) errorMessage = "Preview input no longer matches the current Project."; return false; }
	std::vector<SolutionGenerationOperationFile> files;
	for (const SolutionGenerationArtifact& artifact : preview->artifacts) {
		SolutionGenerationOperationFile file{};
		file.kind = SolutionGenerationOperationFileKind::Artifact;
		file.relativePath = artifact.relativePath;
		file.nextContentHash = artifact.contentHash;
		file.previousExists = false;
		file.nextExists = true;
		files.push_back(std::move(file));
	}
	const std::filesystem::path manifestPath = model.projectRoot / CanonicalManifestRelativePath(descriptor);
	std::vector<SolutionGenerationOperationFile> oldFiles;
	uint32_t modifiedOwnedArtifactCount = 0;
	if (!ReadManifestFiles(manifestPath, oldFiles, errorMessage) || !CaptureOwnedArtifacts(model.projectRoot, oldFiles, modifiedOwnedArtifactCount, errorMessage)) {
		return false;
	}
	if (modifiedOwnedArtifactCount != 0) { errorMessage = "GeneratedFileModified: migration requires unchanged owned artifacts."; return false; }
	for (SolutionGenerationOperationFile& oldFile : oldFiles) {
		if (!ContainsArtifact(preview->artifacts, oldFile.relativePath)) {
			const std::filesystem::path stagedRetiredCopy = preview->stagingRoot / oldFile.relativePath;
			std::error_code filesystemError;
			if (std::filesystem::exists(stagedRetiredCopy, filesystemError)) {
				if (filesystemError || !MatchesHash(stagedRetiredCopy, oldFile.previousContentHash)) {
					errorMessage = "Preview contains a modified copy of a retired artifact.";
					return false;
				}
				std::filesystem::remove(stagedRetiredCopy, filesystemError);
				if (filesystemError) {
					errorMessage = "Preview retired artifact copy could not be removed from staging.";
					return false;
				}
			}
			files.push_back(std::move(oldFile));
		}
	}
	std::string descriptorContent;
	if (!ReadFile(descriptor.GetDescriptorPath(), descriptorContent)) {
		errorMessage = "Project descriptor could not be hashed.";
		return false;
	}
	ProjectDescriptor proposed = MakeProjectFilesDescriptor(descriptor);
	const std::string proposedDescriptorContent = SerializeDescriptor(proposed);
	const std::filesystem::path descriptorRelativePath = ToProjectRelative(model.projectRoot, descriptor.GetDescriptorPath(), errorMessage);
	if (descriptorRelativePath.empty()) {
		return false;
	}
	if (!WriteFile(preview->stagingRoot / descriptorRelativePath, proposedDescriptorContent, errorMessage)) {
		return false;
	}
	files.push_back({ SolutionGenerationOperationFileKind::Descriptor, descriptorRelativePath, HashContent(descriptorContent), HashContent(proposedDescriptorContent), true, true });
	std::string manifestContent;
	if (!ReadFile(manifestPath, manifestContent)) {
		errorMessage = "Existing generation manifest could not be hashed.";
		return false;
	}
	std::string nextManifestContent;
	if (!ReadFile(preview->stagingRoot / FixedManifestRelativePath(), nextManifestContent)) {
		errorMessage = "Preview generation manifest could not be hashed.";
		return false;
	}
	files.push_back({ SolutionGenerationOperationFileKind::Manifest, CanonicalManifestRelativePath(descriptor), HashContent(manifestContent), {}, true, false });
	files.push_back({ SolutionGenerationOperationFileKind::Manifest, FixedManifestRelativePath(), {}, HashContent(nextManifestContent), false, true });
	SolutionGenerationPreview transactionPreview{};
	transactionPreview.stagingRoot = preview->stagingRoot;
	transactionPreview.manifestPath = FixedManifestRelativePath();
	transactionPreview.inputIdentity = preview->inputIdentity;
	transactionPreview.artifacts = preview->artifacts;
	SolutionGenerationTransaction transaction{};
	const bool staged = transaction.Stage({ operationId, descriptor.GetProjectId(), model.projectRoot, std::move(transactionPreview), std::move(files), CanonicalManifestRelativePath(descriptor) }, registry_, errorMessage);
	const bool committed = staged && transaction.Commit(operationId, registry_, errorMessage);
	RefreshRecoverySnapshots();
	return committed;
}

bool ProjectSolutionGenerationService::CommitStaged(const std::string& operationId, std::string& errorMessage) {
	SolutionGenerationTransaction transaction{};
	if (!transaction.CanCommit(operationId, registry_, errorMessage)) { RefreshRecoverySnapshots(); return false; }
	const bool succeeded = transaction.Commit(operationId, registry_, errorMessage);
	RefreshRecoverySnapshots();
	return succeeded;
}

bool ProjectSolutionGenerationService::ResumeCommit(const std::string& operationId, std::string& errorMessage) {
	SolutionGenerationTransaction transaction{};
	if (!transaction.CanResumeCommit(operationId, registry_, errorMessage)) { RefreshRecoverySnapshots(); return false; }
	const bool succeeded = transaction.ResumeCommit(operationId, registry_, errorMessage);
	RefreshRecoverySnapshots();
	return succeeded;
}

bool ProjectSolutionGenerationService::DiscoverPreviews(std::string& errorMessage) {
	previews_.clear();
	const std::filesystem::path root = GetPreviewRoot();
	std::error_code error;
	if (!std::filesystem::exists(root, error)) return !error;
	if (error || !std::filesystem::is_directory(root, error) || HasReparsePointInExistingPath(root)) { errorMessage = "Preview root is unsafe."; return false; }
	for (const std::filesystem::directory_entry& projectEntry : std::filesystem::directory_iterator(root, std::filesystem::directory_options::none, error)) {
		if (error) break;
		if (!projectEntry.is_directory(error) || error || IsReparsePoint(projectEntry.path())) { errorMessage = "Preview root contains an invalid Project directory."; return false; }
		for (const std::filesystem::directory_entry& operationEntry : std::filesystem::directory_iterator(projectEntry.path(), std::filesystem::directory_options::none, error)) {
			if (error) break;
			ProjectSolutionPreviewSnapshot preview{};
			preview.projectId = StringUtility::ToUtf8(projectEntry.path().filename());
			preview.operationId = StringUtility::ToUtf8(operationEntry.path().filename());
			preview.stagingRoot = operationEntry.path();
			if (!operationEntry.is_directory(error) || error || IsReparsePoint(preview.stagingRoot) || !IsOperationId(preview.operationId)) continue;
			const auto manifestRelativePath = PreviewManifestRelativePath(preview.stagingRoot);
			const std::filesystem::path manifestPath = manifestRelativePath.empty() ? preview.stagingRoot : preview.stagingRoot / manifestRelativePath;
			if (!ReadPreviewManifest(manifestPath, preview, preview.detail)) {
				preview.state = ProjectSolutionPreviewState::Failed;
				preview.detail = "Incomplete Preview: " + preview.detail;
				UpsertPreview(std::move(preview));
				continue;
			}
			// 旧Previewの所有元は推定しない。失敗は当該候補だけに留める。
			std::filesystem::path ownerPath;
			if (!ReadPreviewOrigin(preview, ownerPath, preview.detail)) {
				preview.state = ProjectSolutionPreviewState::Failed;
				UpsertPreview(std::move(preview));
				continue;
			}
			preview.descriptorPath = std::move(ownerPath);
			for (const SolutionGenerationArtifact& artifact : preview.artifacts) {
				if (artifact.relativePath.extension() == L".sln") {
					preview.solutionPath = preview.stagingRoot / artifact.relativePath;
					break;
				}
			}
			preview.state = VerifyPreview(preview, preview.detail) ? ProjectSolutionPreviewState::PreviewReady : ProjectSolutionPreviewState::Failed;
			if (preview.state == ProjectSolutionPreviewState::PreviewReady) preview.detail = "Preview Ready - Not Built.";
			UpsertPreview(std::move(preview));
		}
	}
	if (error) { errorMessage = "Preview root could not be enumerated."; return false; }
	// 再起動でも古い候補が先に選ばれないよう、保存済みmanifestの時刻で復元順を固定する。
	std::vector<std::pair<std::filesystem::file_time_type, ProjectSolutionPreviewSnapshot>> orderedPreviews;
	orderedPreviews.reserve(previews_.size());
	for (ProjectSolutionPreviewSnapshot& preview : previews_) {
		std::error_code timestampError;
		const auto timestamp = std::filesystem::last_write_time(preview.stagingRoot / PreviewManifestRelativePath(preview.stagingRoot), timestampError);
		orderedPreviews.emplace_back(timestampError ? (std::filesystem::file_time_type::min)() : timestamp, std::move(preview));
	}
	std::sort(orderedPreviews.begin(), orderedPreviews.end(), [](const auto& left, const auto& right) {
		return left.first != right.first ? left.first < right.first : left.second.operationId < right.second.operationId;
	});
	previews_.clear();
	for (auto& preview : orderedPreviews) previews_.push_back(std::move(preview.second));
	return true;
}

bool ProjectSolutionGenerationService::ReconcileRecovery(std::string& errorMessage) {
	SolutionGenerationTransaction transaction{};
	bool succeeded = true;
	const std::vector<SolutionGenerationOperation> operations = registry_.GetSolutionGenerationOperations();
	for (const SolutionGenerationOperation& operation : operations) {
		if (operation.state == SolutionGenerationOperationState::CommitInProgress || operation.state == SolutionGenerationOperationState::RollbackInProgress || operation.state == SolutionGenerationOperationState::RecoveryRequired) {
			std::string operationError;
			const SolutionGenerationRecoveryResult result = transaction.Recover(operation.operationId, registry_, operationError);
			if (result != SolutionGenerationRecoveryResult::RecoveryRequired) continue;
			succeeded = false;
			if (errorMessage.empty()) errorMessage = operationError;
		}
	}
	RefreshRecoverySnapshots();
	return succeeded;
}

bool ProjectSolutionGenerationService::RecheckRecovery(const std::string& operationId, std::string& errorMessage) {
	SolutionGenerationTransaction transaction{};
	const SolutionGenerationRecoveryResult result = transaction.Recover(operationId, registry_, errorMessage);
	RefreshRecoverySnapshots();
	return result != SolutionGenerationRecoveryResult::NoOperation;
}

bool ProjectSolutionGenerationService::RestorePrevious(const std::string& operationId, std::string& errorMessage) {
	SolutionGenerationTransaction transaction{};
	if (!transaction.CanRestorePrevious(operationId, registry_, errorMessage)) { RefreshRecoverySnapshots(); return false; }
	const bool succeeded = transaction.RestorePrevious(operationId, registry_, errorMessage);
	RefreshRecoverySnapshots();
	return succeeded;
}

std::filesystem::path ProjectSolutionGenerationService::GetPreviewRoot() const {
	return registry_.GetRegistryPath().parent_path() / L"solution-generation" / L"previews";
}

void ProjectSolutionGenerationService::RefreshRecoverySnapshots() {
	recoveries_.clear();
	SolutionGenerationTransaction transaction{};
	for (const SolutionGenerationOperation& operation : registry_.GetSolutionGenerationOperations()) {
		if (operation.state == SolutionGenerationOperationState::Complete || operation.state == SolutionGenerationOperationState::RolledBack) continue;
		ProjectSolutionRecoverySnapshot snapshot{};
		snapshot.operationId = operation.operationId;
		snapshot.projectId = operation.projectId;
		snapshot.state = operation.state;
		snapshot.stagingRoot = operation.stagingRoot;
		snapshot.rollbackRoot = operation.rollbackRoot;
		snapshot.fileCount = static_cast<uint32_t>(operation.files.size());
		snapshot.canRecheck = operation.state == SolutionGenerationOperationState::CommitInProgress || operation.state == SolutionGenerationOperationState::RollbackInProgress || operation.state == SolutionGenerationOperationState::RecoveryRequired;
		std::string commitError;
		snapshot.canCommitStaged = operation.state == SolutionGenerationOperationState::Staged && transaction.CanCommit(operation.operationId, registry_, commitError);
		std::string resumeError;
		snapshot.canResumeCommit = (operation.state == SolutionGenerationOperationState::CommitInProgress || operation.state == SolutionGenerationOperationState::RecoveryRequired) && transaction.CanResumeCommit(operation.operationId, registry_, resumeError);
		std::string restoreError;
		snapshot.canRestorePrevious = (operation.state == SolutionGenerationOperationState::CommitInProgress || operation.state == SolutionGenerationOperationState::RollbackInProgress || operation.state == SolutionGenerationOperationState::RecoveryRequired) && transaction.CanRestorePrevious(operation.operationId, registry_, restoreError);
		snapshot.detail = snapshot.canCommitStaged ? "Staged generation is ready to commit." : snapshot.canResumeCommit ? "Recovery can resume the generation commit." : snapshot.canRestorePrevious ? "Recovery can restore the previous generation." : !commitError.empty() ? commitError : !resumeError.empty() ? resumeError : restoreError;
		recoveries_.push_back(std::move(snapshot));
	}
}

void ProjectSolutionGenerationService::UpsertPreview(ProjectSolutionPreviewSnapshot value) {
	for (ProjectSolutionPreviewSnapshot& current : previews_) {
		if (current.operationId == value.operationId && current.projectId == value.projectId) { current = std::move(value); return; }
	}
	previews_.push_back(std::move(value));
}

bool ProjectSolutionGenerationService::BuildModel(const ProjectDescriptor& descriptor, SolutionGenerationModel& model, std::string& errorMessage, SolutionGenerationLayout outputLayout) const {
	model = {};
	std::filesystem::path specificationPath;
	if (!ProjectBuildSpecification::ResolvePath(descriptor.GetProjectRoot(), specificationPath, errorMessage)) return false;
	ProjectBuildSpecification specification{};
	SolutionGenerationModelBuilder builder{};
	return (outputLayout != SolutionGenerationLayout::ProjectFilesV2 || ValidateProjectFilesSource(descriptor, errorMessage)) && specification.Load(specificationPath, errorMessage) && builder.Build(descriptor, specification, model, errorMessage, outputLayout);
}

bool ProjectSolutionGenerationService::BuildCurrentModel(const std::filesystem::path& descriptorPath, ProjectDescriptor& descriptor, SolutionGenerationModel& model, std::string& errorMessage) const {
	descriptor = {};
	model = {};
	if (!descriptor.Load(descriptorPath, errorMessage)) return false;
	if (!SamePath(descriptor.GetDescriptorPath(), descriptor.GetProjectRoot() / L"game.project.json") || HasReparsePointInExistingPath(descriptor.GetDescriptorPath())) { errorMessage = "V2 requires the canonical game.project.json descriptor."; return false; }
	if (InspectCanonicalManifestSet(descriptor, registry_, errorMessage) != CanonicalManifestState::Unambiguous) return false;
	return BuildModel(descriptor, model, errorMessage);
}
