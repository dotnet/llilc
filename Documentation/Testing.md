# Testing LLILC

## Test Harness

LLILC's test harness uses CoreCLR's test assets as underlying engine. It
provides a LLILC-specific wrapper layer above it. The test harness is located
at llilc\test. Please add this path to your PATH environment variable before
using it.

There are three steps in LLILC testing: building tests, running tests, and
checking results.

## Building tests:

Building tests is performed with CoreCLR script directly. A CoreCLR build
automatically performs building tests. If it is done already, you can choose
to skip this step. If you want to build test as a separate step, do the following:

 ```
cd C:\coreclr\tests
buildtest.cmd clean
 ```
https://github.com/dotnet/coreclr/wiki/Test-instructions for more information.

## Running tests:

Use llilc_runtest.py to run tests. With specified LLILC JIT and pre-built
CoreCLR runtime, llilc_runtest runs CoreCLR testing. You can choose to create
a running result if you want to. The result contains LLILC specific dumps:
per function summary or detailed LLVM IR dumps. You need to run this script
from coreclr\tests directory.
 ```
cd C:\coreclr\tests
llilc_runtest -d verbose -j C:\llvm-build\bin\Debug\llilcjit.dll -c C:\coreclr\bin\Product\Windows_NT.x64.Debug -r C:\results\base
 ```
llilc_runtest --help for more information.

**Note:** You may find that running tests with a pure DEBUG build of LLILC
is very slow.  To get a faster build that still has assertion checking, pass
`-DCMAKE_BUILD_TYPE=RELWITHDEBINFO -DLLVM_ENABLE_ASSERTIONS=ON` to cmake and
`/p:Configuration=RelWithDebInfo` to msbuild when you configure/build LLILC/LLVM.

## Checking results:

Use llilc_checkpass to check the results. It takes two test results, using
one as base result, one as diff result to perform the checking. This step is
LLILC-specific. Because LLILC is still under early stage of development, we
need to guarantee that any function that was successfully jitted by LLILC is
not broken by new change.
 ```
llilc_checkpass -b C:\results\base -d C:\results\diff
 ```
llilc_checkpass --help for more information.

It is required that developer has to guarantee all LLVM IR changes are benign.
It can be achieved with any diff tool.

## Running individual tests:

The process for running individual test cases on Windows in cmd is:
* Copy the LLILCJit.dll in `llvm-build/bin/Debug` to the CoreCLR binary
  directory (`coreclr/bin/Product/<build>/`)
* Run the `llilc/test/LLILCTestEnv.cmd` to set the needed env variables. If
  debug printing is desired, also run `set COMPlus_DumpLLVMIR=[summary, verbose]`.
* Set the code page to 65001 (See #527 and #12).
* Run the test binary via CoreRun.exe.

## Using llilc as an ngen jit:

crossgen.exe is the tool used to generate ngen images for CoreCLR. The
generated native image has .ni.exe or .ni.dll suffix. crossgen requires
mscorlib.ni.dll to be available – you have to generate mscorlib.ni.dll first.
The following environment variable needs to be set (in addition to running
llilc/test/LLILCTestEnv.cmd):
`set COMPlus_AltJitNgen=*`.

## Use Cases

### Developer Use Case
Step 1: Update CoreCLR to latest, and build CoreCLR

Step 2: Update LLILC master to latest, and build your baseline JIT

Step 3: Create a verbose baseline result:

```
cd coreclr\tests
llilc_runtest -j <JIT_PATH> -c <CORECLR_RUNTIME_PATH> -d verbose -r <BASE_RESULT_PATH>
```
Step 4: Create your own branch

Step 5: Do new development or fix issues on your branch, and build your new diff JIT

Step 6: Check your change pass overall testing:

```
cd coreclr\tests
llilc_runtest -j <JIT_PATH> -c <CORECLR_RUNTIME_PATH>
```
If failed, back to step 5 for fixing.

Step 7: Check you change does not cause new jitting failure:

```
cd coreclr\tests
llilc_runtest -j <JIT_PATH> -c <CORECLR_RUNTIME_PATH> -d summary -r <DIFF_RESULT_PATH>
llilc_checkpass -b <BASE_RESULT_PATH> -d <DIFF_RESULT_PATH>
```
If failed, back to step 5 for fixing.

Step 8: Check your change does not cause bad LLVM IR change.

```
llilc_runtest -j <JIT_PATH> -c <CORECLR_RUNTIME_PATH> -d verbose -r <DIFF_RESULT_PATH>
```
Use any diff tool to compare \<BASE_RESULT_PATH\> and \<DIFF_RESULT_PATH\>.
If there is bad LLVM IR change, back to step 5 for fixing.

Step 9: Now you are ready for a pull request.

### Lab CI Use Case
Lab CI usage is the most simplified version. It only checks for overall
pass/fail and does not keep any result.
```
cd coreclr\tests
llilc_runtest -j <JIT_PATH> -c <CORECLR_RUNTIME_PATH>
```

### Lab Nightly Use Case
Step 1. Create a summary result with last-known-good JIT as a baseline or use
a previous last-known-good summary result as a baseline
```
cd  coreclr\tests
llilc_runtest -j <LAST_KNOWN_GOOD_JIT_PATH> -c <CORECLR_RUNTIME_PATH> -d summary -c <BASE_RESULT_PATH>
```

Step 2. Create a summary result with latest JIT as diff result
```
cd coreclr\tests
llilc_runtest -j <LASTEST_JIT_PATH> -c <CORECLR_RUNTIME_PATH> -d summary -r <DIFF_RESULT_PATH>
```

Step 3. Check if there is any new jitting failure
```
cd coreclr\tests
llilc_checkpass -b <BASE_RESULT_PATH> -d <DIFF_RESULT_PATH>
```

Step 4. If llilc_checkpass result is negative, there is unexpected error.

Step 5.If llilc_checkpass result is 0, mark latest JIT as last-known-good JIT.
Result \<DIFF_RESULT_PATH\> could be kept as last-known-good summary result.

Step 6.If llilc_checkpass result is 1, send out failure notice to branch owner.
Branch owner should analyze the case and bring relevant developer for fixing.
