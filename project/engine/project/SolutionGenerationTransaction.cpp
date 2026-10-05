// 役割: journalのowned集合を確定し、旧新manifestを含むcommitと逆順rollbackを行う。
#include "SolutionGenerationTransaction.h"

#include "../utility/StringUtility.h"
#include "../../externals/nlohmann/json.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>

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

	bool IsSafeRelativePath(const std::filesystem::path& path) {
		return !path.empty() && !path.is_absolute() && !path.has_root_name() && !path.has_root_directory() &&
			std::none_of(path.begin(), path.end(), [](const std::filesystem::path& part) { return part == L".."; });
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

	bool MatchesHash(const std::filesystem::path& path, const std::string& expectedHash) {
		std::string content;
		return ReadFile(path, content) && HashContent(content) == expectedHash;
	}

	bool SameRelativePath(const std::filesystem::path& left, const std::filesystem::path& right) {
		return CompareStringOrdinal(left.lexically_normal().c_str(), -1, right.lexically_normal().c_str(), -1, TRUE) == CSTR_EQUAL;
	}

	bool MoveArtifactFile(const std::filesystem::path& source, const std::filesystem::path& destination, bool replace) {
		const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
		return MoveFileExW(source.c_str(), destination.c_str(), flags) != FALSE;
	}

	bool SameRelativePath(const std::filesystem::path& left, const std::filesystem::path& right);

	bool ReadManifestArtifacts(const std::filesystem::path& manifestPath, std::vector<SolutionGenerationOperationFile>& files, std::string& errorMessage) {
		std::string content;
		if (!ReadFile(manifestPath, content)) { errorMessage = "Generation manifest could not be read."; return false; }
		try {
			const json manifest = json::parse(content);
			if (!manifest.is_object() || !manifest.contains("schemaVersion") || !manifest.at("schemaVersion").is_number_integer() || manifest.at("schemaVersion").get<int>() != 1 ||
				!manifest.contains("artifacts") || !manifest.at("artifacts").is_array()) {
				errorMessage = "Generation manifest schema is invalid.";
				return false;
			}
			for (const json& value : manifest.at("artifacts")) {
				if (!value.is_object() || value.size() != 2 || !value.contains("path") || !value.at("path").is_string() || !value.contains("contentHash") || !value.at("contentHash").is_string()) {
					errorMessage = "Generation manifest artifact is invalid.";
					return false;
				}
				SolutionGenerationOperationFile file{};
				file.kind = SolutionGenerationOperationFileKind::Artifact;
				file.relativePath = StringUtility::ToPath(value.at("path").get<std::string>()).lexically_normal();
				file.nextContentHash = value.at("contentHash").get<std::string>();
				file.nextExists = true;
				if (!IsSafeRelativePath(file.relativePath) || file.nextContentHash.empty() ||
					SameRelativePath(file.relativePath, std::filesystem::path(L"project/build/generated/solution-generation.json")) || SameRelativePath(file.relativePath, std::filesystem::path(L"intermediate/project-files/solution-generation.json"))) {
					errorMessage = "Generation manifest artifact path is invalid.";
					return false;
				}
				if (std::any_of(files.begin(), files.end(), [&file](const SolutionGenerationOperationFile& current) { return CompareStringOrdinal(current.relativePath.c_str(), -1, file.relativePath.c_str(), -1, TRUE) == CSTR_EQUAL; })) {
					errorMessage = "Generation manifest artifact path is duplicated.";
					return false;
				}
				files.push_back(std::move(file));
			}
		} catch (const json::exception&) {
			errorMessage = "Generation manifest JSON could not be parsed.";
			return false;
		}
		return true;
	}

	bool VerifyExplicitPreviousSet(const std::vector<SolutionGenerationOperationFile>& manifestFiles, const std::vector<SolutionGenerationOperationFile>& requestFiles,
		const std::filesystem::path& manifestPath, std::string& errorMessage) {
		for (size_t left = 0; left < requestFiles.size(); ++left) {
			for (size_t right = left + 1; right < requestFiles.size(); ++right) {
				if (SameRelativePath(requestFiles[left].relativePath, requestFiles[right].relativePath)) {
					errorMessage = "Generation transaction explicit file union contains a duplicate path.";
					return false;
				}
			}
		}
		for (const SolutionGenerationOperationFile& manifestFile : manifestFiles) {
			const size_t count = static_cast<size_t>(std::count_if(requestFiles.begin(), requestFiles.end(), [&manifestFile](const SolutionGenerationOperationFile& requestFile) {
				return requestFile.kind == SolutionGenerationOperationFileKind::Artifact && requestFile.previousExists && SameRelativePath(requestFile.relativePath, manifestFile.relativePath) && requestFile.previousContentHash == manifestFile.nextContentHash;
			}));
			if (count != 1) {
				errorMessage = "Generation transaction explicit file union does not cover every owned artifact.";
				return false;
			}
		}
		for (const SolutionGenerationOperationFile& requestFile : requestFiles) {
			if (requestFile.kind == SolutionGenerationOperationFileKind::Artifact && requestFile.previousExists && std::none_of(manifestFiles.begin(), manifestFiles.end(), [&requestFile](const SolutionGenerationOperationFile& manifestFile) {
				return SameRelativePath(requestFile.relativePath, manifestFile.relativePath) && requestFile.previousContentHash == manifestFile.nextContentHash;
			})) {
				errorMessage = "Generation transaction explicit file union has an unowned previous artifact.";
				return false;
			}
		}
		const size_t manifestCount = static_cast<size_t>(std::count_if(requestFiles.begin(), requestFiles.end(), [&manifestPath](const SolutionGenerationOperationFile& requestFile) {
			return requestFile.kind == SolutionGenerationOperationFileKind::Manifest && requestFile.previousExists && SameRelativePath(requestFile.relativePath, manifestPath);
		}));
		if (manifestCount != 1) {
			errorMessage = "Generation transaction explicit file union does not contain the fixed manifest replacement.";
			return false;
		}
		return true;
	}

	bool VerifySet(const SolutionGenerationOperation& operation, bool next) {
		for (const SolutionGenerationOperationFile& file : operation.files) {
			const std::filesystem::path target = operation.projectRoot / file.relativePath;
			if (!IsSafeRelativePath(file.relativePath) || !IsWithin(target, operation.projectRoot) || HasReparsePointInExistingPath(target)) return false;
			if (next) {
				if (file.nextExists ? !MatchesHash(target, file.nextContentHash) : std::filesystem::exists(target)) return false;
			} else if (file.previousExists ? !MatchesHash(target, file.previousContentHash) : std::filesystem::exists(target)) {
				return false;
			}
		}
		return true;
	}

	bool IsPreviousFileState(const SolutionGenerationOperationFile& file, const std::filesystem::path& target) {
		return file.previousExists ? MatchesHash(target, file.previousContentHash) : !std::filesystem::exists(target);
	}

	bool IsNextFileState(const SolutionGenerationOperationFile& file, const std::filesystem::path& target) {
		return file.nextExists ? MatchesHash(target, file.nextContentHash) : !std::filesystem::exists(target);
	}

	enum class CommitFileState { Previous, OldMoved, Next, Invalid };

	CommitFileState GetCommitFileState(const SolutionGenerationOperation& operation, const SolutionGenerationOperationFile& file);

	bool CanRestoreOperation(const SolutionGenerationOperation& operation, std::string& errorMessage) {
		if (HasReparsePointInExistingPath(operation.projectRoot) || HasReparsePointInExistingPath(operation.stagingRoot) || HasReparsePointInExistingPath(operation.rollbackRoot)) {
			errorMessage = "Generation rollback path is unsafe.";
			return false;
		}
		for (const SolutionGenerationOperationFile& file : operation.files) {
			const std::filesystem::path target = operation.projectRoot / file.relativePath;
			if (!IsSafeRelativePath(file.relativePath) || !IsWithin(target, operation.projectRoot) || HasReparsePointInExistingPath(target) ||
				HasReparsePointInExistingPath(operation.stagingRoot / file.relativePath) || HasReparsePointInExistingPath(operation.rollbackRoot / file.relativePath)) {
				errorMessage = "Generation rollback artifact path is unsafe.";
				return false;
			}
			if (IsPreviousFileState(file, target)) {
				continue;
			}
			const CommitFileState state = GetCommitFileState(operation, file);
			if (state != CommitFileState::OldMoved && state != CommitFileState::Next) {
				errorMessage = "Generation rollback cannot prove an owned old or new artifact state.";
				return false;
			}
		}
		return true;
	}

	CommitFileState GetCommitFileState(const SolutionGenerationOperation& operation, const SolutionGenerationOperationFile& file) {
		const std::filesystem::path target = operation.projectRoot / file.relativePath;
		const std::filesystem::path staged = operation.stagingRoot / file.relativePath;
		const std::filesystem::path backup = operation.rollbackRoot / file.relativePath;
		// Stage後や再起動後にdirectoryが置き換わっていても、root外へ追従しない。
		if (HasReparsePointInExistingPath(target) || HasReparsePointInExistingPath(staged) || HasReparsePointInExistingPath(backup)) return CommitFileState::Invalid;
		const bool stagedIsNext = file.nextExists && MatchesHash(staged, file.nextContentHash);
		const bool targetIsNext = IsNextFileState(file, target);
		if (!file.previousExists && file.nextExists) {
			if (!std::filesystem::exists(target) && stagedIsNext && !std::filesystem::exists(backup)) return CommitFileState::Previous;
			if (targetIsNext && !std::filesystem::exists(staged) && !std::filesystem::exists(backup)) return CommitFileState::Next;
			return CommitFileState::Invalid;
		}
		if (file.previousExists && file.nextExists) {
			const bool targetIsPrevious = MatchesHash(target, file.previousContentHash);
			const bool backupIsPrevious = MatchesHash(backup, file.previousContentHash);
			if (targetIsPrevious && stagedIsNext && !std::filesystem::exists(backup)) return CommitFileState::Previous;
			if (!std::filesystem::exists(target) && stagedIsNext && backupIsPrevious) return CommitFileState::OldMoved;
			if (targetIsNext && !std::filesystem::exists(staged) && backupIsPrevious) return CommitFileState::Next;
			return CommitFileState::Invalid;
		}
		if (file.previousExists && !file.nextExists) {
			const bool targetIsPrevious = MatchesHash(target, file.previousContentHash);
			const bool backupIsPrevious = MatchesHash(backup, file.previousContentHash);
			if (targetIsPrevious && !std::filesystem::exists(staged) && !std::filesystem::exists(backup)) return CommitFileState::Previous;
			if (!std::filesystem::exists(target) && !std::filesystem::exists(staged) && backupIsPrevious) return CommitFileState::Next;
			return CommitFileState::Invalid;
		}
		return CommitFileState::Invalid;
	}

	bool CanCommitOperation(const SolutionGenerationOperation& operation, bool allowPartial, std::string& errorMessage) {
		if (HasReparsePointInExistingPath(operation.projectRoot) || HasReparsePointInExistingPath(operation.stagingRoot) || HasReparsePointInExistingPath(operation.rollbackRoot) ||
			!std::filesystem::is_directory(operation.stagingRoot) || !IsWithin(operation.rollbackRoot, operation.projectRoot)) {
			errorMessage = "Generation commit path is unsafe.";
			return false;
		}
		for (const SolutionGenerationOperationFile& file : operation.files) {
			const std::filesystem::path target = operation.projectRoot / file.relativePath;
			const std::filesystem::path staged = operation.stagingRoot / file.relativePath;
			const std::filesystem::path backup = operation.rollbackRoot / file.relativePath;
			if (!IsSafeRelativePath(file.relativePath) || !IsWithin(target, operation.projectRoot) || !IsWithin(staged, operation.stagingRoot) || !IsWithin(backup, operation.rollbackRoot)) {
				errorMessage = "Generation commit artifact path is unsafe.";
				return false;
			}
			const CommitFileState state = GetCommitFileState(operation, file);
			if (state == CommitFileState::Invalid || (!allowPartial && state != CommitFileState::Previous)) {
				errorMessage = "Generation commit cannot prove the staged artifact state.";
				return false;
			}
		}
		return true;
	}

	bool UpdateState(ProjectRegistry& registry, SolutionGenerationOperation operation, SolutionGenerationOperationState state, std::string& errorMessage) {
		operation.state = state;
		return registry.UpsertSolutionGenerationOperation(operation, errorMessage) && registry.Save(errorMessage);
	}

	std::vector<const SolutionGenerationOperationFile*> CommitOrder(const SolutionGenerationOperation& operation) {
		std::vector<const SolutionGenerationOperationFile*> ordered;
		for (const SolutionGenerationOperationFile& file : operation.files) {
			if (file.kind == SolutionGenerationOperationFileKind::Artifact && file.nextExists) ordered.push_back(&file);
		}
		for (const SolutionGenerationOperationFile& file : operation.files) {
			if (file.kind == SolutionGenerationOperationFileKind::Descriptor) ordered.push_back(&file);
		}
		for (const SolutionGenerationOperationFile& file : operation.files) {
			if (file.kind == SolutionGenerationOperationFileKind::Artifact && !file.nextExists) ordered.push_back(&file);
		}
		for (const SolutionGenerationOperationFile& file : operation.files) {
			if (file.kind == SolutionGenerationOperationFileKind::Manifest && !file.nextExists) ordered.push_back(&file);
		}
		for (const SolutionGenerationOperationFile& file : operation.files) {
			if (file.kind == SolutionGenerationOperationFileKind::Manifest && file.nextExists) ordered.push_back(&file);
		}
		return ordered;
	}

	bool ContinueCommit(const SolutionGenerationOperation& operation, ProjectRegistry& registry, std::string& errorMessage) {
		std::error_code filesystemError;
		std::filesystem::create_directories(operation.rollbackRoot, filesystemError);
		if (filesystemError || HasReparsePointInExistingPath(operation.rollbackRoot)) { errorMessage = "Generation rollback path is unsafe."; return false; }
		const std::vector<const SolutionGenerationOperationFile*> ordered = CommitOrder(operation);
		for (const SolutionGenerationOperationFile* file : ordered) {
			const CommitFileState state = GetCommitFileState(operation, *file);
			const std::filesystem::path source = operation.stagingRoot / file->relativePath;
			const std::filesystem::path target = operation.projectRoot / file->relativePath;
			const std::filesystem::path backup = operation.rollbackRoot / file->relativePath;
			std::filesystem::create_directories(target.parent_path(), filesystemError);
			std::filesystem::create_directories(backup.parent_path(), filesystemError);
			const bool moveOld = state == CommitFileState::Previous && file->previousExists;
			const bool moveNew = file->nextExists && (state == CommitFileState::Previous || state == CommitFileState::OldMoved);
			if (filesystemError || state == CommitFileState::Invalid || (moveOld && !MoveArtifactFile(target, backup, false)) || (moveNew && !MoveArtifactFile(source, target, false))) {
				std::string rollbackError;
				SolutionGenerationTransaction{}.RestorePrevious(operation.operationId, registry, rollbackError);
				errorMessage = rollbackError.empty() ? "Generation transaction commit failed." : "Generation transaction commit failed and rollback was attempted: " + rollbackError;
				return false;
			}
		}
		if (!VerifySet(operation, true)) { errorMessage = "Generation transaction commit hash verification failed."; return false; }
		return UpdateState(registry, operation, SolutionGenerationOperationState::Complete, errorMessage);
	}
}

