# Exception Handling in the LLILC JIT

## Introduction

This document provides a high-level overview of the LLILC jit's processing
of exception handling constructs in the code it compiles.  It is not a fully
detailed specification, but rather is intended to capture the key design
decisions and rationale behind them.  The first sections provide a brief
background on exception handling in MSIL, LLVM IR, and the CLR; subsequent
sections describe the plan for LLILC.

### Priorities
The first target architecture for LLILC will be x86_64.  Accordingly, this
document currently focuses specifically on that target.

Also, LLILC specifically targets the CoreCLR profile.  Any features of
the Desktop CLR that are not supported in the CoreCLR are not considered
here.  In particular, this means that the extra requirements around
ThreadAbortException on the Destktop CLR are not requirements for
LLILC, and not discussed in this document.

This document pertains specifically to just-in-time compilation.  Details
for ahead-of-time compilation would possibly differ.


## EH Constructs in MSIL

MSIL exception handling constructs are defined in [ECMA-335 Partitions I, II, and
III](http://www.ecma-international.org/publications/standards/Ecma-335.htm).
Briefly, instruction ranges in the input may be marked as protected regions
(aka "try regions"), and each protected region has an associated handler.
When an exception occurs during the execution of a protected region, control
may be transferred to the handler depending on the dynamic state and handler
type.

There are four types of handlers:
 - **Catch handlers** have an associated exception type; a dynamic type test
   is performed against the thrown exception object to determine if the catch
   handles the exception.  Exception propagation stops when a catch handler
   is executed, unless the catch handler performs an explicit rethrow operation.
 - **Fault handlers** execute when any exception is thrown.  When a fault
   handler completes, exception propagation is resumed.
 - **Finally handlers** execute whenever the protected region is exited, whether
   by normal control flow or exceptional control flow.  When the finally
   handler completes, propagation continues in the exception case.
 - **Filter handlers** include a "filter part" that executes at runtime to
   determine whether to handle an exception or have it continue propagating,
   and a "handler part" to which control is transferred if the filter part
   indicates that it should handle the exception.

Exceptions may be raised explicitly by the `throw`/`rethrow` instructions or
implicitly by null dereference, checked arithmetic, etc.  [ECMA-335 Partition
III](http://www.ecma-international.org/publications/standards/Ecma-335.htm)
specifies which MSIL instructions may raise which exceptions.

Exceptions are precise; optimizations may not visibly suppress or reorder
exceptions or modifications to program state that might be visible to a
handler when an exception occurs.

Branches in msil that exit one or more protected regions use a special
`leave` operator rather than one of the normal branch operators; this
signals to the jit that it needs to insert invocations of any finally
handlers associated with protected regions being exited before transferring
control to the `leave` target.


## Contract with CLR Execution Engine

This section describes the constraints that the Execution Engine imposes on
the jit (and its codegen) to support exception processing.

### Raising Exceptions
To raise an exception, jitted code simply calls a helper method that the
Execution Engine exposes for this purpose.

### Unwinding Stack
The jit provides the Execution Engine with a description of each jitted
method's prolog, which can be used to reverse the prolog's effects and
unwind the stack.  The format used to communicate this information is the
same [`UNWIND_INFO` structure](https://msdn.microsoft.com/en-us/library/ddssxxy8.aspx)
used for native Windows stack unwinding, but with the exception handler
information omitted/ignored.

### Handling Exceptions
For each jitted method, the jit provides the Execution Engine with a list of
"EH clauses" that can be used to find the appropriate handler when an exception
is raised.  Each clause identifies a range of instructions constituting the
protected region, the handler type, and a range of instructions constituting
the handler.  Catch clauses also specify the caught exception type, and
filter clauses specify both the "filter part" and the "handler part".

When an exception is raised, the runtime/OS consults the information in the
EH clauses to find the appropriate handler.  Filters' "filter parts" are
invoked during this first pass (before the stack has been unwound) that is
used to identify the ultimate handler.  Once the ultimate handler is found,
the runtime/OS unwinds the stack up to the containing function and calls the
handler (stopping on the way to first call any fault or finally handlers as
necessary).  The handler executes its code and then returns control back to
the runtime (or, if the handler included an explicit rethrow, calls the
rethrow helper function provided by the runtime).  The value returned by a
catch handler or the "handler part" of a filter is the address where the
runtime should resume normal execution.  Finally and fault handlers do not
return a value; when a finally or fault handler returns, the runtime
continues propagating the exception.

In many ways, handlers are separate functions; they are invoked with a call,
return values like functions, and have prologs and epilogs (and their own
stack frames).  Handlers are often referred to as "funclets" for this
reason.  In order to support the underlying runtime/OS bookkeeping for
funclets, the CLR Execution Engine imposes some restrictions on the EH
clauses that the jit provides to it:
  1. The handlers (identified by their instruction ranges) must be disjoint
     from each other.
  2. The handlers must be disjoint from the non-handler code in the function.
  3. The "filter part" and "handler part" of a filter must be adjacent; they
     are reported together in a single EH clause as a triple of code offsets,
     with the assumption that the end of the "filter part" is the start of
     the "handler part".
  4. If any two protected regions overlap, one must be a proper subinterval
     of the other.
  5. If multiple protected regions indicate that they are protected by the
     same handler, all but the first must be annotated as a "duplicate".
  6. The function's non-handler code must collectively be contiguous, and
     precede the handlers.
  7. The EH clauses must be sorted with inner protected regions preceding
     outer protected regions and lower-address protected regions preceding
     higher-address protected regions (this allows a simple linear search
     with early termination to find the clause protecting a given
     instruction address).

Since handlers get called with their own stack frames, but may refer to
local variables in the parent function, they need to find their parent
function's frame.  The runtime assists in this by passing to each handler a
pointer to a fixed point in the frame of either the parent function or a
dynamically enclosing handler funclet.  The prolog for the parent function
stores a pointer to its own frame at a fixed offset in the frame, and the
prolog for each funclet copies the pointer from the provided frame to its
own frame.  This enables finding the parent function's frame, and also
places a restriction on frame layout that this Previous Stack Pointer Symbol
be stored at the same fixed offset in all frames.

The runtime maintains state tracking what exceptions are currently being
processed (there may be more than one because new exceptions may occur
during the execution of handlers).  This state is updated in the runtime's
personality routine before and after calls to handlers.  The helper function
that implements the rethrow operation takes no parameters and consults this
runtime state, and so must occur during the execution of the appropriate
handler.

### Differences Across Targets
These details are the same regardless whether the CLR is running on Windows
or Linux; the Execution Engine handles communicating the necessary
information to the OS and supplying an appropriate personality routine.

The first priority target architecture for LLILC is x86_64.  The details of
this section may differ for other architectures (particularly x86), where
unwinding may proceed differently.


## LLVM IR Model for Exception/Cleanup Flow

Exception handling in LLVM is [documented at llvm.org](http://llvm.org/docs/ExceptionHandling.html).
Briefly, exceptions may only be raised at [`invoke`
instructions](http://llvm.org/docs/LangRef.html#invoke-instruction),
and each `invoke` instruction specifies a landing pad where control is
transferred in the event of an exception.  The [`landingpad`
instruction](http://llvm.org/docs/LangRef.html#i-landingpad) carries data
which the backend can use to generate the descriptors needed for the
runtime/OS to perform unwinding (i.e. the personality function and the set
of referenced exception types).  The code after a `landingpad` may flow back
into the rest of the function, or it may execute the [`resume`
instruction](http://llvm.org/docs/LangRef.html#i-resume) to continue
propagation of an exception out of the current function (Note: this is not
quite the same as the MSIL `rethrow` instruction, which may be caught by an
enclosing handler in the same function).

There's also some current work in flight to support native Windows EH in
LLVM, which will be relevant to LLILC due to similarities between the
requirements imposed by Windows and by the CLR Execution Engine.  [This
thread on llvmdev](http://thread.gmane.org/gmane.comp.compilers.llvm.devel/85783)
describes the most recent proposal.  Briefly, the plan is to outline filters
in the front-end, to add a few new instructions, some of which can be the
target of exception edges (as opposed to exception edges always targeting
`landingpad` instructions), that will describe exception dispatch (i.e. type
testing and dispatch to the appropriate handler or outer dispatch) in a
high-level way with enough atomicity to guarantee that backends which need
the dispatch to be performed by the runtime can report it to the runtime
without intervening optimizations having obscured/altered it to the point
that this is not feasible.


## Potential Sticking Points

There are a number of design points where the .Net Jit/EE have taken a
different approach than most of the targets that LLVM supports.  The
LLILC jit may therefore find itself caught between opposing assumptions of
the LLVM codebase on the one hand and restrictions of the .Net Execution
Engine on the other.  Such cases are described here.  For the most part, the
plan is to follow the approach of the work to support Windows EH in LLVM,
since it faces essentially the same issues.

### Contiguous Regions vs. Freedom of Code-Motion/Block-Layout
The CLR requires each handler to be laid out as a "funclet" whose blocks are
contiguous and that is separated from the non-handler code of the function.
Microsoft compilers have traditionally ensured this by attaching EH region
annotations to the IR (initially populated from the MSIL EH region
annotations) and maintaining them throughout compilation, consulting them
during code-motion to avoid moving code from one region to another and
during block layout to ensure the required contiguity/separation.  LLVM IR
does not carry region annotations, so a different approach is required here.
The current proposal for Windows-compatible EH includes explicit instructions
executed at entry and exit of handlers, from which regions can be inferred
at EH preparation time (with code duplication used to make regions
single-entry where required); LLILC will follow this approach.  The modeling
of `rethrow` and the instruction that ends a catch must ensure that they are
not reordered with respect to each other.

Traditionally, .Net jits have also laid out each protected region (minus any
nested handlers inside the protected region) as a contiguous piece of code
in the main function.  This is not a hard requirement of the runtime
(discrete segments of a non-contiguous region can be reported separately so
long as all but the first are marked as duplicates), and will not be ensured
by LLILC.  This will allow greater freedom to optimize try regions (as
opposed to the relatively cold catch regions that get outlined) and perform
block layout based on performance-centric rather than region-centric
heuristics.

### Unwinder State and Stack Frames
When the CLR invokes a handler, the handler sets up its own stack frame, and
there may be unwinder state on the stack (or other program state, in the
case of filters).  Handlers exit by returning to the unwinder, which will
unwind the stack up to the main function's frame and transfer control back
to it.  Traditional LLVM targets, conversely, transfer control to the
handler with the stack already unwound to the main function's frame;
handlers at their exits call a special helper to signal the end of the catch
to the unwinder, and then simply jump back to the main function.  The CLR
requirements imply that LLILC will need to generate handler prologs and
epilogs, and have a mechanism for finding the parent frame in a funclet (in
order to access local variables).  The current LLVM work to support native
Windows EH has these same requirements, so LLILC should follow that
approach, making sure that the frame-finding part agrees with the Previous
Stack Pointer Symbol handshake with the CLR Execution Engine.

### Nesting Finally Handlers vs. Chaining Cleanups
In MSIL, protected regions can be nested inside each other, and when an
inner region's finally (or fault) handler finishes processing an exception,
the exception is propagated to the outer handler.  This is achieved by
reporting nested protected regions of machine code to the Execution Engine,
which directs the runtime to invoke the outer handler upon return from the
inner handler.  LLVM IR provides a `resume` operator that can be used to
continue propagation out of the current function, but to continue
propagation to an outer cleanup within the function, the inner cleanup
typically just branches to the outer cleanup.  The new proposal for
Windows-compatible EH introduces explicit instructions for branching to the
next outer cleanup, which LLILC will make use of.

### Handler Selection/Dispatch in Landing Pad vs. Runtime
Different exceptions raised at one instruction may need to be handled by
different handlers within the function (e.g. if the instruction is protected
by multiple catch handlers that catch different types of exceptions).  In
LLVM IR, each invoke instruction specifies just a single landing pad for
exceptions; the common convention is that the unwinder, at runtime, will
transfer control to the landing pad, supplying a "selector" (e.g. the
exception type) that explicit code inserted into the function at the landing
pad then uses to direct control to the appropriate handler.  The .Net
runtime uses a different model:  when an exception is raised, there is a
first pass to locate the appropriate handler, that scans the EH tables and
performs the appropriate type tests for catch handlers and invokes filters
(with the stack not yet unwound); then in a second pass the runtime/OS
unwinds the stack, invoking finally/fault handlers as appropriate, and
eventually calling the appropriate catch/filter handler directly.  The
challenge to representing this in LLVM IR is the need to represent the
multiple possible destinations of the exception flow.  The current proposal
for Windows-compatible EH acheives this by chaining the landingpad
replacement instructions to each other, preserving the nesting relationship.

### Implicit Exceptions and Machine Traps
In LLVM IR, the only instruction that can raise an exception is `invoke`.
MSIL instructions implicitly raise exceptions, e.g. NullReferenceException
can arise from any load or store, `div` can raise a DivideByZeroException,
and various arithmetic instructions can raise exceptions on overflow.
Microsoft compilers have traditionally modeled their IR similar to MSIL in
this regard, with exceptions being implicitly raised by the corresponding
operators.  Somewhat related, the CLR has traditionally used machine traps
to actually raise NullReferenceExceptions and DivideByZeroExceptions at
runtime.  The idea of allowing implicit exceptions in LLVM IR has been
[discussed on llvmdev](http://thread.gmane.org/gmane.comp.compilers.llvm.devel/71773),
resulting in consensus opinion that it's not desirable to conflate the check
and load/store operations in LLVM IR.  More recently, [an RFC](http://thread.gmane.org/gmane.comp.compilers.llvm.devel/84837)
has been introduced to allow folding null checks onto loads at the machine IR
level (after using explicit checks throughout optimization).  Accordingly,
the plan for LLILC is to insert explicit tests and throws in the IR for
implicit MSIL exceptions, and run the null check folding pass as an
optimization on targets that support it.  This allows the null checks and
loads/stores to be optimized independently, and allows the runtime to be
ported to new targets without also needing to appropriate machine traps on
those targets.  This will require extending the null check folding
optimization (the current plan is to fold away the compare and branch; for
CoreCLR we also want to fold away the call to the helper that raises the
NullReferenceException).

### One-Pass vs. Two-Pass Exception Handling
LLVM IR is set up to model cleanups that occur en route, working from inner
scopes to outer scopes and up the call stack, to finding and transferring
control to an appropriate handler for a raised exception.  Finally handlers
are the CLR's equivalent of cleanups, but finally (and fault) handlers are
not run until *after* filters in outer scopes and up the call stack (between
the finally and the eventual target handler) have been run.  Modeling this
control flow explicitly and precisely in the IR would be awkward, and would
require complex updates during inlining in the event that a function with a
finally/fault handler is inlined into a filter-protected try region in a
caller.  The SEH support currently being added to LLVM expects filters to be
outlined by the front-end (and support for this outlining has been [added to
clang](http://reviews.llvm.org/rL226760)).  Similar outlining will need to
be performed by LLILC.  With this approach, the invocation of the outlined
filter is modeled as an effect of the `invoke` target (the invoke target
must call an external function to raise an exception, which therefore must
be conservatively modeled as possibly calling the filter function).


## Translation from MSIL to LLVM IR in Reader

This section describes the processing of EH constructs in the MSIL Reader.
The goal is to translate the MSIL constructs into IR constructs that match
LLVM's expectations to the greatest degree possible, and to confine special
semantics to a small number of intrinsics that minimally inhibit
optimization.  This goal is shared with the [Windows EH support in
LLVM](http://thread.gmane.org/gmane.comp.compilers.llvm.devel/85783)
currently being developed, and the plan is to be able to reuse much of that
implementation for funclet extraction and EH descriptor generation.

### Explicit throw
An explicit `throw` operation will be translated into an
[`invoke`](http://llvm.org/docs/LangRef.html#invoke-instruction) of the
appropriate helper function (#defined by `CORINFO_HELP_THROW`).  The unwind
label of the invoke will be a `landingpad` corresponding to the innermost
enclosing protected region.  In the event that there is no enclosing
protected region in the current method, the operation will be translated to
a `call` rather than an `invoke`.

### Implicit exceptions
MSIL instructions that can generate implicit exceptions (NullReferenceException
on load/store, ArithmeticOverflowException, etc.) will be expanded into
sequences of LLVM IR instructions with explicit condition testing and
exception throwing, at least initially.  See [discussion](#implicit-exceptions-and-machine-traps)
above for details/rationale.

### Catch Handlers
LLILC will translate catch handlers the same way that Clang does when
targeting native Windows.  The current proposal is to have an `catchblock`
instruction which is the target of the EH edges within the protected region
and which models the exception dispatch, branching to the catch handler code
or to a corresponding `catchend` (which passes control to the next outer
handler).

### Finally Handlers
Under LLVM's new Windows-compatible EH proposal, a `cleanupblock` instruction
will be added, which can be used as the target of EH edges within a region
protected by a finally handler; the end of the handler is marked by a
corresponding `endcleanup` instruction.

Since a finally can also be entered by normal control flow, LLILC will need
to support entering a cleanup via non-exception flow, as described in
[this thread](http://thread.gmane.org/gmane.comp.compilers.llvm.devel/85783/focus=85980).
Assuming that `endcleanup` takes the form that it has two successors, LLILC
will need to generate explicit continuation selector variables and an explicit
switch on the associated selector in the non-EH successor of `endcleanup` if
the associated finally has more than one label targetd by `leave` instructions
exiting it.  Folding the continuation selectors back into the funclet
callsites can be approached as a subsequent optimization.

One performance consideration of note for finally handlers is that jits
often make a clone of the finally handler for the primary non-exceptional
path, to allow better optimization (and avoid the overhead of a call at
runtime) along that path.  This will be considered after initial bring-up.

### Fault Handlers
Fault handlers will essentially be treated like [finally handlers](#finally-handlers),
with the exception that the reader will not insert code to enter fault handlers
during the processing of `leave` instructions (and in general will never
insert code to enter fault handlers except via exception edges), and therefore
fault handlers don't need associated continuation selector variables (the end
of a fault handler will branch unconditionally to the exception continuation).

### Filter Handlers
Filters have a "filter part" and a "handler part".  The "filter part" will
be outlined at the start of compilation, so that referenced locals can be
moved to closures and the calls to filters implicit in throwing `invoke`
targets are a sound representation of the control flow into and out of
filters.  The "handler part" will be treated similar to a [catch
handler](#catch-handlers), with the difference that, following the LLVM
convention for SEH, the address of the outlined filter function will appear
in the `catchblock` where the type of caught exception appears for catch
handlers.  Additionally, the outlined "filter part" and "handler part" for
each filter must be placed adjacent to each other (with the "filter part"
first) when the funclets are laid out.

### Rethrow
Rethrow will be translated similarly to [throw](#explicit-throw), except
that the call is to `CORINFO_HELP_RETHROW` instead of `CORINFO_HELP_THROW`
(and the rethrow helper takes no arguments, unlike the throw helper which
takes a pointer to the exception object to throw).  The aliasing information
for these calls must interfere with the information on the `unwind` and
`recover` instructions that end catch handlers.

### Leave
When a leave instruction is encountered, if it does not exit a
finally-protected region, it can be treated as a goto.  Otherwise, it must
set the continuation selectors appropriately for any finally-protected
regions it exits (see section on [finally handlers](#finally-handlers)), and
then branch to the innermost finally handler whose protected region it
exits.


## Translation from LLVM IR to EH Tables

The plan for LLILC is to use the LLVM code that is currently being developed
to support native Windows EH in order to identify the structure of the
protected regions; then communicate these regions to the .Net Execution
Engine as EH Clauses.  Details are still TBD, but the region structure being
encoded in the two cases is essentially similar, so a mapping should be
feasible.


## Staging Plan and Current Status

Full EH support will take a while to implement, and many jit tests don't
throw exceptions at runtime and therefore don't require full EH support to
function correctly.  Thus, in order to unblock progress in other areas of
LLILC during bring-up, initially the EH support will be stubbed out, with
just enough functionality for such test programs to pass.  In particular,
code with EH constructs is expected to compile cleanly, but it is only
expected to behave correctly if it does not attempt to raise exceptions at
runtime.  The throw operator and the explicit test/throw sequences for
implicit MSIL exceptions will be implemented on top of the stub support
(with throws using `call` rather than `invoke`), to reflect correct program
semantics and allow compilation of code with conditional exceptions that
will execute correctly if the exception conditions don't arise at run-time.

Once the stub support (with explicit and implicit exceptions) is in place,
the next steps will be to translate handlers in the reader and generate EH
clauses at the end of compilation.  The logical order is to implement the
reader part (which can be validated by inspecting the generated IR) before
the EH clause generation part (which can be validated by executing tests if
the reader part is already in place), for each handler type.  Tests with
handlers would regress (stop compiling cleanly) in the interim.  Also, these
changes may require some iteration between the table part and the reader
part as the details crystallize.  To avoid introducing this churn in the
master branch, the bring-up will be done in a separate EH branch.

Once correct EH support is enabled and pushed back up to the master branch,
EH-centric optimizations and support for other targets will follow.

The current status is that the stub EH support is implemented with support
for both explicit throws and implicit exceptions.

In summary, the plan/status is:
 1. [x] Stub EH support
   - [x] Reader discards catch/filter/fault handlers
   - [x] Explicit throw becomes helper call
   - [x] Continuation passing for finally handlers invoked by `leave`
   - [x] Implicit exceptions expanded to explicit test/throw sequences
     - [x] Null dereference
     - [x] Divide by zero
     - [x] Arithmetic overflow
     - [x] Convert with overflow
     - [x] Array bounds checks
     - [x] Array store checks
 2. [ ] Handler bring-up in EH branch
   - [ ] Catch handler support
     - [x] In reader (includes updating throws to use `invoke` rather than
           `call` with EH edge to `landingpad`)
     - [ ] Funclet prolog/epilog generation, Previous Stack Pointer Symbol
           handshake implemented
     - [ ] EH Clause generation
     - [ ] Reporting funclets back to EE with parent functions in `.text`
   - [ ] Support for `rethrow`
   - [ ] Finally handler support
     - [ ] In reader (includes `leave` processing, continuation selection)
     - [ ] EH Clause generation
   - [ ] Filter handler support
     - [ ] In reader (includes early outlining)
     - [ ] EH Clause generation
   - [ ] Fault handler support
 3. [ ] Migrate changes back into master branch
 4. [ ] EH-specific optimizations
   - [ ] Finally cloning
   - [ ] Null check folding
   - [ ] Others TBD
 5. [ ] Support for other target architectures
 6. [ ] Support for ahead-of-time compilation

(Note: The relative order of optimizations vs. other targets vs.
ahead-of-time is TBD, based on future priorities.)


## Open Questions

 1. Can filter outlining leverage some of the same outlining utilities used
    by the late outlining of handlers in LLVM, or is it best performed
    directly in the reader?
 2. How is LLILC specific functionality added to llvm?  This document
    assumes LLILC can reuse utilities in the windows-msvc target, create its
    own intrinsics, etc.; the engineering specifics of how to accomplish
    that are TBD.
 3. How exactly will the outlined funclets be reported back to the JIT?
    This document doesn't cover the JIT driver structure in LLVM or the
    .Net EE, but assumes that reporting funclets along with the parent
    function is a solvable problem.  The current use of MCJIT for LLILC
    should be convenient here.  The MCJIT driver compiles a module at a time,
    and LLILC represents each MSIL function as a module; adding the outlined
    functions to that module should facilitate grouping/ordering the
    funclets and reporting them as a combined `.text` section to the .Net
    EE as it requires.
