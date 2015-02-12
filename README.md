# MSILC Developers Guide
## Introduction
MSILC is the Microsoft code name for cross-platform .NET tools. 

It currently includes an LLVM-based Jit.

## MSILC Coding Guidelines
### Use of LLVM I/O APIs

Rather than using the printf family of output functions the MSILC team
uses the LLVML I/O APIs. This is a simplified form of C++ output.

- The base type is raw_ostream.
- Derived classes are:
  - raw_fd_ostream, for output to a file descriptor. Two of these are
    made and accessed as follows:
    - outs() is connected to stdout
    - errs() is connected to stderr.
    - dbgs() is a debug stream that is connected to stderr but which
      may optionally have circular buffering but by default it is
      an unbuffered connection to errs().
  - raw_string_ostream connects to a std::string given in its
    - constructor. Any output to that stream is appended to the
    end of the string.
- As with C++ the "<<" operator has overloads for outputting all the
  common data types.
- In addition the form "<< format(formatstring, args)" may be
  used to get the usual printf-stype formatting to a stream.
