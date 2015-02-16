# -------------------------------------------------------------------------
#
# This script provides MSILC development environment and some
# facilities for daily development use.
#
# -------------------------------------------------------------------------

<#
.SYNOPSIS
    Setup MSILC development environment and provide some daily routines

.DESCRIPTION
    This script set up the MSILC environment with the assumptions below:
    
    1. The following software are installed:
    Visual Studio 12.0, Git, CMake, Python, GnuWin32, and DiffMerge

    2. LLVM and MSILC source are downloaded and an LLVM build directory
    are specified. Note that MSILC source should be located at the
    correct place under LLVM: tools\llvm-msilc .
    
    3. A separate MSILC work directory is specified, where CLR, MSILC
    JIT, and test cases from CLR drop will be copied into so that you can
    run test.

    4. The following environment variable are set, or if not, it will be
    picked up from default values listed in function ValidatePreConditions
    if they are valid.

    VS120COMNTOOLS
    GITDIR
    CMAKEDIR
    PYTHONDIR
    GNUWIN32DIR
    LLVMSOURCE
    LLVMBUILD
    WORKMSILCDIR
    DIFFTOOL
    CLRDROP
    MSILCTESTSRC
    ILASMEXE
    CSCEXE

    5. Developer has to provide a powershell global function CopyTestCasesHelper
    in the following form to curstomize their test needs:

    function Global:CopyTestCasesHelper
    {
      CopyTestCase("\CertainDir1\test1.il")
      CopyTestCase("\CertainDir2\test2.cs")
    }

    It will copy test cases located in MSILCTESTSRC into work test directory.

    This script completes the environment setup and reports the status.
    
    Some daily routines are also provided:
    CheckCLR, CopyCLR,
    CheckJIT, CopyJIT,
    CheckTestCases, CopyTestCase, CopyTestCases, 
    CheckWorkDir, CleanWorkDir, CopyWorkDir,
    MSILCRegr, MSILCRegrDiff,
    CopyJITAndRun (cr in short), CopyWorkDirAndRun
    CheckEnv,
    CreateBase, ReBaseOne, ReBaseAll
    ApplyFilter,
    MSILCHelp,
    and some quick directory navigations (cdw, cdp, cdl, cdb)

    Regression test cases will be copied from CLRDrop into work test
    directory through CopyTestCases. The test cases themselves are
    not included in repository. But a list of regressions test cases
    name and its base line for diff are located at MSILC test directory.
    
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
   [string]$Arch="amd64",
   [string]$Build="debug"
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
  $DefaultLLVMSource = "C:\LLVM\"
  $DefaultLLVMBuild = "C:\LLVMBuild\"
  $DefaultWorkMSILCDir = "C:\WorkMSILC\"
  $DefaultDiffTool =  "C:\Program Files\SourceGear\Common\DiffMerge\sgdm.exe"
  $DefaultCLRDrop = "C:\CLRDrop\"
  $DefaultMSILCTestSrc = "C:\MSILCTestSrc\"
  $DefaultILAsmExe = "C:\CLRDrop\ilasm.exe"
  $DefaultCSCExe = "C:\CLRDrop\csc.exe"

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
  if (!$GitExists) {
    $GitExists = Test-Path $DefaultGit;
    if (!$GitExists) {
      Write-Warning "!!! Git not specified." 
    }
    else {
      $Env:GITDIR = $DefaultGit
    }
  }
  else {
    $GitExists = Test-Path $Env:GITDIR
    if (!$GitExists) {
      Write-Warning "!!! Git not available in specified location." 
    }
  }

  # Validate CMake

  $CMakeExists = Test-Path Env:\CMAKEDIR
  if (!$CMakeExists) {
    $CMakeExists = Test-Path $DefaultCMake;
    if (!$CMakeExists) {
      Write-Warning "!!! CMake not specified." 
    }
    else {
      $Env:CMAKEDIR = $DefaultCMake
    }
  }
  else {
    $CMakeExists = Test-Path $Env:CMAKEDIR
    if (!$CMakeExists) {
      Write-Warning "!!! CMake not available in specified location." 
    }
  }

  # Validate Python

  $PythonExists = Test-Path Env:\PYTHONDIR
  if (!$PythonExists) {
    $PythonExists = Test-Path $DefaultPython;
    if (!$PythonExists) {
      Write-Warning "!!! Python not specified." 
    }
    else {
      $Env:PYTHONDIR = $DefaultPython
    }
  }
  else {
    $PythonExists = Test-Path $Env:PYTHONDIR
    if (!$PythonExists) {
      Write-Warning "!!! Python not available in specified location." 
    }
  }

  # Validate GnuWin32

  $GnuWin32Exists = Test-Path Env:\GNUWIN32DIR
  if (!$GnuWin32Exists) {
    $GnuWin32Exists = Test-Path $DefaultGnuWin32;
    if (!$GnuWin32Exists) {
      Write-Warning "!!! GnuWin32 not specified." 
    }
    else {
      $Env:GNUWIN32DIR = $DefaultGnuWin32
    }
  }
  else {
    $GnuWin32Exists = Test-Path $Env:GNUWIN32DIR
    if (!$GnuWin32Exists) {
      Write-Warning "!!! GnuWin32 not available in specified location." 
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

  # Validate CLR Drop

  $CLRDropExists = Test-Path Env:\CLRDROP
  if (!$CLRDropExists) {
    $CLRDropExists = Test-Path $DefaultCLRDrop
    if (!$CLRDropExists) {
      throw "!!! CLR Drop not specified."
    }
    else {
      $Env:CLRDROP = $DefaultCLRDrop
    }
  }
  else {
    $CLRDropExists = Test-Path $Env:CLRDROP
    if (!$CLRDropExists) {
      throw "!!! CLR Drop not available." 
    }
  }

  if ($CLRDropExists) {
    $CLRDropRuntimeExists = Test-Path "$Env:CLRDROP\TestNet\Runtime"
    if (!$CLRDropRuntimeExists) {
      throw "!!! CLR Drop Runtime not available" 
    }
    else {
      $Env:CLRDROPRUNTIME = "$Env:CLRDROP\TestNet\Runtime"
    }
      
    $CLRDropHostExists = Test-Path "$Env:CLRDROP\TestNet\Host"
    if (!$CLRDropHostExists) {
      throw " CLR Drop Host not available"
    }
    else {
      $Env:CLRDROPHOST = "$Env:CLRDROP\TestNet\Host"
    }

	$CLRSysBuildExists = Test-Path "$Env:CLRDROP\SysBuild"
    if (!$CLRSysBuildExists) {
      throw " CLR Drop SysBuild not available"
    }
    else {
      $Env:CLRDROPSYSBUILD = "$Env:CLRDROP\SysBuild"
    }
  }

  # Validate MSILC Work Directory

  $WorkMSILCDirExists = Test-Path Env:\WORKMSILCDIR
  if (!$WorkMSILCDirExists) {
    $WorkMSILCDirExists = Test-Path $DefaultWorkMSILCDir
    if (!$WorkMSILCDirExists) {
      throw "!!! MSILC Work Directory not specified."
    }
    else {
      $Env:WORKMSILCDIR = $DefaultWorkMSILCDir
    }
  }
  else {
    $WorkMSILCDirExists = Test-Path $Env:WORKMSILCDIR
    if (!$WorkMSILCDirExists) {
      throw "!!! MSILC Work Directory not available." 
    }
  }

  # Validate MSILC Test Source

  $MSILCTestSrcExists = Test-Path Env:\MSILCTESTSRC
  if (!$MSILCTestSrcExists) {
    $MSILCTestSrcExists = Test-Path $DefaultMSILCTestSrc
    if (!$MSILCTestSrcExists) {
      throw "!!! MSILC Test Source not specified."
    }
    else {
      $Env:MSILCTESTSRC = $DefaultMSILCTestSrc
    }
  }
  else {
    $MSILCTestSrcExists = Test-Path $Env:MSILCTESTSRC
    if (!$MSILCTestSrcExists) {
      throw "!!! MSILC Test Source not available." 
    }
  }

  # Validate ILASM executable

  $ILAsmExeExists = Test-Path Env:\ILASMEXE
  if (!$ILAsmExeExists) {
    $ILAsmExeExists = Test-Path $DefaultILAsmExe
    if (!$ILAsmExeExists) {
      throw "!!! ilasm executable not specified."
    }
    else {
      $Env:ILASMEXE = $DefaultILAsmExe
    }
  }
  else {
    $ILAsmExeExists = Test-Path $Env:ILASMEXE
    if (!$ILAsmExeExists) {
      throw "!!! ilasm executable not available." 
    }
  }

  # Validate CSC executable

  $CSCExeExists = Test-Path Env:\CSCEXE
  if (!$CSCExeExists) {
    $CSCExeExists = Test-Path $DefaultCSCExe
    if (!$CSCExeExists) {
      throw "!!! csc executable not specified."
    }
    else {
      $Env:CSCEXE = $DefaultCSCExe
    }
  }
  else {
    $CSCExeExists = Test-Path $Env:CSCEXE
    if (!$CSCExeExists) {
      throw "!!! csc executable not available." 
    }
  }

  # Validate Diff Tool

  $DiffExists = Test-Path Env:\DIFFTOOL
  if (!$DiffExists) {
    $DiffExists = Test-Path "$DefaultDiffTool"
    if (!$DiffExists) {
      Write-Warning "!!! Diff Tool not specified." 
    }
    else {
      $Env:DIFFTOOL = "$DefaultDiffTool"
    }
  }
  else {
    $DiffExists = Test-Path "$Env:DIFFTOOL"
    if (!$DiffExists) {
      Write-Warning "!!! Diff Tool not available." 
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

  $TempBat = Join-Path $env:TEMP "getvc$pid.bat"
  #echo "TempBat = $TempBat"
  $File = "$Env:VS120COMNTOOLS\..\..\VC\bin\amd64\vcvars64.bat"
  #echo "VC batch file = $File"
  ("call ""$File""", "echo ENV_VARS_START", "set") | Out-File -Encoding ascii $TempBat
  $CmdOut = cmd /q /c $TempBat

  # If batch file fails we need to propagate the error.
  if ($LASTEXITCODE -gt 0) {
    $CmdOut
    exit $LASTEXITCODE
  }

  Remove-Item $TempBat

  ## Erase our current set of environment variables
  Remove-Item -path env:*

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

  $Global:JitName = "MSILCJit"
  $Env:MSILCJIT = "$Env:LLVMBUILD\bin\$BUILD\$Global:JitName.dll"
  $Env:MSILCTEST = "$Env:MSILCSOURCE\test"
  $Env:MSILCTESTLIST = "$Env:MSILCTEST\MSILCRegr.lst"

  $Env:WORKCLRRUNTIME = "$Env:WORKMSILCDIR\CLRRuntime"
  $Env:WORKCLRCORERUN = "$Env:WORKCLRRUNTIME\CoreRun.exe"
  $Env:WORKMSILCJIT = "$Env:WORKCLRRUNTIME\$Global:JitName.dll"
  $Env:WORKMSILCTEST = "$Env:WORKMSILCDIR\test"
}

# -------------------------------------------------------------------------
#
# Check the status of CLR Drop and its copy in work directory
# 
# -------------------------------------------------------------------------

function Global:CheckCLR
{
  $CLRDropExists = Test-Path $Env:CLRDROP
  if (!$CLRDropExists) {
    Write-Output ("CLR Drop not available.")
  }

  $CLRDropRuntimeExists = Test-Path $Env:CLRDROPRUNTIME
  if (!$CLRDropRuntimeExists) {
    Write-Output ("CLR Drop Runtime not available.") 
  }
     
  $CLRDropHostExists = Test-Path $Env:CLRDROPHOST
  if (!$CLRDropHostExists) {
    Write-Output ("CLR Drop Host not available.")
  }

  $CLRDropSysBuildExists = Test-Path $Env:CLRDROPSYSBUILD
  if (!$CLRDropSysBuildExists) {
    Write-Output ("CLR Drop SysBuild not available.")
  }

  $WorkCLRRuntimeExists = Test-Path $Env:WORKCLRRUNTIME
  if (!$WorkCLRRuntimeExists) {
    Write-Output("CLR Runtime not copied into work yet.")
    $WorkCLRCoreRunExists = Test-Path $Env:WorkCLRCORERUN
    if (!$WorkCLRCoreRunExists) {
      Write-Output ("CLR CoreRun.exe not copiled into work yet.")
    }
  }
}

# -------------------------------------------------------------------------
#
# Check the status of MSILC JIT in LLVM build directory and its copy
# in work directory
# 
# -------------------------------------------------------------------------

function Global:CheckJIT
{
  $MSILCJITExists = Test-Path $Env:MSILCJIT
  if (!$MSILCJITExists) {
    Write-Output ("MSILC JIT has not been built yet.") 
  }

  $WorkMSILCJITExists = Test-Path $Env:WORKMSILCJIT
  if (!$WorkMSILCJITExists) {
    Write-Output ("MSILC JIT has not been copied into work yet.") 
  }
}

# -------------------------------------------------------------------------
#
# Check the status of regression test cases in work test directory, and 
# its baseline in MSILC test directory.
# 
# -------------------------------------------------------------------------

function Global:CheckTestCases
{
  $MSILCTestListExists = Test-Path $Env:MSILCTESTLIST
  if (!$MSILCTestListExists) {
    throw "!!! Regression Test List not available." 
  }
  
  $WorkMSILCTestExists = Test-Path $Env:WORKMSILCTEST
  if (!$WorkMSILCTestExists) {
    Write-Output("Regression Test Cases not copied into work yet.")
  }
  else {
    $Lines = Get-Content $Env:MSILCTESTLIST
    foreach ($Line in $Lines) {
      $TestCaseExists = Test-Path "$Env:WORKMSILCTEST\$Line.exe"
      if (!$TestCaseExists) {
        Write-Output ("Regression Test Case $Line not copied into work yet.")
      }
    }
  }

  $Lines = Get-Content $Env:MSILCTESTLIST
  foreach ($Line in $Lines) {
    $TestCaseBaseLineExists = Test-Path "$Env:MSILCTEST\$Line.base"
    if (!$TestCaseBaseLineExists) {
      Write-Output ("Regression Test Case $Line baseline not created yet.")
    }
  }
}

# -------------------------------------------------------------------------
#
# Check the status of work directory
# 
# -------------------------------------------------------------------------

function global:CheckWorkDir
{
  Write-Output ("Work Directory Status:")
  CheckCLR
  CheckJIT  
  CheckTestCases
}

# -------------------------------------------------------------------------
#
# Quick Directory Navigations
# 
# -------------------------------------------------------------------------

function Global:cdw
{
  cd $Env:WORKMSILCDIR
}

function Global:cdp
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
  Write-Output ("************************************ MSILC Work Environment **************************************")
  Write-Output ("VS120COMNTOOLS     : $Env:VS120COMNTOOLS")
  Write-Output ("GITDIR             : $Env:GITDIR")
  Write-Output ("CMAKEDIR           : $Env:CMAKEDIR")
  Write-Output ("PYTHONDIR          : $Env:PYTHONDIR")
  Write-Output ("GNUWIN32DIR        : $Env:GNUWIN32DIR")
  Write-Output ("LLVMSOURCE         : $Env:LLVMSOURCE")
  Write-Output ("LLVMBUILD          : $Env:LLVMBUILD")
  Write-Output ("MSILCSOURCE        : $Env:MSILCSOURCE")
  Write-Output ("CLRDROP            : $Env:CLRDROP")
  Write-Output ("CLRDROPRUNTIME     : $Env:CLRDROPRUNTIME")
  Write-Output ("CLRDROPHOST        : $Env:CLRDROPHOST")
  Write-Output ("CLRDROPSYSBUILD    : $Env:CLRDROPSYSBUILD")
  Write-Output ("ILASMEXE           : $Env:ILASMEXE")
  Write-Output ("CSCEXE             : $Env:CSCEXE")
  Write-Output ("MSILCJIT           : $Env:MSILCJIT")
  Write-Output ("MSILCTEST          : $Env:MSILCTEST")
  Write-Output ("MSILCTESTLIST      : $Env:MSILCTESTLIST")
  Write-Output ("MSILCTESTSRC       : $Env:MSILCTESTSRC")
  Write-Output ("WORKMSILCDIR       : $Env:WORKMSILCDIR")
  Write-Output ("WORKCLRRUNTIME     : $Env:WORKCLRRUNTIME")
  Write-Output ("WORKCLRCORERUN     : $Env:WORKCLRCORERUN")
  Write-Output ("WORKMSILCJIT       : $Env:WORKMSILCJIT")
  Write-Output ("WORKMSILCTEST      : $Env:WORKMSILCTEST")
  Write-Output ("**************************************************************************************************")

  CheckWorkDir
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
  CleanWorkDir
  CheckEnv
  cdp
}

# -------------------------------------------------------------------------
#
# Copy in CLR
#
# -------------------------------------------------------------------------

function Global:CopyCLR
{
  $WorkCLRRuntimeExists = Test-Path $Env:WORKCLRRUNTIME
  if ($WorkCLRRuntimeExists) {
    Remove-Item $Env:WORKCLRRUNTIME -recurse
  }

  New-Item $Env:WORKCLRRUNTIME -itemtype directory

  pushd .
  cd $Env:WORKCLRRUNTIME

  Write-Output ("Copying CLR Runtime")
  xcopy /Q  $Env:CLRDROPRUNTIME .
  Write-Output ("CLR Runtime copied")
 
  Write-Output ("Copying CLR Host")
  xcopy /Q $Env:CLRDROPHOST .
  Write-Output ("CLR Host copied")

  Write-Output ("Copying CLR SOS")
  xcopy /Q $Env:CLRDROPSYSBUILD\x64\SOS.dll .
  xcopy /Q $Env:CLRDROPSYSBUILD\sos*.dll .
  xcopy /Q $Env:CLRDROPSYSBUILD\mscordaccore*.dll .
  Write-Output ("CLR SOS copied")

  popd
}

# -------------------------------------------------------------------------
#
# Copy in MSILC JIT dll
#
# -------------------------------------------------------------------------

function Global:CopyJIT
{
  $WorkMSILCJitExists = Test-Path $Env:WORKMSILCJIT
  if ($WorkMSILCJitExists) {
    Remove-Item $Env:WORKMSILCJIT
  }

  pushd .
  cd $Env:WORKCLRRUNTIME

  Write-Output ("Copying MSILC JIT")
  copy $Env:MSILCJIT .
  Write-Output ("MSILC JIT Copied")

  popd
}

# -------------------------------------------------------------------------
#
# Copy in .il and create .exe with ilasm for one test case
#
# -------------------------------------------------------------------------

function Global:CopyTestCase([string]$TCWithPath)
{
  copy $Env:MSILCTESTSRC\$TCWithPath $Env:WORKMSILCTEST
  $ILFile = Get-ChildItem $Env:MSILCTESTSRC\$TCWithPath
  $TC = $ILFile.BaseName

  switch ($ILFile.Extension) {
    '.il' {
      & $Env:ILASMEXE /PDB $Env:WORKMSILCTEST\$TC.il
    }
    '.cs' {
      & $Env:CSCEXE /debug:pdbonly $Env:WORKMSILCTEST\$TC.cs
    }
  }
}

# -------------------------------------------------------------------------
#
# Copy in .il for all regression tests in list and create .exe with ilasm
#
# -------------------------------------------------------------------------

function Global:CopyTestCases
{
  $WorkMSILCTestExists = Test-Path $Env:WORKMSILCTEST
  if ($WorkMSILCTestExists) {
    Remove-Item $Env:WORKMSILCTEST -recurse
  }

  New-Item $Env:WORKMSILCTEST -itemtype directory

  pushd .
  cd $Env:WORKMSILCTEST

  Write-Output ("Copying Test Cases")
  
  CopyTestCasesHelper

  Write-Output ("Test Cases Copied")
  
  popd
}

# -------------------------------------------------------------------------
#
# Clean all resources in work directory
#
# -------------------------------------------------------------------------

function Global:CleanWorkDir
{
  $WorkCLRRuntimeExists = Test-Path $Env:WORKCLRRUNTIME
  if ($WorkCLRRuntimeExists) {
    Remove-Item $Env:WORKCLRRUNTIME -recurse -force
  }
  
  $WorkMSILCTestExists = Test-Path $Env:WORKMSILCTEST
  if ($WorkMSILCTestExists) {
    Remove-Item $Env:WORKMSILCTEST -recurse -force
  }
}

# -------------------------------------------------------------------------
#
# Copy in all resources for Work Directory
#
# -------------------------------------------------------------------------

function Global:CopyWorkDir
{
  CopyCLR
  CopyJIT
  CopyTestCases
}

# -------------------------------------------------------------------------
#
# Filter to suppress allowable LLVM IR difference
#
# -------------------------------------------------------------------------

function Global:ApplyFilter([string]$File,[string]$TmpFile)
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

  (Get-Content $File) -replace 'i64 \d{10}\d*', 'i64 NORMALIZED_ADDRESS' | Out-File $TmpFile -Encoding ascii

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
  
  (Get-Content $TmpFile) -replace '%(.*?)\.\d+ addrspace', '%$1.NORMALIZED_TYPEID addrspace' | Out-File $TmpFile -Encoding ascii

  # Suppress type id difference from run to run, string name with double quotes

  (Get-Content $TmpFile) -replace '%"(.*?)\.\d+" addrspace', '%"$1.NORMALIZED_TYPEID" addrspace' | Out-File $TmpFile -Encoding ascii
}

# -------------------------------------------------------------------------
#
# Execute one .exe with CoreRun.exe
#
# -------------------------------------------------------------------------

function Global:ExecuteOne([string]$TC)
{
    $COMPLus_AltJitExists = Test-Path Env:\COMPLus_AltJit
    if ($COMPLus_AltJitExists) {
      $COMPLus_AltJitValue = $Env:COMPLus_AltJit
    }

    $COMPLus_AltJitNameExists = Test-Path Env:\COMPLus_AltJitName
    if ($COMPLus_AltJitNameExists) {
      $COMPLus_AltJitNameValue = $Env:COMPLus_AltJitName
    }

    $Env:COMPLus_AltJit = 1
    $Env:COMPLus_AltJitName = "$Global:JitName.dll"

    $Process = Start-Process $Env:WORKCLRCORERUN $Env:WORKMSILCTEST\$TC.exe -RedirectStandardError $Env:WORKMSILCTEST\$TC.error -RedirectStandardOutput $Env:WORKMSILCTEST\$TC.output -PassThru -Wait -WindowStyle Hidden

    if ($COMPLus_AltJitExists) {
      $Env:COMPLus_AltJit = $COMPLus_AltJitValue
    }
    else {
      Remove-Item Env:\COMPLus_AltJit
    }

    if ($COMPLus_AltJitNameExists) {
      $Env:COMPLus_AltJitName = $COMPLus_AltJitNameValue
    }
    else {
      Remove-Item Env:\COMPLus_AltJitName
    }

    return $Process
}

# -------------------------------------------------------------------------
#
# Run regression test
#
# -------------------------------------------------------------------------

function Global:MSILCRegr([bool]$Diff = $False)
{
  $PassedNodiff = 0
  $PassedDiff = 0
  $Failed = 0

  pushd .

  cd $Env:WORKCLRRUNTIME
  Write-Output ("Running Regression Test Cases")

  if ($Diff) {
    $RunDirExists = Test-Path $Env:WORKMSILCTEST\run
    if ($RunDirExists) {
      Remove-Item $Env:WORKMSILCTEST\run -recurse -force
    }

    $BaseDirExists = Test-Path $Env:WORKMSILCTEST\base
    if ($BaseDirExists) {
      Remove-Item $Env:WORKMSILCTEST\base -recurse -force
    }

    New-Item $Env:WORKMSILCTEST\run -itemtype directory
    New-Item $Env:WORKMSILCTEST\base -itemtype directory
  }

  $Lines = Get-Content $Env:MSILCTESTLIST
  foreach ($Line in $Lines) {
    $Process = ExecuteOne("$Line")

    Get-Content $Env:WORKMSILCTEST\$Line.error > $Env:WORKMSILCTEST\$Line.run
    Get-Content $Env:WORKMSILCTEST\$Line.output >> $Env:WORKMSILCTEST\$Line.run
    Remove-Item $Env:WORKMSILCTEST\$Line.error
    Remove-Item $Env:WORKMSILCTEST\$Line.output
    ApplyFilter -File "$Env:WORKMSILCTEST\$Line.run" -TmpFile "$Env:WORKMSILCTEST\$Line.run.tmp"
    
    $DiffResult = Compare-Object -Ref (Get-Content $Env:WORKMSILCTEST\$Line.run.tmp) -Diff (Get-Content $Env:MSILCTEST\$Line.base) | Tee-Object -file $Env:WORKMSILCTEST\$Line.diff | Measure
    
    if ($Process.ExitCode -eq 43690) {
      if ($DiffResult.Count -eq 0) {
        Write-Output("$Line passed with nodiff.")
        $PassedNodiff++
      }
      else {
        if ($Diff) {
          copy $Env:WORKMSILCTEST\$Line.run.tmp $Env:WORKMSILCTEST\run\$Line.output
          copy $Env:MSILCTEST\$Line.base $Env:WORKMSILCTEST\base\$Line.output
        }

        Write-Output("$Line passed with diff.")
        $PassedDiff++
      }
    }
    else {
      Write-Output("$Line failed.")
      $Failed++
    }

    Remove-Item $Env:WORKMSILCTEST\$Line.run.tmp
  }
  Write-Output ("Regression Test Finished: $PassedNodiff passed with nodiff, $PassedDiff passed with diff, and $Failed failed.")

  if ($Diff) {
    if ($PassedDiff -ne 0) {
      & "$Env:DIFFTOOL" -t1=Base -t2=Run $Env:WORKMSILCTEST\base $Env:WORKMSILCTEST\run
    }
  }

  popd
}

# -------------------------------------------------------------------------
#
# Run regression test and pop up diff tool if there were any diff
#
# -------------------------------------------------------------------------

function Global:MSILCRegrDiff
{
  MSILCRegr -Diff $True
}

# -------------------------------------------------------------------------
#
# Create base for a test case and add it to test list if you want
#
# -------------------------------------------------------------------------
 
function Global:CreateBase([string]$TC, [bool]$AddToList = $true)
{
  $WorkMSILCTestExists = Test-Path $Env:WORKMSILCTEST\$TC.exe
  if (!$WorkMSILCTestExists) {
    Write-Output("Regression Test Case $TC not copied into work yet. No base created.")
  }
  else {
    $Process = ExecuteOne("$TC")

    if ($Process.ExitCode -eq 43690) {
      $BaseExists = Test-Path $Env:MSILCTEST\$TC.base
      if ($BaseExists) {
        Remove-Item $Env:MSILCTEST\$TC.base
      }
      
      Get-Content $Env:WORKMSILCTEST\$TC.error > $Env:MSILCTEST\$TC.base.tmp
      Get-Content $Env:WORKMSILCTEST\$TC.output >> $Env:MSILCTEST\$TC.base.tmp
      Remove-Item $Env:WORKMSILCTEST\$TC.error
      Remove-Item $Env:WORKMSILCTEST\$TC.output
      ApplyFilter -File "$Env:MSILCTEST\$TC.base.tmp" -TmpFile "$Env:MSILCTEST\$TC.base"
      Remove-Item $Env:MSILCTEST\$TC.base.tmp

      if ($AddToList) {
        Add-Content $Env:MSILCTESTLIST "$TC"     
        Write-Output("Test case $TC added to test list with base created.")
      }
      else {
        Write-Output("Test case $TC base created.")
      }
    }
    else {
      Write-Output("Test case $TC execution failed. No base created.")
    }
  }
}

# -------------------------------------------------------------------------
#
# Re-create the base line for one regression test case, list not touched.
#
# -------------------------------------------------------------------------

function Global:ReBaseOne([string]$TC)
{
  CreateBase -TC $TC -AddToList $False
}

# -------------------------------------------------------------------------
#
# Re-create the base line for all regression test cases, list not touched.
#
# -------------------------------------------------------------------------

function Global:ReBaseAll
{
  $Lines = Get-Content $Env:MSILCTESTLIST
  foreach ($Line in $Lines) {
    CreateBase -TC $Line -AddToList $False
  }
}

# -------------------------------------------------------------------------
#
# Copy MSILC JIT and run regression test
#
# -------------------------------------------------------------------------

function Global:CopyJITAndRun
{
  CopyJIT
  MSILCRegr
}

# -------------------------------------------------------------------------
#
# Quick form of CopyJITAndRun
# 
# -------------------------------------------------------------------------

function Global:cr
{
  CopyJIT
  MSILCRegr
}

# -------------------------------------------------------------------------
#
# Copy work directory and run regression test
#
# -------------------------------------------------------------------------

function Global:CopyWorkDirAndRun
{
  CopyWorkDir
  MSILCRegr
}

# -------------------------------------------------------------------------
#
# List and explain available commands
#
# -------------------------------------------------------------------------
function Global:MSILCHelp
{
  Write-Output("ApplyFilter       - Filter to suppress allowable LLVM IR difference. Example: AppyFilter -File FileName -TmpFile TmpFileName")
  Write-Output("CheckCLR          - Check the status of CLR Drop and its copy in work directory. Example: CheckCLR")
  Write-Output("CheckEnv          - Check the status of development environment. Example: CheckEnv")
  Write-Output("CheckJIT          - Check the status of MSILC JIT in LLVM build directory and its copy in work directory. Example: CheckJIT")
  Write-Output("CheckTestCases    - Check the status of regression test cases in work test directory, and its baseline in MSILC test directory. Example: CheckTestCases")
  Write-Output("CheckWorkDir      - Check the status of work directory. Example: CheckWorkDir")
  Write-Output("CleanWorkDir      - Clean all resources in work directory. Example: CleanWorkDir")
  Write-Output("CopyCLR           - Copy in CLR. Example: CopyCLR")
  Write-Output("CopyJIT           - Copy in MSILC JIT dll. Example: CopyJIT")
  Write-Output("CopyJITAndRun     - Copy MSILC JIT and run regression test. Example: CopyJitAndRun.")
  Write-Output("CopyTestCase      - Copy in .il and create .exe with ilasm for one test case. Example: CopyTestCase -TC `"Pacific\newtestcasename`"")
  Write-Output("CopyTestCases     - Copy in .il for all regression tests in list and create .exe with ilasm. Example: CopyTestCases")
  Write-Output("CopyWorkDir       - Copy in all resources for Work Directory. Example: CopyWorkDir")
  Write-Output("CopyWorkDirAndRun - Copy work directory and run regression test. Example: CopyWorkDirAndRun")
  Write-Output("CreateBase        - Create base for a test case and add it to test list if you want. Example: CreateBase -TC TestName, CreateBase -TC TestName -AddToList `$False")
  Write-Output("MSILCRegr         - Run regression test. Example: MSILCRegr")
  Write-Output("MSILCRegrDiff     - Run regression test and pop up diff tool if there were any diff. Example: MSILCRegrDiff")
  Write-Output("MSILCHelp         - List and explain available commands. Example: MSILCHelp")
  Write-Output("ReBaseOne         - Re-create the base line for one regression test case, list not touched. Example: ReBaseOne -TC beq")
  Write-Output("ReBaseAll         - Re-create the base line for all regression test cases, list not touched. Example: ReBaseAll")
  Write-Output("cdb               - cd to LLVM build directory")
  Write-Output("cdl               - cd to LLVM source directory")
  Write-Output("cdp               - cd to MSILC source diretory")
  Write-Output("cdw               - cd to work directory")
  Write-Output("cr                - short form of CopyJITAndRun")
}

# -------------------------------------------------------------------------
#
# The Script
#
# -------------------------------------------------------------------------

MSILCEnvInit