bool SolutionGenerationTransaction::Stage(const SolutionGenerationTransactionRequest& request, ProjectRegistry& registry, std::string& errorMessage) const {
	if (request.operationId.empty() || request.projectId.empty() || request.projectRoot.empty() || request.preview.stagingRoot.empty() || request.preview.manifestPath.empty()) {
		errorMessage = "Generation transaction request is incomplete.";
		return false;
	}
	SolutionGenerationOperation operation{};
	operation.operationId = request.operationId;
	operation.projectId = request.projectId;
	operation.projectRoot = std::filesystem::absolute(request.projectRoot).lexically_normal();
	operation.stagingRoot = std::filesystem::absolute(request.preview.stagingRoot).lexically_normal();
	operation.rollbackRoot = operation.projectRoot / L".solution-generation-rollback" / StringUtility::ToPath(request.operationId);
	if (HasReparsePointInExistingPath(operation.projectRoot) || HasReparsePointInExistingPath(operation.stagingRoot) || HasReparsePointInExistingPath(operation.rollbackRoot) || !std::filesystem::is_directory(operation.stagingRoot) || !IsWithin(operation.rollbackRoot, operation.projectRoot)) {
		errorMessage = "Generation transaction path is unsafe.";
		return false;
	}

	const std::filesystem::path stagedManifest = operation.stagingRoot / request.preview.manifestPath;
	if (!IsSafeRelativePath(request.preview.manifestPath) || !IsWithin(stagedManifest, operation.stagingRoot) || HasReparsePointInExistingPath(stagedManifest) || !std::filesystem::is_regular_file(stagedManifest)) {
		errorMessage = "Generation preview manifest is outside the staging root.";
		return false;
	}
	if (!ReadManifestArtifacts(stagedManifest, operation.files, errorMessage)) return false;
	std::string manifestContent;
	if (!ReadFile(stagedManifest, manifestContent)) { errorMessage = "Generation preview manifest could not be hashed."; return false; }
	if (std::any_of(operation.files.begin(), operation.files.end(), [&request](const SolutionGenerationOperationFile& file) { return CompareStringOrdinal(file.relativePath.c_str(), -1, request.preview.manifestPath.c_str(), -1, TRUE) == CSTR_EQUAL; })) {
		errorMessage = "Generation manifest cannot own itself as an artifact.";
		return false;
	}
	operation.files.push_back({ SolutionGenerationOperationFileKind::Manifest, request.preview.manifestPath, {}, HashContent(manifestContent), false, true });

	// 異なる旧manifest pathは固定V1→V2対だけ。旧journalのpathは変更しない。
	const std::filesystem::path previousManifestPath = request.previousManifestPath.empty() ? request.preview.manifestPath : request.previousManifestPath;
	const bool relocatingManifest = !SameRelativePath(previousManifestPath, request.preview.manifestPath);
	if ((request.preview.manifestPath != std::filesystem::path(L"project/build/generated/solution-generation.json") && request.preview.manifestPath != std::filesystem::path(L"intermediate/project-files/solution-generation.json")) ||
		(previousManifestPath != std::filesystem::path(L"project/build/generated/solution-generation.json") && previousManifestPath != std::filesystem::path(L"intermediate/project-files/solution-generation.json")) ||
		(relocatingManifest && (previousManifestPath != std::filesystem::path(L"project/build/generated/solution-generation.json") || request.preview.manifestPath != std::filesystem::path(L"intermediate/project-files/solution-generation.json") || request.files.empty()))) {
		errorMessage = "Generation manifest relocation is unsupported."; return false;
	}
	if (!request.files.empty()) {
		// 引数が同pathなのに旧新Manifest対を渡す要求も拒否し、旧owned検査を迂回させない。
		const size_t manifestCount = static_cast<size_t>(std::count_if(request.files.begin(), request.files.end(), [](const SolutionGenerationOperationFile& file) {
			return file.kind == SolutionGenerationOperationFileKind::Manifest;
		}));
		const bool invalidManifest = std::any_of(request.files.begin(), request.files.end(), [&request, &previousManifestPath, relocatingManifest](const SolutionGenerationOperationFile& file) {
			if (file.kind != SolutionGenerationOperationFileKind::Manifest) return false;
			if (!relocatingManifest) return !file.nextExists || !SameRelativePath(file.relativePath, request.preview.manifestPath);
			return !((SameRelativePath(file.relativePath, previousManifestPath) && file.previousExists && !file.nextExists) ||
				(SameRelativePath(file.relativePath, request.preview.manifestPath) && !file.previousExists && file.nextExists));
		});
		if (manifestCount != (relocatingManifest ? 2u : 1u) || invalidManifest) {
			errorMessage = "Explicit manifest set does not match the requested previous and next manifest paths.";
			return false;
		}
	}
	const std::filesystem::path finalManifest = operation.projectRoot / previousManifestPath;
	// Serviceの診断後に二重配置になった場合も、新規Stageでは復旧を迂回しない。
	std::error_code manifestError;
	const bool oldManifestExists = std::filesystem::exists(operation.projectRoot / L"project/build/generated/solution-generation.json", manifestError);
	if (manifestError) { errorMessage = "Old generation manifest could not be inspected."; return false; }
	const bool newManifestExists = std::filesystem::exists(operation.projectRoot / L"intermediate/project-files/solution-generation.json", manifestError);
	if (manifestError) { errorMessage = "New generation manifest could not be inspected."; return false; }
	if (oldManifestExists && newManifestExists) {
		errorMessage = "Old and new manifests both exist. Recover the matching journal before staging a new operation.";
		return false;
	}
	// 反対layoutのmanifestが一件だけある場合も、同path要求で新たな二重配置を作らない。
	if (!relocatingManifest && (request.preview.manifestPath == std::filesystem::path(L"intermediate/project-files/solution-generation.json") ? oldManifestExists : newManifestExists)) {
		errorMessage = "Other layout manifest exists. Use explicit migration or recovery before staging.";
		return false;
	}
	if (relocatingManifest && (HasReparsePointInExistingPath(finalManifest) || !std::filesystem::is_regular_file(finalManifest) || std::filesystem::exists(operation.projectRoot / request.preview.manifestPath))) { errorMessage = "Manifest relocation requires exactly one safe old manifest."; return false; }
	std::vector<SolutionGenerationOperationFile> oldFiles;
	if (HasReparsePointInExistingPath(finalManifest)) { errorMessage = "Previous manifest path is unsafe."; return false; }
	if (std::filesystem::exists(finalManifest)) {
		if (!ReadManifestArtifacts(finalManifest, oldFiles, errorMessage)) return false;
		if (request.files.empty()) {
			for (const SolutionGenerationOperationFile& oldFile : oldFiles) {
				const std::filesystem::path target = operation.projectRoot / oldFile.relativePath;
				if (!IsWithin(target, operation.projectRoot) || HasReparsePointInExistingPath(target) || !MatchesHash(target, oldFile.nextContentHash)) {
					errorMessage = "GeneratedFileModified: an owned artifact no longer matches its manifest.";
					return false;
				}
			}
			std::string previousManifestContent;
			if (!ReadFile(finalManifest, previousManifestContent)) { errorMessage = "Existing generation manifest could not be hashed."; return false; }
			oldFiles.push_back({ SolutionGenerationOperationFileKind::Manifest, request.preview.manifestPath, HashContent(previousManifestContent), {}, true, false });
		} else if (!VerifyExplicitPreviousSet(oldFiles, request.files, previousManifestPath, errorMessage)) {
			return false;
		}
	}

	if (!request.files.empty()) {
		// next集合はstaged manifestと一致しなければならない。origin等を混入させない。
		for (const SolutionGenerationOperationFile& staged : operation.files) {
			if (std::count_if(request.files.begin(), request.files.end(), [&staged](const SolutionGenerationOperationFile& file) {
				return file.kind == staged.kind && file.nextExists && SameRelativePath(file.relativePath, staged.relativePath) && file.nextContentHash == staged.nextContentHash;
			}) != 1) { errorMessage = "Explicit next set does not match the staged manifest."; return false; }
		}
		for (const SolutionGenerationOperationFile& file : request.files) {
			if (file.kind == SolutionGenerationOperationFileKind::Artifact && file.nextExists && std::none_of(operation.files.begin(), operation.files.end(), [&file](const SolutionGenerationOperationFile& staged) {
				return staged.kind == file.kind && SameRelativePath(staged.relativePath, file.relativePath) && staged.nextContentHash == file.nextContentHash;
			})) { errorMessage = "Explicit next set contains an unowned artifact."; return false; }
		}
		operation.files = request.files;
	}

	for (SolutionGenerationOperationFile& file : operation.files) {
		const std::filesystem::path stagedFile = operation.stagingRoot / file.relativePath;
		const std::filesystem::path target = operation.projectRoot / file.relativePath;
		if (!IsSafeRelativePath(file.relativePath) || !IsWithin(stagedFile, operation.stagingRoot) || !IsWithin(target, operation.projectRoot) || HasReparsePointInExistingPath(target) || HasReparsePointInExistingPath(stagedFile) ||
			(file.nextExists && !MatchesHash(stagedFile, file.nextContentHash)) ||
			(!file.nextExists && std::filesystem::exists(stagedFile))) {
			errorMessage = "Generation transaction artifact is unsafe or modified in staging.";
			return false;
		}
		if (request.files.empty()) {
			const auto old = std::find_if(oldFiles.begin(), oldFiles.end(), [&file](const SolutionGenerationOperationFile& current) { return CompareStringOrdinal(current.relativePath.c_str(), -1, file.relativePath.c_str(), -1, TRUE) == CSTR_EQUAL; });
			if (old != oldFiles.end()) { file.previousExists = true; file.previousContentHash = old->nextExists ? old->nextContentHash : old->previousContentHash; }
			else if (std::filesystem::exists(target)) { errorMessage = "GeneratedFileModified: target is not owned by the previous manifest."; return false; }
		}
		if (file.previousExists && !MatchesHash(target, file.previousContentHash)) {
			errorMessage = "GeneratedFileModified: previous generation file no longer matches its journal.";
			return false;
		}
		if (!file.previousExists && std::filesystem::exists(target)) {
			errorMessage = "GeneratedFileModified: target is not owned by the previous manifest.";
			return false;
		}
	}
	return UpdateState(registry, operation, SolutionGenerationOperationState::Staged, errorMessage);
}

