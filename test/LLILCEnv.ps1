# -------------------------------------------------------------------------
#
# This script provides LLILC development environment and test harness.
#
# -------------------------------------------------------------------------

<#
.SYNOPSIS
    Setup LLILC development environment and provide test harness.

.DESCRIPTION
    This script set up the LLILC environment with the assumptions below:
    
    1. The following software are installed:
    Visual Studio 12.0, Git, CMake, Python, GnuWin32, and DiffMerge
 
    The script will check if the desired executable is already on path.
    If not, it will stop going further.

    2. LLVM and LLILC local repositories are created. Note that LLILC
    should be located at the correct place under tools\LLILC in LLVM 
    repository. The only environment variable you have to specify
    is LLVMSOURCE.
    
    This script completes the environment setup and reports the status.
    
    Typical steps to use this envrironment and test harness:

    A. In first launch, BuildAll, it will build LLVM and LLILC.
    In daily use, replace BuildAll with just Build, which only builds LLILC.
    
    B. The second step is to do BuildTest. It will build the regression
    tests in downloaded CoreCLR test assets.

    C. The third step is to do RunTest. It will run the CoreCLR regression
    tests, reports results, and create LLVM IR diff against baseline.

    D. The fourth step is to do CheckDiff. If there were any diff,
    you can use CheckDiff to bring the diffs into DiffMerge to examine
    in details.
    
    This script provides some daily routines beside the above four
    common tasks:

    LLILCHelp, CopyJIT, ReBaseAll, ApplyFilter, CheckEnv 
    
    LLILC is under the umbrella of LLVM. A default location is used for
    LLVM build directory. You can override the location by specifying
    environment variable LLVMBUILD. 

    Under the hood, LLILC uses CoreCLR's test assets and a CoreCLR
    Runtime paired with LLILC JIT for testing. The two parts will
    be downloaded and put in a default location. You can override
    the location by specifying environment variable LLILCTESTRESULT.

.PARAMETER Arch
    Target Architecture

.PARAMETER Build
    Debug or Release Build
    
.EXAMPLE
    LLILCEnv.ps1
    LLILCEnv.ps1 -Arch x64 -Build Debug
#>

[CmdletBinding()]
Param(
   [string]$Arch="x64",
   [string]$Build="Debug"
)

# -------------------------------------------------------------------------
#
# Validate preconditions: software installations and environment variables
# 
# -------------------------------------------------------------------------

function ValidatePreConditions
{
  # Validate Visual Studio

  $VSExists = Test-Path Env:\VS120COMNTOOLS
  if (!$VSExists) {
      throw "!!! Visual Studio 12.0 not installed." 
  }

  # Validate Git

  IsOnPath -executable "git.exe" -software "Git"

  # Validate CMake

  IsOnPath -executable "cmake.exe" -software "CMake"

  # Validate Python
  
  IsOnPath -executable "python.exe" -software "Python"

  # Validate GnuWin32

  IsOnPath -executable "wget.exe" -software "GnuWin32"

  # Validate Diff Tool
  
  IsOnPath -executable "sgdm.exe" -software "DiffMerge"

  # Validate LLVM

  $LLVMSourceExists = Test-Path Env:\LLVMSOURCE
  if (!$LLVMSourceExists) {
    throw "!!! LLVM Source not specified."
  }
  else {
    $LLVMSourceExists = Test-Path $Env:LLVMSOURCE
    if (!$LLVMSourceExists) {
      throw "!!! LLVM Source not available in specified location." 
    }
  }

  # Validate LLILC
  $LLILCSource = LLILCSource
  $LLILCSourceExists = Test-Path $LLILCSource
  if (!$LLILCSourceExists) {
    throw "!!! LLILC Source not available in correct place."
  }

  # Validate LLVMBuild

  $LLVMBuildExists = Test-Path Env:\LLVMBUILD
  if (!$LLVMBuildExists) {
    Write-Warning "LLVM build directory is not specified."
    $DefaultLLVMBuild = DefaultLLVMBuild
    Write-Warning "Default LLVM build directory: $DefaultLLVMBuild"
  }

  # Validate LLILCTESTRESULT

  $LLILCTestResultExists = Test-Path Env:\LLILCTESTRESULT
  if (!$LLILCTestResultExists) {
    Write-Warning "LLILC test result directory is not specified."
    $DefaultLLILCTestResult = DefaultLLILCTestResult
    Write-Warning "Default LLILC test result directory: $DefaultLLILCTestResult"
  }
}

