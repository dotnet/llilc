# LLILC FAQ

+ Q: What is LLILC?  
  A: LLILC is an MSIL code generator based on LLVM.  Today LLILC can produce a JIT
  compiler that can be used with [CoreCLR](https://github.com/dotnet/coreclr), but our
  over-arching aim is to allow easier porting of managed languages to any platform
  while supporting a spectrum of compilation strategies.

+ Q: What targets will LLILC generate code for?  
  A: Initially x64.  In future: any target that LLVM supports is
  possible (x86,  ARM-64, ...)

+ Q: What platforms will LLILC target?  
  A: LLILC targets the same platforms as CoreCLR: Linux, MAC OS X and Windows

+ Q: How does the LLILC JIT compiler relate to the CoreCLR RyuJIT compiler?  
  A: Their aims are different: RyuJIT is a high-performance,
  custom JIT, in that it implements its own optimizatons
  and codegen, specific to C#.  It matches the high throughput required by
  the CoreCLR runtime.  LLILC, by contrast, uses the LLVM framework,
  which provides industrial-strength optimizations, leveraged by
  the Clang C++ compiler.  It thereby offers a familiar environment
  within which developers can implement low-level, ‘backend’ tools,
  such as code analyzers, obfuscators, verifiers, etc.

+ Q: How do I get started?  
  A: See the [Welcome Document](Welcome.md)

+ Q: Are there any demos?  
  A: Not as yet.  We need to extend the JIT to handle more
  tests first.

+ Q: How does LLILC differ from Roslyn in the features it provides
  for building software tools?  
  A: Roslyn provides *frontend* support, whilst LLILC
  (via LLVM) provides *backend* support:

  + Roslyn exposes the data structures of a *frontend* – so it’s ideal
  for building tools such as IDEs, with syntax coloring, warning
  squiggles, use/definition lookup, refactoring, etc; or
  translators that convert C# to some other language; or
  pretty-print formatters; or analyzers that check conformance
  with coding guidelines (think FxCop).  

  + LLILC, via LLVM, exposes the data structures of a *backend* – so
  it’s ideal for building tools that inject extra code (eg: performance
  profiler, code tracer, debugger, parallelism analyzer, race
  detector); or super-optimizing code (eg: auto-vectorizer, auto-parallelizer);
  or test-case reduction.

+ Q: When will the LLILC AOT (Ahead-Of-Time) compiler be ready?  
  A: At the moment, CoreCLR does not include an AOT.  We envisage
  that the LLILC AOT will work in two kinds of environment – those
  that allow runtime codegen, and those where the entire code is converted to native binary.

+ Q: How does LLILC relate to the .NET Native work?  
  A: .NET Native provides a broad tool-chain, targeting Windows.  The LLILC AOT could
  be used as the compiler component of .NET Native to target other platforms.
