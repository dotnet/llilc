REM Usage: llilc_roslyn_stage.cmd workspace buildsubdir stagedir
REM 
REM Will first delete (if present) and then create %workspace%/roslyn/%stagedir%
REM directory and set it up as the directory
REM from which Roslyn will run on CoreClr, with LLILC as JIT.
REM buildsubdir is the subdirectory of the workspace where LLVM (and LLILC) were built.

setlocal
set WORKSPACE=%1
set buildsubdir=%2
set buildtype=%3
set stagedir=%4
cd %WORKSPACE%
call "%VS140COMNTOOLS%..\..\VC\vcvarsall.bat" x86
echo on
cd roslyn
if exist %stagedir%/ rd /s /q %stagedir%
mkdir %stagedir%
xcopy /y %buildsubdir%\bin\%buildtype%\llilcjit.dll %stagedir%
xcopy /S /Q Binaries\Debug\core-clr\* %stagedir%
xcopy /S /Q /Y %WORKSPACE%\coreclr\bin\Product\Windows_NT.x64.Debug\* %stagedir%
set command=python %WORKSPACE%\llvm\tools\llilc\test\llilc_run.py  -c %WORKSPACE%\roslyn\%stagedir% -r corerun.exe /v -a %WORKSPACE%\roslyn\%stagedir%\csc.exe -- %%*
echo %command% > %stagedir%\runcsc.cmd
echo exit /b %%ERRORLEVEL%% >> %stagedir%\runcsc.cmd

rd /s /q Binaries
msbuild /m /v:d /p:CSCTOOLPATH=%WORKSPACE%\roslyn\%stagedir% /p:CSCTOOLEXE=runcsc.cmd /p:UseRoslynAnalyzers=false src/Compilers/CSharp/CscCore/CscCore.csproj
endlocal