# -------------------------------------------------------------------------
#
# A list of resource location functions
#
# -------------------------------------------------------------------------

function Global:LLILCSource
{
  return "$Env:LLVMSOURCE\tools\llilc"
}

function Global:LLILCTest
{
  $LLILCSource = LLILCSource
  return "$LLILCSource\test"
}

function Global:LLILCJit([string]$Build="Debug")
{
  $LLVMBuild = LLVMBuild
  $LLILCJit = "$LLVMBUILD\bin\$Build\LLILCJit.dll"
  return $LLILCJit
}

function Global:DefaultLLILCTestResult
{
  return "$Env:TEMP\LLILCTestResult"
}

function Global:LLILCTestResult
{
  $LLILCTestResultExists = Test-Path Env:\LLILCTESTRESULT
  if (!$LLILCTestResultExists) {
    $DefaultLLILCTestResult = DefaultLLILCTestResult 
    return "$DefaultLLILCTestResult"
  }
  else {
    return $Env:LLILCTESTRESULT
  }
}

function Global:CoreCLRRuntime
{
  $LLILCTestResult = LLILCTestResult
  return "$LLILCTestResult\CoreCLRRuntime"
}

function Global:CoreCLRTestAssets
{
  $LLILCTestResult = LLILCTestResult
  return "$LLILCTestResult\CoreCLRTestAssets"
}

function Global:DefaultLLVMBuild
{
  return "$Env:TEMP\LLVMBuild"
}

function Global:LLVMBuild
{
  $LLVMBuildExists = Test-Path Env:\LLVMBUILD
  if (!$LLVMBuildExists) {
    $DefaultLLVMBuild = DefaultLLVMBuild
    return "$DefaultLLVMBuild"
  }
  else {
    return $Env:LLVMBUILD
  }
}

# -------------------------------------------------------------------------
#
# Get the CoreCLR Version
#
# -------------------------------------------------------------------------

function Global:CoreCLRVersion
{
  $LLILCSource = LLILCSOURCE
  (Get-Content $LLILCSource\utils\packages.config) | ForEach-Object { 
    if ($_ -match 'package id="(.*?)" version="(.*?)"') {
      $Result = $matches[1] + "." + $matches[2]
      return $Result
    }
  }   
}

# -------------------------------------------------------------------------
#
# Get the build of downloaded CoreCLR package
#
# -------------------------------------------------------------------------


function Global:CoreCLRBuild
{
  $CoreCLRVersion = CoreCLRVersion

  if ($CoreCLRVersion -match "Debug") {
    $CoreCLRBuild = "Debug"
  }
  else {
    $CoreCLRBuild = "Release"
  }
  return $CoreCLRBuild
}

# -------------------------------------------------------------------------
#
# Set Visual Studio Command line environment variables.
# 
# -------------------------------------------------------------------------

function SetVCVars
{
  # This is complicated two things:
  # 1. When powershell invokes a cmd or bat file, any environment variables
  # changes made by that file are lost when the file finishes executing.
  # So one must resort to trickery to capture the environment variables.
  # 2. by the fact that the path to the batch
  # file that set these has blanks in it. 
  #
  # To work around the limitations of cmd we create a temporary batch file and
  # execute it to capture the new environment variables.
  # Create the temp file in the user's temp directory and
  # use the current pid to avoid comflict with other instances
  # that might be running.

  $TempBat = Join-Path $Env:TEMP "getvc$pid.bat"
  #echo "TempBat = $TempBat"
  $File = "$Env:VS120COMNTOOLS\VsDevCmd.bat"
  #echo "VC batch file = $File"
  ("call ""$File""", "echo ENV_VARS_START", "set") | Out-File -Encoding ascii $TempBat
  $CmdOut = cmd /q /c $TempBat

  # If batch file fails we need to propagate the error.
  if ($LASTEXITCODE -gt 0) {
    $CmdOut
    exit $LASTEXITCODE
  }

  Remove-Item $TempBat | Out-Null

  ## Erase our current set of environment variables
  Remove-Item -path env:* | Out-Null

  ## Go through the environment variables returned by cmd.exe.
  ## For each of them, set the variable in our local environment.

  $FoundStartFlag = $false

  foreach ($Line in $CmdOut) {
    if ($Line -eq "ENV_VARS_START") {
      Write-Output ""
      $FoundStartFlag = $true
      continue
    }

    if ($FoundStartFlag -and ($Line -match "^(.*?)=(.*)$")) {
      $N = $matches[1]
      if ($N -eq "prompt") {
        # Ignore: Setting the prompt environment variable has no
        #         connection to the PowerShell prompt
      } elseif ($N -eq "title") {
        $host.ui.rawui.windowtitle = $matches[2];
        Set-Item -Path "env:$N" -Value $matches[2];
      } else {
        Set-Item -Path "env:$N" -Value $matches[2];
      }
    }
    elseif (!$FoundStartFlag) {
      # Output prior to our special flag is stuff generated by
      # setenv.cmd. Just pass it on to whomever is watching.
      Write-Output $Line
    }
  }
}

