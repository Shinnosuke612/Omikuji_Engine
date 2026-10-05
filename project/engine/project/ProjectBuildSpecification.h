// 役割: Game Projectのtracked build specificationを検証する。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct ProjectBuildToolchain {
	std::string platform;
	std::string platformToolset;
	std::string windowsSdk;
};

struct ProjectBuildExternalReferences {
	std::string assimp;
	std::string directXTexProject;
	std::string imguiProject;
};

struct ProjectBuildRuntime {
	bool copyResources = true;
	std::string resourceRoot;
};

struct ProjectBuildSourceSet {
	std::string name;
	std::string root;
	std::vector<std::string> include;
	std::vector<std::string> files;
};

class ProjectBuildSpecification {
public:
	static constexpr int kSchemaVersion = 1;

	// 読取専用で新旧の配置を解決する。二重配置・不正な既存候補は失敗し、
	// 両方欠落時はLegacy Manual判定のため旧候補を返す。内容の検証はLoadが担う。
	static bool ResolvePath(const std::filesystem::path& projectRoot, std::filesystem::path& specificationPath, std::string& errorMessage);

	bool Load(const std::filesystem::path& specificationPath, std::string& errorMessage);
	bool Validate(std::string& errorMessage) const;

	const std::filesystem::path& GetPath() const;
	const std::vector<std::string>& GetConfigurations() const;
	const ProjectBuildToolchain& GetToolchain() const;
	const ProjectBuildExternalReferences& GetExternals() const;
	const ProjectBuildRuntime& GetRuntime() const;
	const ProjectBuildSourceSet& GetEngineSourceSet() const;
	const ProjectBuildSourceSet& GetGameSourceSet() const;
	const ProjectBuildSourceSet& GetEntrySourceSet() const;

private:
	std::filesystem::path path_;
	int schemaVersion_ = kSchemaVersion;
	std::vector<std::string> configurations_;
	ProjectBuildToolchain toolchain_{};
	ProjectBuildExternalReferences externals_{};
	ProjectBuildRuntime runtime_{};
	ProjectBuildSourceSet engine_{};
	ProjectBuildSourceSet game_{};
	ProjectBuildSourceSet entry_{};
};
