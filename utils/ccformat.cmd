REM ***********************************************
REM Copyright (c) Microsoft. All rights reserved.
REM Licensed under the MIT license. See LICENSE file in the project root for full license information. 
REM ***********************************************

@echo off
setlocal

REM ***********************************************
REM Setup
REM ***********************************************

if "%LLVMSOURCE%"=="" (
    echo.No LLVMSOURCE
    set RETURN=2
    goto done
)

if "%LLVMBUILD%"=="" (
    echo.No LLVMBUILD
    set RETURN=2
    goto done
)

if "%LLILCSOURCE%"=="" (
   set LLILCSOURCE=%LLVMSOURCE%\tools\llilc
)

set LLILCLIB=%LLILCSOURCE%\lib
set LLILCINC=%LLILCSOURCE%\include
set CLRINC=%LLILCINC%\clr
set LLVMINC=%LLVMSOURCE%\include

SET RETURN=0
set FORMATFIX=
set TIDYFIX=
set FORMAT=Yes
set TIDY=Yes
set BASE=origin
set TIDYCHECKS=llvm*,misc*,microsoft*

:parse

REM ***********************************************
REM Driver
REM ***********************************************

if "%1"=="" (
    goto done_parsing
) else if /I .%1==./fix ( 
    set FORMATFIX=-i
    set TIDYFIX=-fix
) else if /I .%1==./untidy ( 
    set TIDY=No
) else if /I .%1==./noformat ( 
    set FORMAT=No    
) else if /I .%1==./checks (     
    set TIDYCHECKS=%2
    shift /1
) else if /I .%1==./base (
    set BASE=%2
    shift /1
) else if /I .%1==./help ( 
    goto help   
) else if /I .%1==./? ( 
    goto help
) else (
    echo.Unknown option %1
    set RETURN=3
    goto done
)

shift /1
goto parse

:done_parsing

REM ***********************************************
REM Run Clang Tidy
REM ***********************************************
:tidy

if .%TIDY%==.No (
    goto format
)

setlocal EnableDelayedExpansion
set INC=
set TEMPINC=%INCLUDE%
:loop
    for /f "tokens=1* delims=;" %%i in ("%TEMPINC%") do (
      set INC=!INC! -isystem "%%i"
      set TEMPINC=%%j
    )
    if defined TEMPINC goto loop

set FLAGS=-target x86_64-pc-win32-msvc -fms-extensions -fms-compatibility -fmsc-version=1800
set INC=%INC% -I%LLILCLIB%\Jit -I%LLILCLIB%\MSILReader -I%LLILCINC% -I%LLVMINC% -I%CLRINC% -I%LLILCINC%\Driver -I%LLILCINC%\Jit -I%LLILCINC%\Reader -I%LLILCINC%\Pal -I%LLVMBUILD%\tools\LLILC\lib\Reader -I%LLVMBUILD%\tools\LLILC\include -I%LLVMBUILD%\include 
set DEF=-D_DEBUG -D_CRT_SECURE_NO_DEPRECATE -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -D_CRT_NONSTDC_NO_WARNINGS -D_SCL_SECURE_NO_DEPRECATE -D_SCL_SECURE_NO_WARNINGS -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS-D__STDC_LIMIT_MACROS -D_GNU_SOURCE -DLLILCJit_EXPORTS -D_WINDLL -D_MBCS -DNOMINMAX

pushd %LLILCSOURCE%
for /f %%f in ('dir /s /b *.c *.cpp') do (
   clang-tidy %TIDYFIX% -checks=%TIDYCHECKS% -header-filter="LLILC.*(Reader)|(Jit)|(Pal)" %%f -- %FLAGS% %DEF% %INC%
   if ERRORLEVEL 1 set RETURN=1
)
popd


REM ***********************************************
REM Run Clang Format
REM ***********************************************
:format

if .%FORMAT%==.No (
    goto done
)

git diff %BASE% -U0 2>nul | clang-format-diff -p1 %FORMATFIX%

goto done

:help

REM ***********************************************
REM Help Section
REM ***********************************************

echo.Usage: ccFormat [/Check] [/Fix] [/?]
echo.  /fix          Fix failures when possible
echo.  /untidy       Don't run clang-tidy
echo.  /noformat     Don't run clang-format
echo.  /checks ^<chk^> Clang Tidy checks to run
echo.  /base ^<base^>  Base for obtaining Diffs 
echo.  /help         Display this help message.
echo.
echo.Requirements:
echo.  Environment variables: LLVMSOURCE, LLVMBUILD
echo.  Tools (on path): clang-format.exe, clang-tidy.exe clang-format-diff.py
echo.  Tool Dependencies: Python 2.7
echo.

:done 

exit /b %RETURN%