# -------------------------------------------------------------------------
#
# Check if executable is already on the Path
#
# -------------------------------------------------------------------------

function IsOnPath([string]$executable, [string]$software)
{
  if (-Not (Get-Command $executable -ErrorAction SilentlyContinue)) {
    throw  "!!! $executable not on path. Check the installation of $software."
  }
}

# -------------------------------------------------------------------------
#
# Do the rest of environment setup after validation
# 
# -------------------------------------------------------------------------

function CompleteEnvInit
{
  SetVCVars
  
  CreateLLILCTestResultDirectory

  CreateLLVMBuildDirectory

  GetCLRTestAssets
    
  NuGetCLR
}

# -------------------------------------------------------------------------
#
# Download nuget.exe
# 
# -------------------------------------------------------------------------

function Global:DownloadNuGet
{
  $CoreCLRRuntime = CoreCLRRuntime
  $NuGetExists = Test-Path $CoreCLRRuntime\NuGet.exe
  if (!$NuGetExists) {
    pushd .
    cd $CoreCLRRuntime
    wget http://nuget.org/NuGet.exe -OutFile NuGet.exe
    popd
  }
}

# -------------------------------------------------------------------------
#
# Perform a CoreCLR package Nuget.
# 
# -------------------------------------------------------------------------

function Global:NuGetCLR
{
  $CoreCLRRuntime = CoreCLRRuntime
  $CoreCLRRuntimeExists = Test-Path $CoreCLRRuntime
  $LLILCSource = LLILCSource

  if (!$CoreCLRRuntimeExists) {
    New-Item $CoreCLRRuntime -ItemType directory | Out-Null
  }
  else {
    Write-OutPut("CoreCLR Runtime already downloaded.")
    return
  }

  Write-OutPut("Downloading NuGet.exe...")
  DownloadNuGet
  copy $LLILCSource\utils\NuGet.config $CoreCLRRuntime
  copy $LLILCSource\utils\packages.config $CoreCLRRuntime
  pushd .
  cd $CoreCLRRUNTIME
  Write-OutPut("Performing a CoreCLR package NuGet...")    
  & .\NuGet.exe install
  popd
}

# -------------------------------------------------------------------------
#
# Get CLR Test Assets by cloning CoreCLR repo.
# 
# -------------------------------------------------------------------------

function Global:GetCLRTestAssets
{
  $CoreCLRTestAssets = CoreCLRTestAssets
  $CoreCLRTestAssetsExists = Test-Path $CoreCLRTestAssets
  if (!$CoreCLRTestAssetsExists) {
    pushd .
    New-Item $CoreCLRTestAssets -itemtype Directory  | Out-Null
    cd $CoreCLRTestAssets
    git clone https://github.com/dotnet/coreclr.git
    popd
  }
  else {
    Write-OutPut("CoreCLR Test Assets already downloaded.")
  }
}

# -------------------------------------------------------------------------
#
# Check the status of development environment.
# 
# -------------------------------------------------------------------------

