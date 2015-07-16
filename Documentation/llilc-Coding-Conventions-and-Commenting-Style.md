# LLILC Coding Guidelines

## General Coding Guidelines

The LLILC project is structured as a subproject of LLVM so we are using the
same coding conventions as LLVM. The LLVM coding conventions are at
<http://llvm.org/docs/CodingStandards.html>.  We use clang-format to
check/enforce some formatting rules, as described on our 
[code formatting document](Code-Formatting.md).

## Commenting Guidelines

In particular <http://llvm.org/docs/CodingStandards.html#commenting> has
guidelines for commenting code and using Doxygen markup. For a complete
description of Doxygen markup see <http://www.doxygen.org/manual/docblocks.html>.

We can summarize these as follows:

* Use the "///" style of comment unless that file is written in C or may be
  included into a C file.

* For Doyxygen markup, use the "\" form, e.g. \brief, rather than the "@"
  form, e.g. @brief.

* Use the \file command to turn the standard file header into a file-level
  comment.

* Include descriptive \brief paragraphs for all public interfaces (public
  classes, member and non-member functions). Explain API use and purpose in
  \brief paragraphs, don’t just restate the information that can be inferred
  from the API name. Put detailed discussion into separate paragraphs.

* To refer to parameter names inside a paragraph, use the \p name command.
  Don’t use the \arg name command since it starts a new paragraph that
  contains documentation for the parameter.

* Wrap non-inline code examples in \code ... \endcode.

* To document a function parameter, start a new paragraph with the \param
  name command. If the parameter is used as an out or an in/out parameter,
  use the \param [out] name or \param [in,out] name command, respectively.

* To describe function return value, start a new paragraph with the \returns
  command

* For classes and class members that are declared in a header, put the Doxygen
  comments there. Otherwise put the comments in the implementation file.

* For declaration of fields of a class, if the declaration is short
  use the "//<" form of document comments at the end of the line,
  if the comment will fit on not more than two lines.
  If the comment would be longer than two lines, put the documentation
  comment before the declaration.
  
* When a documentation comment precedes a declaration, separate it from
  preceding declarations by a blank line.
  
* For virtual methods, put the documentation on the method declaration in the
  base class that first introduced the method. Do not put documentation on
  the method overrides. In the Doxygen output the comments from the base
  class method will also be shown for the overriding methods.

In addition to these recommendations from the LLVM coding standards I have
the following recommendations:

* Doxygen supports Markdown, so use it in your comments when it can do what
  you want. See <http://www.stack.nl/~dimitri/doxygen/manual/markdown.html>

* In order to provide greater logical clarity on the semantics of classes
  and methods I suggest using the Doxygen
    [\pre](http://www.stack.nl/~dimitri/doxygen/manual/commands.html#cmdpre),
    [\post](http://www.stack.nl/~dimitri/doxygen/manual/commands.html#cmdpost),
    and [\invariant](http://www.stack.nl/~dimitri/doxygen/manual/commands.html#cmdinvariant)
    [special commands](http://www.stack.nl/~dimitri/doxygen/manual/commands.html)
    to specify method preconditions and postconditions and class invariants.

## Use of LLVM I/O APIs

Rather than using the printf family of output functions the LLILC team
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
