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

if "%MSILCSOURCE%"=="" (
   set MSILCSOURCE=%LLVMSOURCE%\Tools\MSILC
)

set MSILCLIB=%MSILCSOURCE%\lib
set MSILCINC=%MSILCSOURCE%\include
set CLRINC=%MSILCINC%\clr
set LLVMINC=%LLVMSOURCE%\include

SET RETURN=0
set FIX=
set FORMAT=Check
set TIDY=Yes

:parse

REM ***********************************************
REM Driver
REM ***********************************************

if "%1"=="" (
    goto done_parsing
) else if /I .%1==./fix ( 
    set FIX=-fix
) else if /I .%1==./untidy ( 
    set TIDY=No
) else if /I .%1==./noformat ( 
    set FORMAT=No    
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

:tidy

REM ***********************************************
REM Run Clang Tidy
REM ***********************************************

if .%TIDY%==.No (
    goto Format
)

set INC=-I%MSILCLIB%\Jit -I%MSILCLIB%\MSILReader -I%MSILCINC% -I%LLVMINC% -I%CLRINC% -I%MSILCINC%\Driver -I%MSILCINC%\Jit -I%MSILCINC%\Reader -I%LLVMBUILD%\tools\MSILC\lib\Reader -I%LLVMBUILD%\tools\MSILC\include -I%LLVMBUILD%\include 
set DEF=-D_DEBUG -D_CRT_SECURE_NO_DEPRECATE -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -D_CRT_NONSTDC_NO_WARNINGS -D_SCL_SECURE_NO_DEPRECATE -D_SCL_SECURE_NO_WARNINGS -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS-D__STDC_LIMIT_MACROS -D_GNU_SOURCE -DMSILCJit_EXPORTS -D_WINDLL -D_MBCS 

pushd %MSILCSOURCE%
for /f %%f in ('dir /s /b *.c *.cpp') do (
   clang-tidy %FIX% -checks=llvm*,misc*,microsoft* -header-filter="MSILC.*(Reader)|(Jit)" %%f -- %DEF% %INC%
   if ERRORLEVEL 1 set RETURN=1
)
popd

:Format

REM ***********************************************
REM Run Clang Format
REM ***********************************************

if .%FORMAT%==.No (
    goto done
)

if .%FIX%==.-fix (
    goto FormatFix
)

:FormatCheck

REM Clang Format -- check only
REM Check if the formatted output from clang-format 
REM is any different from the source code file

set TMP_DIR=%TMP%\ccformat
IF NOT EXIST %TMP_DIR% mkdir %TMP_DIR%

pushd %MSILCSOURCE%
for /f %%f in ('dir /s /b *.c *.cpp') do (
   clang-format -style=LLVM %%f > %TMP_DIR%\%%~nxf
   diff -u %%f %TMP_DIR%\%%~nxf
   if ERRORLEVEL 1 set RETURN=1
)
popd

REM CLR headers are exempt from formatting checks

pushd %MSILCINC%
for /f %%d in ('dir /AD /b') do (
    if /i not .%%d==.clr (
        pushd %%d
        for /f %%f in ('dir /s /b *.h *.inc 2^>nul') do (
            clang-format -style=LLVM %%f > %TMP_DIR%\%%~nxf
            diff -u %%f %TMP_DIR%\%%~nxf
            if ERRORLEVEL 1 set RETURN=1
        )
        popd
    )
)
popd

rmdir /q /s %TMP_DIR% 

goto done

:FormatFix

REM Clang Format -- fix errors

pushd %MSILCSOURCE%
for /f %%f in ('dir /s /b *.c *.cpp') do (
   clang-format.exe -style=LLVM -i %%f
)
popd

pushd %MSILCINC%
for /f %%d in ('dir /AD /b') do (
    if /i not .%%d==.clr (
        pushd %%d
        for /f %%f in ('dir /s /b *.h *.inc 2^>nul') do (
           clang-format.exe -style=LLVM -i %%f
        )
        popd
    )
)
popd

goto done

:help

REM ***********************************************
REM Help Section
REM ***********************************************

echo.Usage: ccFormat [/Check] [/Fix] [/?]
echo.  /fix      Fix failures when possible
echo.  /untidy   Don't run clang-tidy
echo.  /noformat Don't run clang-format
echo.  /help     Display this help message.
echo.
echo.Requirements:
echo.  Environment variables: LLVMSOURCE, LLVMBUILD
echo.  Tools (on path): clang-format.exe, clang-tidy.exe
echo.

:done 

exit /b %RETURN%