function Global:CheckEnv
{
  $LLVMBuild = LLVMBuild
  $LLILCSource = LLILCSource
  $LLILCTest = LLILCTest
  $LLILCTestResult = LLILCTestResult
  $CoreCLRTestAssets =  CoreCLRTestAssets
  $CoreCLRRuntime = CoreCLRRuntime
  $CoreCLRVersion = CoreCLRVersion

  Write-Output("************************************ LLILC Work Environment **************************************")
  Write-Output("LLVM Source         : $Env:LLVMSOURCE")
  Write-Output("LLVM Build          : $LLVMBuild")
  Write-Output("LLILC Source        : $LLILCSource")
  Write-Output("LLILC Test          : $LLILCTest")
  Write-Output("LLILC Test Result   : $LLILCTestResult")
  Write-Output("CoreCLR Test Assets : $CoreCLRTestAssets")
  Write-Output("CoreCLR Runtime     : $CoreCLRRuntime")
  Write-Output("CoreCLR Version     : $CoreCLRVersion")
  Write-Output("**************************************************************************************************")
}

# -------------------------------------------------------------------------
#
# Create LLILC Test Result Directory. It will hold CoreCLR test assets,
# CoreCLR package, and Diff results.
# 
# -------------------------------------------------------------------------

function CreateLLILCTestResultDirectory
{
  $LLILCTestResult = LLILCTestResult
  $LLILCTestResultExist = Test-Path $LLILCTestResult
  if (!$LLILCTestResultExist) {
    New-Item $LLILCTestResult -itemtype Directory  | Out-Null
  }
}

# -------------------------------------------------------------------------
#
# Create LLVM build directory
# 
# -------------------------------------------------------------------------

function CreateLLVMBuildDirectory
{
  $LLVMBuild = LLVMBuild
  $LLVMBuildExists = Test-Path $LLVMBuild
  if (!$LLVMBuildExists) {
    New-Item $LLVMBuild -itemtype Directory  | Out-Null
  }
}

# -------------------------------------------------------------------------
#
# Setup LLILC development environment.
# 
# -------------------------------------------------------------------------

function LLILCEnvInit
{
  ValidatePreConditions
  CompleteEnvInit
  CheckEnv

  Write-Output("Use LLILCHelp for a list of commands. Use CheckEnv for a list of work environment.")

  # start with LLILC Source Directory
  $LLILCSource = LLILCSource
  cd $LLILCSource
}

# -------------------------------------------------------------------------
#
# Copy in LLILC JIT dll into CoreCLR runtime
#
# -------------------------------------------------------------------------

function Global:CopyJIT([string]$Build="Debug")
{
  $CoreCLRRuntime = CoreCLRRuntime
  $CoreCLRVersion = CoreCLRVersion
  $LLILCJit = LLILCJit($Build)
  $JitName = "LLILCJit.dll"

  $WorkLLILCJitExists = Test-Path $CoreCLRRuntime\$CoreCLRVersion\bin\$JitName
  if ($WorkLLILCJitExists) {
    Remove-Item $CoreCLRRuntime\$CoreCLRVersion\bin\$JitName | Out-Null
  }

  pushd .
  cd $CoreCLRRuntime\$CoreCLRVersion\bin\

  Write-Output ("Copying LLILC JIT")
  copy $LLILCJit $JitName
  Write-Output ("LLILC JIT Copied")

  popd
}

# -------------------------------------------------------------------------
#
# Configure LLVM Solution
#
# -------------------------------------------------------------------------

function Global:ConfigureLLVM([string]$Arch="x64")
{
  $LLVMBuild = LLVMBuild

  pushd .
  cd $LLVMBuild
  if ($Arch -eq "x64") {
    cmake -G "Visual Studio 12 2013 Win64" $Env:LLVMSOURCE -DLLVM_TARGETS_TO_BUILD:STRING=X86
  }
  else {
    cmake -G "Visual Studio 12" $Env:LLVMSOURCE -DLLVM_TARGETS_TO_BUILD:STRING=X86
  }
  popd
}

# -------------------------------------------------------------------------
#
# Build LLVM including LLILC JIT
#
# -------------------------------------------------------------------------

function Global:BuildLLVM([string]$Arch="x64", [string]$Build="Debug", [bool]$Parallel=$False)
{
  $LLVMBuild = LLVMBuild
  $TempBat = Join-Path $Env:TEMP "buildllvm.bat"
  $File = "$Env:VS120COMNTOOLS\..\..\VC\vcvarsall.bat"

  $MSwitch = ""
  if ($Parallel) {
    $MSwitch = " /m "
  }

  ("call ""$File"" x86", "msbuild $LLVMBuild\LLVM.sln /p:Configuration=$Build /p:Platfrom=$Arch /t:ALL_BUILD $MSwitch") | Out-File -Encoding ascii $TempBat
  
  Write-Output ("Building LLVM...")
  cmd /c $TempBat
  Remove-Item -force $TempBat | Out-Null
  CopyJIT -Build $Build
}

