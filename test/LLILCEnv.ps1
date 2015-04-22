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

    2. LLVM and LLILC local repositories are created. The location of
    LLVM source has to be specified with envrironment variable LLVMSOURCE.
    LLILC repo can be placed in the default location under LLVMSOURCE\tools,
    it will be automatically picked up. The LLILC repo can be placed out of
    the LLVM source tree also. In this case, environment variable LLILCSOURCE
    has to be used to specify that location.

    A default location will be used for LLVM build directory. You can override
    the location by specifying environmental variable LLVMBUILD. If LLILC
    source is at default location in tree, LLILC will be built as part of LLVM
    solution automatically. If LLILC is placed out of tree, a default LLILC
    build directory will be used. You can override the location by specifying
    environmental variable LLILCBUILD.
    
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

    llilc, CopyJIT, ReBaseAll, ApplyFilter, CheckEnv 
    
    Under the hood, LLILC uses CoreCLR's test assets and a CoreCLR
    Runtime paired with LLILC JIT for testing. The two parts will
    be downloaded and put in a default location. You can override
    the location by specifying environment variable LLILCTESTRESULT.

    As of now, only release version of CoreCLR Runtime is available.
    Debug version of CoreCLR Runtime support will be added when it
    is available.

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
   [string]$Build="Debug",
   [switch]$NoTestUpdate
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

  # IsOnPath -executable "grep.exe" -software "GnuWin32"

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
  $LLILCSourceExists = Test-Path Env:\LLILCSOURCE
  if (!$LLILCSourceExists) {
    Write-Warning "LLILC source directory is not specified."
    $DefaultLLILCSource = DefaultLLILCSource
    $LLILCSourceExists = Test-Path $DefaultLLILCSource
    if (!$LLILCSourceExists) {
      throw "!!! Default LLILC source not available."
    }
    else {
      Write-Warning "Default LLILC source directory: $DefaultLLILCSource"
    }
  }
  else {
    $LLILCSourceExists = Test-Path $Env:LLILCSOURCE
    if (!$LLILCSourceExists) {
      throw "!!! LLILC source not available in specified location." 
    }
  }

  # Validate LLVMBuild

  $LLVMBuildExists = Test-Path Env:\LLVMBUILD
  if (!$LLVMBuildExists) {
    Write-Warning "LLVM build directory is not specified."
    $DefaultLLVMBuild = DefaultLLVMBuild
    Write-Warning "Default LLVM build directory: $DefaultLLVMBuild"
  }

  # Validate LLILCBuild

  $OutOfTree = Test-Path Env:\LLILCSOURCE
  if ($OutOfTree) {
    $LLILCBuildExists = Test-Path Env:\LLILCBUILD
    if (!$LLILCBuildExists) {
      Write-Warning "LLILC build directory is not specified."
      $DefaultLLILCBuild = DefaultLLILCBuild
      Write-Warning "Default LLILC build directory: $DefaultLLILCBuild"
    }
  }

  # Validate CoreCLR

  $CORECLRSOURCEExists = Test-Path Env:\CORECLRSOURCE
  if (!$CORECLRSOURCEExists) {
    Write-Warning "CORECLRSOURCE not specificed.  Specify CORECLRSOURCE in the environment."
    throw "CORECLRSOURCE not specified"
  }
}

# -------------------------------------------------------------------------
#
# A list of resource location functions
#
# -------------------------------------------------------------------------

# set CoreCLRSource variable from the environment.
$Global:CoreCLRSource = $ENV:CORECLRSOURCE
$Global:CoreCLRRuntime = "$CoreCLRSource\bin\Product\Windows_NT.$Arch.$Build"
$Global:CoreCLRTest = "$CoreCLRSource\bin\tests\Windows_NT.$Arch.$Build"

function Global:DefaultLLILCSource
{
  return "$Env:LLVMSOURCE\tools\llilc"
}

function Global:LLILCSource
{
  $LLILCSourceExists = Test-Path Env:\LLILCSOURCE
  if (!$LLILCSourceExists) {
    $DefaultLLILCSource = DefaultLLILCSource
    return "$DefaultLLILCSource"
  }
  else {
    return "$Env:LLILCSOURCE"
  }
}

