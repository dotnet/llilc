# LLILC Bring up Milestones

To meet the overarching goal of fully functional JIT and AOT code 
generators we've broken out some intermediate milestones that we 
think are good steps along the way.  Each of these milestones represent 
a new level of functionality or robustness on the way to a production 
quality tool.

### Milestones
* **"Hello World"** - Using LLILC, JIT all the methods for runtime start-up
   and "Hello World" console app execution.  This is the classic first
   app. Success is that the CLR comes up, and prints "Hello World" and
   exits cleanly.
* **corefx tests on Windows** - Run corefx unit tests as an integration test. This
  tests a broad set of framework functionality. Success is a clean
  test run using run-test from the corefx repo.
* **corefx tests on Linux** - Same as above just on Linux.
* **JIT SelfHost on Linux** - Pass ~6k Jit SelfHost tests on Linux.
  These tests are currently being added to coreclr. Success will
  be running clean for the whole set.
* **JIT SelfHost Stress on Linux** - Pass ~6k JIT SelfHost tests under GCStress.
  This will do basic validation of the GC implementation.
* **Roslyn on Linux** - Full Roslyn compiling itself for CoreCLR on Linux with
  the JIT.
* **AOT Roslyn on Linux** - Full Roslyn compiling itself as a command line
  AOT tool.

We'll cross these off as we get through them but we're evaluating open issues 
with respect to how fixing them will advance these goals.

Note:  To add a milestone to this list open an issue to start the discussion,
 then create a PR for a community review.