# -------------------------------------------------------------------------
#
# This script provides MSILC development environment and test harness.
#
# -------------------------------------------------------------------------

<#
.SYNOPSIS
    Setup MSILC development environment and provide test harness.

.DESCRIPTION
    This script set up the MSILC environment with the assumptions below:
    
    1. The following software are installed:
    Visual Studio 12.0, Git, CMake, Python, GnuWin32, and DiffMerge
 
    If the software are installed in default location, or the desired
    executable is already on path, this environment will pick them up 
    automatically. If you install any of them to a different place and
    the executable is not on path, please specify them through the
    following environment variables:

    VS120COMNTOOLS
    GITDIR
    CMAKEDIR
    PYTHONDIR
    GNUWIN32DIR
    DIFFTOOL

    2. CoreCLR, LLVM and MSILC local repositories are created. Note that 
    MSILC should be located at the correct place under tools\MSILC in 
    LLVM repository. MSILC uses CoreCLR's test assets to run regressions.
    Please specify the following environment variable:

    LLVMSOURCE
    LLVMBUILD
    CORECLRREPO

    This script completes the environment setup and reports the status.
    
    This scirpt also provides some daily routines used for daily
    development and test harness. It tries to make common task
    as simple as possible and also provides some routines for
    finer control.
     
    Most commonly used routines are:
    Build, BuildTest, RunTest, CheckDiff

    Routines with finer control are:
    MSILCHelp, NuGetCLR, CoreCLRVersion, CopyJIT, 
    CheckJIT, CheckCLR, CheckStatus, CheckEnv,
    ReBaseAll, ApplyFilter,
    ConfigureLLVM, BuildLLVM, 
    and some quick directory navigations
    
    In first launch of this environment, a CoreCLR package Nuget will be
    performed and it will be placed under MSILCSOURCE\test\TestResult.
    This directory will be ignored by Git as it is specied in .gitignore.

    To build MSILC, just run Build.
    To build CoreCLR regression tests, just run BuildTest.
    To run CoreCLR regression tests, just run RunTest.
    To check the details of the difference of intermediate LLVM IR against
    baseline, just run CheckDiff. It will bring up DiffMerge.

    Under the hood, MSILC uses CoreCLR's test assets, BuildTest will
    build CoreCLR regression tests under CORECLRREPO\binaries\test.
    To run CoreCLR with MSILC, MSILCJit.dll has to be copied to where
    CoreRun.exe resides in CoreCLR package. Build will automatically
    copy it over once it is built. CopyJIT can do so whenever you can.
    As MSILC is still in early developement stage, intermediate LLVM
    IR dump is used to protect the work we have achieved. A baseline
    of such dump is kept in MSILCSOURCE\test\BaseLine. 

.PARAMETER Arch
    Target Architecture

.PARAMETER Build
    Debug or Release Build
    