function Global:LLILCTest
{
  $LLILCSource = LLILCSource
  return "$LLILCSource\test"
}

function Global:LLILCJit([string]$Build="Debug")
{
  $OutOfTree = Test-Path Env:\LLILCSOURCE

  if ($OutOfTree) {
    $LLILCBuild = LLILCBuild
    $LLILCJit = "$LLILCBuild\bin\$Build\LLILCJit.dll"
  }
  else {
   $LLVMBuild = LLVMBuild
   $LLILCJit = "$LLVMBUILD\bin\$Build\LLILCJit.dll"
  }
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

function Global:DefaultLLILCBuild
{
  return "$Env:TEMP\LLILCBuild"
}

function Global:LLILCBuild
{
  $LLILCBuildExists = Test-Path Env:\LLILCBUILD
  if (!$LLILCBuildExists) {
    $DefaultLLILCBuild = DefaultLLILCBuild
    return "$DefaultLLILCBuild"
  }
  else {
    return $Env:LLILCBUILD
  }
}

function Global:CoreCLRRuntime
{
  return "$CoreCLRSource"
}

function Global:CoreCLRTestAssets
{
  return "$CoreCLRSource"
}

function Global:CoreCLRTestTargetBinaries([string]$Arch="x64", [string]$Build="Debug")
{
  return "$CoreCLRSource\bin\tests\Windows_NT.$Arch.$Build"
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


#function Global:CoreCLRBuild
#{
#  $CoreCLRVersion = CoreCLRVersion

#  if ($CoreCLRVersion -match "Debug") {
#    $CoreCLRBuild = "Debug"
#  }
#  else {
#    $CoreCLRBuild = "Release"
#  }
#  return $CoreCLRBuild
#}

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
        $host.ui.rawui.windowtitle = $matches[2]
        Set-Item -Path "env:$N" -Value $matches[2]
      } else {
        Set-Item -Path "env:$N" -Value $matches[2]
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

function Global:IsOnPath([string]$executable, [string]$software)
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

  CreateLLILCBuildDirectory
  
  #CreateLLILCTestResultDirectory

  CreateLLVMBuildDirectory
    
  #NuGetCLR
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
    Invoke-WebRequest http://nuget.org/NuGet.exe -OutFile NuGet.exe
    $NuGetExists = Test-Path $CoreCLRRuntime\Nuget.exe

    if (!$NuGetExists) {
      throw "!!! NuGet failed to successfully download."
    }

    popd
  }
}

# -------------------------------------------------------------------------
#
# Clear NuGet Cache
# 
# -------------------------------------------------------------------------

