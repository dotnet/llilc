Welcome to LLILC
================

Build Status
------------

|           |    Windows    |    Linux    |
|-----------|---------------|-------------|
| **Debug** |[![Build status](http://dotnet-ci.cloudapp.net/job/dotnet_llilc_windows_debug_win64/badge/icon)](http://dotnet-ci.cloudapp.net/job/dotnet_llilc_windows_debug_win64/)|[![Build Status](http://dotnet-ci.cloudapp.net/job/dotnet_llilc_linux_debug/badge/icon)](http://dotnet-ci.cloudapp.net/job/dotnet_llilc_linux_debug/)|
|**Release**|[![Build status](http://dotnet-ci.cloudapp.net/job/dotnet_llilc_windows_release_win64/badge/icon)](http://dotnet-ci.cloudapp.net/job/dotnet_llilc_windows_release_win64/)|[![Build Status](http://dotnet-ci.cloudapp.net/job/dotnet_llilc_linux_release/badge/icon)](http://dotnet-ci.cloudapp.net/job/dotnet_llilc_linux_release/)|


Introduction
-------------

LLILC is an **LL**VM based MS**IL** **C**ompiler - we pronounce it 'lilac' -
with a goal of producing a set of cross-platform .NET code generation tools.
Today LLILC is being developed against [dotnet/CoreCLR](https://github.com/dotnet/coreclr)
for use as a JIT, but an ahead of time (AOT) compiler is planned for the future.  

See the [wiki](https://github.com/dotnet/llilc/wiki) for more information.
It has a more complete discussion of our background and goals as well as
"getting started" details and developer information.


Supported Platforms
-------------------

Our initial supported platform is [Windows](https://github.com/dotnet/llilc/wiki/Getting-Started-For-Windows),
but [Linux and Mac OSX](https://github.com/dotnet/llilc/wiki/Getting-Started-For-Linux-and-OS-X)
support are under development.


Contributions
-------------

LLILC is just starting up.  Only a few tests are working and there are lots
of places where we need help.  Please see our [issues](https://github.com/dotnet/llilc/issues)
or the [contributing page](https://github.com/dotnet/llilc/wiki/Contributing)
for how to pitch in.
