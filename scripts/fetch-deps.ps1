<#
.SYNOPSIS
Fetches the build dependencies that cannot be committed.

.DESCRIPTION
MilkDrop links against D3DX9, a deprecated Direct3D 9 utility library. Microsoft
publishes it as the Microsoft.DXSDK.D3DX NuGet package, which is the supported
replacement for installing the long discontinued DirectX SDK (June 2010) and
avoids that installer's S1023 failure on machines carrying a newer VC++ runtime.

The package cannot be vendored into a public repository. Its terms permit
distributing the redistributable DLLs inside an application you build, but not
publishing the headers and import library, which are SDK components rather than
distributable code. So they are fetched here instead, into a directory the
repository ignores.

Run once after cloning. Re-running is harmless and re-downloads nothing if the
files are already in place.
#>
[CmdletBinding()]
param(
    # Fetch again even if the files are already present.
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$PackageId      = 'Microsoft.DXSDK.D3DX'
$PackageVersion = '9.29.952.8'

$repoRoot = Split-Path -Parent $PSScriptRoot
$destRoot = Join-Path $repoRoot 'code\third_party\d3dx9'

$marker = Join-Path $destRoot 'include\d3dx9.h'
if ((Test-Path $marker) -and -not $Force) {
    Write-Host "D3DX9 already present at $destRoot. Use -Force to refetch."
    exit 0
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("milkrun-deps-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null

try {
    $nupkg = Join-Path $work 'd3dx.nupkg'
    $url   = "https://www.nuget.org/api/v2/package/$PackageId/$PackageVersion"

    Write-Host "Fetching $PackageId $PackageVersion ..."
    Invoke-WebRequest -Uri $url -OutFile $nupkg -UseBasicParsing

    $extract = Join-Path $work 'extract'
    Expand-Archive -Path $nupkg -DestinationPath $extract -Force

    $src = Join-Path $extract 'build\native'
    if (-not (Test-Path $src)) {
        throw "The package layout changed: expected build\native inside $PackageId."
    }

    foreach ($d in @('include', 'lib\x86', 'bin\x86')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $destRoot $d) | Out-Null
    }

    # Only the D3DX9 pieces. The package also carries D3DX10 and D3DX11, which
    # nothing here uses.
    Copy-Item (Join-Path $src 'include\d3dx9*.h')   (Join-Path $destRoot 'include') -Force
    Copy-Item (Join-Path $src 'include\d3dx9*.inl') (Join-Path $destRoot 'include') -Force
    Copy-Item (Join-Path $src 'include\rmxf*.h')    (Join-Path $destRoot 'include') -Force
    Copy-Item (Join-Path $src 'release\lib\x86\d3dx9.lib') (Join-Path $destRoot 'lib\x86') -Force

    # These two ship beside the exe. d3dx9_43 delegates shader compilation to
    # D3DCompiler_43, so shipping one without the other fails at run time.
    Copy-Item (Join-Path $src 'release\bin\x86\D3DX9_43.dll')      (Join-Path $destRoot 'bin\x86') -Force
    Copy-Item (Join-Path $src 'release\bin\x86\D3DCompiler_43.dll') (Join-Path $destRoot 'bin\x86') -Force

    Copy-Item (Join-Path $extract 'LICENSE.txt') (Join-Path $destRoot 'LICENSE.txt') -Force

    Write-Host "D3DX9 ready at $destRoot"
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
