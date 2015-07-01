# Background

LLILC is an Open-Source project that Compiles msIL (.NET) code to native
binary, using the LLVM compiler framework.  We pronounce it "lilac". The
project will provide both a JIT ("Just-In-Time") and an AOT ("Ahead-Of-Time")
compiler targeting [CoreCLR](https://github.com/dotnet/coreclr).  

The LLILC LLVM based toolchain is a companion project to the CoreCLR RyuJIT
providing the community with an accessible infrastructure for experimentation
and porting to new targets. Our goal is to make it easy(-ier) to make new
tools or take C# to new platforms.  Our initial supported platform is Windows
but we plan to extend support to Linux and Mac in the near term.  

For more background on the .NET Architecture, see its [ECMA Specification](http://www.ecma-international.org/publications/standards/Ecma-335.htm).