bool SolutionGenerationTransaction::Commit(const std::string& operationId, ProjectRegistry& registry, std::string& errorMessage) const {
	const SolutionGenerationOperation* stored = registry.FindSolutionGenerationOperation(operationId);
	if (!stored || stored->state != SolutionGenerationOperationState::Staged) { errorMessage = "Generation transaction is not staged."; return false; }
	const SolutionGenerationOperation operation = *stored;
	if (!CanCommitOperation(operation, false, errorMessage)) return false;
	if (!UpdateState(registry, operation, SolutionGenerationOperationState::CommitInProgress, errorMessage)) return false;
	return ContinueCommit(operation, registry, errorMessage);
}

bool SolutionGenerationTransaction::CanCommit(const std::string& operationId, const ProjectRegistry& registry, std::string& errorMessage) const {
	const SolutionGenerationOperation* stored = registry.FindSolutionGenerationOperation(operationId);
	if (!stored || stored->state != SolutionGenerationOperationState::Staged) { errorMessage = "Generation transaction is not staged."; return false; }
	return CanCommitOperation(*stored, false, errorMessage);
}

bool SolutionGenerationTransaction::ResumeCommit(const std::string& operationId, ProjectRegistry& registry, std::string& errorMessage) const {
	const SolutionGenerationOperation* stored = registry.FindSolutionGenerationOperation(operationId);
	if (!stored || (stored->state != SolutionGenerationOperationState::CommitInProgress && stored->state != SolutionGenerationOperationState::RecoveryRequired)) { errorMessage = "Generation transaction cannot resume commit."; return false; }
	const SolutionGenerationOperation operation = *stored;
	if (!CanCommitOperation(operation, true, errorMessage)) return false;
	if (!UpdateState(registry, operation, SolutionGenerationOperationState::CommitInProgress, errorMessage)) return false;
	return ContinueCommit(operation, registry, errorMessage);
}