# -------------------------------------------------------------------------
#
# Configure and Build LLVM including LLILC JIT
#
# -------------------------------------------------------------------------

function Global:BuildAll([string]$Arch="x64", [string]$Build="Debug", [bool]$Parallel=$False)
{
  ConfigureLLVM -Arch $Arch
  BuildLLVM -Arch $Arch -Build $Build -Parallel $Parallel
}

# -------------------------------------------------------------------------
#
# Build LLILC JIT
#
# -------------------------------------------------------------------------

function Global:Build([string]$Build="Debug")
{
  $LLVMBuild = LLVMBuild
  $TempBat = Join-Path $Env:TEMP "buildllilc.bat"
  $File = "$Env:VS120COMNTOOLS\..\..\VC\vcvarsall.bat"
  
  ("call ""$File"" x86", "devenv /Build Debug  /Project LLILCReader $LLVMBuild\LLVM.sln", "devenv /Build Debug  /Project llilcjit $LLVMBuild\LLVM.sln") | Out-File -Encoding ascii $TempBat

  cmd /c $TempBat
  Remove-Item -force $TempBat | Out-Null
  CopyJIT -Build $Build
}

# -------------------------------------------------------------------------
#
# Apply Filter to suppress allowable LLVM IR difference in pipeline
#
# -------------------------------------------------------------------------

function Global:ApplyFilterAll{
    param(  
    [Parameter(
        Position=0, 
        Mandatory=$true, 
        ValueFromPipeline=$true,
        ValueFromPipelineByPropertyName=$true)
    ]
    [Alias('FullName')]
    [String[]]$FilePath
    ) 

    process {
       foreach($path in $FilePath)
       {
           ApplyFilter $path
       }
    }
}

# -------------------------------------------------------------------------
#
# Filter to suppress allowable LLVM IR difference
#
# -------------------------------------------------------------------------

function Global:ApplyFilter([string]$File)
{
  # Suppress address difference from run to run
  # Assume the address is at least 10-digit number
  # 
  # Example 1:
  # 
  # Normalize
  # %2 = call i64 inttoptr (i64 140704958972024 to i64 (i64)*)(i64 140704956891884)
  # to
  # %2 = call i64 inttoptr (i64 NORMALIZED_ADDRESS to i64 (i64)*)(i64 NORMALIZED_ADDRESS)
  #
  # Example 2:
  #
  # Normalize
  # %3 = icmp eq i64 140704956891886, %2
  # to
  # %3 = icmp eq i64 NORMALIZED_ADDRESS, %2

  (Get-Content $File) -replace 'i64 \d{10}\d*', 'i64 NORMALIZED_ADDRESS' | Out-File $File -Encoding ascii

  # Suppress type id difference from run to run
  #
  # Example 1:
  # 
  # Normalize
  # %3 = load %System.AppDomainSetup.239 addrspace(1)** %1
  # to
  # %3 = load %System.AppDomainSetup.NORMALIZED_TYPEID addrspace(1)** %1
  #
  # Example 2:
  #
  # Normalize
  # %0 = alloca %AppDomain.24 addrspace(1)*
  # to
  # %0 = alloca %AppDomain.NORMALIZED_TYPEID addrspace(1)*
  
  (Get-Content $File) -replace '%(.*?)\.\d+ addrspace', '%$1.NORMALIZED_TYPEID addrspace' | Out-File $File -Encoding ascii

  # Suppress type id difference from run to run, string name with double quotes

  (Get-Content $File) -replace '%"(.*?)\.\d+" addrspace', '%"$1.NORMALIZED_TYPEID" addrspace' | Out-File $File -Encoding ascii
}

# -------------------------------------------------------------------------
#
# Exclude test cases from running
#
# -------------------------------------------------------------------------

function Global:ExcludeTest([string]$Arch="x64", [string]$Build="Release")
{
  $CoreCLRTestAssets = CoreCLRTestAssets
  pushd .
  cd $CoreCLRTestAssets\coreclr\binaries\tests\$Arch\$Build\JIT\CodeGenBringUpTests
  del DblRem*
  del FpRem*
  del div2*
  del localloc*
  popd
}

