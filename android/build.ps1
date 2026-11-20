$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$cosmoRoot = Split-Path -Parent $repoRoot
$envScript = Join-Path $cosmoRoot 'android-toolchain\setup-env.ps1'
if (-not (Test-Path -LiteralPath $envScript -PathType Leaf)) {
    throw "Android toolchain setup was not found at $envScript."
}
. $envScript

$sharewareFiles = @(
    (Join-Path $repoRoot 'gamedata\COSMO1.STN'),
    (Join-Path $repoRoot 'gamedata\COSMO1.VOL')
)
if ($sharewareFiles | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) }) {
    $sharewareUrl = 'https://archive.org/download/CosmosCosmicAdventure/CosmosCosmicAdventure-ForbiddenPlanet-Adventure1Of3V1.20sw1992apogeeSoftwareLtd.action.zip'
    $expectedHashes = @{
        'COSMO1.STN' = '55963b5331a7159fe6ca134e9ab807e59e6ef798057fc28a1081c1e6a273ff52'
        'COSMO1.VOL' = 'f578a1b72423e83b363c5c019faac5b96124f39bc4a2ec4ee09cabc29f3a63fd'
    }
    $sharewareWork = Join-Path $env:GRADLE_USER_HOME 'shareware-fetch'
    $sharewareArchive = Join-Path $sharewareWork 'shareware.zip'
    $sharewareExtract = Join-Path $sharewareWork 'extracted'
    New-Item -ItemType Directory -Force -Path $sharewareWork | Out-Null

    try {
        Invoke-WebRequest -Uri $sharewareUrl -OutFile $sharewareArchive
        Expand-Archive -LiteralPath $sharewareArchive -DestinationPath $sharewareExtract -Force

        foreach ($fileName in $expectedHashes.Keys) {
            $sourceFile = Get-ChildItem -LiteralPath $sharewareExtract -Recurse -File -Filter $fileName |
                Select-Object -First 1
            if (-not $sourceFile) {
                throw "The shareware archive did not contain $fileName."
            }
            $actualHash = (Get-FileHash -LiteralPath $sourceFile.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actualHash -ne $expectedHashes[$fileName]) {
                throw "Checksum mismatch for $fileName. Expected $($expectedHashes[$fileName]), got $actualHash."
            }
            Copy-Item -LiteralPath $sourceFile.FullName -Destination (Join-Path $repoRoot "gamedata\$fileName") -Force
        }
    } finally {
        $resolvedWork = (Resolve-Path -LiteralPath $sharewareWork).Path
        $cacheRoot = (Resolve-Path -LiteralPath $env:GRADLE_USER_HOME).Path
        if (-not $resolvedWork.StartsWith($cacheRoot + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to clean up outside the Gradle cache: $resolvedWork"
        }
        Remove-Item -LiteralPath $resolvedWork -Recurse -Force
    }
}

Push-Location $PSScriptRoot
try {
    & '.\gradlew.bat' --no-daemon assembleDebug
    if ($LASTEXITCODE -ne 0) {
        throw "Gradle build failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}

$apk = Join-Path $PSScriptRoot 'app\build\outputs\apk\debug\app-debug.apk'
Write-Host "Android APK created: $apk"
