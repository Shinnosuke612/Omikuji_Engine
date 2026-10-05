// 役割: エディタで編集可能なリソースパスの候補を定義する。
#pragma once

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

namespace EditableResourcePath {

inline bool IsProjectRoot(const std::filesystem::path& path) {
	// 判定対象はSource root（project）。descriptorはその親のGame rootに置く。
	std::error_code descriptorError;
	if (CompareStringOrdinal(path.filename().c_str(), -1, L"project", -1, TRUE) == CSTR_EQUAL &&
		std::filesystem::is_directory(path, descriptorError) && !descriptorError &&
		std::filesystem::is_regular_file(path.parent_path() / "game.project.json", descriptorError) && !descriptorError &&
		std::filesystem::is_directory(path / "resources", descriptorError) && !descriptorError) {
		return true;
	}

	// descriptorを持たない旧Snapshotも使えるよう、Template置換markerのfallbackを残す。
	std::error_code error;
	return std::filesystem::exists(path / "CG2_2025_04_14.vcxproj", error) &&
		std::filesystem::exists(path / "resources", error);
}

inline std::filesystem::path FindProjectRootFrom(std::filesystem::path base) {
	std::error_code error;
	base = std::filesystem::absolute(base, error);
	while (!base.empty()) {
		if (IsProjectRoot(base)) {
			return base;
		}
		if (IsProjectRoot(base / "project")) {
			return base / "project";
		}

		const std::filesystem::path parent = base.parent_path();
		if (parent == base) {
			break;
		}
		base = parent;
	}
	return {};
}

inline std::filesystem::path FindProjectRoot() {
	std::error_code error;
	if (const std::filesystem::path root =
		FindProjectRootFrom(std::filesystem::current_path(error)); !root.empty()) {
		return root;
	}

	wchar_t modulePath[MAX_PATH]{};
	if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0) {
		return FindProjectRootFrom(std::filesystem::path(modulePath).parent_path());
	}
	return {};
}

inline std::filesystem::path Resolve(const std::filesystem::path& requestedPath) {
	if (requestedPath.is_absolute()) {
		const std::filesystem::path root = FindProjectRoot();
		if (!root.empty()) {
			std::filesystem::path resourceRelativePath;
			bool foundResources = false;
			for (const std::filesystem::path& component : requestedPath) {
				if (!foundResources && component == "resources") {
					foundResources = true;
				}
				if (foundResources) {
					resourceRelativePath /= component;
				}
			}
			if (foundResources) {
				return (root / resourceRelativePath).lexically_normal();
			}
		}
		return requestedPath.lexically_normal();
	}

	const std::filesystem::path root = FindProjectRoot();
	if (!root.empty()) {
		return (root / requestedPath).lexically_normal();
	}

	std::error_code error;
	return std::filesystem::absolute(requestedPath, error).lexically_normal();
}

// resources配下の論理パスを、開発中はプロジェクト側、配布時は実行側へ解決する。
inline std::filesystem::path ResolveResource(
	const std::filesystem::path& requestedPath
) {
	if (requestedPath.is_absolute()) {
		return Resolve(requestedPath);
	}

	const std::filesystem::path normalized = requestedPath.lexically_normal();
	auto component = normalized.begin();
	if (
		component != normalized.end() &&
		(*component).generic_string() == "resources"
	) {
		return Resolve(normalized);
	}
	return Resolve(std::filesystem::path("resources") / normalized);
}

// 狭い文字列でパスを受け取る外部ライブラリ向けに、日本語を含む親ディレクトリを相対化する。
inline std::filesystem::path ToWorkingDirectoryRelative(
	const std::filesystem::path& requestedPath
) {
	std::error_code error;
	const std::filesystem::path absolutePath = requestedPath.is_absolute()
		? requestedPath
		: std::filesystem::absolute(requestedPath, error);
	if (error) {
		return requestedPath.lexically_normal();
	}

	const std::filesystem::path workingDirectory =
		std::filesystem::current_path(error);
	if (error) {
		return requestedPath.lexically_normal();
	}

	const std::filesystem::path relativePath = std::filesystem::relative(
		absolutePath,
		workingDirectory,
		error
	);
	return error || relativePath.empty()
		? requestedPath.lexically_normal()
		: relativePath.lexically_normal();
}

inline std::filesystem::path ToProjectRelative(const std::filesystem::path& path) {
	const std::filesystem::path root = FindProjectRoot();
	if (root.empty()) {
		return path.lexically_normal();
	}
	std::error_code error;
	const std::filesystem::path relative = std::filesystem::relative(path, root, error);
	return error ? path.lexically_normal() : relative.lexically_normal();
}

inline std::filesystem::path BackupPath(const std::filesystem::path& targetPath) {
	std::filesystem::path result = targetPath;
	result += ".bak";
	return result;
}

inline bool ReadTextFile(const std::filesystem::path& path, std::string& text) {
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open()) {
		return false;
	}
	std::string loaded(
		(std::istreambuf_iterator<char>(file)),
		std::istreambuf_iterator<char>()
	);
	if (file.bad() || loaded.empty()) {
		return false;
	}
	text = std::move(loaded);
	return true;
}

inline bool ReadText(const std::filesystem::path& requestedPath, std::string& text) {
	const std::filesystem::path targetPath = Resolve(requestedPath);
	const std::filesystem::path candidates[] = {
		targetPath,
		BackupPath(targetPath)
	};

	for (const std::filesystem::path& candidate : candidates) {
		if (ReadTextFile(candidate, text)) {
			return true;
		}
	}
	return false;
}

inline bool WriteTextAtomically(
	const std::filesystem::path& requestedPath,
	const std::string& text
) {
	if (text.empty()) {
		return false;
	}

	const std::filesystem::path targetPath = Resolve(requestedPath);
	const std::filesystem::path parentPath = targetPath.parent_path();
	std::error_code error;
	if (!parentPath.empty()) {
		std::filesystem::create_directories(parentPath, error);
		if (error) {
			return false;
		}
	}

	std::filesystem::path temporaryPath = targetPath;
	temporaryPath += ".tmp";
	{
		std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
		if (!file.is_open()) {
			return false;
		}
		file.write(text.data(), static_cast<std::streamsize>(text.size()));
		file.flush();
		if (!file.good()) {
			file.close();
			std::filesystem::remove(temporaryPath, error);
			return false;
		}
	}

	if (std::filesystem::exists(targetPath, error)) {
		CopyFileW(
			targetPath.c_str(),
			BackupPath(targetPath).c_str(),
			FALSE
		);
	}

	if (!MoveFileExW(
		temporaryPath.c_str(),
		targetPath.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
	)) {
		std::filesystem::remove(temporaryPath, error);
		return false;
	}
	return true;
}

} // namespace EditableResourcePath
