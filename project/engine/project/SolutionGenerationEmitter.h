// 役割: validated modelからstaging preview用のMSBuild artifactをemitする。
#pragma once

#include "SolutionGenerationModel.h"

#include <filesystem>
#include <string>
#include <vector>

struct SolutionGenerationArtifact {
	std::filesystem::path relativePath;
	std::string contentHash;
};

struct SolutionGenerationPreview {
	std::filesystem::path stagingRoot;
	std::filesystem::path manifestPath;
	std::string inputIdentity;
	std::vector<SolutionGenerationArtifact> artifacts;
};

class SolutionGenerationEmitter {
public:
	// XMLの出力意味を変える改修時に更新する。ゲームソース本文の変更では更新しない。
	static constexpr const char* kPolicyRevision = "automatic-generation-1";
	std::string ComputeInputIdentity(const SolutionGenerationModel& model) const;
	bool EmitPreview(const SolutionGenerationModel& model, const std::filesystem::path& stagingRoot,
		SolutionGenerationPreview& output, std::string& errorMessage) const;
};
