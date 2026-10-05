// 役割: descriptor配置を検証し、明示layoutの生成モデルとSource集合を構築する。
#include "SolutionGenerationModel.h"

#include "../utility/StringUtility.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

namespace {
	constexpr std::array<std::uint8_t, 16> kUuidNamespace = {
		0xA2, 0xA5, 0x15, 0x1D, 0x63, 0x1D, 0x5E, 0xEA,
		0x8B, 0x5D, 0x3A, 0xA4, 0x38, 0xC0, 0xA2, 0xA3
	};

	bool IsReparsePoint(const std::filesystem::path& path) {
		const DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
	}

	bool HasReparsePointInExistingPath(const std::filesystem::path& path) {
		std::filesystem::path current = path.root_path();
		if (!current.empty() && IsReparsePoint(current)) {
			return true;
		}
		for (const std::filesystem::path& part : path.relative_path()) {
			current /= part;
			std::error_code error;
			if (!std::filesystem::exists(current, error) || error) {
				break;
			}
			if (IsReparsePoint(current)) {
				return true;
			}
		}
		return false;
	}

	int CompareOrdinal(const std::wstring& left, const std::wstring& right, bool ignoreCase) {
		return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), ignoreCase ? TRUE : FALSE) - CSTR_EQUAL;
	}

	std::wstring NormalizedPathString(const std::filesystem::path& path) {
		return path.lexically_normal().generic_wstring();
	}

	bool IsPathWithin(const std::filesystem::path& child, const std::filesystem::path& parent) {
		const std::wstring childText = NormalizedPathString(child);
		std::wstring parentText = NormalizedPathString(parent);
		while (parentText.size() > 1 && (parentText.back() == L'/' || parentText.back() == L'\\')) {
			parentText.pop_back();
		}
		if (CompareOrdinal(childText, parentText, true) == 0) {
			return true;
		}
		if (childText.size() <= parentText.size() ||
			CompareOrdinal(childText.substr(0, parentText.size()), parentText, true) != 0) {
			return false;
		}
		return childText[parentText.size()] == L'/' || childText[parentText.size()] == L'\\';
	}

	bool IsExcludedDirectory(const std::filesystem::path& path) {
		const std::wstring name = path.filename().generic_wstring();
		for (const wchar_t* excluded : { L".git", L".ai-work", L".agents", L".codex", L".vs", L"generated", L"Logs", L"output", L"outputs" }) {
			if (CompareOrdinal(name, excluded, true) == 0) {
				return true;
			}
		}
		return false;
	}

	bool IsIgnoredFile(const std::filesystem::path& path) {
		const std::wstring name = path.filename().generic_wstring();
		for (const wchar_t* suffix : { L".user", L".suo", L".bak", L".tmp" }) {
			if (name.size() >= wcslen(suffix) &&
				CompareOrdinal(name.substr(name.size() - wcslen(suffix)), suffix, true) == 0) {
				return true;
			}
		}
		return false;
	}

	bool IsExtension(const std::filesystem::path& path, const wchar_t* extension) {
		return CompareOrdinal(path.extension().generic_wstring(), extension, true) == 0;
	}

	class Sha1 {
	public:
		void Update(const std::uint8_t* data, std::size_t size) {
			for (std::size_t index = 0; index < size; ++index) {
				buffer_[bufferSize_++] = data[index];
				if (bufferSize_ == buffer_.size()) {
					Transform(buffer_.data());
					bitCount_ += 512;
					bufferSize_ = 0;
				}
			}
		}

		std::array<std::uint8_t, 20> Finalize() {
			const std::uint64_t originalBitCount = bitCount_ + static_cast<std::uint64_t>(bufferSize_) * 8;
			buffer_[bufferSize_++] = 0x80;
			if (bufferSize_ > 56) {
				while (bufferSize_ < 64) buffer_[bufferSize_++] = 0;
				Transform(buffer_.data());
				bufferSize_ = 0;
			}
			while (bufferSize_ < 56) buffer_[bufferSize_++] = 0;
			for (int shift = 56; shift >= 0; shift -= 8) buffer_[bufferSize_++] = static_cast<std::uint8_t>(originalBitCount >> shift);
			Transform(buffer_.data());
			std::array<std::uint8_t, 20> result{};
			for (std::size_t index = 0; index < state_.size(); ++index) {
				for (int shift = 24; shift >= 0; shift -= 8) result[index * 4 + (3 - shift / 8)] = static_cast<std::uint8_t>(state_[index] >> shift);
			}
			return result;
		}

	private:
		static std::uint32_t RotateLeft(std::uint32_t value, int count) { return (value << count) | (value >> (32 - count)); }

		void Transform(const std::uint8_t* block) {
			std::array<std::uint32_t, 80> words{};
			for (std::size_t index = 0; index < 16; ++index) {
				words[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
					(static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
					(static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
					static_cast<std::uint32_t>(block[index * 4 + 3]);
			}
			for (std::size_t index = 16; index < words.size(); ++index) words[index] = RotateLeft(words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16], 1);
			std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4];
			for (std::size_t index = 0; index < words.size(); ++index) {
				std::uint32_t function = 0;
				std::uint32_t constant = 0;
				if (index < 20) { function = (b & c) | ((~b) & d); constant = 0x5A827999; }
				else if (index < 40) { function = b ^ c ^ d; constant = 0x6ED9EBA1; }
				else if (index < 60) { function = (b & c) | (b & d) | (c & d); constant = 0x8F1BBCDC; }
				else { function = b ^ c ^ d; constant = 0xCA62C1D6; }
				const std::uint32_t temporary = RotateLeft(a, 5) + function + e + constant + words[index];
				e = d; d = c; c = RotateLeft(b, 30); b = a; a = temporary;
			}
			state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d; state_[4] += e;
		}

		std::array<std::uint32_t, 5> state_ = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
		std::array<std::uint8_t, 64> buffer_{};
		std::size_t bufferSize_ = 0;
		std::uint64_t bitCount_ = 0;
	};

	std::string StableGuid(const std::string& projectId, const char* targetKind) {
		const std::string name = "cg3.solution-generation/v1/" + projectId + "/" + targetKind;
		Sha1 sha1;
		sha1.Update(kUuidNamespace.data(), kUuidNamespace.size());
		sha1.Update(reinterpret_cast<const std::uint8_t*>(name.data()), name.size());
		std::array<std::uint8_t, 20> digest = sha1.Finalize();
		digest[6] = static_cast<std::uint8_t>((digest[6] & 0x0F) | 0x50);
		digest[8] = static_cast<std::uint8_t>((digest[8] & 0x3F) | 0x80);
		std::ostringstream stream;
		stream << '{' << std::uppercase << std::hex << std::setfill('0');
		for (std::size_t index = 0; index < 16; ++index) {
			if (index == 4 || index == 6 || index == 8 || index == 10) stream << '-';
			stream << std::setw(2) << static_cast<unsigned int>(digest[index]);
		}
		stream << '}';
		return stream.str();
	}

	bool SamePathIgnoreCase(const std::filesystem::path& left, const std::filesystem::path& right) {
		return CompareOrdinal(NormalizedPathString(left), NormalizedPathString(right), true) == 0;
	}

	bool IsDescriptorSolutionLayout(
		const ProjectDescriptor& descriptor,
		const std::filesystem::path& expectedDirectory,
		std::string& errorMessage
	) {
		const std::filesystem::path solutionPath = descriptor.ResolveProjectPath(descriptor.GetPaths().solution).lexically_normal();
		const std::filesystem::path hostProjectPath = descriptor.ResolveProjectPath(descriptor.GetPaths().msbuildProject).lexically_normal();
		const std::filesystem::path expectedSolution = (expectedDirectory / StringUtility::ToPath(descriptor.GetProjectId() + ".sln")).lexically_normal();
		const std::filesystem::path expectedHostProject = (expectedDirectory / StringUtility::ToPath(descriptor.GetProjectId() + ".vcxproj")).lexically_normal();
		if (!SamePathIgnoreCase(solutionPath, expectedSolution) || !SamePathIgnoreCase(hostProjectPath, expectedHostProject)) {
			errorMessage = "Project descriptor Solution layout is unsupported.";
			return false;
		}
		if (solutionPath.parent_path().empty() || !SamePathIgnoreCase(solutionPath.parent_path(), hostProjectPath.parent_path())) {
			errorMessage = "Project descriptor Solution and MSBuild Project must share a directory.";
			return false;
		}
		return true;
	}

	bool SourcePathLess(const SolutionGenerationSourceFile& left, const SolutionGenerationSourceFile& right) {
		const std::wstring leftPath = NormalizedPathString(left.projectRelativePath);
		const std::wstring rightPath = NormalizedPathString(right.projectRelativePath);
		const int insensitive = CompareOrdinal(leftPath, rightPath, true);
		return insensitive != 0 ? insensitive < 0 : CompareOrdinal(leftPath, rightPath, false) < 0;
	}

	bool EnumerateSourceSet(const ProjectBuildSourceSet& sourceSet, const std::filesystem::path& sourceDirectory, std::vector<SolutionGenerationSourceFile>& output, std::string& errorMessage) {
		const std::filesystem::path sourceRoot = (sourceDirectory / std::filesystem::path(sourceSet.root)).lexically_normal();
		std::error_code error;
		if (!IsPathWithin(sourceRoot, sourceDirectory) || !std::filesystem::is_directory(sourceRoot, error) || error || HasReparsePointInExistingPath(sourceRoot)) {
			errorMessage = "Build source root is invalid or contains a reparse point.";
			return false;
		}
		std::filesystem::recursive_directory_iterator iterator(sourceRoot, std::filesystem::directory_options::none, error);
		const std::filesystem::recursive_directory_iterator end;
		if (error) {
			errorMessage = "Build source directory could not be enumerated.";
			return false;
		}
		while (iterator != end) {
			if (error) {
				errorMessage = "Build source directory could not be enumerated.";
				return false;
			}
			const std::filesystem::path path = iterator->path();
			if (IsReparsePoint(path)) {
				errorMessage = "Build source path contains a reparse point.";
				return false;
			}
			std::error_code statusError;
			const bool directory = iterator->is_directory(statusError);
			if (statusError) {
				errorMessage = "Build source entry could not be inspected.";
				return false;
			}
			if (directory) {
				if (IsExcludedDirectory(path)) iterator.disable_recursion_pending();
			} else if (iterator->is_regular_file(statusError) && !statusError && !IsIgnoredFile(path) && (IsExtension(path, L".cpp") || IsExtension(path, L".h"))) {
				output.push_back({ IsExtension(path, L".cpp") ? SolutionGenerationFileKind::Compile : SolutionGenerationFileKind::Include, path.lexically_normal(), std::filesystem::relative(path, sourceDirectory, statusError).lexically_normal() });
				if (statusError || output.back().projectRelativePath.empty() || output.back().projectRelativePath.has_root_name() || output.back().projectRelativePath.has_root_directory()) {
					errorMessage = "Build source path could not be made Project-relative.";
					return false;
				}
			}
			iterator.increment(error);
		}
		if (error) {
			errorMessage = "Build source directory could not be enumerated.";
			return false;
		}
		std::sort(output.begin(), output.end(), SourcePathLess);
		return true;
	}
}

