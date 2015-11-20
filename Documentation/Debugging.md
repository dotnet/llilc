# Debugging LLILC

## Background
Currently LLILC is used as a Just-in-time compiler. This means
that as a test is being run control passes between these agents:

* The CoreRun.exe or CoreConsole.exe programs are invoked from 
  the Windows command line (or programatically). These then
  start up 
* The CoreClr which includes
  * The CLR Virtual Machine
  * The CLR JIT (RyuJit)
  * Runtime support routines that may be called by JITed code.
* The LLILC JIT, which mostly replaces RyuJit when the 
  COMPlus_AltJit options are specified.
* The LLVM compilation system.
* The code that was produced by LLVM on behalf of LLILC.

Execution starts in the CLR which first sets up the execution
environment, e.g. creating an APPDomain. The process of doing
this involves executing some managed code so the JIT is
called to compile this code which is then executed. 

After the execution environment has been setup the CLR
loads the test assembly and starts the test execution by
first requesting that the JIT compile the main program entrypoint
and then by passing control to the entrypoint.

Control then alternates between the Jit and the code generated
by the JIT, with the CLR managing the transitions between them.

Bugs may be present in any of the above agents but we will 
classify them as either:

1. Compile-time failure bugs: These are bugs that cause LLILC code
   to terminate, or give unexpected error messages.
   It includes hitting features that are not-yet-implemented.
2. Compile-time translation bugs: These are bugs in LLILC
   translation to LLVM that result in bad code.
3. LLVM bugs: These are bugs in LLVM that result in bad code.
4. Runtime bugs: These are bugs in the code generated
   by LLVM on behalf of LLILC, due either to bugs in LLILC
   or LLVM. Or these could be caused by bugs in RyuJit
   for methods it compiled (see below), though this
   is less likely.
5. Environmental bugs: These are bugs in the runtime library,
   or in the CLR. 
   
Debugging these problems will generally require running
LLILC under the control of a debugger. However there are
environment variables that can be set to cause LLILC
to output useful diagnostic information as it run, so
that information may be sufficient to diagnose some
problems without use of the debugger.

## LLILC Setup

Currently LLILC runs as an alternate JIT for the CoreClr, 
with RyuJit being the primary (or backup) JIT. The 
way this works is that environment variables
(described below) specify which methods LLILC should 
attempt to compile. When LLILC attempts to compile a 
method it may either succeed (or at least think it has
succeeded) or fail. Methods which LLILC has either
not attempted to compile, or which LLILC attempted
but failed to compile, are compiled by RyuJit.

### Method Set Syntax
Some of the environment variables mentioned below
take a string value that is interpreted as specifying
a set of methods. The syntax for such specifications
is given by the following grammar, which follows
the usual EBNF convention:

* Items surrounded by "{" and "}" are repeated
  zero or more times.
* Items surrounded by "[" and "]" are optional
* Alternates are separated by "|"
* Literal characters are enclosed in double quotes.
* Upper-case names are used for non-terminals
* Lower-case names are used for lexical entities
* Lexical entities are specified using regular expressions.

```
MethodSet = MethodId { whiteSpace MethodId }
MethodId = "*"
     | [ ClassName ":" ] MethodName [ "(" NumArgs ")" ]

ClassName = token
MethodName = token
NumArgs = { digit }
whitespace = {[ \t]}
digit = [0-9]
token = {[^ :()]}
```

The methods represented by a MethodSet are the union
of those specified by the MethodIds, or
all methods if "*" is given.

A non-"*" MethodId may have three components:

* The ClassName of the method. This can be empty if
  the MethodId starts with a ":"
* The name of method within the class. this can be
  empty if there is a preceding ":" or following
  "(". 
* A specified number of arguments.

Any combination of these may be absent (but not all of them). 

A method that is a candidate for compilation is matched
against each MethodId of the MethodSet. If any of them
give a match the method is considered a member of the set.

A method is matched against a non-"*" MethodId by comparing
its class name, method-name, and number of arguments
against those specified in the MethodId. There is a
mismatch only if a property specified in the MethodId
does not match the method's property. 

If an environment variable whose value is logically 
a MethodSet is either not given, or else has an empty 
string as value, then the corresponding method set
is empty.

### Environment Variables Controlling LLILC
The environment variables controlling LLILC are the following.
The environemnt variable names are not case-sensitive,
however we use some capitalization for the sake of readability.
All of the environment variables must start with the 
"COMPlus_" prefix, otherwise the CLR will ignore them.

