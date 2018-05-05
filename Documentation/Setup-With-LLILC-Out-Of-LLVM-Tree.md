# Setup With LLILC Out-Of LLVM Tree

The [Getting Started for Windows doc](Getting-Started-For-Windows.md) tells
how to configure LLILC when the LLILC repository is placed in the LLVM/tools
subdirectory. This page tells how to configure LLVM and LLILC when the LLILC
workspace is separate from the LLVM workspace. This might be desirable, for
example, if you want to have multiple LLILC workspaces (for working on
multiple branches simultaneously) but want to share a single LLVM workspace
and LLVM build.

We document which steps are the same and which steps that are different
from the [Getting Started for Windows document](Getting-Started-For-Windows.md).

Steps that are the same are:

* Install Git
* Clone and build CoreCLR:
* Clone the Microsoft fork of LLVM to your PC
* Install Python
* Install CMake
* Install Visual Studio 2013
* Install [Doxygen](http://www.stack.nl/~dimitri/doxygen/)
* Install [graphviz](http://graphviz.org/)

In what follows, let

* $coreclr-repo-dir be the directory where the the coreclr was cloned.
* $coreclr-build-dir be $coreclr-repo-dir\bin\Product\$platform.$arch.$build> directory.
* $llvm-repo-dir be the directory where LLVM was cloned.
* $llvm-build-dir be the directory where LLVM was built.
* $llilc-repo-dir be the directory where LLILC was cloned
* $llilc-build-dir be the directory where LLILC is built

$coreclr-build-dir is fixed relative to $coreclr-repo-dir, but all of the others can be
placed wherever you like. Below we use paths like $llilc-repo-dir/.. to denote the
parent directory of $llilc-repo-dir. When we do a `cd` to a directory we assume you
have previously created the directory. To simplify the description we also assume you
will be enabling Doxygen.

Steps that are different:

* Clone LLILC to your PC
  * Unlike the [Getting Started for Windows](Getting-Started-For-Windows.md)
    directions, create a separate LLILC directory.:

    ```
    > cd $llilc-repo-dir/..
    > git clone https://github.com/dotnet/llilc
    ```

  * This will create a directory tree under $llilc-repo-dir that contains
    the cloned sources.

* Create a Visual Studio Solution for LLVM
  * Create a directory to hold the LLVM build output:

    ```
    > mkdir $llvm-build-dir
    ```

  * Run cmake from within the newly-created `llvm-build` directory with the
    Visual Studio backend to generate the LLVM solution:

    ```
    > cd $llvm-build-dir
    > cmake -G "Visual Studio 12 2013 Win64" $llvm-repo-dir -DLLVM_OPTIMIZED_TABLEGEN=ON -DLLVM_TARGETS_TO_BUILD:STRING=X86 -DLLVM_ENABLE_DOXYGEN=ON
    ```

  * This will generate `LLVM.sln` inside the $llvm-build-dir directory.

* Build LLVM from the command line
  * Change directories to the LLVM build directory and set up environment
    variables for the Visual Studio toolchain:

    ```
    > cd $llvm-build-dir
    > "%VS120COMNTOOLS%\..\..\VC\vcvarsall.bat" x64
    ```

  * Start the build. Note that If you have a multi-core machine, you may
    request a faster, parallel build by adding the `/m` flag to the command
    line, optionally specifying the degree of parallelism (e.g. `/m:4`).

    ```
    > msbuild LLVM.sln /p:Configuration=Debug /p:Platform=x64 /t:ALL_BUILD
    ```

* Create a Visual Studio Solution for LLILC
  * Create a directory to hold the LLILC build output:

    ```
    > mkdir $llilc-build-dir
    ```

  * Run cmake from within the newly-created `llilc-build` directory with the
    Visual Studio backend to generate the LLVM solution:

    ```
    > cd $llilc-build-dir
    > cmake -G "Visual Studio 12 2013 Win64" $llilc-repo-dir -DWITH_CORECLR=$coreclr-build-dir -DWITH_LLVM=$llvm-build-dir -DLLVM_OPTIMIZED_TABLEGEN=ON -DLLVM_TARGETS_TO_BUILD:STRING=X86 -DLLVM_ENABLE_DOXYGEN=ON
    ```

  * This will generate `LLILC.sln` inside the $llilc-build-dir directory.
  * Currently there is a bug in the LLVM configuration that leaves some file
    paths in Windows format with back slashes. If you get an error message
    when configuring, like
    ```
    Syntax error in cmake code at

        share/llvm/cmake/LLVMExports.cmake:233

    when parsing string

        ...C:\Program Files (x86)\...

    Invalid escape sequence \P
    ```
    you can work around it by editing the offending line by
    changing the back slashes to forward slashes.

* Build LLILC from the command line
  * Change directories to the LLVM build directory and set up environment
    variables for the Visual Studio toolchain:

    ```
    > cd $llilc-build-dir
    > "%VS120COMNTOOLS%\..\..\VC\vcvarsall.bat" x64
    ```

  * Start the build. Note that If you have a multi-core machine, you may
    request a faster, parallel build by adding the `/m` flag to the command
    line, optionally specifying the degree of parallelism (e.g. `/m:4`).

    ```
    > msbuild LLILC.sln /p:Configuration=Debug /p:Platform=x64 /t:ALL_BUILD
    ```

  * To run doxgen over the LLILC sources use the following command line:

    ```
    > msbuild LLVM.sln /p:Configuration=Debug /p:Platform=x64 /t:doxygen-llilc
    ```