bool SolutionGenerationTransaction::CanResumeCommit(const std::string& operationId, const ProjectRegistry& registry, std::string& errorMessage) const {
	const SolutionGenerationOperation* stored = registry.FindSolutionGenerationOperation(operationId);
	if (!stored || (stored->state != SolutionGenerationOperationState::CommitInProgress && stored->state != SolutionGenerationOperationState::RecoveryRequired)) { errorMessage = "Generation transaction cannot resume commit."; return false; }
	return CanCommitOperation(*stored, true, errorMessage);
}

bool SolutionGenerationTransaction::RestorePrevious(const std::string& operationId, ProjectRegistry& registry, std::string& errorMessage) const {
	const SolutionGenerationOperation* stored = registry.FindSolutionGenerationOperation(operationId);
	if (!stored) { errorMessage = "Generation transaction was not found."; return false; }
	const SolutionGenerationOperation operation = *stored;
	if (!CanRestoreOperation(operation, errorMessage)) return false;
	if (!UpdateState(registry, operation, SolutionGenerationOperationState::RollbackInProgress, errorMessage)) return false;
	const std::vector<const SolutionGenerationOperationFile*> ordered = CommitOrder(operation);
	for (auto iterator = ordered.rbegin(); iterator != ordered.rend(); ++iterator) {
		const SolutionGenerationOperationFile& file = **iterator;
		const std::filesystem::path target = operation.projectRoot / file.relativePath;
		const std::filesystem::path backup = operation.rollbackRoot / file.relativePath;
		std::error_code filesystemError;
		if (IsPreviousFileState(file, target)) {
			continue;
		}
		if (file.nextExists) {
			std::filesystem::remove(target, filesystemError);
		}
		if (filesystemError || (file.previousExists && !MoveArtifactFile(backup, target, false))) {
			UpdateState(registry, operation, SolutionGenerationOperationState::RecoveryRequired, errorMessage);
			errorMessage = "Generation rollback could not restore an owned artifact.";
			return false;
		}
	}
	if (!VerifySet(operation, false)) { UpdateState(registry, operation, SolutionGenerationOperationState::RecoveryRequired, errorMessage); errorMessage = "Generation rollback hash verification failed."; return false; }
	return UpdateState(registry, operation, SolutionGenerationOperationState::RolledBack, errorMessage);
}

