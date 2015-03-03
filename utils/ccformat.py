#!/usr/bin/env python

import sys
import argparse
import os
import subprocess
import platform

def runTidy(args):
  if args.llilc_source == None:
    print >> sys.stderr, "Please specify --llilc-source or set the " \
                          "LLILC_SOURCE environment variable."
    return 1

  tidyFix = "-fix" if args.fix else ""

  inc = ""
  tempinc = ""

  if os.environ.get('INCLUDE') != None:
    tempinc = os.environ['INCLUDE']

  if tempinc != "":
    tempincList = tempinc.split(';')
    for item in tempincList:
      inc = inc + "-issytem " + item

  clang_args = ""
  if args.compile_commands == None:
    # TODO: platform checks and defines

    if args.llvm_source == None:
      print >> sys.stderr, "Please specify --llvm-source or set the " \
                           "LLVM_SOURCE environment variable."
      return 1

    if args.llvm_build == None:
      print >> sys.stderr, "Please specify --llvm-build or set the " \
                           "LLVM_BUILD environment variable."
      return 1

    llilcLib = os.path.join(args.llilc_source, "lib")
    llilcInc = os.path.join(args.llilc_source, "include")
    clrInc = os.path.join(llilcInc, "clr")
    llvmInc = os.path.join(args.llvm_source, "include")

    flags = "-target x86_64-pc-win32-msvc -fms-extensions -fms-compatibility \
        -fmsc-version=1800"
    inc = inc + " -I" + os.path.join(llilcLib, "Jit") \
        + " -I" + os.path.join(llilcLib, "MSILReader") \
        + " -I" + llilcInc + " -I" + llvmInc + " -I" + clrInc \
        + " -I" + os.path.join(llilcInc, "Driver") \
        + " -I" + os.path.join(llilcInc, "Jit") \
        + " -I" + os.path.join(llilcInc, "Reader") \
        + " -I" + os.path.join(llilcInc, "Pal") \
        + " -I" + os.path.join(args.llvm_build, "tools", "LLILC", "lib", "Reader") \
        + " -I" + os.path.join(args.llvm_build, "tools", "LLILC", "include") \
        + " -I" + os.path.join(args.llvm_build, "include")
    setDef = "-D_DEBUG -D_CRT_SECURE_NO_DEPRECATE -D_CRT_SECURE_NO_WARNINGS " \
        + "-D_CRT_NONSTDC_NO_DEPRECATE -D_CRT_NONSTDC_NO_WARNINGS " \
        + "-D_SCL_SECURE_NO_DEPRECATE -D_SCL_SECURE_NO_WARNINGS " \
        + "-D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS " \
        + "-D__STDC_LIMIT_MACROS -D_GNU_SOURCE -DLLILCJit_EXPORTS -D_WINDLL " \
        + "-D_MBCS -DNOMINMAX"
    clang_args = "-- " + flags + " " + inc + " " + setDef
  else:
    clang_args = "-p " + args.compile_commands

  returncode = 0
  for dirname,subdir,files in os.walk(args.llilc_source):
    for filename in files:
      if filename.endswith(".c") or filename.endswith(".cpp"):
        filepath = os.path.join(dirname, filename)
        errorlevel = subprocess.call(args.clang_tidy_binary + " " + tidyFix
            + " -checks=" + args.checks
            + " -header-filter=\"" + args.llilc_source
            + ".*(Reader)|(Jit)|(Pal)\" " + filepath
            + " " + clang_args, shell=True)
        if errorlevel == 1:
          returncode = 1
  
  return returncode

def runFormat(args):
  formatFix = "-i" if args.fix else ""
  subprocess.call("git diff " + args.base + "-U0 2>nul | "
      + args.clang_format_diff + " -p1 " + formatFix, shell=True)
  return 0

def main(argv):
  parser = argparse.ArgumentParser(description=
                   "Report and optionally fix formatting errors in the LLILC "
                   "sources.")
  parser.add_argument("--clang-tidy-binary", metavar="PATH",
            default="clang-tidy", help="path to clang-tidy binary")
  parser.add_argument("--clang-format-diff", metavar="PATH",
            default="clang-format-diff.py",
            help="path to clang-format-diff tool")
  parser.add_argument("--compile-commands", metavar="PATH",
            help="path to a directory containing a JSON compile commands " \
                 "database used to determine clang-tidy options")
  parser.add_argument("--llvm-source", metavar="PATH",
            default=os.environ.get("LLVMSOURCE"),
            help="path to LLVM sources (only necessary when a compile " \
                 "commands database has not been specified)")
  parser.add_argument("--llvm-build", metavar="PATH",
            default=os.environ.get("LLVMBUILD"),
            help="path to LLVM build (only necessary when a compile commands " \
                 "database has not beenspecified)")
  parser.add_argument("--llilc-source", metavar="PATH",
            default=os.path.join(os.path.dirname(__file__), ".."),
            help="path to LLILC sources")
  parser.add_argument("--fix", action="store_true", default=False,
            help="fix failures when possible")
  parser.add_argument("--untidy", action="store_true", default=False,
            help="Don\'t run clang-tidy")
  parser.add_argument("--noformat", action="store_true", default=False,
            help="Don\'t run clang-format-diff")
  parser.add_argument("--checks", default="llvm*,misc*,microsoft*",
            help="clang-tidy checks to run")
  parser.add_argument("--base", metavar="BRANCH", default="origin",
            help="Base for obtaining diffs")
  args = parser.parse_args(argv)

  returncode=0
  if not args.untidy:
    returncode = runTidy(args)
    if returncode != 0:
      return returncode

  if not args.noformat:
    runFormat(args)
  
  return returncode

if __name__ == "__main__":
  main(sys.argv[1:])
