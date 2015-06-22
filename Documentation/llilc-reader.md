# LLILC Reader

## Introduction

LLILC reader is part of LLILC JIT and is responsible for translating
MSIL instructions into LLVM IR.  The semantics of MSIL instructions is
described in [ECMA-335](http://www.ecma-international.org/publications/files/ECMA-ST/ECMA-335.pdf).

The reader operates in two passes: the first pass builds LLVM basic
blocks and the second pass translates the instructions.  The resulting
LLVM IR is passed back to the JIT driver for further LLVM processing.

The reader is provided a pointer to an instance of ICorJitInfo
interface.  It’s a rich interface implemented by CoreCLR Execution
Engine.  The reader calls methods of that interface to resolve MSIL
tokens, get information about types, fields, code locations and much
more.

## Main Classes

The two main classes comprising the reader are ReaderBase and GenIR.
GenIR derives from ReaderBase. ReaderBase encapsulates MSIL processing
and is IR-agnostic.  It operates on opaque classes such as IRNode,
FlowGraphNode, FlowGraphEdgeList, EHRegion.  GenIR implements
LLVM-specific functionality required by ReaderBase.  This separation
allows us to decouple msil processing from IR creation and makes the
code more maintainable and easier to evolve.  A legacy jit was
implemented using the same ReaderBase and a different implementation of
GenIR.

## Reader Driver

The main driver for the reader is ReaderBase::msilToIR.  The driver has
the steps below:

-   Execute [Pre-pass](#pre-pass) to allow the client to do initialization

-   Get special debugger sequence points if necessary (currently not yet
    implemented).  This is not required for correct code execution.

-   Create EH region tree and set EH info (more details in [Exception
    Handling in the LLILC JIT](https://github.com/dotnet/llilc/blob/master/Documentation/llilc-jit-eh.md))

-   Store the generics context on the stack and report its location to
    the EE if necessary

-   Build flow graph in [First pass](#first-pass)

-   Execute [Second pass](#second-pass) that translates MSIL instructions

-   Remove unused flow graph nodes

-   Execute [Post-pass](#post-pass) to allow the client a chance to finish up

## Pre-pass

An instance of GenIR translates a single function.  GenIR::readerPrePass
is responsible for initial setup.  The steps it performs:

-   Initialize target pointer size

-   Create an llvm::Function object

-   Create an entry block

-   Generate alloca instructions for locals and parameters.  We do that
    to simplify IR creation.  LLVM requires all register values to be in
    SSA form but does not require memory objects to be in SSA form.  We
    rely on LLVM’s mem2reg to promote these alloca instructions to
    registers and construct SSA.  This technique is described in chapter
    7.3 of [this tutorial](http://llvm.org/docs/tutorial/LangImpl7.html).

-   Check whether the function has features that the reader hasn’t
    implemented yet

## First Pass

First pass is responsible for building LLVM basic blocks. The only instructions
that are inserted are terminating branches and switches. Blocks that end with
returns are empty after the first pass. That effectively creates the flow graph
of the function. fgBuildBasicBlocksFromBytes is the main driver.  The two main
steps are described below.

### Initial Basic Block Build-up

The first step is reading byte codes and creating basic blocks based on
branches, switches, and returns.  The targets of branches are temporary
blocks. The MSIL offsets corresponding to these temporary branch target
blocks are saved in NodeOffsetListArray.

### Branch Target Adjustment

Once the initial set of basic blocks is built, the algorithm finds the
real blocks corresponding to the branch target MSIL offsets and replaces
temporary target blocks with the real ones.  This may involve splitting a
basic block if a target of a branch has offset that’s in the middle of
the block.

## Second Pass

In the second pass the reader first walks the flow graph in depth-first preorder
(starting with the head block) to identify unused blocks.  Then the reader walks
the graph in the order of MSIL offsets (for blocks that propagate operand stack)
and translates MSIL instructions for each block.  Currently no sort on the
blocks is required since the first pass already creates the blocks propagating
operand stacks in MSIL offset order. No new control flow is introduced in this
pass except for exception checks (null checks, bounds checks, etc.) and
conditional helper calls.  Extra care should be taken with any changes to
control flow graph in the second pass to make sure they don't interfere with
dominance computation we do for class initialization.

ReaderBase::readBytesForFlowGraphNode\_Helper is the method that
iterates over basic block bytes.

### Type Translation

GenIR::getType is the main entry point for translating CorInfoTypes to
LLVM Types.

-   The translation of primitive types is straightforward.  All of
    CorInfoType primitive types have direct equivalents in LLVM type
    system.

-   nativeint and nativeuint are represented as IntegerType with the
    target pointer size.

-   We use addressspace to distinguish managed pointers (interior or object)
    from unmanaged ones.  Addressspace 0 is used for unmanaged pointers and
    addressspace 1 is used for managed pointers (interior or object).
    GenIR::getUnmanagedPointerType and GenIR::getManagedPointerType should be
    used for creating pointers.

-   We intend to use LLVM types to recover information about GC pointers
    in value types for GC info.

-   We represent value classes as LLVM Structs and reference classes as
    managed pointers to LLVM Structs.

-   We maintain two maps for ensuring that each class corresponds to a
    single LLVM Type: ClassTypeMap and ArrayTypeMap.  ClassTypeMap is
    indexed by CORINFO\_CLASS\_HANDLE and is used for non-array types.
    ArrayTypeMap is indexed by `<element type, element handle, array
    rank, is vector>` tuple.  The reason for that is that two different
    CORINFO\_CLASS\_HANDLEs can identify the same array: the actual
    array handle and the handle for its MethodTable.

-   We construct LLVM Structs with accurate information about fields
    including vtable slot for objects and struct padding.  This allows us
    to use struct GEP instructions for accessing fields.

### MSIL Instruction Translation

GenIR is responsible for translating MSIL instructions to LLVM
instructions.  It uses IRBuilder apis.

The instructions currently implemented:

-   Constant loading (ldc variants)

-   Indirect loading (ldind variants)

-   Indirect storing (stind variants)

-   Method argument loading (ldarg variants)

-   Method argument address loading (ldarga variants)

-   Method argument storing (starg variants)

-   Local variable loading (ldloc variants)

-   Local variable reference loading (ldloca variants)

-   Local variable storing (stloc variants)

-   Arithmetical instructions (add, sub, mul, div, rem, neg and their
    variants)

-   Overflow arithmetical instructions (add.ovf, sub.ovf, mul.ovf and
    their variants)

-   Bitwise instructions (and, or, xor, not)

-   Shift instructions (shl, shr, shr.un)

-   Conversion instructions (conv variants)

-   Overflow conversion instructions (conv.ovf variants)

-   Logical condition check instructions (ceq, cgt, clt and their
    variants)

-   Unconditional branching instructions (br variants)

-   Conditional branching instructions (brfalse, brtrue and their
    variants)

-   Comparative branching instructions (beq, bne, bge, bgt, ble, blt and
    their variants)

-   The switch instruction

-   The ret instruction

-   Addressing fields (ldfld, ldsfld, ldflda, ldsflda, stfld, stsfld)

-   Manipulating class and value type instances (ldnull, ldstr, newobj,
    castclass, isinst, ldtoken, sizeof, box, ldobj, stobj, unbox, mkrefany,
    refanytype, refanyval)

-   Vector instructions (newarr, ldlen)

-   Calls (call, calli, ldftn, ldvirtftn, calls that
    require virtual stub dispatch, constrained virtual calls)

-   Method argument list (arglist)

-   Stack manipulation (nop, dup, pop)

-   Block operations (cpblk, initblk)

-   Local block allocation (localloc)

-   Debugging breakpoint (break)

<a name="Not implemented"></a>The instructions not yet implemented:

-   Calls (jmp, tail calls)

### Stack Maintenance

Each MSIL instruction may modify operand stack associated with its basic
block.  If the block is not empty on exit from the basic block, additional
processing is needed to make sure the block successors can use the
values on the predecessor’s stack.  GenIR::maintainOperandStack is
responsible for setting up successors’ operand stacks in this case.  For
successors of the current block such that the current block is their
only predecessor, the algorithm simply copies the predecessor’s stack.
For successors of the current block such that the current block is not
their only predecessor, the algorithm creates PHI instructions in the
successor blocks and pushes them on the operand stacks. It's possible that
the corresponding values from the operand stacks of the blocks
predecessors are of different types. [ECMA-335](http://www.ecma-international.org/publications/files/ECMA-ST/ECMA-335.pdf)
allows merging of the following values:
* float value with a double value (the result is of type double);
* nativeint value with an int32 value (the result is of type nativeint);
* two gc pointer values (the result is of the closest common supertype).
We need to process all of the block's predecessors in order to finalize the
types of the block's PHI instructions. That is the reason we process the nodes
that propagate operand stacks in the order of MSIL offsets.  Note that
[ECMA-335](http://www.ecma-international.org/publications/files/ECMA-ST/ECMA-335.pdf)
section III.1.7.5 prohibits non-empty operand stacks on backwards branches.

## Post-Pass

The post-pass inserts the necessary code for keeping generic context
alive and cleans up memory used by the reader.

## Support for Ngen

llilc will be used as a jit for native image generator (Ngen). The tool for generating
Ngen images in CoreCLR is crossgen.exe. crossgen.exe exposes the same jit interface as
the execution engine in normal jit compilation. The important difference is that handles
for strings, methods, etc. returned via jit interface in Ngen scenario are not resolved
addresses. These handles should be reported back to crossgen via recordRelocation method
along with the code locations referring to those handles.

## Future Work

-   [Implement remaining MSIL instructions.](#user-content-Not%20implemented)

-   [ReaderBase has some code to enable inlining in the reader. We need
    to decide whether we do inlining in the reader or in a subsequent
    pass and update the reader accordingly.](https://github.com/dotnet/llilc/issues/239)

-   Possibly enable a limited set of reader-time optimizations (like
    [avoiding redundant class initialization](https://github.com/dotnet/llilc/issues/38),
    [using pc-relative call forms](https://github.com/dotnet/llilc/issues/296),
    [replacing readonly static field loads with constants](https://github.com/dotnet/llilc/issues/294),
    [deferring lowering of certain constructs](https://github.com/dotnet/llilc/issues/292),
    [hot/cold options for loading strings](https://github.com/dotnet/llilc/issues/286)).

-   [Produce more precise aliasing annotations.](https://github.com/dotnet/llilc/issues/291)

-   [Recognize vectorizable types and emit proper vector IR.](https://github.com/dotnet/llilc/issues/323)

-   [Support synchronized methods.](https://github.com/dotnet/llilc/issues/271)

-   [Handle methods with security checks.](https://github.com/dotnet/llilc/issues/301)

-   [Support intrinsics.](https://github.com/dotnet/llilc/issues/281)

-   [NGEN: record relocations via Jit Interface.] (https://github.com/dotnet/llilc/issues/655)

-   [NGEN: verify that we process handle indirections correctly.] (https://github.com/dotnet/llilc/issues/656)

-   [NGEN: GS cookie constant needs to be accessed through an indirection.] (https://github.com/dotnet/llilc/issues/658)
