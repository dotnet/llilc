# GC Support in LLILC

This page describes the functional requirements for supporting the
[CoreCLR](https://github.com/dotnet/coreclr) GC in the LLILC codebase,
the GC support currently offered by [LLVM](http://llvm.org), the
problems that arise mapping the CoreCLR requirements onto what LLVM supports,
and suggested plans for building out functionality in LLILC.

## CoreCLR Requirements

The CoreCLR's GC normally runs as a generational, fully relocating, precise,
stop-the-world collector.  The GC supports weak, pinning and interior pointers.
Exterior pointers are not supported.  Code may be required to be fully
interruptible or may be partially interruptible.  The GC in the CoreCLR also
supports a conservative mode which greatly simplifies the obligations of the
JIT.

For the full CoreCLR GC, a JIT has several responsibilities:

1.  Use write barriers for stores of GC pointers or value classes with GC
    pointer members (typically via helper calls to the runtime).
2.  Determine the set of live GC roots in stack slots and registers.
    Slots may be untracked or tracked. All slots must be properly initialized,
    if necessary, before the first safepoint.
3.  Ensure no exterior pointer is live at a safepoint.
4.  Track whether each reference is an object, interior, or pinned reference.
    Special reporting may also be needed for this pointers in methods that have
    them.
5.  Ensure that there are enough safepoints along every possible dynamic
    execution path to avoid long or indefinite GC pauses.
6.  Determine if there are any regions where GC is not supported, for instance
    parts of some prologs and epilogs or the middle of some quasi-atomic
    instruction sequence. Make sure there are no safepoints in such regions.
7.  In some cases, ensure that special pointers (pinned pointers, this pointers,
    generic contexts) are kept alive throughout a suitable region of code by
    reporting them at the requisite safepoints. This keep alive region might
    extend beyond the normal range determined by liveness analysis.
8.  Fill in a GC Info object describing the above information for use by
    the runtime. Note that in the CoreCLR the reporting format is architecture
    dependent and may require various encoding techniques.
9.  Ensure that correct transitions are made between managed and unmanaged
    (i.e. GC-unaware) code.

In conservative mode only 9. applies, but:

1.  The JIT must ensure that at a safepoint, each live object with a root in
    the current stack frame is referenced by at least one object or interior
    pointer (more specifically, this excludes live objects that are only
    referenced by exterior pointers).

There are some related JIT responsibilities that help ensure type safety for
verifiable programs - for instance GC pointer updates in memory must be atomic
so that no other thread can observe a partially updated pointer.  Those
responsibilities are outside the scope of this document.

## GC Support in LLVM

LLVM currently supports a model for precise GC known as GCRoot.  There is a new
model under development called Statepoints.  We'll discuss each of these in
turn.

### GCRoot

In the [`GCRoot`](http://llvm.org/docs/GarbageCollection.html) approach, each
location that might contain a GC pointer is reported in the method prolog via
an `llvm.gcroot` intrinsic call.  This call effectively makes these locations
escape.

Safepoints are modelled as calls to functions that can read and write escaped
memory.  Thus no GC pointer can be kept live in registers across a safepoint,
since its value might change across the call.  Also, this means only stack
slots are reportable in the GC info.  All `GCRoot` slots are considered live at
all safepoints, so the GC info reflects that these are untracked locations.
All reported slots must be initialized in the prolog or at a suitable early
point.  It is up to the compiler to ensure that all reportable locations that
live across a safepoint are described by a `GCRoot` call - this may include
temporaries from complex expression trees.  Explicit nulling may be needed
after temporaries die to ensure these temporary slots don't hold onto objects
longer than necessary.  It's up to the IR creator to ensure all this is done
correctly.

Eventually the `llvm.gcroot` is lowered out as a normal alloca, and there
doesn't seem to be any special protection left after that point.

`GCRoot` includes support for late insertion of safepoints.  However this
functionality appears to not be fully implemented.  `GCRoot` supports custom
encoding of the GC information.

### Statepoints
Philip Reames at Azul Systems has written up a couple of objections to the
`GCRoot` approach and suggested an alternative he called
[`Statepoints`](http://llvm.org/docs/Statepoints.html).

Part of Reames' motivation was better support for a precise, fully relocating
collector, something that is also required for CoreCLR's GC. Given that
`Statepoints` provide a useful superset of what GCRoot provides, and that
the work is ongoing and still a bit malleable, it seems prudent to build upon
this work for supporting the CoreCLR GC in LLILC.

`Statepoints` essentially allow for tracked location reporting. Each safepoint
can describe a different set of locations to report to the GC, including
registers. The per-method GC read-write potential at safepoints is made
manifest in the SSA form by explicit addition of the live-in GC pointers and
live-out GC pointers. Safepoints also allow reporting derived pointers.
The reliance on SSA for GC liveness reporting at safepoints means that GC
pointers found on the stack at safepoints (say from fields in value types)
cannot be reported.

There is now experimental LLVM support for parts of the `Statepoint` proposal.
Current plans for finishing the work would not include custom formatting;
instead the results appear in the LLVM
[StackMap](http://llvm.org/docs/StackMaps.html) format.

`Statepoints` also allow for late insertion of safepoints, though how this
happens is still being worked out. One aspect of this is to use LLVM's
address spaces to distinguish GC pointers from ordinary pointers. In
particular, GC pointers are required to be in `addrspace(1)` or be in an
address space with some distinguishing GC attribute. Then identifying the set
of necessary safepoints and liveness analysis could drive wiring up the
SSA uses and definitions. We have adopted this address space convention in
LLILC.

`Statepoints` also support the notion of statepoints which are transitions
between code that uses different GC modes ("GC transitions").  LLILC can
(and does) make use of GC transitions to support calls from managed to
unmanaged code.

## Open Issues - Correctness

Given the aspects of GC that LLVM supports and the features that the CoreCLR
GC requires, here are the open issues in using LLVM to generate correct and
complete GC Info.

### Loss of GC Pointers

GC pointers can get lost if they're cast to non-pointer types and then cast
back, or cast to non-GC pointer address spaces and then cast back. However,
the consistent use of `addrspace(1)` for GC pointers should inhibit most
of this, since LLVM is aware that pointer representations may change when a
pointer is cast from one address space to another, so these casts must be
done explicitly via `addrspacecast`. Violations are flagged by IR constructors
and checkers. So for the most part we'll just need to audit places where an
ordinary pointer or non-pointer is cast to `addrspace(1)` - there should
 be very few legitimate cases of this.

### Unreported GC Pointers

Once the GC dependence of GC pointers on safepoints is made explicit in the IR,
a fragile invariant arises. A new derived pointer can be created before a
safepoint and used after it without apparent error. It seems plausible that
we can check for this by running a second GC pointer liveness pass downstream
after the point safepoints are made manifest; in this framework, no GC pointer
should be live across a safepoint.

### Aggregates Containing GC Pointers

Value types in the CLR may contain GC pointer-valued fields. The current
`Statepoint` proposal does not support reporting GC pointers from non-SSA
sources.

### Interior Pointers

While the use of `addrspace (1)` or similar convention helps locate GC
pointers, it does not distinguish between
[object and interior pointers](https://github.com/dotnet/llilc/issues/28)
We need to find a way to do this since we must report these differently.

Initially, at some performance cost, we can report all references as interior
pointers.

Note that it is not possible in general to use derived pointer reporting for
interior pointers. Interior pointers may be passed as arguments to calls and
returned as results by function calls, and in these cases the callee has no
way of reporting the base pointer. Furthermore, since the default assumption
for parameters is that they are object pointers, we'll need some sort of
metadata or similar to distinguish  cases where they're actually interior
pointers.

To report interior pointers the GC can keep track of starting addresses of
all objects such that if you point within an object it can find the base
address by searching for the nearest pointer that represents the start of
the object.

### Exterior Pointers

The case of exterior pointers requires additional information to determine
which object the pointer is pointing to, since if they point into an object
at a higher address the GC can't tell which object is that pointer's base.

It seems likely that LLVM's optimization passes might create exterior pointers
in places (e.g. pointing before or after the object). The CoreCLR GC can't
tolerate these, so we'll need to ensure that no exterior pointer is live at a
safepoint.

### Pinned Pointers

[Pinned pointers](https://github.com/dotnet/llilc/issues/29)
must be reported live and pinned for at least the duration of
the pin. If a pinned pointer is copied, at least one of the references must be
reported as pinned at each safepoint in the pinned range.

Translating these constraints into IR has proven tricky in other compilers
we've worked with. The problem with pinned pointers from the compiler standpoint
is that the uses of the pinned objects aren't data-dependent on them.
Pinned is an attribute of a local (of managed pointer type) in the MSIL.
The sequence is something like

    pinnedLocal.mp = ...
    otherLocal.up = convert pinnedLocal.mp
    ... uses of otherLocal.up ...
    pinnedLocal.mp = null

The constraint is something like "given any use that is transitively
data-dependent on `otherLocal` (with maybe a special case to break out if
`otherLocal` gets converted back to a reported pointer), if that use
gets moved passed a store to `pinnedLocal` then it can't further get moved
past a safepoint".  We've never seen a clearly written contract about this
(that takes into account optimizer freedom), so we don't know what's
guaranteed if somebody takes the address of `pinnedLocal` and the compiler
can't tell which calls or indirect stores might or might not update it.

### Custom GC Info Encoding

As noted, the current `Statepoint` work does not support custom encoding
formats.  [Adding this](https://github.com/dotnet/llilc/issues/31)
seems straightforward, but we might push to have the
base layer provide a more customizable foundation.

### Support for Fully Interruptible GC

Neither `GCRoot` nor `Statepoints` support fully interruptible GC Info.
We'll have to find out how crucially the CoreCLR depends upon this feature.
It seems plausible we can simply support partially interruptible GC.

### GC Pointers and GC Info for Funclets

Depending on how EH features are implemented, we may need custom support for
reporting GC info from funclets.

## Open Issues - Performance

Here are the open issues in using LLVM to generate performant code in the
presence of GC references and reporting rules. Most of these are not urgent
but some might be good jumping-off points for community engagement.

### Minimization of untracked references

Untracked locations require zero-initialization in the prolog even if the
slots being initialized are conditionally live at that point. In certain
pathological cases (e.g. a method with a large switch where each switch arm
introduces GC reference locals) the overhead of this zeroing can become a
serious performance issue.

### Enregistration of GC pointers across Calls

Ideally we'd be able to keep GC pointers in registers across calls. The current
`Statepoint` work supports this in abstract but in practice all the GC
pointers are spilled to memory at call sites.

Naturally there are some follow-on complications.The GC must be aware of the
spill locations of callee save registers, since they might contain GC pointers
 from the caller and the location of these is only known to the callee.
Location info for spills may be obtainable from the regular unwind data or
might require a custom reporting format.

### Callee-Save Spills and Restores

If a callee save is spilled to the stack across a safepoint, the callee save
must be restored from the stack, even if the register spilled has not been
modified along the path leading to the restore. This handles the case where
the register is not live at the safepoint, and the spilled value is updated
by the GC. For example:

        spill RBX
        br i1 %p, label %1, label %2

    ;<label>:1
        ...modify RBX
        br label %3

    ;<label>:2
        call[safepoint] f()
        br label %3

    ;<label>:3
        restore RBX
        ret

Here `RBX` is not live at the call to `f()` and so does not need to be
reported at the safepoint. An optimizer might choose to tail-duplicate
leading to the following code, where at label `2` it appears the restore
of `RBX` is unnecessary.

        spill RBX
        br i1 %p, label %1, label %2

    ;<label>:1
        ...modify RBX
        restore RBX
        ret

    ;<label>:2
        call[safepoint] f()  // GC might modify spilled RBX here
        restore RBX          // so this restore is not redundant
        ret

### Callee-Saves and GC transitions

In methods that might call into unmanaged code, the compiler must ensure
that either
a. no callee saves contain GC references at unmanaged call sites, or
b. any callee saves that do contain GC references at such call sites are
   spilled to known locations that can be found and (if the gc is
   relocating) reliably updated.

Note this might mean spilling the callee save even if it's not used in the
method, since the callee has no way of knowing if that register contains
a GC pointer or not (preferably shrink wrapping the spill so it happens only
if the unmanaged calls are actually going to happen).

### Stack Layout

Ideally GC pointers that require zero initialization in the prolog are laid out
contiguously on the stack so that they can be zeroed by a single memset or
similar.

### Stack Packing

Ideally GC pointers that require stack locations would be packed into the
smallest number of stack slots possible, both to minimize frame size and to
reduce the amount of up-front zeroing that is needed.

### Shrink Wrapping

Initialization of GC pointers to null and spills of callee saves should be
deferrable.


### Side effecting calls that can't cause GCs

Both the `GCRoot` and `Statepoint` approaches rely on broad alias escapes to
indicate that GC references might change. In `GCRoot` this is used to indicate
all GC references (including those on the local frame) while in `Statepoint` it
is used currently just for heap and static references. A more refined approach
would be to have the alias model establish a distinguished region of
relocatable references and have safepoints only alias those. This would allow
calls that have side effects but can't trigger GC to bypass safepoint reporting.

### Dead Stores and Nulled Pointers

It is common to assign null to a GC pointer when it's no longer needed locally,
to avoid holding onto objects that aren't needed. Without some precautions
these stores of null may appear as dead stores since there are typically no
apparent reads downstream. The code generator can remove the store and trim
the lifetime for tracked locations, but must keep the store for untracked
locations.

### No need to report Null Pointers

Locations known to contain null pointers don't need to be reported to the GC.

### Optimizing Write Barriers

Write barriers can impose significant performance overhead. In some cases this
overhead can be mitigated by optimizations:

-   Removing unnecessary write barriers - for instance, no barriers are
    necessary on newly created objects that haven't crossed a
    safepoint. Note this will entail code-motion restrictions;
    a store with an elided barrier cannot cross a safepoint.
-   Write barrier calls may be inlined or tailored to mesh with the allocator's
    needs. The actual barrier typically has fairly low register cross section,
    so customizing its register usage during register allocation is appealing.
    If the barrier is inlined, special care must be taken to prevent subsequent
    code motion.
-   Merged barriers - the underlying GC may track suspect regions of older
    generations at page or larger granularity, so stores to contiguous or
    locally dense regions may be handled with a single write barrier. This can
    be especially useful in copy loops. No safepoint can appear between the
    actual writes and the ultimate reporting barrier.
-   Storing null to a location may not require a write barrier.
-   It is often beneficial to defer lowering of stores requiring write barriers
    into helper calls so that upstream optimizations can deal with them more or
    less as normal stores.

### Safepoints May Inhibit Legal Optimizations

Once safepoints are inserted, a number of legal optimizations may no longer be
possible. For instance:

-   The non-GC fields of a class are not modified at safepoints, so it should
    be safe to CSE loads of such fields across safepoints.
-   Subsequent optimization may dead-code a GC pointer use below a safepoint.
    We'd like to be able to propagate this above the safepoint to remove even
    more code, but would be blocked by the apparent use and definition at the
    safepoint.

These considerations may bias us towards late insertion of safepoints; however,
it may be advisable to insert safepoints early in some places (loops, for
instance) so that the loop optimizer can observe the need for a safepoint
and plan accordingly.

## Implementing GC Support in LLVM

At this point we are very early in the development process, so parts of these
plans are tentative and somewhat vague. We welcome the advice and participation
of the community in helping us to refine and improve our plans.

### No GC

We currently create a GC info with no locations reported. We use
`addrspace(1)` to mark GC pointers in the IR, but will have no way of knowing
if we're doing it properly. We'll only run small tests that do not trigger GCs.
This will be sufficient for building up most of the other Jit functionality.

### Conservative GC

Initially, we'll enable the
[conservative mode](https://github.com/dotnet/llilc/issues/27)
of the CoreCLR GC.  We've taken a preliminary look and this
seems to work well enough, but we need to do further vetting. Alternatively we
can adjust the GC's segment size to forestall GC as long as possible. This
should allow us to run a substantial number of tests.

If for some reason this doesn't pan out, we can also look into modifying the
GC segements sizes to defer GC as long as possible.

### Statepoint V1

This will be an initial implementation using `Statepoints`. We'll have to
implement custom encoding (We can leverage the
[GC encoding library](ttps://github.com/dotnet/llilc/issues/30)
that the CoreCLR provides for this) and
[find some method](https://github.com/dotnet/llilc/issues/30)
of distinguishing interior pointers from object pointers. This will possibly
be a dataflow analysis along with some hinting from LLVM metadata or similar.
We'll have to find some way to report
[GC fields from aggregates](https://github.com/dotnet/llilc/issues/33),
maybe by blending some of the `GCRoot` support back in for such slots.
As needed we plan to help fill in missing pieces of
the experimental implementation in LLVM. For instance, we'll probably implement
our take on the
[late insertion of safepoints](https://github.com/dotnet/llilc/issues/32).
And we'll likely try and write a
[downstream checker](https://github.com/dotnet/llilc/issues/34)
that runs at encoding time
or similar to verify that no unreported GC pointers exist.

At this point we hope to have a functionally correct GC with partially
interruptible GC info reported back to the CoreCLR. We can start running GC
stress and related tests and see how robust this solution is.

### Statepoint V2

Based on our experiences with Statepoint V1 we'll likely want to work with
the community to enhance the `Statepoint` work. The specifics here will
depend on what we've learned and what problems we encounter.

### Generating GC Tables

CoreCLR requires the generation of GC Tables that contain pointer 
liveness information for each method. To achieve this, we read 
the liveness information from the `__llvm.stackmaps` section, and 
translate it to the CoreCLR format. We obtain the information by 
post-processing LLVM generated binary, rather than adding a new 
StackMap generator (for CoreCLR format) to LLVM itself. 
While this involves a two step translation, it helps avoid:
* Compatibility conflicts because of LLVM core having to change 
  whenever there is a change in CoreCLR.
* Bugs because of changes not correctly propagated to multiple 
  stack-map generators in LLVM.

Generating GC Tables involves the following steps:

1. Enable StackMap section generation for COFF in LLVM.
2. Parse the `__llvm.stackmaps` section.
3. Collect certain information about the stack frame 
   (ex: which of the callee save registers are saved) not available
   in the StackMap section by parsing other sections, if necessary.
4. Encode information using CoreCLR's GCInfo Encoder
  * Report each call-site (GC safepoint).
  * Assign slots IDs for each unique register and stack locations.
  * For each slot, report liveness based on the location records 
    in `__llvm.stackmaps` section.

CoreCLR permits reportng certain slots as "untracked" GC-pointers,
for stackmap-size reduction or throughput reasons. 
However, LLVM currently does not support untracked reporting -- 
stack-locations cannot be assumed to remain live through entire function, 
due to transformations like stack coalescing. Therefore, LLILC will report
the liveness of all slots as tracked pointers.

## Summary

The requirements imposed on a code generator by the CoreCLR present new
challenges for GC reporting in LLVM, both for correct reporting and for
optimization in the presence of GC reporting requirements.

LLILC is a new JIT for the CoreCLR that will leverage LLVM for code
generation. We plan to work with the community to enhance LLVM to become a
robust and performant platform for managed code generators like LLILC.

## Terminology

A *safepoint* is a point in code where a GC can safely occur.

A method is *partially interruptible* if it contains at least one safepoint.
A method (or region of a method) is *fully interruptible* if every instruction
boundary is a safepoint.

A GC algorithm works by determining the transitive closure of objects reachable
via *GC pointers* (or GC references) from a set of *GC roots*. GC roots are
typically found in distinct memory regions: static memory, the runtime stacks
and register state of active threads, and sometimes in heap memory.

A GC algorithm is *conservative* if the set of locations it scans for GC roots
is a superset of all the possible locations that may contain GC roots.
A GC algorithm is *precise* (or accurate) if the locations is scans for GC
roots must exactly match all the possible locations for GC roots.  An algorithm
may be conservative in some memory regions and precise in others.

A GC algorithm is *relocating* if during a GC, GC pointers may be updated to
refer to the new location of an object.  An algorithm update pointers found in
certain regions but not others.  For instance, GC pointers from the stack might
not be updated while GC pointers in the heap might be updatable. If the
algorithm relocates pointers from all regions, it is *fully relocating*.
When a GC is relocating pointers from some memory region, it necessarily
implies the GC reporting is precise in that region.

A *weak pointer* or weak reference is a GC pointer that does not keep an object
live.  These can be set to null during GC, even if a GC is not relocating.

A *stackmap* or *GC map* info is used to describe the set of root locations
to scan at a safepoint. The map may describe stack locations (via base
register + offset) or machine registers. Other information is used to establish
the sets of root locations on the heap and in the stack; the code generator
is usually not involved in this. The *GC Info* for a method is the set of all
stackmaps and other GC reporting information.

An *object* or *base* pointer is a pointer to the base address of a GC
collectable object. A *managed* or *interior* pointer is a pointer to an
address within an object or possibly a pointer to the address just beyond the
object. A *derived* pointer is a pointer that is offset from an object pointer
but is logically related to the object pointer.  A derived pointer may or may
not be an interior pointer. We will use the term *exterior* pointer to refer to
the case where the derived pointer is not an interior pointer -- hence an
exterior pointer points beyond the logical extent of the object. Note
dereferencing exterior pointers can violate type safety.

A GC collectable object is *pinned* at a safepoint if it is not allowed to be
relocated if a GC occurs at that safepoint.

A root location within a method is *tracked* if it can be reported as a root at
some safepoints and not reported at others (where the determination of where
it must be reported being driven by liveness analysis). If no liveness analysis
is performed on the root location, or the location is live across the entire
method (and hence would be reported in all safepoints) then the location is
effectively *untracked*.  Locations must generally be initialized before the
first safepoint in the method so that the GC does not encounter garbage values.
Untracked locations are typically initialized in the method's prolog.
Untracked locations may be reported via separately from safepoints in GC Info.

A GC may support collecting just a subset of objects. Typically this is done
by segregating objects by age (a generational GC). This usually requires
special attention when writing GC references to memory, via *write barriers*.

Some GC algorithms support GC running concurrently with code execution.  Often
this requires special attention to be paid when GC references are read, via
*read barriers*.

To ensure that a GC can be performed promptly when needed, the code generator
must ensure that the number of instructions executed between safepoints is
bounded. Often this means the code generator will insert additional safepoints
(aka *GC probes* or *GC polls*) at places in the code.

A *managed* method is one whose code is created by the JIT or similar compiler
that is aware of the special requirements imposed by GC (and more generally
the requirements of the managed runtime environment).

A *native* (or *unmanaged*) method is a code sequence that is not aware of
the special requirements for GC reporting. Typically these are assembly or
C++ methods or similar.

A *PInvoke* (Platform Invoke) is a call to native code from a managed method.
A *Reverse PInvoke* is a call to a managed method from native code.

## Staging Plan

No.  | Implementation | Testing | Issue | Status 
---- |--------------- | --------| ------| -------  
1 | Insert GC Safepoints: Run the PlaceSafepoints and RewriteSafepointsForGC phases before Code generation, and ensure that statepoints are inserted and lowered correctly | LLILC tests pass with Conservative GC | [32](https://github.com/dotnet/llilc/issues/32) | Completed 
2 |	Bring GCInfo library to LLILC: Use the GCInfo library to encode Function size correctly; no live pointers reported at GC-safe points | LLILC tests pass with Conservative GC  | [30](https://github.com/dotnet/llilc/issues/30) | Completed
3 | Report GC liveness: Encode GC pointer liveness information in the CLR format using the GC-Encoding library | A few functions compiled by LLILC with correct GCInfo | [31](https://github.com/dotnet/llilc/issues/31) | Completed
4 | Test Pass |  CoreCLR tests pass with Precise GC | [670](https://github.com/dotnet/llilc/issues/670) | Completed
5 | Add GC-specific stress tests| All existing and new tests pass | [696](https://github.com/dotnet/llilc/issues/696) | In Progress | 
6 | GC Stress testing | Run the LLILC tests in GCStress mode; some GCStress testing running regularly in the lab | |
7 | Special reporting for pinned pointers | Code with pinned pointers handled by LLILC | [29](https://github.com/dotnet/llilc/issues/29) | Completed |
8 | Support aggregates containing GC pointers  | Code with GC-aggregates handled by LLILC | [33](https://github.com/dotnet/llilc/issues/33) | Completed |
9 | Fully-Interruptible code: Investigate whether fully interruptible code should be supported | Test and GCStress Pass | [473](https://github.com/dotnet/llilc/issues/473) | |
10 | Lower Write barriers to Calls late | Test and GCStress Pass | [471](https://github.com/dotnet/llilc/issues/471) | | 
11 | Place Safepoint-polls only where necessary for CoreCLR runtime | Test and GCStress Pass | [425](https://github.com/dotnet/llilc/issues/425) | |
12 | Track GC-pointers in registers | Test and GCStress Pass | [474](https://github.com/dotnet/llilc/issues/474) | | 
13 | Implement GC Checker | Test and GCStress Pass | [34](https://github.com/dotnet/llilc/issues/34) | |
14 | Identify Object and Managed pointers differently| Test and GCStress Pass | [28](https://github.com/dotnet/llilc/issues/28) | |
15 | Implement necessary support to enable Precise GC when LLVM optimizations are turned on for LLILC | Test and GCStress Pass in an optimized LLILC build | | |
