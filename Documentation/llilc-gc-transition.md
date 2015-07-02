# GC Transition Support in LLILC

The CoreCLR runtime supports calls from managed (i.e. GC-aware) code to
unmanaged (i.e. GC-unaware) code. In the runtime's parlance, such calls are
termed "P/Invokes" (short for Platform Invoke); in LLILC/LLVM they are referred
to as "unmanaged calls" or "GC transitions". Because the target of such a call
is not aware of the CoreCLR GC, such calls require runtime-specific setup and
teardown code. The nature of this transition code requires code in both LLILC
and LLVM. A brief discussion of the specifics of GC transitions in CoreCLR and
the support for these transitions in LLILC and LLVM follows.

## GC transitions in CoreCLR

In order for the CoreCLR garbage collector to perform garbage collection it must
be able to
1. logically pause all threads that are running managed code, and
2. scan each thread's stack for GC references.

### Thread suspension and GC modes

To meet the first requirement above without physically suspending arbitrary
threads (and thus aggravating the chance of deadlocks in the runtime), each
thread that the runtime is tracking is assigned one of two modes: cooperative
mode and preemptive mode.

#### Cooperative mode

Threads in cooperative mode are considered to be running managed code. These
threads are therefore potentially modifying GC references, and must cooperate
with the runtime in order to reach a GC safepoint before a collection may
occur. All managed code (and almost all JIT compiled code) will run in
cooperative mode.

#### Preemptive mode

Threads in preemptive mode are considered to be running unmanaged code. Such a
thread obeys three invariants:
1. It must not currently be manipulating GC references, and all live GC
   references must be reachable from managed frames.
2. It must have registered the last managed frame on its stack with the runtime
   so that the GC may avoid scanning unmanaged frames.
3. It must not reenter managed code if a GC is in progress.

Threads that are running in preemptive mode need not be driven to a GC safe
point before a collection, as invariants (1) and (3) guarantee that the
managed code on such threads is already logically paused. Invariants (1) and
(2) serve to satisfy the requirement that the GC must be able to scan the
thread's stack for GC references.

### Transitioning from managed and unmanaged code

Transitioning from managed to unmanaged code boils down to switching the active
thread from cooperative to preemptive mode while ensuring that the invariants
associated with preemptive mode are obeyed. Abstractly, this looks something
like the following, where `>> {Managed,Unmanaged} code` indicates the type of
code currently running on the thread:

```
>> Managed code
- Ensure all GC references in registers that might be updated by unmanaged code
  have been spilled to locations that will not be updated by unmanaged code.
  This satisfies the first preemptive mode invariant.
- Register this frame as the last managed frame on this thread. This satisfies
  the second preemptive mode invariant.
- Flip this thread's GC mode to preemptive mode.

>> Unmanaged code
- Run unmanaged code. Usually this is just a call to an unmanaged function.
- Once the unmanaged code has run, flip this thread's GC mode to cooperative
  mode.

>> Managed code
- Check to see if a collection is in progress and if so, block this thread until
  it completes. This satisfies the third preemptive mode invariant.
```

## Supporting GC transitions in LLILC and LLVM

Ideally, LLILC would be able to emit all of the necessary transition code in
LLVM IR. Unfortunately, it is not possible to do so while preserving the
preemptive mode invariants:
- The frame registration, mode transitions, and collection rendezvous must be
  positioned immediately before and after the unmanaged code. LLVM IR has no
  way of expressing this restriction.
- Registering the managed frame requires knowing the address of the first
  managed instruction following the unmanaged code. Again, LLVM IR has no
  way of expressing this.

As a result, the gc.statepoint intrinsic has been extended to support marking
certain statepoints as transitions between code with differing GC models.
These calls are then lowered differently by the backend, which is given an
opportunity to lower the GC transition prologue and epilogue in a
GC-model-specific fashion. In the case of CoreCLR, the backend is responsible
for emitting the frame registration, mode transitions, and the collection
rendezvous, while the frontend is responsible for allocating (and partially
initializing) the frame registration data structure and marking calls to
unamanged code as GC transitions. Example IR is given below.

```
!0 = !{!"rsp"}
!1 = !{!"rbp"}

void @DomainNeutralILStubClass.IL_STUB_PInvoke() gc "coreclr" {
  %InlinedCallFrame = alloca [64 x i8] ; Frame registration data structure for CoreCLR
  %ThreadPointer = alloca [0 x i8]*    ; Pointer to current CoreCLR thread

  ; Get the current thread of execution
  %0 = getelementptr inbounds [64 x i8], [64 x i8]* %InlinedCallFrame, i32 0, i32 8
  %1 = call [0 x i8]* inttoptr (i64 140505971852800 to [0 x i8]* (i8*, i64)*)(i8* %0, i64 %param2)
  store [0 x i8]* %1, [0 x i8]** %ThreadPointer

  ; Record the stack pointer in the frame registration data
  %2 = getelementptr inbounds [64 x i8], [64 x i8]* %InlinedCallFrame, i32 0, i32 40
  %3 = bitcast i8* %2 to i64*
  %4 = call i64 @llvm.read_register.i64(metadata !0)
  store i64 %4, i64* %3

  ; Record the frame pointer in the frame registration data
  %5 = getelementptr inbounds [64 x i8], [64 x i8]* %InlinedCallFrame, i32 0, i32 56
  %6 = bitcast i8* %5 to i64*
  %7 = call i64 @llvm.read_register.i64(metadata !1)
  store i64 %7, i64* %6

  ; Push the current frame onto the frame registration stack
  %10 = getelementptr inbounds [64 x i8], [64 x i8]* %InlinedCallFrame, i32 0, i32 8
  %11 = load [0 x i8]*, [0 x i8]** %ThreadPointer
  %12 = getelementptr inbounds [0 x i8], [0 x i8]* %11, i32 0, i32 16
  %13 = bitcast i8* %12 to i8**
  store i8* %10, i8** %13

  ; Compute addresses for GC transition code
  %14 = getelementptr inbounds [64 x i8], [64 x i8]* %InlinedCallFrame, i32 0, i32 48
  %15 = bitcast i8* %14 to i8**
  %16 = getelementptr inbounds [0 x i8], [0 x i8]* %11, i32 0, i32 12
  %17 = load i64, i64* inttoptr (i64 140505987192288 to i64*)

  ; Perform the call.
  ;
  ; The fifth argument (i32 1) marks this statepoint as a GC transition. The
  ; backend will generate code to spill callee-saves, perform the GC mode
  ; switch, and check for a collection before reentering managed
  ; code. The four arguments immediately preceding the final argument provide
  ; data that is needed by the transition code.
  call void (i64, i32, void()*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_isVoidf(i64 0, i32 0, void ()* null, i32 0, i32 1, i32 4, i8** %15, i8* %16, i32* inttoptr (i64 140505987311808 to i32*), i64 %17, i32 0)

  ; Pop the current frame from the registration stack
  store i8* null, i8** %15
  %18 = getelementptr inbounds [64 x i8], [64 x i8]* %InlinedCallFrame, i32 0, i32 16
  %19 = bitcast i8* %18 to i8**
  %20 = load i8*, i8** %19
  store i8* %20, i8** %13

  ret void
}
```
