$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$bundle = Join-Path $repo 'release\paintify-obs'
$bin = Join-Path $bundle 'bin\64bit'
$data = Join-Path $bundle 'data'
$licenses = Join-Path $bundle 'licenses'
$plugin = Join-Path $repo 'build-obs-ninja\paintify-obs.dll'
$renderer = Join-Path $repo 'build-live\gpu-sbr.exe'
if (!(Test-Path -LiteralPath $plugin)) { throw "Build the OBS plugin first: $plugin" }
if (!(Test-Path -LiteralPath $renderer)) { throw "Build the GPU renderer first: $renderer" }
New-Item -ItemType Directory -Force -Path $bin, $data, $licenses | Out-Null
Copy-Item -LiteralPath $plugin -Destination $bin
Copy-Item -LiteralPath $renderer -Destination $data
Copy-Item -LiteralPath (Join-Path $repo 'shaders') -Destination $data -Recurse -Force
Copy-Item -LiteralPath (Join-Path $repo 'obs\locale') -Destination $data -Recurse -Force
Copy-Item -LiteralPath (Join-Path $repo 'obs\INSTALL.txt') -Destination $bundle
Copy-Item -LiteralPath (Join-Path $repo 'README.md') -Destination $bundle
$obsDeps = Join-Path $repo 'build-obs-ninja\vcpkg_installed\x64-windows\bin'
$rendererDeps = Join-Path $repo 'build-live\vcpkg_installed\x64-windows\bin'
foreach ($entry in @(
    @{ Name = 'SpoutDX.dll'; Source = $obsDeps; Target = $bin },
    @{ Name = 'Spout.dll'; Source = $rendererDeps; Target = $data },
    @{ Name = 'glfw3.dll'; Source = $rendererDeps; Target = $data }
)) {
    $source = Join-Path $entry.Source $entry.Name
    if (!(Test-Path -LiteralPath $source)) { throw "Runtime dependency not found: $source" }
    Copy-Item -LiteralPath $source -Destination $entry.Target
}
Copy-Item -LiteralPath (Join-Path $obsDeps '..\share\spout2\copyright') -Destination (Join-Path $licenses 'Spout2.txt')
Copy-Item -LiteralPath (Join-Path $rendererDeps '..\share\glfw3\copyright') -Destination (Join-Path $licenses 'GLFW.txt')
Copy-Item -LiteralPath (Join-Path $repo 'obs\COPYING') -Destination (Join-Path $licenses 'Paintify-OBS-GPL-2.0.txt')
Compress-Archive -LiteralPath $bundle -DestinationPath (Join-Path $repo 'release\paintify-obs-windows.zip') -Force
Write-Output (Join-Path $repo 'release\paintify-obs-windows.zip')