.EXAMPLE
    MSILCEnv.ps1
    MSILCEnv.ps1 -Arch amd64 -Build debug
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

  # If environment variables are not specified, default values will be picked up
  # if they are valid.
   
  $DefaultVSCOMNTOOLS = "C:\Program Files (x86)\Microsoft Visual Studio 12.0\Common7\Tools\"
  $DefaultGit = "C:\Program Files (x86)\Git\"
  $DefaultCMake = "C:\Program Files (x86)\CMake\"
  $DefaultPython = "C:\Python34\"
  $DefaultGnuWin32 = "C:\GnuWin32\"
  $DefaultDiffTool =  "C:\Program Files\SourceGear\"

  $DefaultLLVMSource = "C:\LLVM\"
  $DefaultLLVMBuild = "C:\LLVMBuild\"
  $DefaultCoreCLRRepo = "C:\coreclr\"
    
  # Validate Visual Studio

  $VSExists = Test-Path Env:\VS120COMNTOOLS
  if (!$VSExists) {
    $VSExists = Test-Path $DefaultCOMNTOOLS;
    if (!$VSExists) {
      throw "!!! Visual Studio 12.0 not specified." 
    }
    else {
      $Env:VS120COMNTOOLS = $DefaultCOMNTOOLS
    }
  }
  else {
    $VSExists = Test-Path $Env:VS120COMNTOOLS
    if (!$VSExists) {
      throw "!!! Visual Studio 12.0 not available in specified location." 
    }
  }

  # Validate Git

  $GitExists = Test-Path Env:\GITDIR
  $GitOnPath = IsOnPath("git.exe")
  if (!$GitOnPath -And !$GitExists) {
    $GitExists = Test-Path $DefaultGit;
    if (!$GitExists) {
      throw "!!! Git not specified." 
    }
    else {
      $Env:GITDIR = $DefaultGit
    }
  }
  elseif (!$GitOnPath) {
    $GitExists = Test-Path $Env:GITDIR
    if (!$GitExists) {
      throw "!!! Git not available in specified location." 
    }
  }

  # Validate CMake

  $CMakeExists = Test-Path Env:\CMAKEDIR
  $CMakeOnPath = IsOnPath("cmake.exe")
  if (!$CMakeOnPath -And !$CMakeExists) {
    $CMakeExists = Test-Path $DefaultCMake;
    if (!$CMakeExists) {
      throw "!!! CMake not specified." 
    }
    else {
      $Env:CMAKEDIR = $DefaultCMake
    }
  }
  elseif (!$CMakeOnPath) {
    $CMakeExists = Test-Path $Env:CMAKEDIR
    if (!$CMakeExists) {
      throw "!!! CMake not available in specified location." 
    }
  }

  # Validate Python

  $PythonExists = Test-Path Env:\PYTHONDIR
  $PythonOnPath = IsOnPath("python.exe")
  if (!$PythonOnPath -And !$PythonExists) {
    $PythonExists = Test-Path $DefaultPython;
    if (!$PythonExists) {
      throw "!!! Python not specified." 
    }
    else {
      $Env:PYTHONDIR = $DefaultPython
    }
  }
  elseif (!$PythonOnPath) {
    $PythonExists = Test-Path $Env:PYTHONDIR
    if (!$PythonExists) {
      throw "!!! Python not available in specified location." 
    }
  }

  # Validate GnuWin32

  $GnuWin32Exists = Test-Path Env:\GNUWIN32DIR
  $GnuWin32OnPath = IsOnPath("grep.exe")
  if (!$GnuWin32OnPath -And !$GnuWin32Exists) {
    $GnuWin32Exists = Test-Path $DefaultGnuWin32;
    if (!$GnuWin32Exists) {
      throw "!!! GnuWin32 not specified." 
    }
    else {
      $Env:GNUWIN32DIR = $DefaultGnuWin32
    }
  }
  elseif (!$GnuWin32OnPath) {
    $GnuWin32Exists = Test-Path $Env:GNUWIN32DIR
    if (!$GnuWin32Exists) {
      throw "!!! GnuWin32 not available in specified location." 
    }
  }

  # Validate Diff Tool

  $DiffToolExists = Test-Path Env:\DIFFTOOL
  $DiffToolOnPath = IsOnPath("sgdm.exe")
  if (!$DiffToolOnPath -And !$DiffToolExists) {
    $DiffToolExists = Test-Path "$DefaultDiffTool"
    if (!$DiffToolExists) {
      throw "!!! Diff Tool not specified." 
    }
    else {
      $Env:DIFFTOOL = "$DefaultDiffTool"
    }
  }
  elseif (!$DiffToolOnPath) {
    $DiffToolExists = Test-Path "$Env:DIFFTOOL"
    if (!$DiffToolExists) {
      throw "!!! Diff Tool not available in specified location." 
    }
  }

  # Validate LLVM

  $LLVMSourceExists = Test-Path Env:\LLVMSOURCE
  if (!$LLVMSourceExists) {
    $LLVMSourceExists = Test-Path $DefaultLLVMSOURCE
    if (!$LLVMSourceExists) {
      throw "!!! LLVM Source not specified."
    }
    else {
      $Env:LLVMSOURCE = $DefaultLLVMSource
    }
  }
  else {
    $LLVMSourceExists = Test-Path $Env:LLVMSOURCE
    if (!$LLVMSourceExists) {
      throw "!!! LLVM Source not available." 
    }
  }

  $LLVMBuildExists = Test-Path Env:\LLVMBUILD
  if (!$LLVMBuildExists) {
    $LLVMBuildExists = Test-Path $DefaultLLVMBuild
    if (!$LLVMBuildExists) {
      throw "!!! LLVM Build Directory not specified."
    }
    else {
      $Env:LLVMBUILD = $DefaultLLVMBuild
    }
  }
  else {
    $LLVMBuildExists = Test-Path $Env:LLVMBUILD
    if (!$VSExists) {
      throw "!!! LLVM Build Directory not available." 
    }
  }

  # Validate MSILC

  if ($LLVMSourceExists) {
    $MSILCSourceExists = Test-Path "$Env:LLVMSOURCE\tools\llvm-msilc"
    if (!$MSILCSourceExists) {
      throw "!!! MSILC Source not available."
    }
    else {
      $Env:MSILCSOURCE = "$Env:LLVMSOURCE\tools\llvm-msilc"
    }
  }

  # Validate CoreCLR Repository

  $CoreCLRRepoExists = Test-Path Env:\CORECLRREPO
  if (!$CoreCLRRepoExists) {
    $CoreCLRRepoExists = Test-Path $DefaultCoreCLRRepo
    if (!$CoreCLRRepoExists) {
      throw "!!! CoreCLR Repository not specified."
    }
    else {
      $Env:CORECLRREPO = $DefaultCoreCLRRepo
    }
  }
  else {
    $CoreCLRRepoExists = Test-Path $Env:CORECLRREPO
    if (!$CoreCLRRepoExists) {
      throw "!!! CoreCLR Repository not available." 
    }
  }
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

