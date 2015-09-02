REM Usage: llilc_roslyn_stage.cmd workspace buildsubdir stagedir
REM 
REM Will first delete (if present) and then create %workspace%/roslyn/%stagedir%
REM directory and set it up as the directory
REM from which Roslyn will run on CoreClr, with LLILC as JIT.
REM buildsubdir is the subdirectory of the workspace where LLVM (and LLILC) were built.

setlocal
set WORKSPACE=%1
set buildsubdir=%2
set stagedir=%3
cd %WORKSPACE%
xcopy /y %buildsubdir%\bin\RelWithDebInfo\llilcjit.dll coreclr\bin\Product\Windows_NT.x64.Debug
call "%VS140COMNTOOLS%..\..\VC\vcvarsall.bat" x86
echo on
cd roslyn
if exist %stagedir%/ rd /s /q %stagedir%
mkdir %stagedir%
xcopy /S /Q Binaries\Debug\core-clr\* %stagedir%
rename %stagedir%\csc.exe csc.dll
copy /y %WORKSPACE%\coreclr\bin\Product\Windows_NT.x64.Debug\CoreConsole.exe %stagedir%\csc.exe
set command=C:\Python34\python %WORKSPACE%\llvm\tools\llilc\test\llilc_run.py  -c %WORKSPACE%\coreclr\bin\Product\Windows_NT.x64.Debug  -a %WORKSPACE%\roslyn\%stagedir%\csc.exe -- %%*
echo %command% > %stagedir%\runcsc.cmd
echo exit /b %%ERRORLEVEL%% >> %stagedir%\runcsc.cmd

rd /s /q Binaries
msbuild /m /v:d /p:CSCTOOLPATH=%WORKSPACE%\roslyn\%stagedir% /p:CSCTOOLEXE=runcsc.cmd src/Compilers/CSharp/CscCore/CscCore.csproj
endlocal