bool SolutionGenerationTransaction::CanRestorePrevious(const std::string& operationId, const ProjectRegistry& registry, std::string& errorMessage) const {
	const SolutionGenerationOperation* stored = registry.FindSolutionGenerationOperation(operationId);
	if (!stored) {
		errorMessage = "Generation transaction was not found.";
		return false;
	}
	return CanRestoreOperation(*stored, errorMessage);
}

SolutionGenerationRecoveryResult SolutionGenerationTransaction::Recover(const std::string& operationId, ProjectRegistry& registry, std::string& errorMessage) const {
	const SolutionGenerationOperation* stored = registry.FindSolutionGenerationOperation(operationId);
	if (!stored) return SolutionGenerationRecoveryResult::NoOperation;
	const SolutionGenerationOperation operation = *stored;
	if (VerifySet(operation, true)) return UpdateState(registry, operation, SolutionGenerationOperationState::Complete, errorMessage) ? SolutionGenerationRecoveryResult::Completed : SolutionGenerationRecoveryResult::RecoveryRequired;
	if (VerifySet(operation, false)) return UpdateState(registry, operation, SolutionGenerationOperationState::RolledBack, errorMessage) ? SolutionGenerationRecoveryResult::RolledBack : SolutionGenerationRecoveryResult::RecoveryRequired;
	UpdateState(registry, operation, SolutionGenerationOperationState::RecoveryRequired, errorMessage);
	if (errorMessage.empty()) errorMessage = "Generation Incomplete: neither the previous nor new artifact set matches its journal.";
	return SolutionGenerationRecoveryResult::RecoveryRequired;
}
