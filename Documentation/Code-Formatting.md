# Code Formatting

The LLILC project is structured as a subproject of LLVM so we are using the
same coding conventions as LLVM. See the [coding convention document](llilc-Coding-Conventions-and-Commenting-Style.md)
for more details.  To maintain these conventions, we require all contributors
to run our code formatting script over their changes before submitting a pull
request. The code formatting script is found at LLILC\utils\ccformat.py.

## Prerequisites

* clang-format should be on your path. clang-format can either be installed
  by downloading Clang's prebuilt binaries from LLVM directly (http://llvm.org/releases/download.html)
  or building clang from source.

## Checking Formatting

To check the formatting of any submissions, contributors should run from
their main LLILC enlistment directory:

> utils\ccformat.py

This will run clang-format.exe over all of the LLILC source, and print any
formatting errors that it found.

If you do not have clang-format on your path, you can specify where the
script should look for clang-format:

> utils\ccformat.py --clang-format \<full path to clang-format\>

If you do not want to see the diffs, but only want to know if you have
formatting errors, you can run with --hide-diffs:

> utils\ccformat.py --hide-diffs

## Fixing Formatting errors

If ccformat.py informs you that there are diffs between your code and the
formatted version returned by clang-format, contributors need to run with
--fix to automatically fix formatting errors:

> utils\ccformat.py --fix

## Running clang-tidy

Currently, we do not run clang-tidy by default with ccformat.py. If a
contributor would like to run clang-tidy, he can run ccformat.py with --tidy:

> utils\ccformat.py --tidy

There are various options contributors can pass to clang-tidy which can be
seen by running:

> utils\ccformat.py --help

## Running ccformat without clang-format

Contributors may choose to run ccformat.py without running clang-format
(when running tidy, for example). To do so, run ccformat.py with the
--noformat switch:

> utils\ccformat.py --noformat
