# LLILC at Six Months - a Retrospective

October 2015

Back in April we announced LLILC,
an open-source [CoreCLR](https://github.com/dotnet/coreclr) compatible JIT
based on [LLVM](http://llvm.org). Since then we’ve been
hard at work on LLILC, and most of the core code generation is implemented.
LLILC is able to pass thousands of test cases on Windows and Linux, can pre-jit
(NGEN) anything it can jit, and can successfully jit the
[Roslyn](https://github.com/dotnet/roslyn) compiler compiling itself.

Our experiences with LLVM both as a code base and a community
continue to impress and inspire us. Work continues on both EH and GC, and
while we’ve made great progress on both fronts, and benefited from valuable
cross-industry collaboration, there is much still to do. The CoreCLR’s
requirements continue to push both us and LLVM into interesting new directions.
This document touches on our progress to date, and the challenges that remain.

## Background

The [CoreCLR](https://github.com/dotnet/coreclr) is an open-source,
cross-platform implementation of the 
[.Net Runtime](http://www.ecma-international.org/publications/standards/Ecma-335.htm).
It includes a VM, JIT, and the core class library (mscorlib).

[LLILC](https://github.com/dotnet/llilc) is an [LLVM](http://llvm.org)-based
code generator for the CoreCLR. We initially began bringing up LLILC as a JIT
(just in time) compiler for the CoreCLR, with the intention of leveraging the
JIT core for ahead of time (AOT) compilation as well. LLILC offers us the
potential for high performance, for rapid retargeting, and for ready
experimentation with code generation.

![JIT Architecture](./Images/JITArch.png)

In the CoreCLR the JIT is isolated from the core runtime and type
system by a bidirectional "JIT/EE" interface. Among other things, this
interface enables the CoreCLR to support an *alt-jit* mechanism where
two JITs are loaded into the runtime, and if the first JIT is unable
to compile a method, the second JIT takes over.  We used this
configuration to bring up LLILC in stages, bailing out whenever LLILC
encountered an unimplemented feature or hit an error, so that the
CoreCLR’s regular JIT (RyuJit) could process the method and allow the
app to continue running and requesting more methods to jit. This setup
allowed us to run tests end-to-end with a partially functional JIT. As
of the time LLILC was first announced, the LLILC JIT was bailing out
on a handful of the 370 methods called when running a simple *Hello,
world!* program.

## Current Status

We’re now at the point where LLILC can jit all the methods in some fairly
complex scenarios. [Roslyn](https://github.com/dotnet/roslyn) is a C# compiler
written entirely in C#, and LLILC can jit Roslyn compiling itself. We’ve
broadened our implementation to support some partial AOT (ahead-of-time)
scenarios known as NGEN and Ready2Run. NGEN enables pre-jitting most methods
and IL Stubs as well as a selection of generic instantiations. We can NGEN
all of Roslyn and use this version to compile Roslyn. We’ve also ported a
couple of thousand basic jit tests into the CoreCLR and LLILC is able to pass
the majority of these tests in both JIT and NGEN modes. LLILC can also run
cross-platform on Linux and OSX. LLILC has begun supporting some advanced
features like vectorization. Major work remains for full garbage collection
(GC) and exception handling (EH) support, and for good code size and code
quality (CS & CQ) and respectable jit throughput (TP). We’ll discuss all these
in more detail in the sections that follow.

## Leveraging LLVM

We have a copy of LLVM in a [public github repo](https://github.com/Microsoft/llvm)
and maintain our own branch where we stage provisional changes to LLVM. We’ve
been quite good at keeping up with the mainline development in LLVM and are
happy to report we’re rarely broken by ongoing development. Our intention has
always been to upstream as quickly as possible. We’re still working on making
good at that intention, but so far the number of changes we’ve had to make are
fairly modest.

To the best of our ability we’ve tried hard to leverage LLVM features in LLILC
wherever possible. We’ve learned a lot, are getting a much better feel for how
LLVM works, and have even made some modest contributions back into LLVM itself.
Things have generally gone well but we sometimes find ourselves needing to
model things in unexpected ways. 

The next couple of sections highlight some examples.

### Modelling CLR Types as LLVM Types

The CoreCLR has a rich type system, but a lot of this richness is hidden from
the JIT. LLVM’s type system is intentionally kept simple. Nonetheless we
decided early on to try and have a fairly expressive representation of CoreCLR
types as LLVM types, for a couple of reasons: first, we wanted to be able to
re-derive GC info from LLVM types when generating our stack maps; second,
because doing so gives (at least high-level) LLVM IR a much richer semantic
context - instance, we can use GEPs instead of flattening address math - and
lastly, because we anticipate one day we may desire to run type-based
optimizations on LLVM IR.

We’ve hit various problems along the way, none of them reaching the level of
really blocking us:

* LLVM types are created on demand; it took us a few iterations to get the
right algorithm in place to handle complex type graphs.
* Inheritance is not described in the LLVM types; instead each derived class
incorporates a duplicate set of fields from its base classes. We found one case
where the CoreCLR hides some information about types, and worked around it.
* Unions are generally handled via byte arrays, with the exception of unions
that contain GC references. These references can’t overlap non-GC reference
fields, so we split them out separately. 
* The CoreCLR has semantically distinct types with equivalent representations.
On a 64 bit system, `native int` and `long` are identical, yet get treated 
differently in places. We haven’t found a good way to maintain this distinction.
* We have natural names for types, which we use liberally, but ran into cases
where we wanted named types to use structural equivalence. For instance, a
generic value class (struct) with a GC-pointer field might be described in some
places using the actual field type, and in others a reference to the generic
sharing placeholder type `System.__Canon`. These disparate descriptions of
struct types might converge at some join point. We ended up using
pointer-to-struct types to describe these cases, because we moved away from
having first-class aggregates (see below) and so now can use pointer casts at
the joins to pave over the differences.

The table below shows some examples of C# types and the LLVM types we produce
to model them on a 64 bit host. Note the use of `addrspace(1)` pointers for
GC references. For *ref classes*, the initial `[0 x i64*]*` field describes the
vtable -- an unknown-sized table of pointers.

| C# Syntax for Type |	LLVM Type           |
|--------------------|----------------------|
| `object`           |	`<{ [0 x i64*]* }>` |
| `string`           |  `<{ [0 x i64*]*, i32, [0 x i16] }>` |
| `string[]`         |  `<{ [0 x i64*]*, i32, [4 x i8],  [0 x %System.String addrspace(1)*] }>` |
| `A`                | `<{ %System.String addrspace(1)*, i32, [4 x i8] }>`|
| `B`                | `<{ %System.Object addrspace(1)*, [8 x i8] }>` |

where `A` is defined as

```C#
[StructLayout(LayoutKind.Explicit)]
public struct A
{
    [FieldOffset(0)]public string Name;
    [FieldOffset(8)]public int Size;
}	
```

and `B` is defined as

```C#
[StructLayout(LayoutKind.Explicit)]
public struct C
{
    [FieldOffset(0)]public A X;
    [FieldOffset(0)]public string Name;
    [FieldOffset(8)]public int Size;
}	
```

Also note the values in LLVM IR produced by LLILC are actually typed as pointers
to the above types, both for *ref classes* like `object`, since they always
live on the GC Heap, and for *value classes* like `A`, because in our
translation they always live in memory to avoid having first-class aggregate
values in LLVM IR.

### First-Class Aggregates

In our initial bring-up we used first-class aggregates to represent structures.
We’d used this approach successfully in prior compilers, and the official CLR
spec uses first-class aggregates in describing MSIL semantics, but it soon
became clear we’d be better off in LLVM keeping these structures in memory
and referring to them via pointers. This required some rework and some
bookkeeping to remember which pointers were really by value structures.
It wasn’t that bad because we already had to demote structures into memory 
at call boundaries.

### Explicit Exception Checks

Many MSIL opcodes can cause semantically meaningful exceptions (some of them
more than one kind). We were accustomed to IR that could support operation and 
operand exceptions -- for instance loads and stores via null pointers could raise
`NullReference` exceptions -- with the actual exception check being implicit in
the IR. LLVM IR, however, has an exception model that matches up better with
C++, where exceptions can only happen at calls. While we (and others) in the
LLVM community have had discussions about enabling somewhat more general models
in LLVM IR, for now we make all our exception checks explicit in the IR.

The current LLVM direction seems like a reasonably good compromise for an AOT 
compiler: explicit checks in HIR with the ability to fold them into implicit
checks at the machine level. For a JIT, this is something we’ll potentially
revisit down the road, though the issue of which model is preferable is far
from clear, with tradeoffs on a number of fronts. 

Implicit checks may not be efficient on all architectures. In high-level IR,
the ability for a wide variety of operations to raise exceptions imposes a
semantic burden on IR maintenance and optimization. For instance, hoisting a
load that can throw now has control flow implications.

Explicit checks give the IR a nice "separation of concerns" aspect: a load or
store won’t have complex behaviors. The check IR has the same shape and
semantic as other compare and branch operations and so is more naturally
amenable to common optimizations. But there is extra IR volume created via the
explicit checks, and this impacts the throughput and code size of a JIT. For
example, when compiling `Buffer.Memmove`, LLILC ends up generating 85 explicit
null checks. LLILC places the throw blocks out of line, and so the tail end of
the method looks like this:

```LLVM
ThrowNullRef:                                     ; preds = %20
  call void bitcast (i64* @"AnyJITHelper::JitHelper" to void ()*)() #1, !dbg !22
  unreachable, !dbg !22

ThrowNullRef2:                                    ; preds = %23
  call void bitcast (i64* @"AnyJITHelper::JitHelper" to void ()*)() #1
  unreachable

ThrowNullRef4:                                    ; preds = %28
  call void bitcast (i64* @"AnyJITHelper::JitHelper" to void ()*)() #1, !dbg !23
  unreachable, !dbg !23

; ... (82 more)...
```

Despite having identical instruction sequences, these blocks can’t be
tail merged because they carry unique debugging information, and this
information is crucial to preserve, since it allows users to diagnose
which null check actually failed. To be fair, many of these checks can
be optimized away, but that puts us in a bit of a pickle - if we
generate naive IR, throughput may be poor (and code size large)
because of the high IR volume, and if we run optimization phases to try
and reduce the volume of IR (and reduce code size), throughput may be
poor because of time needed to run the optimizations.

### Custom Calling Conventions

The CoreCLR uses non-standard calling conventions in places. For example, on
Windows x64, a call made via virtual stub dispatch must pass an extra argument
in R11. We found this quite straightforward to model in LLVM:

```
def CC_X86_64_CLR_VirtualDispatchStub : CallingConv<[
  // The first pointer-sized argument is passed in R11
  CCIfType<[i64], CCAssignToReg<[R11]>>,
  // Otherwise, drop to normal X86-64 CC
  CCDelegateTo<CC_X86_64_C>
]>;

CCIfCC<"CallingConv::CLR_VirtualDispatchStub",
         CCDelegateTo<CC_X86_64_CLR_VirtualDispatchStub>>,
```

### Platform Invoke (aka PInvoke)

When managed code invokes native code, the JIT must generate extra information
on the stack frame of the caller, and manipulate this information at each call
site that invokes native code. Among other things this state helps the VM
determine when control has transitioned outside the world of the VM. 

These calls are also typically GC safepoints, so we have added additional
arguments to the statepoints to encapsulate the extra state that must be
manipulated, and added custom expansions for this state when present.

### Inline Stack Probes (aka `__chkstk`)

On Windows, the OS requires that a thread’s stack segment grow in a controlled
fashion. This is accomplished by having the compiler carefully probe the stack
page by page whenever a large enough stack adjustment is needed (either at
function entry or via some alloca). There’s a function called `__chkstk` in the
CRT that is used for this purpose. Unfortunately, the CRT routine is not
generally available in the CoreCLR runtime environment, so traditionally the
JITs have implemented the stack probing via inline expansions.

Getting this to work was a bit of a challenge, since the in-method-body
expansions from alloca happen before allocation, and the prolog expansion
happens after allocation. It still not 100% clear to us what the annotation
requirements are in machine code.

### NGEN

NGEN is a way for CoreCLR to pre-JIT methods and cache the code. We’ve based
LLILC on the MCJIT, using the ORC JIT api layer. This framework assumes we 
intend to actually run the code. But in the NGEN case we don’t run the code and
don’t actually need to process the relocations - instead they’re simply
recorded and acted on when the NGEN image is loaded. Getting this to work
required a bit of special plumbing in the ORC JIT layers.

### Vectorization

The .Net Framework now has a vector math package and the new RyuJIT supports
[vectorization](http://blogs.msdn.com/b/dotnet/archive/2014/04/07/the-jit-finally-proposed-jit-and-simd-are-getting-married.aspx).
We had our summer intern Sergey Andreenko implement the
vectorization in LLILC and are happy to report it was quite straightforward and
simple. This is done by recognizing special vector types and operators in the
MSIL and translating them into vector LLVM IR.

For example, when jitting code like

```C#
  using Point = System.Numerics.Vector2;

  Point a = new Point(10, 50);
  Point b = new Point(10, 10);
  Point c = a * b;
```

LLILC produces the following LLVM IR:

```LLVM
  %1 = addrspacecast <2 x float>* %loc0 to <2 x float> addrspace(1)*, !dbg !12
  store <2 x float> <float 1.000000e+01, float 5.000000e+01>, 
        <2 x float> addrspace(1)* %1, !dbg !18
  %2 = addrspacecast <2 x float>* %loc1 to <2 x float> addrspace(1)*, !dbg !19
  store <2 x float> <float 1.000000e+01, float 1.000000e+01>, 
        <2 x float> addrspace(1)* %2, !dbg !20
  %3 = load <2 x float>, <2 x float>* %loc0, align 8, !dbg !21
  %4 = load <2 x float>, <2 x float>* %loc1, align 8, !dbg !21
  %5 = fmul <2 x float> %3, %4, !dbg !22
  store <2 x float> %5, <2 x float>* %loc2, !dbg !22
  %6 = addrspacecast <2 x float>* %loc2 to <2 x float> addrspace(1)*, !dbg !23
```

and in turn this produces the following assembly code:

```Assembly
  mov         rax,7F909DA0510h  
  mov         rax,qword ptr [rax]  
  mov         qword ptr [rsp+38h],rax   
  mov         rax,7F909DA0520h  
  movaps      xmm0,xmmword ptr [rax]      
  movlps      qword ptr [rsp+30h],xmm0  
  movq        xmm1,mmword ptr [rsp+38h]  
  mulps       xmm1,xmm0  
  movlps      qword ptr [rsp+28h],xmm1
```

## Extending LLVM: GC

In the [GC document](llilc-gc.md) we gave an overview of the challenges we face in
supporting the CoreCLR’s GC. Much of our work to date has been facilitated by
enabling a conservative GC mode in the CoreCLR while we wait for our precise GC
implementation to mature. 

The CoreCLR’s GC is a precise, relocating collector. This means that at places
where GC can occur (safepoints), all heap references must be accurately
described to the GC, and all of them may change values at a GC.

For precise GC, LLILC has built upon the statepoint representation for GC
safepoints. Safepoints are modelled as calls, and the possible modifications to
GC references are made explicit in the IR as extra arguments to (and return 
values from) the statepoint wrapper for the calls. This information is later
used to explicitly spill these references to memory, and then report those
locations via LLVM-created per-safepoint stackmaps. 

We've extended the statepoint work to handle a couple of specific cases
introduced by CoreCLR's *managed pointers*: the liveness algorithm must see
through integer-pointer conversions, and the reporting can screen out
pointers known not to point into the GC heap (like addresses of locals).

LLILC parses the LLVM stackmaps to produce the GC Info format that the
CoreCLR expects. The CLR format required some new data we couldn’t get
from the original stackmaps, so we worked along with the community to
create V2 enhanced stackmaps.  So far, this has worked well.

The CoreCLR’s GC mode (conservative or precise) is set when the
runtime initializes. By leveraging the alt-jit mechanism we can enable
precise GC and run tests end-to-end, allowing LLILC to bail out of
methods where it can’t generate correct GC info. We’re now able to jit
the majority of methods using LLILC with precise GC enabled in this
fashion.


However, there are a number of key problems left to tackle:

* We are bailing out when methods have GC references that aren’t SSA
values (for instance GC references from structs on the stack
frame). The plan here is to report the GC references in the struct as
an untracked lifetimes, using the LLVM type to uncover the locations
of the GC references within the struct.
* We have been looking at how to handle exterior pointers (pointers that
logically refer to some heap object, but point either before or after the
object). It seems possible we can devise new GC Info encodings and a bit of GC
fixup to support these.
* We are placing statepoints in the IR fairly early on with the intent of
locking down semantics before any optimizations might run. To be fair, we
haven’t enabled optimizations yet, so we don’t actually know if we’ll hit
problems when we do enable optimizations, and we’ll probably want to move the
statepoint placement later since the explicit statepoints may inhibit useful
and legal optimizations.
* We have some serious concerns about throughput. Running in precise mode with
safepoint insertion makes LLILC notably slower than running in conservative
mode, and LLILC is probably already too slow even in conservative mode. There
can be hundreds or thousands of live GC references at a safepoint. Making these
explicit in the IR is costly. Abstractly, GC reporting is a liveness issue, and
we’d like to pay costs for it accordingly. Encoding a liveness problem in SSA
as we do with statepoints is a questionable tradeoff: SSA works best on sparse
problems, and GC liveness at statepoints is a potentially dense problem. To be
fair, we haven’t looked into the causes of the slower throughput or looked at
how it might be mitigated, but fundamentally we think we may need a different
approach if LLILC’s throughput is ever to be competitive with the current
CoreCLR JIT.

## Extending LLVM: EH

The CoreCLR also has a complex exception handling model. There are a [variety of
mechanisms](llilc-jit-eh.md) LLILC must support for proper exception handling,
cleanup, and propagation.

Operations in MSIL can cause exceptions. As noted above we’re currently
modelling this by doing explicit checks in LLVM IR. Exceptions are raised by
calling throw helper methods provided by the runtime. Programs may guard
against exceptions by using try regions with associated filter, catch, finally,
and fault blocks. LLILC must produce code for these regions and blocks,
properly model the runtime interactions during exception processing in the IR,
and create descriptive side tables so that the runtime is able to unwind the
stack and process exceptions properly as each stack frame is unwound.

As of this writing we’re currently collaborating on enhancing the LLVM IR along
with LLVM developers working on implementing support for MSVC-compatible C++ EH
and Structured Exception Handling (SEH) in Clang and LLVM. SEH and CoreCLR EH
are similar and both somewhat more involved than C++ EH.  This work is going
well and should provide us the flexibility we need to recover the EH region
structure from the IR - a necessary step in creating the proper tables. 

Our implementation also requires that some of the EH region blocks be
implemented as separate small functions (aka funclets) that can be invoked by
the runtime. Funclets and their parent functions have some complex
co-dependence that LLILC will need to handle. For instance, the runtime does
not support having GC references in funclets' stack frames, so if a funclet
needs to spill a GC reference it must reserve and use a stack slot in the
parent function for this purpose.

## Future Challenge: Throughput

Our ambition is to make LLILC a high-performance JIT - by which we mean, LLILC
can quickly produce decent quality code. We’re aware that the common wisdom is
that LLVM may not be lean enough to make this possible, but our intention is to
see how far we can push things. Our current measurements show LLILC to be
approximately 4x slower than our production RyuJit when LLILC is in
conservative GC mode, and even slower than this when we enable precise GC.
We haven’t spent much time looking at this yet, and we don’t have much
experience in figuring out how to make LLVM run faster, so we have a lot to
learn here.

As a fallback we may decide we can only use LLILC either in ahead-of-time (AOT)
or tiered JIT scenarios.

## Future Challenge: Code Quality and Code Size

The flip side of throughput is that the code the jit produces must be of
reasonable quality. For a first-tier JIT, optimizations must be carefully
considered, since time is of the essence. We currently haven’t tried enabling
much of LLVM’s optimization capabilities and don’t yet know what the time vs
size and quality tradeoffs will look like. As an initial data point, LLILC
code is about 2x larger than RyuJit’s code. We should be able to be much more
competitive with some basic cleanup opts.

We’re also aware that managed code like MSIL will produce some unique
challenges for optimization. We’ve already noted the need to focus on exception
check elimination: many checks are redundant or can be removed with suitable
range-aware optimizations. Eliminating these checks is crucial for getting good
performance. It very much remains to be seen how effective LLVM’s current
optimizations will be at cleaning things up here.

## Summary

LLILC has made great strides in the past six months, but there are big
challenges ahead in our quest to make it a first-class code generator for the
CoreCLR. 

LLVM - both code and community - has been a pleasure to work with so far, and we
look forward to ongoing fruitful collaboration.