* COMPlus_AltJit. This is a MethodSet. Its conventional value is
  "*" which means compile all methods with LLILC. Note that if
  this environment variable is not set LLILC will not even be 
  loaded by the CLR and RyuJit will be used for all compilation.
* COMPlus_AltJitName. This is the file name of the LLILC JIT
  dll including its ".dll" suffix. To be used the LLICL JIT
  dll must be placed in the same directory as the CLR,
  so the name does not include any directory part. 
  The name of this dll as built is "llilc.dll". However
  some of our tools insert a time-stamp in the name when
  copying it to the CLR directory. 
* COMPlus_GCConservative, if non-null and non-empty,
  use conservative garbage collection.
* COMPlus_InsertStatePoints, if non-null and non-empty,
  use precise garbage collection. 
* COMPlus_ZapDisable. If set to 1 means CLR should not use
  precompiled NGen methods but require that all methods be
  JITed. 
* COMPlus_DumpLLVMIR may be given the value of either
  "summary" or "verbose" (case insensitive). If either
  is specified then LLILC reports on the success or failure
  of JITTING each method. If the method failed to compile
  a reason is given. If "verbose" is specified then in 
  addition the LLVM IR is dumped for every method.
* COMPlus_JitGCInfoLogging, if non-null and non-empty, 
  GCInfo encoding logs should be emitted
* COMPlus_ALtJitExclude is a MethodSet. LLILC will only
  attempt to compile methods which are in the COMPlus_AltJit
  set, but not in the COMPlus_ALtJitExclude set.
* COMPlus_AltJitBreakAtJitStart is a MethodSet. This is used
  when running LLILC under the control of a debugger.
  It causes a "break" message to be written for
  matching methods when the method is about to be
  jitted. The message is written from the 
  LLILCJit::compileMethod method in file
  llilc\lib\Jit\LLILCJit.cpp. The user must put
  a breakpoint on the statement the write
  out the message in order to actually get a
  break.    
* COMPlus_ALtJitMSILDump is a MethodSet. The MSIL for
  methods in the set is dumped before JITTING the 
  method. 
* COMPlus_ALtJitLLVMDump is a MethodSet. The LLVM IR for
  methods in the set is dumped just after LLILC has
  finished reading the MSIL and converting it to LLVM IR.
* COMPlus_ALtJitCodeRangeDump is a MethodSet. For methods
  in the set, the starting and ending address of the method's
  code, and the code size, is printed after the method has
  been JITTED. This can be useful in identifying which
  method contains a given address.
* COMPlus_SIMDIntrinc, if non-null and non-empty, 
  use SIMD intrinsics.
* COMPlus_AltJitOptions. If specified, this contains
  options that are passed to the LLVM backend via its
  cl::ParseEnvironmentOptions method.

### Environment Variables Affecting the CoreCLR
There are a large number environment variables that
can affect the CoreCLR. These are described in the
"coreclr\src\inc\clrconfigvalues.h" file.

### Environment Variables Affecting the CoreRun.exe
When CoreRun.exe is used to run a managed program
the following environment variables are consulted:

* CORE_ROOT, if specified, is used to find the coreclr.dll
  and related dlls.
* CORE_LIBRARIES, if specified, is used to find additional
  assemblies that may be required by the program that
  is executing. It is a semi-colon-separated list of
  directories in which to look for the assemblies.

## Running A Single Test from the command line
To run a single test from the command line, use the
python script llilc_run.py. Assuming your 
current directory is at the LLILC root, use

```
python test\llilc_run.py -h
```

to get help on the options the script takes.

## Running A Single Test using Visual Studio
To run a single test from Visual Studio, use the
python script make_sln.py to make a solution
file. Assuming your current directory is at the LLILC root, use

```
python test\make_sln.py -h
```

to get help on the options the script takes.  The solution is setup to
run the test you specify with environment variables set to specify
LLILC as the alternate JIT. The solution file has the full set of
environment variables that LLILC recognizes, but by default with empty
values.  You can then change these as desired.  If you specify the
--invoke-vs option then Visual Studio will be invoked to open the
solution after creating it.

By default the solution has the debugger type set to
"Managed (CoreClr)". This is the setting that lets you do 
source-level debugging of the application. To set breakpoints
and step in the LLILC code you should set this to "Auto" instead.

## Runing A Single Test using windbg
You can also use llilc_run.py to run a single test using windbg.
To do that, supply the --windbg-and-args option followed
by the full path to windbg and any parameters you want to
pass to windbg. 

