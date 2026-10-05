# 役割: Templateの許可ファイルから独立したGame Projectをstagingへ作成し、検証後に出力先へ確定する。
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectId,

    [Parameter(Mandatory = $true)]
    [string]$DisplayName,

    [Parameter(Mandatory = $true)]
    [string]$DestinationRoot,

    [string]$StartSceneId = "main",

    [ValidateSet("LegacyRoot", "GroupedV1", "ProjectFilesV2")]
    [string]$OutputLayout = "LegacyRoot"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Fail([string]$Message) {
    throw $Message
}

function Resolve-FullPath([string]$Path) {
    try {
        return [System.IO.Path]::GetFullPath($Path)
    } catch {
        Fail ("Pathを解決できません: " + $Path)
    }
}

function Test-PathWithin(
    [string]$Child,
    [string]$Parent
) {
    $normalizedParent = $Parent.TrimEnd('\', '/')
    return $Child.Equals(
        $normalizedParent,
        [System.StringComparison]::OrdinalIgnoreCase
    ) -or $Child.StartsWith(
        $normalizedParent + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase
    )
}

function Assert-RelativePath(
    [string]$BasePath,
    [string]$RelativePath,
    [string]$Label
) {
    if ([System.IO.Path]::IsPathRooted($RelativePath) -or
        $RelativePath -match '(^|[\\/])\.\.([\\/]|$)') {
        Fail ("相対Pathではありません (" + $Label + "): " + $RelativePath)
    }
    $resolved = Resolve-FullPath (Join-Path $BasePath $RelativePath)
    $base = Resolve-FullPath $BasePath
    if (-not (Test-PathWithin $resolved $base)) {
        Fail ("許可root外のPathです (" + $Label + "): " + $RelativePath)
    }
}

function Read-Utf8([string]$Path) {
    return [System.IO.File]::ReadAllText($Path)
}

function Write-Utf8(
    [string]$Path,
    [string]$Content
) {
    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrEmpty($parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $encoding)
}

function Count-Exact(
    [string]$Text,
    [string]$Marker,
    [bool]$IgnoreCase = $false
) {
    $comparison = if ($IgnoreCase) {
        [System.StringComparison]::OrdinalIgnoreCase
    } else {
        [System.StringComparison]::Ordinal
    }
    $count = 0
    $offset = 0
    while ($true) {
        $found = $Text.IndexOf($Marker, $offset, $comparison)
        if ($found -lt 0) {
            break
        }
        $count++
        $offset = $found + $Marker.Length
    }
    return $count
}

function Assert-ExactCount(
    [string]$Path,
    [string]$Marker,
    [int]$Expected,
    [bool]$IgnoreCase = $false
) {
    $count = Count-Exact (Read-Utf8 $Path) $Marker $IgnoreCase
    if ($count -ne $Expected) {
        Fail ("Source drift: " + $Path + " のmarker出現数が " +
            $Expected + " ではなく " + $count + " です: " + $Marker)
    }
}

function Replace-ExactChecked(
    [string]$Path,
    [string]$Marker,
    [string]$Replacement,
    [int]$Expected,
    [bool]$IgnoreCase = $false
) {
    Assert-ExactCount $Path $Marker $Expected $IgnoreCase
    $content = Read-Utf8 $Path
    Write-Utf8 $Path ($content.Replace($Marker, $Replacement))
}

function Test-ExcludedRelativePath(
    [string]$RelativePath,
    [string]$LeafName,
    $Manifest
) {
    $parts = $RelativePath -split '[\\/]'
    foreach ($directoryName in @($Manifest.excludedDirectoryNames)) {
        if ($parts -contains [string]$directoryName) {
            return $true
        }
    }
    foreach ($pattern in @($Manifest.excludedFilePatterns)) {
        if ($LeafName -like [string]$pattern) {
            return $true
        }
    }
    return $false
}

function Copy-ManifestDirectory(
    [string]$RelativeRoot,
    [string]$SourceProjectRoot,
    [string]$StagingProject,
    $Manifest
) {
    Assert-RelativePath $SourceProjectRoot $RelativeRoot "copyDirectories"
    $sourceRoot = Resolve-FullPath (Join-Path $SourceProjectRoot $RelativeRoot)
    if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) {
        Fail ("copy directoryが存在しません: " + $RelativeRoot)
    }
    $sourceItem = Get-Item -LiteralPath $sourceRoot -Force
    if (($sourceItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        Fail ("copy directoryがreparse pointです: " + $RelativeRoot)
    }

    Get-ChildItem -LiteralPath $sourceRoot -Recurse -Force -File |
        ForEach-Object {
            $suffix = $_.FullName.Substring($sourceRoot.Length).TrimStart('\', '/')
            $relativeFile = Join-Path $RelativeRoot $suffix
            $isReparse =
                (($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
            if (-not $isReparse -and
                -not (Test-ExcludedRelativePath $suffix $_.Name $Manifest)) {
                $destination = Join-Path $StagingProject $relativeFile
                $destinationParent = Split-Path -Parent $destination
                New-Item -ItemType Directory -Path $destinationParent -Force |
                    Out-Null
                Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
            }
        }
}

function Copy-ManifestFile(
    [string]$RelativePath,
    [string]$SourceBase,
    [string]$DestinationBase,
    [string]$Label,
    $Manifest
) {
    Assert-RelativePath $SourceBase $RelativePath $Label
    $source = Resolve-FullPath (Join-Path $SourceBase $RelativePath)
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        Fail ($Label + "が存在しません: " + $RelativePath)
    }
    if (Test-ExcludedRelativePath $RelativePath (Split-Path -Leaf $RelativePath) $Manifest) {
        Fail ($Label + "が除外Patternに該当します: " + $RelativePath)
    }
    $destination = Join-Path $DestinationBase $RelativePath
    $destinationParent = Split-Path -Parent $destination
    New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
}

function Assert-NoForbiddenOutput(
    [string]$OutputRoot,
    [string]$ProjectId
) {
    $forbiddenPaths = @(
        ".git",
        ".ai-work",
        ".agents",
        ".codex",
        ".vs",
        "generated",
        "Logs",
        "output",
        "outputs",
        "project/externals/generated",
        "project/editor_settings.json",
        "project/editor_settings.json.bak",
        "project/editor_settings.json.tmp",
        "project/imgui.ini",
        "project/CG2_2025_04_14.sln",
        "project/CG2_2025_04_14.vcxproj",
        "project/CG2_2025_04_14.vcxproj.filters",
        "project/resources/scenes/title.scene.json",
        "project/resources/scenes/gameplay.scene.json"
    )
    foreach ($relativePath in $forbiddenPaths) {
        if (Test-Path -LiteralPath (Join-Path $OutputRoot $relativePath)) {
            Fail ("生成物に禁止Pathがあります: " + $relativePath)
        }
    }

    $forbiddenFiles = Get-ChildItem -LiteralPath $OutputRoot -Recurse -Force -File |
        Where-Object {
            $_.Name -like "*.user" -or
            $_.Name -like "*.suo" -or
            $_.Name -like "*.bak" -or
            $_.Name -like "*.tmp"
        }
    if (@($forbiddenFiles).Count -gt 0) {
        Fail ("生成物にlocal／backup fileがあります: " +
            ($forbiddenFiles[0].FullName))
    }
}

function Assert-GeneratedProject(
    [string]$OutputRoot,
    [string]$ProjectId,
    [string]$StartSceneId,
    [string]$MainGuid,
    [string]$SolutionGuid,
    [string]$ExpectedEditableProjectMarker,
    [string]$ExpectedSolutionPath
) {
    $projectRoot = Join-Path $OutputRoot "project"
    $buildSpecificationPath = Join-Path $OutputRoot "config/project.build.json"
    if (-not (Test-Path -LiteralPath $buildSpecificationPath -PathType Leaf)) {
        Fail "生成Projectのconfig/project.build.jsonがありません"
    }
    $buildSpecification = Read-Utf8 $buildSpecificationPath | ConvertFrom-Json
    if ($null -eq $buildSpecification -or
        $null -eq $buildSpecification.PSObject.Properties['schemaVersion'] -or
        $buildSpecification.schemaVersion -ne 1) {
        Fail "生成Projectのbuild specification schemaVersionが不正です"
    }
    $sln = Join-Path $projectRoot ($ProjectId + ".sln")
    $vcx = Join-Path $projectRoot ($ProjectId + ".vcxproj")
    $filters = Join-Path $projectRoot ($ProjectId + ".vcxproj.filters")
    foreach ($path in @($sln, $vcx, $filters)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            Fail ("生成Project fileがありません: " + $path)
        }
    }

    Assert-ExactCount $sln $ProjectId 2
    Assert-ExactCount $sln $MainGuid 7
    Assert-ExactCount $sln $SolutionGuid 1
    Assert-ExactCount $vcx $MainGuid 1 $true
    Assert-ExactCount $vcx ("<RootNamespace>" + $ProjectId +
        "</RootNamespace>") 1

    $header = Join-Path $projectRoot "engine/utility/EditableResourcePath.h"
    $winApp = Join-Path $projectRoot "engine/base/WinApp.cpp"
    Assert-ExactCount $header $ExpectedEditableProjectMarker 1
    Assert-ExactCount $winApp ('L"' + $ProjectId + 'WindowClass"') 1
    $readmePath = Join-Path $OutputRoot "README.md"
    Assert-ExactCount $readmePath $ExpectedSolutionPath 1
    Assert-ExactCount $readmePath "{{SOLUTION_PATH}}" 0

    $oldMarkers = @(
        @{ Path = $sln; Marker = "CG2_2025_04_14"; Count = 0 },
        @{ Path = $sln; Marker = "{4B19D937-553A-49D6-A14F-5AE56BAF0FE5}"; Count = 0 },
        @{ Path = $vcx; Marker = "{4b19d937-553a-49d6-a14f-5ae56baf0fe5}"; Count = 0 },
        @{ Path = $vcx; Marker = "<RootNamespace>CG220250414</RootNamespace>"; Count = 0 },
        @{ Path = $header; Marker = "CG2_2025_04_14.vcxproj"; Count = 0 },
        @{ Path = $winApp; Marker = 'L"CG2WindowClass"'; Count = 0 },
        @{ Path = $winApp; Marker = 'L"CG2"'; Count = 0 }
    )
    foreach ($entry in $oldMarkers) {
        Assert-ExactCount $entry.Path $entry.Marker $entry.Count
    }

    $catalogPath = Join-Path $projectRoot "resources/scenes/scenes.json"
    $scenePath = Join-Path $projectRoot ("resources/scenes/" +
        $StartSceneId + ".scene.json")
    $particlePath = Join-Path $projectRoot "resources/particles/scene_particles.json"
    $catalog = Read-Utf8 $catalogPath | ConvertFrom-Json
    $scene = Read-Utf8 $scenePath | ConvertFrom-Json
    $particleLayout = Read-Utf8 $particlePath | ConvertFrom-Json
    if ($catalog.startScene -ne $StartSceneId -or
        $catalog.scenes.Count -ne 1 -or
        $catalog.scenes[0].path -ne ("resources/scenes/" + $StartSceneId +
            ".scene.json")) {
        Fail "生成Scene catalogのstartScene／pathが不正です"
    }
    if ($scene.version -ne 27 -or $scene.entities.Count -ne 0) {
        Fail "生成開始Sceneが空のcurrent-version documentではありません"
    }
    if ($particleLayout.version -ne 2 -or
        @($particleLayout.assets.PSObject.Properties).Count -ne 0 -or
        @($particleLayout.scenes.PSObject.Properties).Count -ne 0) {
        Fail "生成scene_particles.jsonが空layoutではありません"
    }

    foreach ($required in @(
        "resources/shaders",
        "resources/scenes",
        "resources/prefabs",
        "resources/particles",
        "resources/noise0.png",
        "resources/noise1.png",
        "resources/rostock_laage_airport_4k.dds",
        "resources/particles/core_burst.json",
        "resources/particles/ring_burst.json",
        "resources/circleEntity.png",
        "resources/gradationLine.png"
    )) {
        if (-not (Test-Path -LiteralPath (Join-Path $projectRoot $required))) {
            Fail ("baseline Asset／directoryがありません: " + $required)
        }
    }

    Assert-NoForbiddenOutput $OutputRoot $ProjectId
}

$oldProjectName = "CG2_2025_04_14"
$oldMainGuidUpper = "{4B19D937-553A-49D6-A14F-5AE56BAF0FE5}"
$oldMainGuidLower = "{4b19d937-553a-49d6-a14f-5ae56baf0fe5}"
$oldSolutionGuid = "{1104DBB8-CC95-4A38-9FB3-2F4258C1A685}"

$ProjectId = $ProjectId.Trim()
$DisplayName = $DisplayName.Trim()
$StartSceneId = $StartSceneId.Trim()
if ($ProjectId -notmatch '^[A-Za-z][A-Za-z0-9_]{0,63}$') {
    Fail "ProjectIdは英字始まりのASCII識別子にしてください"
}
if ($ProjectId -ieq $oldProjectName) {
    Fail "ProjectIdは現在Project名と異なる値にしてください"
}
if ($ProjectId -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$') {
    Fail "ProjectIdがWindows device予約名です"
}
if ($DisplayName.Length -lt 1 -or $DisplayName.Length -gt 80 -or
    $DisplayName -match '[\x00-\x1F\x7F"\\]') {
    Fail "DisplayNameは1〜80文字で、制御文字・引用符・backslashを含めないでください"
}
if ($StartSceneId -notmatch '^[a-z][a-z0-9_-]{0,63}$') {
    Fail "StartSceneIdは小文字始まりのASCII識別子にしてください"
}
$editableProjectMarker = if ($OutputLayout -eq "GroupedV1") {
    "build/generated/$ProjectId/$ProjectId.vcxproj"
} else {
    "$ProjectId.vcxproj"
}
$solutionRelativePath = if ($OutputLayout -eq "ProjectFilesV2") {
    "intermediate/project-files/$ProjectId.sln"
} elseif ($OutputLayout -eq "GroupedV1") {
    "project/build/generated/$ProjectId/$ProjectId.sln"
} else {
    "project/$ProjectId.sln"
}

$sourceProjectRoot = Resolve-FullPath (Join-Path $PSScriptRoot "..")
$repositoryRoot = Resolve-FullPath (Join-Path $sourceProjectRoot "..")
$templateRoot = Resolve-FullPath (Join-Path $PSScriptRoot "../templates/empty_game")

# 設定不備で出力を作らないため、staging作成前に新旧候補を解決する。
Assert-RelativePath $repositoryRoot "config/project.build.json" "build specification"
Assert-RelativePath $sourceProjectRoot "build/project.build.json" "legacy build specification"
$currentBuildSpecification = Resolve-FullPath (Join-Path $repositoryRoot "config/project.build.json")
$legacyBuildSpecification = Resolve-FullPath (Join-Path $sourceProjectRoot "build/project.build.json")
$currentBuildExists = Test-Path -LiteralPath $currentBuildSpecification
$legacyBuildExists = Test-Path -LiteralPath $legacyBuildSpecification
if ($currentBuildExists -and $legacyBuildExists) {
    Fail "build specificationが新旧両方に存在します。二重配置を解消してください"
}
$sourceBuildSpecification = if ($currentBuildExists) {
    $currentBuildSpecification
} else {
    $legacyBuildSpecification
}
if (-not (Test-PathWithin $sourceBuildSpecification $repositoryRoot) -or
    -not (Test-Path -LiteralPath $sourceBuildSpecification -PathType Leaf)) {
    Fail ("build specificationが存在しないか、不正なfileです: " + $sourceBuildSpecification)
}
Read-Utf8 $sourceBuildSpecification | Out-Null

$manifestPath = Join-Path $templateRoot "template-manifest.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    Fail "template-manifest.jsonがありません"
}
$manifest = Read-Utf8 $manifestPath | ConvertFrom-Json
if ($manifest.schemaVersion -ne 1 -or $manifest.templateId -ne "empty-game") {
    Fail "template manifestのschemaVersion／templateIdが不正です"
}

$manifestPaths = @(
    @($manifest.copyDirectories) +
    @($manifest.copyFiles) +
    @($manifest.resourceFiles)
)
$duplicateManifestPath = $manifestPaths |
    Group-Object |
    Where-Object { $_.Count -gt 1 }
if ($duplicateManifestPath) {
    Fail ("manifest内Pathが重複しています: " +
        $duplicateManifestPath[0].Name)
}
foreach ($directory in @($manifest.copyDirectories)) {
    Assert-RelativePath $sourceProjectRoot ([string]$directory) "copyDirectories"
}
foreach ($file in @($manifest.copyFiles)) {
    if ([string]$file -ieq "build/project.build.json") { continue }
    Assert-RelativePath $sourceProjectRoot ([string]$file) "copyFiles"
}
foreach ($file in @($manifest.resourceFiles)) {
    Assert-RelativePath (Join-Path $sourceProjectRoot "resources") ([string]$file) "resourceFiles"
}

$sourceSln = Join-Path $sourceProjectRoot ($oldProjectName + ".sln")
$sourceVcx = Join-Path $sourceProjectRoot ($oldProjectName + ".vcxproj")
$sourceHeader = Join-Path $sourceProjectRoot "engine/utility/EditableResourcePath.h"
$sourceWinApp = Join-Path $sourceProjectRoot "engine/base/WinApp.cpp"
Assert-ExactCount $sourceSln $oldProjectName 2
Assert-ExactCount $sourceSln $oldMainGuidUpper 7
Assert-ExactCount $sourceSln $oldSolutionGuid 1
Assert-ExactCount $sourceVcx $oldMainGuidLower 1 $true
Assert-ExactCount $sourceVcx "<RootNamespace>CG220250414</RootNamespace>" 1
Assert-ExactCount $sourceHeader ($oldProjectName + ".vcxproj") 1
# V2は既知のdescriptor対応Sourceだけ。全文識別であり自動更新はしない。
if ($OutputLayout -eq "ProjectFilesV2") {
    if (-not (Test-Path -LiteralPath $sourceHeader -PathType Leaf)) { Fail "Template Source root detector is missing." }
    $headerItem = Get-Item -LiteralPath $sourceHeader -Force
    while ($null -ne $headerItem) {
        if (($headerItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { Fail "Template Source root detector path contains a reparse point." }
        if ($headerItem -is [IO.FileInfo]) { $headerItem = $headerItem.Directory } else { $headerItem = $headerItem.Parent }
    }
    # ReadAllTextのBOM自動除去と重ねず、C++側と同じ一個だけを除去する。
    $rootHeader = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($sourceHeader))
    # 文化依存の比較はBOMを無視し得るため、BOMなしの先頭文字を削らないよう厳密比較する。
    if ($rootHeader.StartsWith([string][char]0xFEFF, [StringComparison]::Ordinal)) { $rootHeader = $rootHeader.Substring(1) }
    $rootHeader = $rootHeader.Replace("`r`n", "`n")
    $fingerprint = [System.Numerics.BigInteger]::Parse("14695981039346656037")
    $modulus = [System.Numerics.BigInteger]::Pow(2, 64)
    foreach ($byte in [Text.Encoding]::UTF8.GetBytes($rootHeader)) {
        $fingerprint = ([System.Numerics.BigInteger]::op_ExclusiveOr($fingerprint, [System.Numerics.BigInteger]$byte) * [System.Numerics.BigInteger]::Parse("1099511628211")) % $modulus
    }
    if (([uint64]$fingerprint).ToString("X16") -ne "1D00E42E62656193") {
        Fail "Template Source root detector is not the verified descriptor-aware version. Update the Template Source before creating V2."
    }
}

Assert-ExactCount $sourceWinApp 'L"CG2WindowClass"' 1
Assert-ExactCount $sourceWinApp 'L"CG2"' 1

$destinationRootFull = Resolve-FullPath $DestinationRoot
if (-not (Test-Path -LiteralPath $destinationRootFull -PathType Container)) {
    Fail "DestinationRootが存在するdirectoryではありません"
}
if (Test-PathWithin $destinationRootFull $repositoryRoot) {
    Fail "DestinationRootは現在repositoryの外側に指定してください"
}
$finalPath = Resolve-FullPath (Join-Path $destinationRootFull $ProjectId)
if (Test-Path -LiteralPath $finalPath) {
    Fail ("出力先が既に存在します。上書きしません: " + $finalPath)
}
if (Test-PathWithin $finalPath $repositoryRoot) {
    Fail "出力先が現在repository内です"
}

$newMainGuid = ([guid]::NewGuid()).ToString("B").ToUpperInvariant()
do {
    $newSolutionGuid = ([guid]::NewGuid()).ToString("B").ToUpperInvariant()
} while ($newSolutionGuid -eq $newMainGuid)

$runToken = ([guid]::NewGuid()).ToString("N")
$stagingName = ".creating-" + $ProjectId + "-" + $runToken
$stagingPath = Join-Path $destinationRootFull $stagingName
$stagingProject = Join-Path $stagingPath "project"

try {
    New-Item -ItemType Directory -Path $stagingPath -ErrorAction Stop | Out-Null
    New-Item -ItemType Directory -Path $stagingProject -Force | Out-Null

    foreach ($directory in @($manifest.copyDirectories)) {
        Copy-ManifestDirectory ([string]$directory) $sourceProjectRoot `
            $stagingProject $manifest
    }
    foreach ($file in @($manifest.copyFiles)) {
        if ([string]$file -ieq "build/project.build.json") { continue }
        Copy-ManifestFile ([string]$file) $sourceProjectRoot $stagingProject `
            "copyFiles" $manifest
    }
    # Manifestが旧形式でも、仕様はroot/configへ一度だけbyte copyする。
    Assert-RelativePath $stagingPath "config/project.build.json" "build specification destination"
    $stagedBuildSpecification = Resolve-FullPath (Join-Path $stagingPath "config/project.build.json")
    if (-not (Test-PathWithin $sourceBuildSpecification $repositoryRoot) -or
        -not (Test-Path -LiteralPath $sourceBuildSpecification -PathType Leaf) -or
        -not (Test-PathWithin $stagedBuildSpecification (Resolve-FullPath $stagingPath)) -or
        (Test-Path -LiteralPath $stagedBuildSpecification)) {
        Fail "build specificationのcopy元／copy先が不正です"
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $stagedBuildSpecification) -Force | Out-Null
    Copy-Item -LiteralPath $sourceBuildSpecification -Destination $stagedBuildSpecification -ErrorAction Stop
    foreach ($file in @($manifest.resourceFiles)) {
        Copy-ManifestFile ([string]$file) `
            (Join-Path $sourceProjectRoot "resources") `
            (Join-Path $stagingProject "resources") `
            "resourceFiles" $manifest
    }

    $templateReadme = Read-Utf8 (Join-Path $templateRoot "README.md.template")
    $readme = $templateReadme.Replace("{{PROJECT_ID}}", $ProjectId).Replace(
        "{{DISPLAY_NAME}}", $DisplayName).Replace("{{START_SCENE_ID}}", $StartSceneId).Replace(
        "{{SOLUTION_PATH}}", $solutionRelativePath)
    Write-Utf8 (Join-Path $stagingPath "README.md") $readme
    Write-Utf8 (Join-Path $stagingPath ".gitignore") `
        (Read-Utf8 (Join-Path $templateRoot "root.gitignore"))
    Write-Utf8 (Join-Path $stagingPath ".gitattributes") `
        (Read-Utf8 (Join-Path $templateRoot "root.gitattributes"))
    Write-Utf8 (Join-Path $stagingProject ".gitignore") `
        (Read-Utf8 (Join-Path $templateRoot "project.gitignore"))

    $stagedSln = Join-Path $stagingProject ($oldProjectName + ".sln")
    $stagedVcx = Join-Path $stagingProject ($oldProjectName + ".vcxproj")
    $stagedHeader = Join-Path $stagingProject "engine/utility/EditableResourcePath.h"
    $stagedWinApp = Join-Path $stagingProject "engine/base/WinApp.cpp"
    Replace-ExactChecked $stagedSln $oldProjectName $ProjectId 2
    Replace-ExactChecked $stagedSln $oldMainGuidUpper $newMainGuid 7
    Replace-ExactChecked $stagedSln $oldSolutionGuid $newSolutionGuid 1
    Replace-ExactChecked $stagedVcx $oldMainGuidLower $newMainGuid 1 $true
    Replace-ExactChecked $stagedVcx "<RootNamespace>CG220250414</RootNamespace>" `
        ("<RootNamespace>" + $ProjectId + "</RootNamespace>") 1
    Replace-ExactChecked $stagedHeader ($oldProjectName + ".vcxproj") `
        $editableProjectMarker 1
    Replace-ExactChecked $stagedWinApp 'L"CG2WindowClass"' `
        ('L"' + $ProjectId + 'WindowClass"') 1
    Replace-ExactChecked $stagedWinApp 'L"CG2"' `
        ('L"' + $DisplayName + '"') 1

    $targetSln = Join-Path $stagingProject ($ProjectId + ".sln")
    $targetVcx = Join-Path $stagingProject ($ProjectId + ".vcxproj")
    $targetFilters = Join-Path $stagingProject ($ProjectId + ".vcxproj.filters")
    Move-Item -LiteralPath $stagedSln -Destination $targetSln
    Move-Item -LiteralPath $stagedVcx -Destination $targetVcx
    Move-Item -LiteralPath (Join-Path $stagingProject `
        ($oldProjectName + ".vcxproj.filters")) -Destination $targetFilters

    New-Item -ItemType Directory -Path (Join-Path $stagingProject "resources/scenes") -Force |
        Out-Null
    New-Item -ItemType Directory -Path (Join-Path $stagingProject "resources/prefabs") -Force |
        Out-Null
    New-Item -ItemType Directory -Path (Join-Path $stagingProject "resources/particles") -Force |
        Out-Null

    $catalog = [ordered]@{
        scenes = @([ordered]@{
            id = $StartSceneId
            name = "Main"
            path = "resources/scenes/$StartSceneId.scene.json"
            runtimeProfile = "RUNTIME"
        })
        startScene = $StartSceneId
        startup = [ordered]@{
            debug = "EDITOR"
            development = "EDITOR"
            release = "RUNTIME"
        }
        version = 1
    }
    Write-Utf8 (Join-Path $stagingProject "resources/scenes/scenes.json") `
        ($catalog | ConvertTo-Json -Depth 8)

    $sceneDocument = [ordered]@{
        entities = @()
        sceneName = "Main"
        version = 27
    }
    Write-Utf8 (Join-Path $stagingProject `
        ("resources/scenes/$StartSceneId.scene.json")) `
        ($sceneDocument | ConvertTo-Json -Depth 8)

    $emptyParticleLayout = [ordered]@{
        assets = [ordered]@{}
        scenes = [ordered]@{}
        version = 2
    }
    Write-Utf8 (Join-Path $stagingProject `
        "resources/particles/scene_particles.json") `
        ($emptyParticleLayout | ConvertTo-Json -Depth 8)

    Assert-GeneratedProject $stagingPath $ProjectId $StartSceneId `
        $newMainGuid $newSolutionGuid $editableProjectMarker $solutionRelativePath

    if (Test-Path -LiteralPath $finalPath) {
        Fail "検証後に出力先が作成されました。競合のためrenameしません"
    }
    Move-Item -LiteralPath $stagingPath -Destination $finalPath
    Write-Output ("生成完了: " + $finalPath)
    Write-Output ("Solution: " + (Join-Path $finalPath $solutionRelativePath))
} catch {
    $failure = $_
    if ($stagingPath -and (Test-Path -LiteralPath $stagingPath)) {
        $resolvedStaging = Resolve-FullPath $stagingPath
        $safeParent = (Resolve-FullPath $destinationRootFull).TrimEnd('\', '/')
        $safeName = Split-Path -Leaf $resolvedStaging
        if ($safeName -eq $stagingName -and
            (Split-Path -Parent $resolvedStaging).Equals(
                $safeParent,
                [System.StringComparison]::OrdinalIgnoreCase
            )) {
            Remove-Item -LiteralPath $resolvedStaging -Recurse -Force
        } else {
            Write-Error ("staging安全検証に失敗したためcleanupを行いません: " +
                $resolvedStaging)
        }
    }
    throw $failure
}
