# Getting Started for Windows

Note: all commands are typed from a Windows Command Prompt, and `c:\dotnet`
may be replaced in all commands below with the directory of your choice.

* Install Git
  * Follow the instructions at [git-scm downloads](http://git-scm.com/downloads).
    All of the installer's defaults are acceptable.

* Install CMake
  * Follow the instructions at [CMake Install](http://www.cmake.org/install/)
  * Make sure to select "Add CMake to the system PATH for all users" when
    running the installer (all other defaults are acceptable).

* Clone and build CoreCLR:
    ```
    $ git clone https://github.com/dotnet/coreclr
    $ cd coreclr
    $ .\build.cmd
    $ cd ..
    ```
    After it completes, the build will indicate where the CoreCLR binaries
    are available. Make a note of this location
    (typically bin/Product/Windows_NT.x64.debug).

* Clone the Microsoft fork of LLVM to your PC
  * Change directories to where you want to place your clone and run `git`
    to clone the Microsoft fork of LLVM:
        ```
        > cd c:\dotnet
        > git clone -b MS https://github.com/microsoft/llvm
        ```

  * This will create a directory tree under `llvm` that contains the cloned
    sources.

* Clone LLILC to your PC
  * Because LLILC is structured as an LLVM tool, it is canonically cloned
    into the `tools` subdirectory of the LLVM tree created above:

    ```
    > cd c:\dotnet\llvm\tools
    > git clone https://github.com/dotnet/llilc
    ```

  * This will create a directory tree under `llilc` that contains the cloned
    sources.

* Install Python
  * Follow the instructions at [Python Downloads](https://www.python.org/downloads/)
  * Choose Python 2.7.9 for Windows

* Install Visual Studio 2013
  * Follow the instructions at [Visual Studio](http://www.visualstudio.com/en-us/products/visual-studio-community-vs)
    to install the free Community edition of Visual Studio 2013.
  * Update the product to [Visual Studio Update 4](http://www.microsoft.com/en-us/download/details.aspx?id=39305)

* If you want to run [Doxygen](http://www.stack.nl/~dimitri/doxygen/) to
  produce documentation from your code comments, then in addition do the following:
  * Install [Doxygen](http://www.stack.nl/~dimitri/doxygen/) using the
    instructions on its web site. The LLVM web site is using Doxygen 1.7.6.1
    however the 1.8 series added support for Markdown formatting. We would like
    to use Markdown in our comments, so use the latest version of Doxygen.
  * Install [graphviz](http://graphviz.org/) using instructions on their
    site. The current version no longer modifies your path, so you should
    manually modify your path so that it includes "dot".
* Create a Visual Studio Solution for LLVM + LLILC
  * Create a directory to hold the LLVM build output:

    ```
    > cd c:\dotnet
    > mkdir llvm-build
    ```

  * Run cmake from within the newly-created `llvm-build` directory with the
    Visual Studio backend to generate the LLVM solution:

    ```
    > cd llvm-build
    > cmake -G "Visual Studio 12 2013 Win64" ..\llvm -DWITH_CORECLR=<coreclr path>\bin\Product\$platform.$arch.$build> -DLLVM_OPTIMIZED_TABLEGEN=ON
    ```
    note: for Windows $platform should resolve to 'Windows_NT'

  * However if you also want to run Doxygen, use the following instead:

    ```
    > cd llvm-build
    > cmake -G "Visual Studio 12 2013 Win64" ..\llvm -DLLVM_ENABLE_DOXYGEN=ON -DWITH_CORECLR=<coreclr path>\bin\Product\$platform.$arch.$build> -DLLVM_OPTIMIZED_TABLEGEN=ON
    ```

  * This will generate `LLVM.sln` inside the `llvm-build` directory.

* Build LLVM + LLILC from the command line
  * Change directories to the LLVM build directory and set up environment
    variables for the Visual Studio toolchain:

    ```
    > cd c:\dotnet\llvm-build
    > "%VS120COMNTOOLS%\..\..\VC\vcvarsall.bat" x64
    ```

  * Start the build. Note that If you have a multi-core machine, you may
    request a faster, parallel build by adding the `/m` flag to the command
    line, optionally specifying the degree of parallelism (e.g. `/m:4`).

    ```
    > msbuild LLVM.sln /p:Configuration=Debug /p:Platform=x64 /t:ALL_BUILD
    ```

  * To run doxygen over the LLILC sources use the following command line:

    ```
    > msbuild LLVM.sln /p:Configuration=Debug /p:Platform=x64 /t:doxygen-llilc
    ```

The build steps above can also be done from Visual Studio by going to the
solution explorer, right clicking the desired project (e.g. ALL_BUILD or
  doxygen-llilc) and selecting "build".
