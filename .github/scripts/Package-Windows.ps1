[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo',
    [switch] $BuildInstaller
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
            "${ProjectRoot}/release/${ProductName}-*-windows-*-installer.exe"
        )
    }

    Remove-Item @RemoveArgs

    Log-Group "Archiving ${ProductName}..."
    $CompressArgs = @{
        Path = (Get-ChildItem -Path "${ProjectRoot}/release/${Configuration}" -Exclude "${OutputName}*.*")
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Log-Group

    if ( $BuildInstaller ) {
        # Inno Setup 6: cmake/windows/resources/installer-Windows.iss
        $Iscc = (Get-Command iscc -ErrorAction SilentlyContinue).Source
        if ( ! $Iscc ) {
            $Iscc = @(
                "${env:ProgramFiles(x86)}/Inno Setup 6/ISCC.exe"
                "${env:ProgramFiles}/Inno Setup 6/ISCC.exe"
                "${env:LOCALAPPDATA}/Programs/Inno Setup 6/ISCC.exe"
            ) | Where-Object { Test-Path $_ } | Select-Object -First 1
        }
        if ( ! $Iscc ) {
            throw 'Inno Setup 6 (ISCC.exe) not found; it builds the Windows installer.'
        }

        Log-Group "Building the installer for ${ProductName}..."
        $IsccArgs = @(
            "/DProductName=${ProductName}"
            "/DDisplayName=$($BuildSpec.displayName)"
            "/DProductVersion=${ProductVersion}"
            "/DAuthor=$($BuildSpec.author)"
            "/DWebsite=$($BuildSpec.website)"
            "/DSourceDir=$(Resolve-Path "${ProjectRoot}/release/${Configuration}")"
            "/DOutputDir=$(Resolve-Path "${ProjectRoot}/release")"
            "/DOutputBaseFilename=${OutputName}-installer"
            "${ProjectRoot}/cmake/windows/resources/installer-Windows.iss"
        )
        Invoke-External $Iscc @IsccArgs
        Log-Group
    }
}

Package
