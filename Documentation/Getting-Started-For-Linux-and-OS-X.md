# Getting Started for Linux and OS X

## Caveats

Support for LLILC on Linux and OS X is still in its early stages. While the
instructions below should get you up and running, the workflow is a bit rough
around the edges and there are certainly holes with respect to what is
supported. The most obvious missing piece is that without following the steps
on the [CoreCLR wiki](https://github.com/dotnet/coreclr/wiki/Building-and-Running-CoreCLR-on-Linux)
to obtain a version of mscorlib that is functional on these platforms, there
is no way to run or test the output of the LLILC build.

## Prerequisites

The prerequisites for building LLILC on Linux and OS X are the same as the
[prerequisites for building LLVM](http://llvm.org/docs/GettingStarted.html#software)
with CMake unioned with the prerequisites for building [CoreCLR](https://github.com/dotnet/coreclr).

* [GCC](http://gcc.gnu.org) >= 4.7.0
* [Clang](http://clang.llvm.org/) >= 3.5.0
* [Python](http://python.org) >= 2.7
* [CMake](http://cmake.org) >= 2.8.8
* [libunwind](http://www.nongnu.org/libunwind/) >= 1.1

In addition, LLILC requires very recent builds of the [Microsoft fork of LLVM](https://github.com/microsoft/llvm)
and [CoreCLR](https://github.com/dotnet/coreclr). Instructions for fetching
and building both can be found below.

## Getting and building the code

1. Clone and build CoreCLR:

    ```
    $ git clone https://github.com/dotnet/coreclr
    $ cd coreclr
    $ ./build.sh
    $ cd ..
    ```

    After it completes, the build will indicate where the CoreCLR binaries
    are available. Make a note of this location
    (typically binaries/Product/<os>.x64.debug).

2. Clone the Microsoft fork of LLVM:

    ```
    $ git clone -b MS https://github.com/microsoft/llvm
    ```

3. Clone LLILC:

    ```
    $ cd llvm/tools
    $ git clone https://github.com/dotnet/llilc
    $ cd ..
    ```

4. Configure LLVM and LLILC:

    ```
    $ mkdir build
    $ cd build
    $ cmake -DWITH_CORECLR=../../coreclr/path/to/CoreCLR/binaries -DLLVM_OPTIMIZED_TABLEGEN=ON ..
    ```

    Ie, ../../coreclr/bin/Product/OSX.x64.Debug

5. Build LLVM and LLILC:

    ```
    $ make
    ```

    If all goes well, the build will complete without errors.
