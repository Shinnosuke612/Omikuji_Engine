// 役割: Solution生成に必要なvalidated source／target modelを構築する。
#pragma once

#include "ProjectBuildSpecification.h"
#include "ProjectDescriptor.h"

#include <filesystem>
#include <string>
#include <vector>

enum class SolutionGenerationFileKind {
	Compile,
	Include
};

// descriptorの現在配置と、次に生成する配置を混同しない。
enum class SolutionGenerationLayout { LegacyRoot, GroupedV1, ProjectFilesV2 };

enum class SolutionGenerationTargetKind {
	Engine,
	Game,
	Host
};

struct SolutionGenerationSourceFile {
	SolutionGenerationFileKind kind = SolutionGenerationFileKind::Compile;
	std::filesystem::path absolutePath;
	std::filesystem::path projectRelativePath;
};

struct SolutionGenerationTarget {
	SolutionGenerationTargetKind kind = SolutionGenerationTargetKind::Host;
	std::string name;
	std::string stableGuid;
	std::vector<SolutionGenerationSourceFile> sourceFiles;
};

struct SolutionGenerationModel {
	SolutionGenerationLayout descriptorLayout = SolutionGenerationLayout::LegacyRoot;
	SolutionGenerationLayout outputLayout = SolutionGenerationLayout::ProjectFilesV2;
	std::filesystem::path projectRoot;
	std::filesystem::path sourceDirectory;
	std::filesystem::path artifactDirectory;
	ProjectBuildToolchain toolchain;
	ProjectBuildExternalReferences externals;
	ProjectBuildRuntime runtime;
	std::vector<std::string> configurations;
	std::vector<SolutionGenerationTarget> targets;
};

class SolutionGenerationModelBuilder {
public:
	// descriptorは三配置を読む。出力はV1/V2だけで、Sourceやdescriptorは変更しない。
	bool Build(
		const ProjectDescriptor& descriptor,
		const ProjectBuildSpecification& specification,
		SolutionGenerationModel& output,
		std::string& errorMessage,
		SolutionGenerationLayout outputLayout = SolutionGenerationLayout::ProjectFilesV2
	) const;
};
