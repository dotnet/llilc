Welcome to LLILC
================

[![Join the chat at https://gitter.im/dotnet/llilc](https://badges.gitter.im/Join%20Chat.svg)](https://gitter.im/dotnet/llilc?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge)

Build Status
------------

|           |    Windows    |    Linux    |
|-----------|---------------|-------------|
| **Debug** |[![Build status](http://dotnet-ci.cloudapp.net/job/dotnet_llilc/job/master/job/windows_nt_debug/badge/icon)](http://dotnet-ci.cloudapp.net/job/dotnet_llilc/job/master/job/windows_nt_debug/)|[![Build Status](http://dotnet-ci.cloudapp.net/job/dotnet_llilc/job/master/job/ubuntu_debug/badge/icon)](http://dotnet-ci.cloudapp.net/job/dotnet_llilc/job/master/job/ubuntu_debug/)|
|**Release**|[![Build status](http://dotnet-ci.cloudapp.net/job/dotnet_llilc/job/master/job/windows_nt_release/badge/icon)](http://dotnet-ci.cloudapp.net/job/dotnet_llilc/job/master/job/windows_nt_release/)|[![Build Status](http://dotnet-ci.cloudapp.net/job/dotnet_llilc/job/master/job/ubuntu_release/badge/icon)](http://dotnet-ci.cloudapp.net/job/dotnet_llilc/job/master/job/ubuntu_release/)|


Introduction
-------------

LLILC is an **LL**VM based MS**IL** **C**ompiler - we pronounce it 'lilac' -
with a goal of producing a set of cross-platform .NET code generation tools.
Today LLILC is being developed against [dotnet/CoreCLR](https://github.com/dotnet/coreclr)
for use as a JIT, as well as an cross platform platform object emitter and disassembler
that is used by CoreRT as well as other dotnet utilites.  

See the [documentation](Documentation/Welcome.md) for more information.
It has a more complete discussion of our background and goals as well as
"getting started" details and developer information.

ObjectWriter for [CoreRT](https://github.com/dotnet/corert):
CoreRT project uses ObjectWriter that lives in its own branch in this repo,
if you want to build it then follow instructions from getting started, but use the following branches:
1. latest LLVM [version](https://github.com/llvm-mirror/llvm) and apply this [patch](https://reviews.llvm.org/D29483) or take the known working version from [Microsoft/llvm/CoreRT_ObjectWriter branch](https://github.com/dotnet/llilc/tree/ObjectWriter);
2. [llilc/ObjectWriter branch](https://github.com/dotnet/llilc/tree/ObjectWriter);

libcoredistools: CoreCLR has a ongoing dependency on libcoredistools which is built out of this repo and placed into build/lib/libcoredistools.dylib|so|dll. To build coredistools follow the default workflow for building llilc/llvm on the master branch.


Supported Platforms
-------------------

Our initial supported platform is [Windows](Documentation/Getting-Started-For-Windows.md),
but [Linux and Mac OS X](Documentation/Getting-Started-For-Linux-and-OS-X.md)
support are under development.

Contributions
-------------

Please see our [issues](https://github.com/dotnet/llilc/issues)
or the [contributing document](Documentation/Areas-To-Contribute.md)
for how to pitch in.