function IsOnPath([string]$executable)
{
  if (Get-Command $executable -ErrorAction SilentlyContinue) {
    return $TRUE
  }
  else {
    return $FALSE
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

  # Only add directories to path if the executables are not already on PATH
  if (-Not (IsOnPath("git.exe"))) {
    $Env:PATH = $Env:PATH + ";$Env:GITDIR\cmd"
  }

  if (-Not (IsOnPath("cmake.exe"))) {
    $Env:PATH = $Env:PATH + ";$Env:CMAKEDIR\bin"
  }

  if (-Not (IsOnPath("python.exe"))) {
    $Env:PATH = $Env:PATH + ";$Env:PYTHONDIR"
    $Env:PATH = $Env:PATH + ";$Env:PYTHONDIR\DLLs"
  }
  
  if (-Not (IsOnPath("grep.exe"))) {
    $Env:PATH = $Env:PATH + ";$Env:GNUWIN32DIR\bin"
  }

  if (-Not (IsOnPath("sdgm.exe"))) {
    $Env:PATH = $Env:PATH + ";$Env:DIFFTOOL\Common\DiffMerge"
  }

  $Global:JitName = "MSILCJit"
  $Global:JitArch = $Arch
  $Global:JitBuild = $Build

  $Global:CoreCLRVersion = CoreCLRVersion
  $Global:CoreCLRArch = $Global:JitArch
  if ($Global:CoreCLRVersion -match "Debug") {
    $Global:CoreCLRBuild = "Debug"
  }
  else {
    $Global:CoreCLRBuild = "Release"
  }
  $Env:MSILCJIT = "$Env:LLVMBUILD\bin\$Global:JitBuild\MSILCJit.dll"
  $Env:MSILCTEST = "$Env:MSILCSOURCE\test\"

  $Env:WORKCLRRUNTIME = "$Env:MSILCTEST\TestResult\"
  $Env:WORKCLRCORERUN = "$Env:WORKCLRRUNTIME\$Global:CoreCLRVersion\bin\CoreRun.exe"
  
  NuGetCLR

  $Env:Core_Root = "$Env:WORKCLRRUNTIME\$Global:CoreCLRVersion\bin"
}

# -------------------------------------------------------------------------
#
# Download nuget.exe
# 
# -------------------------------------------------------------------------

function Global:DownloadNuGet
{
  $NuGetExists = Test-Path $Env:WORKCLRRUNTIME\NuGet.exe
  if (!$NuGetExists) {
    pushd .
    cd $Env:WORKCLRRUNTIME
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
  $WorkCLRRuntimeExists = Test-Path $Env:WORKCLRRUNTIME

  if (!$WorkCLRRuntimeExists) {
    New-Item $Env:WORKCLRRUNTIME -ItemType directory | Out-Null
  }
  else {
    Write-OutPut("CoreCLR Runtime already downloaded.")
    return
  }

  Write-OutPut("Downloading NuGet.exe...")
  DownloadNuGet

  if (!$WorkCLRRuntimeExists) {
    copy $Env:MSILCSOURCE\utils\NuGet.config $Env:WORKCLRRUNTIME
    copy $Env:MSILCSOURCE\utils\packages.config $Env:WORKCLRRUNTIME
    pushd .
    cd $Env:WORKCLRRUNTIME
    Write-OutPut("Performing a CoreCLR package NuGet...")    
    & .\NuGet.exe install
    popd
  }
}

# -------------------------------------------------------------------------
#
# Check the status of CoreCLR Runtime
# 
# -------------------------------------------------------------------------

function Global:CheckCLR
{
  $WorkCLRRuntimeExists = Test-Path $Env:WORKCLRRUNTIME\$Global:CoreCLRVersion
  if (!$WorkCLRRuntimeExists) {
    Write-Output("CoreCLR Runtime not ready yet.")
  }
  else {
    $WorkCLRCoreRunExists = Test-Path $Env:WorkCLRCORERUN
    if (!$WorkCLRCoreRunExists) {
      Write-Output("CoreCLR CoreRun.exe not ready yet.")
    }
  }
}

# -------------------------------------------------------------------------
#
# Check the status of MSILC JIT in LLVM build directory and its copy
# in CoreCLR Runtime directory
# 
# -------------------------------------------------------------------------

function Global:CheckJIT
{
  $MSILCJITExists = Test-Path $Env:MSILCJIT
  if (!$MSILCJITExists) {
    Write-Output ("MSILC JIT not built yet.") 
  }

  $WorkMSILCJITExists = Test-Path $Env:WORKCLRRUNTIME\$Global:CoreCLRVersion\bin\MSILCJit.dll
  if (!$WorkMSILCJITExists) {
    Write-Output("MSILC JIT not copied into CoreCLR Runtime yet.") 
  }
}

# -------------------------------------------------------------------------
#
# Check the status of CoreCLR Runtime and MSILC JIT
# 
# -------------------------------------------------------------------------

function global:CheckStatus
{
  CheckCLR
  CheckJIT  
}

# -------------------------------------------------------------------------
#
# Quick Directory Navigations
# 
# -------------------------------------------------------------------------

function Global:cdc
{
  cd $Env:CORECLRREPO
}

function Global:cdm
{
  cd $Env:MSILCSOURCE
}

function Global:cdl
{
  cd $Env:LLVMSOURCE
}

function Global:cdb
{
  cd $Env:LLVMBUILD
}

# -------------------------------------------------------------------------
#
# Check the status of development environment.
# 
# -------------------------------------------------------------------------

function Global:CheckEnv
{
  Write-Output("************************************ MSILC Work Environment **************************************")
  Write-Output("VS120COMNTOOLS     : $Env:VS120COMNTOOLS")
  Write-Output("GITDIR             : $Env:GITDIR")
  Write-Output("CMAKEDIR           : $Env:CMAKEDIR")
  Write-Output("PYTHONDIR          : $Env:PYTHONDIR")
  Write-Output("GNUWIN32DIR        : $Env:GNUWIN32DIR")
  Write-Output("LLVMSOURCE         : $Env:LLVMSOURCE")
  Write-Output("LLVMBUILD          : $Env:LLVMBUILD")
  Write-Output("MSILCSOURCE        : $Env:MSILCSOURCE")
  Write-Output("CORECLRREPO        : $Env:CORECLRREPO")
  Write-Output("MSILCJIT           : $Env:MSILCJIT")
  Write-Output("MSILCTEST          : $Env:MSILCTEST")
  Write-Output("WORKCLRRUNTIME     : $Env:WORKCLRRUNTIME")
  Write-Output("WORKCLRCORERUN     : $Env:WORKCLRCORERUN")
  Write-Output("CoreCLRVersion     : $Global:CoreCLRVersion")
  Write-Output("Core_Root          : $Env:Core_Root")
  Write-Output("**************************************************************************************************")

  CheckStatus
}

# -------------------------------------------------------------------------
#
# Setup MSILC development environment.
# 
# -------------------------------------------------------------------------

function MSILCEnvInit
{
  ValidatePreConditions
  CompleteEnvInit
  CheckEnv
  cdm
}

# -------------------------------------------------------------------------
#
# Copy in MSILC JIT dll
#
# -------------------------------------------------------------------------

function Global:CopyJIT
{
  $WorkMSILCJitExists = Test-Path $Env:WORKCLRRUNTIME\$Global:CoreCLRVersion\bin\MSILCJit.dll
  if ($WorkMSILCJitExists) {
    Remove-Item $Env:WORKCLRRUNTIME\$Global:CoreCLRVersion\bin\MSILCJit.dll | Out-Null
  }

  pushd .
  cd $Env:WORKCLRRUNTIME\$Global:CoreCLRVersion\bin

  Write-Output ("Copying MSILC JIT")
  copy $Env:MSILCJIT .
  Write-Output ("MSILC JIT Copied")

  popd
}

# -------------------------------------------------------------------------
#
# Configure LLVM Solution
#
# -------------------------------------------------------------------------

function Global:ConfigureLLVM
{
  pushd .
  cd $Env:LLVMBUILD
  if ($Global:JitArch -eq "x64") {
    cmake -G "Visual Studio 12 2013 Win64" $Env:LLVMSOURCE
  }
  else {
    cmake -G "Visual Studio 12" $Env:LLVMSOURCE
  }
  popd
}

# -------------------------------------------------------------------------
#
# Build LLVM including MSILC JIT
#
# -------------------------------------------------------------------------

function Global:BuildLLVM
{
  $TempBat = Join-Path $Env:TEMP "buildllvm.bat"
  $File = "$Env:VS120COMNTOOLS\..\..\VC\vcvarsall.bat" 
  ("call ""$File"" x86", "msbuild $Env:LLVMBUILD\LLVM.sln /p:Configuration=$Global:JitBuild /p:Platfrom=$Global:JitArch /t:ALL_BUILD") | Out-File -Encoding ascii $TempBat
  $CmdOut = cmd /c $TempBat
  Remove-Item -force $TempBat | Out-Null
  CopyJIT
}

# -------------------------------------------------------------------------
#
# Build MSILC JIT
#
# -------------------------------------------------------------------------

function Global:Build
{
  $TempBat = Join-Path $Env:TEMP "buildmsilc.bat"
  $File = "$Env:VS120COMNTOOLS\..\..\VC\vcvarsall.bat" 
  ("call ""$File"" x86", "msbuild $Env:LLVMBUILD\LLVM.sln /p:Configuration=$Global:JitBuild /p:Platfrom=$Global:JitArch /t:msilcreader /p:BuildProjectReferences=false", "msbuild $Env:LLVMBUILD\LLVM.sln /p:Configuration=$Global:JitBuild /p:Platfrom=$Global:JitArch /t:msilcjit /p:BuildProjectReferences=false") | Out-File -Encoding ascii $TempBat
  $CmdOut = cmd /c $TempBat
  Remove-Item -force $TempBat | Out-Null
  CopyJIT
}

# -------------------------------------------------------------------------
#
# Get the CoreCLR Version
#
# -------------------------------------------------------------------------

function Global:CoreCLRVersion
{
  (Get-Content $Env:MSILCSOURCE\utils\packages.config) | ForEach-Object { 
    if ($_ -match 'package id="(.*?)" version="(.*?)"') {
      $Result = $matches[1] + "." + $matches[2]
      return $Result
    }
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

function Global:ExcludeTest
{
  pushd .
  cd $Env:CORECLRREPO\binaries\tests\x64\release\JIT\CodeGenBringUpTests
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

function Global:BuildTest
{
  pushd .
  cd $Env:CORECLRREPO\tests
  .\buildtest $Global:CoreCLRArch $Global:CoreCLRBuild clean
  ExcludeTest
  popd
}

# -------------------------------------------------------------------------
#
# Run MSILC enabled CoreCLR regression tests
#
# -------------------------------------------------------------------------

function Global:RunTest
{
  # Workaround exception handling issue
  chcp 65001 | Out-Null

  $Env:SkipTestAssemblies = "Common;Exceptions;GC;Loader;managed;packages;Regressions;runtime;Tests;TestWrappers_x64_release;Threading" 
  pushd .
  cd $Env:CORECLRREPO\tests
  .\runtest $Global:CoreCLRArch $Global:CoreCLRBuild EnableMSILC $Env:Core_Root 
  CheckDiff -Create $True -UseDiffTool $False
  popd  
}

# -------------------------------------------------------------------------
#
# Re-create the base line for all MSILC enabled regression test cases.
#
# -------------------------------------------------------------------------

function Global:ReBaseAll
{
  $BaseLineExists = Test-Path $Env:MSILCTEST\BaseLine
  if ($BaseLineExists) {
    Remove-Item -recurse -force $Env:MSILCTEST\BaseLine | Out-Null
  }
  New-Item -itemtype directory $Env:MSILCTEST\BaseLine | Out-Null

  Copy-Item -recurse "$Env:CORECLRREPO\binaries\tests\$Global:CoreCLRArch\$CoreCLRBuild\Reports\*" -Destination $Env:MSILCTEST\BaseLine
  Get-ChildItem -recurse -path $Env:MSILCTEST\BaseLine | Where {$_.FullName -match "output.txt"} | Remove-Item -force
  Get-ChildItem -recurse -path $Env:MSILCTEST\BaseLine | Where {$_.FullName -match "error.txt"} | ApplyFilterAll
}

# -------------------------------------------------------------------------
#
# Check the LLVM IR dump difference against baseline.
#
# -------------------------------------------------------------------------

function Global:CheckDiff([bool]$Create = $false, [bool]$UseDiffTool = $True)
{
  Write-Output ("Checking diff...")
  $DiffExists = Test-Path $Env:WORKCLRRUNTIME\Diff
  if ($Create) {
    if ($DiffExists) {
      Remove-Item -recurse -force $Env:WORKCLRRUNTIME\Diff | Out-Null
    }

    New-Item -itemtype directory $Env:WORKCLRRUNTIME\Diff | Out-Null
    New-Item -itemtype directory $Env:WORKCLRRUNTIME\Diff\Base | Out-Null
    New-Item -itemtype directory $Env:WORKCLRRUNTIME\Diff\Run | Out-Null

    $TotalCount = 0;
    $DiffCount = 0;
    Get-ChildItem -recurse -path $Env:CORECLRREPO\binaries\tests\$Global:CoreCLRArch\$CoreCLRBuild\Reports | Where {$_.FullName -match "error.txt"} | `
    Foreach-Object {
      $TotalCount = $TotalCount + 1
      $RunFile = $_.FullName
      $PartialPathMatch = $_.FullName -match "Reports\\(.*)"
      $PartialPath = $matches[1]
      $BaseFile = "$Env:MSILCTEST\BaseLine\$PartialPath"
      copy $RunFile $Env:WORKCLRRUNTIME\Diff\Run
      ApplyFilter("$Env:WORKCLRRUNTIME\Diff\Run\$_")
      $DiffResult = Compare-Object -Ref (Get-Content $BaseFile) -Diff (Get-Content $Env:WORKCLRRUNTIME\Diff\Run\$_)
      if ($DiffResult.Count -ne 0) {
        copy $BaseFile $Env:WORKCLRRUNTIME\Diff\Base
        $DiffCount = $DiffCount + 1
      }
      else {
        Remove-Item -force $Env:WORKCLRRUNTIME\Diff\Run\$_ | Out-Null
      }
    }

    if ($DiffCount -eq 0) {
      Write-Output ("There is no diff.")
      Remove-Item -recurse -force $Env:WORKCLRRUNTIME\Diff | Out-Null
    }
    else {
      Write-Output ("$DiffCount out of $TotalCount have diff.")
      if ($UseDiffTool) {
        & sgdm -t1=Base -t2=Run $Env:WORKCLRRUNTIME\Diff\Base $Env:WORKCLRRUNTIME\Diff\Run
      }
    }
  }
  else {
    if (!$DiffExists) {
      Write-Output ("There is no diff.")
    }
    else {
      if ($UseDiffTool) {
        & sgdm -t1=Base -t2=Run $Env:WORKCLRRUNTIME\Diff\Base $Env:WORKCLRRUNTIME\Diff\Run
      }
      else {
        $TotalCount = 0;
        $DiffCount = 0;
        Get-ChildItem -recurse -path $Env:CORECLRREPO\binaries\tests\$Global:CoreCLRArch\$CoreCLRBuild\Reports | Where {$_.FullName -match "error.txt"} | `
        Foreach-Object {
          $TotalCount = $TotalCount + 1;
        }

        Get-ChildItem -recurse -path $Env:WORKCLRRUNTIME\Diff\Run | Where {$_.FullName -match "error.txt"} | `
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

function Global:MSILCHelp
{
  Write-Output("ApplyFilter       - Filter to suppress allowable LLVM IR difference. Example: AppyFilter -File FileName -TmpFile TmpFileName")
  Write-Output("Build             - Build MSILC JIT. Example: Build")
  Write-Output("BuildLLVM         - Build LLVM including MSILC JIT. Example: Build LLVM")
  Write-Output("BuildTest         - Build CoreCLR regression tests. Example: BuildTest")
  Write-Output("CheckCLR          - Check the status of CoreCLR Runtime. Example: CheckCLR")
  Write-Output("CheckDiff         - Check the LLVM IR dump diff between run and baseline. Example: CheckDiff")
  Write-Output("CheckEnv          - Check the status of development environment. Example: CheckEnv")
  Write-Output("CheckJIT          - Check the status of MSILC JIT in LLVM build directory and its copy in CLR directory. Example: CheckJIT")
  Write-Output("CheckStatus       - Check the status of CoreCLR Runtime and MSILC JIT. Example: CheckStatus")
  Write-Output("ConfigureLLVM     - Create LLVM solution file. Example : ConfigureLLVM")
  Write-Output("CopyJIT           - Copy MSILC JIT dll into CoreCLR Runtime. Example: CopyJIT")
  Write-Output("CoreCLRVersion    - Get CoreCLR package version")
  Write-Output("MSILCHelp         - List and explain available commands. Example: MSILCHelp")
  Write-Output("NuGetCLR          - NuGet CoreCLR package. Example: NuGetCLR")
  Write-Output("ReBaseAll         - Re-create the base line for all regression test cases. Example: ReBaseAll")
  Write-Output("RunTest           - Run MSILC enabled CoreCLR regression tests. Example: RunTest")
  Write-Output("cdb               - cd to LLVM build directory")
  Write-Output("cdl               - cd to LLVM source directory")
  Write-Output("cdm               - cd to MSILC source diretory")
  Write-Output("cdw               - cd to work directory")
}

# -------------------------------------------------------------------------
#
# The Script
#
# -------------------------------------------------------------------------

MSILCEnvInit