# -------------------------------------------------------------------------
#
# Build CoreCLR regression tests
#
# -------------------------------------------------------------------------

function Global:BuildTest([string]$Arch="x64", [string]$Build="Release")
{
  $CoreCLRTestAssets = CoreCLRTestAssets

  pushd .
  cd $CoreCLRTestAssets\coreclr\tests
  .\buildtest $Arch $Build clean
  ExcludeTest
  popd
}

# -------------------------------------------------------------------------
#
# Return the number of failures of RunTest
# Return -1 if the log file does not exist
#
# -------------------------------------------------------------------------

function Global:CheckFailure([string]$Arch="x64", [string]$Build="Release")
{
  $CoreCLRTestAssets = CoreCLRTestAssets
  $RunResult = "$CoreCLRTestAssets\coreclr\binaries\Logs\TestRunResults_"
  $RunResult  = $RunResult + "$Arch"
  $RunResult  = $RunResult + "__$Build.log"
  $RunResultsExists = Test-Path $RunResult
  if (!$RunResultsExists) {
    return -1;
  }
  else {
    Get-Content $RunResult | Where-Object { $_.Contains("Failed: ") } | ForEach-Object { $_ -match "Failed: (\d+)," } | Out-Null
    return $matches[1]
  }
}

# -------------------------------------------------------------------------
#
# Run LLILC enabled CoreCLR regression tests
#
# -------------------------------------------------------------------------

function Global:RunTest([string]$Arch="x64", [string]$Build="Release")
{
  $CoreCLRTestAssets = CoreCLRTestAssets
  $CoreCLRRuntime = CoreCLRRuntime
  $CoreCLRVersion = CoreCLRVersion
  
  # Workaround exception handling issue
  chcp 65001 | Out-Null

  $Env:SkipTestAssemblies = "Common;Exceptions;GC;Loader;managed;packages;Regressions;runtime;Tests;TestWrappers_x64_release;Threading" 
  pushd .
  cd $CoreCLRTestAssets\coreclr\tests

  .\runtest $Arch $Build EnableAltJit LLILCJit $CoreCLRRuntime\$CoreCLRVersion\bin | Write-Host
  CheckDiff -Create $True -UseDiffTool $False -Arch $Arch -Build $Build
  $NumFailures = CheckFailure -Arch $Arch -Build $Build
  popd
  return $NumFailures 
}

# -------------------------------------------------------------------------
#
# Re-create the base line for all LLILC enabled regression test cases.
#
# -------------------------------------------------------------------------

function Global:ReBaseAll([string]$Arch="x64", [string]$Build="Release")
{
  $LLILCTest = LLILCTest
  $CoreCLRTestAssets = CoreCLRTestAssets

  $BaseLineExists = Test-Path $LLILCTest\BaseLine
  if ($BaseLineExists) {
    Remove-Item -recurse -force $LLILCTest\BaseLine | Out-Null
  }
  New-Item -itemtype directory $LLILCTest\BaseLine | Out-Null

  Copy-Item -recurse "$CoreCLRTestAssets\coreclr\binaries\tests\$Arch\$Build\Reports\*" -Destination $LLILCTest\BaseLine
  Get-ChildItem -recurse -path $LLILCTest\BaseLine | Where {$_.FullName -match "output.txt"} | Remove-Item -force
  Get-ChildItem -recurse -path $LLILCTest\BaseLine | Where {$_.FullName -match "error.txt"} | ApplyFilterAll
}

# -------------------------------------------------------------------------
#
# Check the LLVM IR dump difference against baseline.
#
# -------------------------------------------------------------------------