function Global:ClearNuGetCache
{
  $NuGetCacheExists = Test-Path $Env:LOCALAPPDATA\NuGet\Cache
  if ($NuGetCacheExists) {
    Remove-Item $Env:LOCALAPPDATA\NuGet\Cache\*
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
  Write-OutPut("Clear NuGet Cache...")
  ClearNuGetCache
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
  throw "Use pre enlisted CoreCLR Drop"
}

# -------------------------------------------------------------------------
#
# Check the status of development environment.
# 
# -------------------------------------------------------------------------

function Global:CheckEnv
{
  $OutOfTree = Test-Path Env:\LLILCSOURCE

  $LLVMBuild = LLVMBuild
  if ($OutofTree) {
    $LLILCBuild = LLILCBuild
  }
  $LLILCSource = LLILCSource
  $LLILCTest = LLILCTest
  $LLILCTestResult = LLILCTestResult
  $CoreCLRTestAssets =  CoreCLRTestAssets
  $CoreCLRRuntime = CoreCLRRuntime
  #$CoreCLRVersion = CoreCLRVersion

  Write-Output("************************************ LLILC Work Environment **************************************")
  Write-Output("LLVM Source         : $Env:LLVMSOURCE")
  Write-Output("LLVM Build          : $LLVMBuild")
  Write-Output("LLILC Source        : $LLILCSource")
  if ($OutOfTree) {
    Write-Output("LLILC Build         : $LLILCBuild")
  }
  Write-Output("LLILC Test          : $LLILCTest")
  Write-Output("LLILC Test Result   : $LLILCTestResult")
  Write-Output("CoreCLR Test Assets : $CoreCLRTestAssets")
  Write-Output("CoreCLR Runtime     : $CoreCLRRuntime")
  #Write-Output("CoreCLR Version     : $CoreCLRVersion")
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
# Create LLILC build directory
# 
# -------------------------------------------------------------------------

function CreateLLILCBuildDirectory
{
  $OutOfTree = Test-Path Env:\LLILCSOURCE
  if ($OutOfTree) {
    $LLILCBuild = LLILCBuild
    $LLILCBuildExists = Test-Path $LLILCBuild
    if (!$LLILCBuildExists) {
      New-Item $LLILCBuild -itemtype Directory  | Out-Null
    }
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

  Write-Output("Use llilc for a list of commands. Use CheckEnv for a list of work environment.")

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
  $CoreCLRBinaries = $CoreCLRRuntime
  #$CoreCLRVersion = CoreCLRVersion
  $LLILCJit = LLILCJit($Build)
  $JitName = "LLILCJit.dll"

  $WorkLLILCJitExists = Test-Path $CoreCLRBinaries\$JitName
  if ($WorkLLILCJitExists) {
    Remove-Item $CoreCLRBinaries\$JitName | Out-Null
  }

  pushd $CoreCLRBinaries

  Write-Output ("Copying LLILC JIT $LLILCJit $JitName")
  copy $LLILCJit $JitName
  Write-Output ("LLILC JIT Copied")

  popd
}

# -------------------------------------------------------------------------
#
# CMake Configuration based on architecture
#
# -------------------------------------------------------------------------

function Global:CMakeConfig([string]$Arch="x64")
{
  if ($Arch -eq "x64") {
    $CMakeConfig = "Visual Studio 12 2013 Win64"
  }
  else {
    $CMakeConfig = "Visual Studio 12"
  }
  return $CMakeConfig
}

# -------------------------------------------------------------------------
#
# Configure LLVM Solution
#
# -------------------------------------------------------------------------

function Global:ConfigureLLVM([string]$Arch="x64", [string]$Build="Debug")
{
  $LLVMBuild = LLVMBuild

  pushd .
  cd $LLVMBuild
  $CMakeConfig = CMakeConfig -Arch $Arch
  cmake -G $CMakeConfig $Env:LLVMSOURCE -DLLVM_TARGETS_TO_BUILD:STRING=X86 -DCMAKE_BUILD_TYPE:STRING=$Build
  popd
}

# -------------------------------------------------------------------------
#
# Configure LLILC solution if LLILC is out of LLVM source tree
#
# -------------------------------------------------------------------------

function Global:ConfigureLLILC([string]$Arch="x64", [string]$Build="Debug")
{
  $OutOfTree = Test-Path Env:\LLILCSOURCE
  if (!$OutOfTree) {
    return
  }

  $LLVMBuild = LLVMBuild  
  $LLILCBuild = LLILCBuild

  pushd .
  cd $LLILCBuild
  $CMakeConfig = CMakeConfig -Arch $Arch
  cmake -G $CMakeConfig $Env:LLILCSOURCE -DWITH_LLVM:STRING=$LLVMBuild -DCMAKE_BUILD_TYPE:STRING=$Build
  popd
}

# -------------------------------------------------------------------------
#
# Build LLVM, including LLILC if it is in default location in LLVM source.
#
# -------------------------------------------------------------------------

function Global:BuildLLVM([string]$Arch="x64", [string]$Build="Debug", [bool]$Parallel=$True)
{
  $LLVMBuild = LLVMBuild
  $TempBat = Join-Path $Env:TEMP "buildllvm.bat"
  $File = "$Env:VS120COMNTOOLS\..\..\VC\vcvarsall.bat"

  if ($Parallel) {
    $MSwitch = " /m "
  }
  else {
    $MSwitch = ""
  }

  ("call ""$File"" x86", "msbuild $LLVMBuild\LLVM.sln /p:Configuration=$Build /p:Platfrom=$Arch /t:ALL_BUILD $MSwitch") | Out-File -Encoding ascii $TempBat
  
  Write-Output ("Building LLVM...")
  cmd /c $TempBat
  Remove-Item -force $TempBat | Out-Null

  $OutOfTree = Test-Path Env:\LLILCSOURCE
  if (!$OutOfTree) {
    CopyJIT -Build $Build
  }
}

# -------------------------------------------------------------------------
#
# Build LLILC JIT
#
# -------------------------------------------------------------------------

function Global:Build([string]$Build="Debug")
{
  $OutOfTree = Test-Path Env:\LLILCSOURCE

  $TempBat = Join-Path $Env:TEMP "buildllilc.bat"
  $File = "$Env:VS120COMNTOOLS\..\..\VC\vcvarsall.bat"

  if ($OutOfTree) {
    $LLILCBuild = LLILCBuild
    $WhatToBuild = "$LLILCBuild\LLILC.sln"
  }
  else {
    $LLVMBuild = LLVMBuild
    $WhatToBuild = "/Project llilcjit $LLVMBuild\LLVM.sln"
  }

  ("call ""$File"" x86", "devenv /Build $Build $WhatToBuild") | Out-File -Encoding ascii $TempBat

  cmd /c $TempBat
  Remove-Item -force $TempBat | Out-Null
  CopyJIT -Build $Build
}

# -------------------------------------------------------------------------
#
# Configure and Build LLVM including LLILC JIT
#
# -------------------------------------------------------------------------

function Global:BuildAll([string]$Arch="x64", [string]$Build="Debug", [bool]$Parallel=$True)
{
  ConfigureLLVM -Arch $Arch -Build $Build
  BuildLLVM -Arch $Arch -Build $Build -Parallel $Parallel

  $OutOfTree = Test-Path Env:\LLILCSOURCE
  if ($OutOfTree) {
    ConfigureLLILC -Arch $Arch -Build $Build
    Build -Build $Build
  }
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
# Using python script applyfilter.py for filtering
#
# -------------------------------------------------------------------------

function Global:ApplyFilter([string]$File)
{ 
  $LLILCTest = LLILCTest
  & $LLILCTest\applyfilter.py $File "$File.tmp"
  Remove-Item -force $File | Out-Null
  Rename-Item "$File.tmp" $File | Out-Null   
}

# -------------------------------------------------------------------------
#
# Exclude test cases from running
#
# -------------------------------------------------------------------------

function Global:ExcludeTest([string]$Arch="x64", [string]$Build="Release")
{
  # Excluding Interop\ICastable\Castable*
  # remove ICastable until it can be debugged
  pushd $CoreCLRTest\Interop\ICastable
  del Castable*
  popd

  # Excluding JIT\CodeGenBringUpTests\div2*,localloc*
  pushd $CoreCLRTest\JIT\CodeGenBringUpTests
  del div2*
  del localloc*
  popd

  # Excluding JIT\jit64\gc\misc\eh1*,funclet*,fgtest1*,
  # struct6_5*,struct7_1*,structfpseh5_1*,structfpseh6_1*,
  # structret6_1*,structret6_2*,structret6_3*
  pushd $CoreCLRTest\JIT\jit64\gc\misc
  del eh1*
  del funclet*
  del fgtest1*
  del struct6_5*
  del struct7_1*
  del structfpseh5_1*
  del structfpseh6_1*
  del structret6_1*
  del structret6_2*
  del structret6_3*
  popd

  # Excluding JIT\jit64\gc\regress\vswhidbey\339415*,143837*
  pushd $CoreCLRTest\JIT\jit64\gc\regress\vswhidbey
  del 339415*
  del 143837*
  popd

  # Excluding JIT\jit64\opt\cse\arrayexpr2*, fieldexpr2*,
  # fieldExprUnchecked1*, HugeArray*, HugeArray1*
  # hugeexpr1*, HugeField1*, HugeField2*, hugeSimpleExpr1*
  # mixedexpr1*, simpleexpr4*, staticFieldExprUnchecked1*
  pushd $CoreCLRTest\JIT\jit64\opt\cse
  del arrayexpr2*
  del fieldexpr2*
  del fieldExprUnchecked1*
  del HugeArray*
  del HugeArray1*
  del hugeexpr1*
  del HugeField1*
  del HugeField2*
  del hugeSimpleExpr1*
  del mixedexpr1*
  del simpleexpr4*
  del staticFieldExprUnchecked1*
  popd 

  # Excluding JIT\Directed\cmov\Bool_Or_Op*, Double_Or_Op*
  # Bool_And_Op*, Bool_No_Op*, Int_Or_Op*, Float_Xor_Op*,
  # Int_And_Op*, Float_And_Op*, Bool_Xor_Op*, Double_And_Op*,
  # Float_Or_Op*, Int_Xor_Op*, Double_Xor_Op*
  pushd $CoreCLRTest\JIT\Directed\cmov
  del Bool_Or_Op*
  del Double_Or_Op*
  del Bool_And_Op*
  del Bool_No_Op*
  del Int_Or_Op*
  del Float_Xor_Op*
  del Int_And_Op*
  del Float_And_Op*
  del Bool_Xor_Op*
  del Double_And_Op*
  del Float_Or_Op*
  del Int_Xor_Op*
  del Double_Xor_Op*
  popd

  # Excluding JIT\opt\Inline\Inline_Handler*, Inline_Vars*, InlineThrow*
  pushd $CoreCLRTest\JIT\opt\Inline
  del Inline_Handler*
  del Inline_Vars*
  del InlineThrow*
  popd

}

# -------------------------------------------------------------------------
#
# Build CoreCLR regression tests
#
# -------------------------------------------------------------------------

function Global:BuildTest([string]$Arch="x64", [string]$Build="Debug")
{
  pushd $CoreCLRSource\tests
  .\buildtest $Arch $Build clean
  ExcludeTest $Arch $Build
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
  $RunResult = "$CoreCLRTestAssets\bin\Logs\TestRunResults_Windows_NT__"
  $RunResult  = $RunResult + "$Arch"
  $RunResult  = $RunResult + "__$Build.log"
  $RunResultsExists = Test-Path $RunResult
  if (!$RunResultsExists) {
    return -1
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

function Global:RunTest
{
  Param(
  [string]$Arch="x64", 
  [string]$Build="Debug",
  [string]$Jit="",
  [string]$Result="",
  [string]$Dump="NoDump"
  )

  #$CoreCLRTestAssets = CoreCLRTestAssets
  #$CoreCLRRuntime = CoreCLRRuntime
  #$CoreCLRVersion = CoreCLRVersion
  $LLILCTest = LLILCTest
  $LLILCTestResult = LLILCTestResult
  $CoreCLRTestTargetBinaries = CoreCLRTestTargetBinaries -Arch $Arch -Build $Build
  
  # Workaround exception handling issue
  chcp 65001 | Out-Null

  # Reserve the old jit and copy in the specified jit.
  if ($Jit -ne "") {
    #$CoreCLRRuntime = CoreCLRRuntime
    #$CoreCLRVersion = CoreCLRVersion
    Rename-Item $CoreCLRRuntime\LLILCJit.dll $CoreCLRRuntime\\LLILCJit.dll.backupcopy
    Copy-Item $Jit $CoreCLRRuntime\LLILCJit.dll
  }

  # Protect old value and set the new value for DUMPLLVMIR
  $DumpExists = Test-Path Env:\COMPLus_DUMPLLVMIR
  if ($DumpExists) {
    $OldDump = $Env:COMPlus_DUMPLLVMIR
  } 
  $Env:COMPlus_DUMPLLVMIR = $Dump

  # The set of test cases that are currently ignored
  $Env:SkipTestAssemblies = "Common;Exceptions;GC;Loader;managed;packages;Regressions;runtime;Tests;TestWrappers_x64_release;Threading" 

  # Run the test
  pushd .
  cd $CoreCLRSource\tests
  .\runtest $Arch $Build TestEnv $LLILCTest\LLILCTestEnv.cmd $CoreCLRRuntime | Write-Host
  popd

  # Restore old value for DUMPLLVMIR
  if ($DumpExists) {
    $Env:COMPlus_DUMPLLVMIR = $OldDump;
  } 

  # Restore the old jit
  if ($Jit -ne "") {
    Remove-Item $CoreCLRRuntime\LLILCJit.dll | Out-Null
    Rename-Item $CoreCLRRuntime\LLILCJit.dll.backupcopy $CoreCLRRuntime\LLILCJit.dll | Out-Null
  }

  # Copy out result, normalize it in case of verbose dump.
  if ($Result -ne "") {
    $ResultExists = Test-Path $LLILCTestResult\$Result
    if ($ResuLtExists) {
      Remove-Item -recurse -force $LLILCTestResult\$Result | Out-Null
    }
    New-Item -itemtype directory $LLILCTestResult\$Result | Out-Null
    Copy-Item -recurse "$CoreCLRTestTargetBinaries\Reports\*" -Destination $LLILCTestResult\$Result
    Get-ChildItem -recurse -path $LLILCTestResult\$Result | Where {$_.FullName -match "output.txt"} | Remove-Item -force
    if ($Dump -eq "Verbose") {
      Write-Host ("Applying filter on verbose LLVM IR dump...")
      Get-ChildItem -recurse -path $LLILCTestResult\$Result | Where {$_.FullName -match "error.txt"} | ApplyFilterAll
    }
  }

  $NumFailures = CheckFailure -Arch $Arch -Build $Build

  # If there aren't any failures, return $True to say we passed
  # Otherwise return false
  if ($NumFailures -eq 0) {
    return $True
  }
  else {
    return $False
  }
}

# -------------------------------------------------------------------------
#
# Check the LLVM IR dump difference against baseline.
#
# -------------------------------------------------------------------------

function Global:CheckDiff
{
  Param(
  [Parameter(Mandatory=$True)][string]$Base,
  [Parameter(Mandatory=$True)][string]$Diff,
  [Parameter(Mandatory=$False)][switch]$Summary,
  [Parameter(Mandatory=$False)][switch]$UseDiffTool
  )

  $LLILCTestResult = LLILCTestResult
  
  Write-Host ("Checking diff...")

  if ($UseDiffTool) {
    # Validate Diff Tool  
    IsOnPath -executable "sgdm.exe" -software "DiffMerge"
  }

  $CompareExists = Test-Path $LLILCTestResult\Compare

  # Refresh a new Compare subdirectory
  if ($CompareExists) {
    Remove-Item -recurse -force $LLILCTestResult\Compare | Out-Null
  }

  New-Item -itemtype directory $LLILCTestResult\Compare | Out-Null
  New-Item -itemtype directory $LLILCTestResult\Compare\$Base | Out-Null
  New-Item -itemtype directory $LLILCTestResult\Compare\$Diff | Out-Null

  if (!$Summary) {
    # Deal with verbose LLVM IR comparison
    $TotalCount = 0
    $DiffCount = 0
    Get-ChildItem -recurse -path $LLILCTestResult\$Diff | Where {$_.FullName -match "error.txt"} | `
    Foreach-Object {
      $TotalCount = $TotalCount + 1
      $DiffFile = $_.FullName
      $PartialPathMatch = $_.FullName -match "$Diff\\(.*)"
      $PartialPath = $matches[1]
      $BaseFile = "$LLILCTestResult\$Base\$PartialPath"
      $DiffResult = Compare-Object -Ref (Get-Content $BaseFile) -Diff (Get-Content $DiffFile)
      if ($DiffResult.Count -ne 0) {
        Copy-Item $BaseFile $LLILCTestResult\Compare\$Base
        Copy-Item $DiffFile $LLILCTestResult\Compare\$Diff
        $DiffCount = $DiffCount + 1
      }
    }
    if ($DiffCount -eq 0) {
      Write-Host ("There is no diff.")
      Remove-Item -recurse -force $LLILCTestResult\Compare | Out-Null
    }
    else {
      Write-Host ("$DiffCount out of $TotalCount have diff.")
      if ($UseDiffTool) {
        & sgdm -t1=Base -t2=Diff $LLILCTestResult\Compare\$Base $LLILCTestResult\Compare\$Diff
      }
    }
    return $DiffCount
  }
  else {
    # Deal with summary comparison
    $TotalCount = 0
    $DiffCount = 0
    $NewlyFailedMethods = 0
    $NewlyPassedMethods = 0
    Get-ChildItem -recurse -path $LLILCTestResult\$Diff | Where {$_.FullName -match "error.txt"} | `
    Foreach-Object {
      $TotalCount = $TotalCount + 1
      $DiffFile = $_.FullName
      $PartialPathMatch = $_.FullName -match "$Diff\\(.*)"
      $PartialPath = $matches[1]
      $BaseFile = "$LLILCTestResult\$Base\$PartialPath"
      $DiffResult = Compare-Object -Ref (Get-Content $BaseFile) -Diff (Get-Content $DiffFile)
      if ($DiffResult.Count -ne 0) {
        Copy-Item $BaseFile $LLILCTestResult\Compare\$Base
        Copy-Item $DiffFile $LLILCTestResult\Compare\$Diff
        $DiffCount = $DiffCount + 1 
      }
      Foreach ($SummaryLine in $DiffResult) {
        if ($SummaryLine -match 'Successfully(.*)(<=)') {
          $NewlyFailedMethods++
        }
        if ($SummaryLine -match 'Successfully(.*)(=>)') {
          $NewlyPassedMethods++
        }
      }
    }

    if ($DiffCount -eq 0) {
      Write-Host ("There is no diff.")
      Remove-Item -recurse -force $LLILCTestResult\Compare | Out-Null
    }
    else {
      Write-Host ("$DiffCount out of $TotalCount have diff.")
      if ($NewlyFailedMethods -eq 0) {
        Write-Host ("All previously successfully jitted methods passed jitting with diff jit.")
      }
      else {
        Write-Host ("$NewlyFailedMethods methods successfully jitted by base jit now FAILED in diff jit.")
      }
      
      Write-Host ("$NewlyPassedMethods methods now successfully jitted in diff jit.")
      
      if ($UseDiffTool) {
        & sgdm -t1=Base -t2=Diff $LLILCTestResult\Compare\$Base $LLILCTestResult\Compare\$Diff
      }
    }
    return $NewlyFailedMethods
  }
}

# -------------------------------------------------------------------------
#
# List and explain available commands
#
# -------------------------------------------------------------------------

function Global:llilc([string]$Command="")
{
  $ListAll = $False
  if ($Command -eq "") {
    $ListAll = $True
  }

  if ($ListAll -Or ($Command -eq "ApplyFilter")) {
    Write-Output("ApplyFilter       - Filter to suppress allowable LLVM IR difference.")
    Write-Output("                    Example: AppyFilter -File FileName")
  }

  if ($ListAll -Or ($Command -eq "Build")) {
    Write-Output("Build             - Build LLILC JIT.")
    Write-Output("                    Example: Build -Build Debug")
  }

  if ($ListAll -Or ($Command -eq "BuildAll")) {
    Write-Output("BuildAll          - Configure and Build LLVM including LLILC JIT.")
    Write-Output("                    Example: BuildAll -Arch x64 -Build Debug -Parallel `$True")
  }

  if ($ListAll -Or ($Command -eq "BuildTest")) {
    Write-Output("BuildTest         - Build CoreCLR regression tests.")
    Write-Output("                    Example: BuildTest -Arch x64 -Build Release")
  }

  if ($ListAll -Or ($Command -eq "CheckDiff")) {
    Write-Output("CheckDiff         - Check the LLVM IR dump or summary diff between run and baseline.")
    Write-Output("                    Example: CheckDiff -Base BaseResultName -Diff DiffResultName -Summary -UseDiffTool")
  }

  if ($ListAll -Or ($Command -eq "CheckEnv")) {
    Write-Output("CheckEnv          - List the LLILC work environment.")
    Write-Output("                    Example: CheckEnv")
  }

  if ($ListAll -Or ($Command -eq "CopyJIT")) {
    Write-Output("CopyJIT           - Copy LLILC JIT dll into CoreCLR Runtime.")
    Write-Output("                    Example: CopyJIT -Build Debug")
  }

  if ($ListAll) {
    Write-Output("llilc             - List and explain available commands.")
    Write-Output("                    Example: llilc RunTest")
  }

  if ($ListAll -Or ($Command -eq "RunTest")) {
    Write-Output("RunTest           - Run LLILC enabled CoreCLR regression tests.")
    Write-Output("                    Example: RunTest -Arch x64 -Build Release -Jit FullPathToJit -Result ResultName -Dump Summary")
  }
}

# -------------------------------------------------------------------------
#
# The Script
#
# -------------------------------------------------------------------------

LLILCEnvInit