bool SolutionGenerationModelBuilder::Build(const ProjectDescriptor& descriptor, const ProjectBuildSpecification& specification, SolutionGenerationModel& output, std::string& errorMessage, SolutionGenerationLayout outputLayout) const {
	output = {};
	try {
		if (!descriptor.Validate(errorMessage) || descriptor.GetEngineMode() != ProjectEngineMode::Snapshot) {
			errorMessage = descriptor.GetEngineMode() == ProjectEngineMode::Snapshot ? errorMessage : "Managed-source generation inputs are not implemented.";
			return false;
		}
		if (!specification.Validate(errorMessage)) return false;
		const std::filesystem::path projectRoot = descriptor.GetProjectRoot().lexically_normal();
		const std::filesystem::path sourceDirectory = (projectRoot / L"project").lexically_normal();
		const std::filesystem::path groupedDirectory = sourceDirectory / L"build" / L"generated" / StringUtility::ToPath(descriptor.GetProjectId());
		const std::filesystem::path projectFilesDirectory = projectRoot / L"intermediate" / L"project-files";
		if (outputLayout != SolutionGenerationLayout::GroupedV1 && outputLayout != SolutionGenerationLayout::ProjectFilesV2) {
			errorMessage = "Generation output layout is unsupported.";
			return false;
		}
		const std::filesystem::path artifactDirectory = outputLayout == SolutionGenerationLayout::ProjectFilesV2 ? projectFilesDirectory : groupedDirectory;
		std::string layoutError;
		if (IsDescriptorSolutionLayout(descriptor, sourceDirectory, layoutError)) output.descriptorLayout = SolutionGenerationLayout::LegacyRoot;
		else if (IsDescriptorSolutionLayout(descriptor, groupedDirectory, layoutError)) output.descriptorLayout = SolutionGenerationLayout::GroupedV1;
		else if (IsDescriptorSolutionLayout(descriptor, projectFilesDirectory, layoutError)) output.descriptorLayout = SolutionGenerationLayout::ProjectFilesV2;
		else { errorMessage = "Project descriptor Solution layout is unsupported."; return false; }
		output.outputLayout = outputLayout;
		std::filesystem::path expectedSpecificationPath;
		if (!ProjectBuildSpecification::ResolvePath(projectRoot, expectedSpecificationPath, errorMessage)) return false;
		if (sourceDirectory.empty() || !SamePathIgnoreCase(specification.GetPath(), expectedSpecificationPath) || !SamePathIgnoreCase(sourceDirectory.parent_path(), projectRoot)) {
			errorMessage = "Build specification path does not match the selected Project configuration.";
			return false;
		}

		if (HasReparsePointInExistingPath(sourceDirectory) || HasReparsePointInExistingPath(artifactDirectory)) {
			errorMessage = "Project build directory contains a reparse point.";
			return false;
		}
		output.projectRoot = projectRoot;
		output.sourceDirectory = sourceDirectory;
		output.artifactDirectory = artifactDirectory;
		output.toolchain = specification.GetToolchain();
		output.externals = specification.GetExternals();
		output.runtime = specification.GetRuntime();
		output.configurations = specification.GetConfigurations();
		SolutionGenerationTarget engine{ SolutionGenerationTargetKind::Engine, descriptor.GetProjectId() + ".Engine", StableGuid(descriptor.GetProjectId(), "Engine") };
		SolutionGenerationTarget game{ SolutionGenerationTargetKind::Game, descriptor.GetProjectId() + ".Game", StableGuid(descriptor.GetProjectId(), "Game") };
		SolutionGenerationTarget host{ SolutionGenerationTargetKind::Host, descriptor.GetProjectId(), StableGuid(descriptor.GetProjectId(), "Host") };
		if (!EnumerateSourceSet(specification.GetEngineSourceSet(), sourceDirectory, engine.sourceFiles, errorMessage) ||
			!EnumerateSourceSet(specification.GetGameSourceSet(), sourceDirectory, game.sourceFiles, errorMessage)) {
			output = {};
			return false;
		}
		for (const std::string& entryFile : specification.GetEntrySourceSet().files) {
			const std::filesystem::path path = (sourceDirectory / std::filesystem::path(entryFile)).lexically_normal();
			std::error_code entryError;
			if (!IsPathWithin(path, sourceDirectory) || !std::filesystem::is_regular_file(path, entryError) || entryError || HasReparsePointInExistingPath(path)) {
				errorMessage = "Build entry source file is missing or invalid.";
				output = {};
				return false;
			}
			const std::filesystem::path relativePath = std::filesystem::relative(path, sourceDirectory, entryError).lexically_normal();
			if (entryError || relativePath.empty() || relativePath.has_root_name() || relativePath.has_root_directory()) {
				errorMessage = "Build entry source path could not be made Project-relative.";
				output = {};
				return false;
			}
			host.sourceFiles.push_back({ SolutionGenerationFileKind::Compile, path, relativePath });
		}
		for (const SolutionGenerationTarget* target : { &engine, &game, &host }) {
			for (const SolutionGenerationSourceFile& file : target->sourceFiles) {
				for (const SolutionGenerationTarget* other : { &engine, &game, &host }) {
					for (const SolutionGenerationSourceFile& otherFile : other->sourceFiles) {
						if ((target != other || &file != &otherFile) && SamePathIgnoreCase(file.projectRelativePath, otherFile.projectRelativePath)) {
							errorMessage = "Build source file is assigned to multiple targets.";
							output = {};
							return false;
						}
					}
				}
			}
		}
		output.targets = { std::move(engine), std::move(game), std::move(host) };
		return true;
	} catch (const std::filesystem::filesystem_error&) {
		output = {};
		errorMessage = "Build source path could not be resolved.";
		return false;
	}
}