function Global:CheckDiff([bool]$Create = $false, [bool]$UseDiffTool = $True, [string]$Arch="x64", [string]$Build="Release")
{
  $LLILCTest = LLILCTest
  $LLILCTestResult = LLILCTestResult
  $CoreCLRTestAssets = CoreCLRTestAssets

  Write-Output ("Checking diff...")
  $DiffExists = Test-Path $LLILCTestResult\Diff
  if ($Create) {
    if ($DiffExists) {
      Remove-Item -recurse -force $LLILCTestResult\Diff | Out-Null
    }

    New-Item -itemtype directory $LLILCTestResult\Diff | Out-Null
    New-Item -itemtype directory $LLILCTestResult\Diff\Base | Out-Null
    New-Item -itemtype directory $LLILCTestResult\Diff\Run | Out-Null

    $TotalCount = 0;
    $DiffCount = 0;
    Get-ChildItem -recurse -path $CoreCLRTestAssets\coreclr\binaries\tests\$Arch\$Build\Reports | Where {$_.FullName -match "error.txt"} | `
    Foreach-Object {
      $TotalCount = $TotalCount + 1
      $RunFile = $_.FullName
      $PartialPathMatch = $_.FullName -match "Reports\\(.*)"
      $PartialPath = $matches[1]
      $BaseFile = "$LLILCTest\BaseLine\$PartialPath"
      copy $RunFile $LLILCTestResult\Diff\Run
      ApplyFilter("$LLILCTestResult\Diff\Run\$_")
      $DiffResult = Compare-Object -Ref (Get-Content $BaseFile) -Diff (Get-Content $LLILCTestResult\Diff\Run\$_)
      if ($DiffResult.Count -ne 0) {
        copy $BaseFile $LLILCTestResult\Diff\Base
        $DiffCount = $DiffCount + 1
      }
      else {
        Remove-Item -force $LLILCTestResult\Diff\Run\$_ | Out-Null
      }
    }

    if ($DiffCount -eq 0) {
      Write-Output ("There is no diff.")
      Remove-Item -recurse -force $LLILCTestResult\Diff | Out-Null
    }
    else {
      Write-Output ("$DiffCount out of $TotalCount have diff.")
      if ($UseDiffTool) {
        & sgdm -t1=Base -t2=Run $LLILCTestResult\Diff\Base $LLILCTestResult\Diff\Run
      }
    }
  }
  else {
    if (!$DiffExists) {
      Write-Output ("There is no diff.")
    }
    else {
      if ($UseDiffTool) {
        & sgdm -t1=Base -t2=Run $LLILCTestResult\Diff\Base $LLILCTestResult\Diff\Run
      }
      else {
        $TotalCount = 0;
        $DiffCount = 0;
        Get-ChildItem -recurse -path $CoreCLRTestAssets\coreclr\binaries\tests\$Arch\$Build\Reports | Where {$_.FullName -match "error.txt"} | `
        Foreach-Object {
          $TotalCount = $TotalCount + 1;
        }

        Get-ChildItem -recurse -path $LLILCTestResult\Diff\Run | Where {$_.FullName -match "error.txt"} | `
        Foreach-Object {
          $DiffCount = $DiffCount + 1;
        }
        Write-Output ("$DiffCount out of $TotalCount have diff.")
      }
    }
  }
}

# -------------------------------------------------------------------------
#
# List and explain available commands
#
# -------------------------------------------------------------------------

function Global:LLILCHelp
{
  Write-Output("ApplyFilter       - Filter to suppress allowable LLVM IR difference. Example: AppyFilter -File FileName")
  Write-Output("Build             - Build LLILC JIT. Example: Build -Build Debug")
  Write-Output("BuildAll          - Configure and Build LLVM including LLILC JIT. Example: BuildLLVM -Arch x64 -Build Debug -Parallel `$False")
  Write-Output("BuildTest         - Build CoreCLR regression tests. Example: BuildTest -Arch x64 -Build Release")
  Write-Output("CheckDiff         - Check the LLVM IR dump diff between run and baseline. Example: CheckDiff -Create `$False -UseDiffTool `$True -Arch x64 -Build Release")
  Write-Output("CheckEnv          - List the LLILC work environment. Example: CheckEnv")
  Write-Output("CopyJIT           - Copy LLILC JIT dll into CoreCLR Runtime. Example: CopyJIT -Build Debug")
  Write-Output("LLILCHelp         - List and explain available commands. Example: LLILCHelp")
  Write-Output("ReBaseAll         - Re-create the base line for all regression test cases. Example: -Arch x64 -Build Release")
  Write-Output("RunTest           - Run LLILC enabled CoreCLR regression tests. Example: RunTest -Arch x64 -Build Release")
}

# -------------------------------------------------------------------------
#
# The Script
#
# -------------------------------------------------------------------------

LLILCEnvInit
