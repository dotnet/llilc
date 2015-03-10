#!/usr/bin/env python

import sys
import argparse
import os
import subprocess
import platform
import StringIO
import string
import difflib

def expandPath(path):
    return os.path.abspath(os.path.expanduser(path))

def runTidy(args):
  if args.llilc_source == None:
    print >> sys.stderr, "Please specify --llilc-source or set the " \
                          "LLILC_SOURCE environment variable."
    return 1

  tidyFix = "-fix" if args.fix else ""

  osInc = ""
  osIncList = []

  if os.environ.get('INCLUDE') != None:
    osInc = os.environ['INCLUDE']

  if osInc != "":
    osIncList = osInc.split(';')

  clangArgs = ""
  if args.compile_commands is None:
    # TODO: platform checks and defines

    if args.llvm_source is None:
      print >> sys.stderr, "Please specify --llvm-source or set the " \
                           "LLVM_SOURCE environment variable."
      return 1

    if args.llvm_build is None:
      print >> sys.stderr, "Please specify --llvm-build or set the " \
                           "LLVM_BUILD environment variable."
      return 1

    llilcSrc = expandPath(args.llilc_source)
    llilcBuild = ""
    if args.llilc_build != None:
      llilcBuild = expandPath(args.llilc_build)
    else:
      llilcBuild = expandPath(os.path.join(args.llvm_build, "tools", "llilc"))
    llilcLib = os.path.join(llilcSrc, "lib")
    llilcInc = os.path.join(llilcSrc, "include")

    flags = [
        "-target x86_64-pc-win32-msvc",
        "-fms-extensions",
        "-fms-compatibility",
        "-fmsc-version=1800"
    ]
    includes = [
        os.path.join(llilcLib, "Jit"),
        os.path.join(llilcLib, "Reader"),
        llilcInc,
        os.path.join(expandPath(args.llvm_source), "include"),
        os.path.join(llilcInc, "clr"),
        os.path.join(llilcInc, "Driver"),
        os.path.join(llilcInc, "Jit"),
        os.path.join(llilcInc, "Reader"),
        os.path.join(llilcInc, "Pal"),
        os.path.join(llilcBuild, "lib", "Reader"),
        os.path.join(llilcBuild, "include"),
        os.path.join(expandPath(args.llvm_build), "include")
    ]
    defines = [
        "_DEBUG",
        "_CRT_SECURE_NO_DEPRECATE",
        "_CRT_SECURE_NO_WARNINGS",
        "_CRT_NONSTDC_NO_DEPRECATE",
        "_CRT_NONSTDC_NO_WARNINGS",
        "_SCL_SECURE_NO_DEPRECATE",
        "_SCL_SECURE_NO_WARNINGS",
        "__STDC_CONSTANT_MACROS",
        "__STDC_FORMAT_MACROS",
        "__STDC_LIMIT_MACROS",
        "_GNU_SOURCE",
        "LLILCJit_EXPORTS",
        "_WINDLL",
        "_MBCS",
        "NOMINMAX"
    ]

    clangArgs = " ".join(["--"] + flags 
                         + ["-I" + i for i in includes] \
                         + ["-isystem" + i for i in osIncList] \
                         + ["-D" + d for d in defines])
  else:
    clangArgs = " ".join(["-p", expandPath(args.compile_commands)])

  returncode = 0
  for dirname,subdir,files in os.walk(llilcSrc):
    for filename in files:
      if filename.endswith(".c") or filename.endswith(".cpp"):
        filepath = os.path.join(dirname, filename)
        errorlevel = subprocess.call(" ".join([args.clang_tidy, tidyFix,
            "-checks=" + args.checks,
            "-header-filter=\"" + llilcSrc + ".*(Reader)|(Jit)|(Pal)\"",
            filepath, clangArgs]), shell=True)
        if errorlevel == 1:
          returncode = 1
  
  return returncode

def runFormat(args):
  formatFix = "-i" if args.fix else ""
  returncode = 0
  llilcSrc = expandPath(args.llilc_source)

  if args.formatall:
    llilcSrc = expandPath(args.llilc_source)
    for dirname,subdir,files in os.walk(llilcSrc):
      if ".git" in dirname \
          or dirname == os.path.join(llilcSrc, "include", "clr"):
        continue
      for filename in files:
        if filename.endswith(".c") or filename.endswith(".cpp") or \
          filename.endswith(".h"):
          filepath=os.path.join(dirname, filename)
          proc = subprocess.Popen(" ".join([args.clang_format, filepath, 
              formatFix, "2>" + os.devnull]), shell=True, stdout=subprocess.PIPE)
            
          output,error = proc.communicate()
          
          # Compute the diff if not fixing
          if not args.fix:
            with open(filepath) as f:
              code = f.read().splitlines()
            formatted_code = StringIO.StringIO(output).read().splitlines()
            diff = difflib.unified_diff(code, formatted_code,
                                        filepath, filepath,
                                        '(before formatting)', '(after formatting)')
            diff_string = string.join(diff, '\n')
            if len(diff_string) > 0:
              # If there was a diff, print out the file name.
              print(filepath)
              if args.print_diffs:
                sys.stdout.write(diff_string)
              returncode = -1
  else:
    noindex = ""

    # base and no-index are mutually exclusive
    base = "" if args.noindex else args.base
    
    if args.noindex:
      noindex = "--no-index"
      if args.left == "" or args.right == "":
        print("User must specify paths to repositories to be diffed with --no-index")
        print("usage: ccformat.py --no-index --left <path to left> --right <path to right>")
        return -2

    proc = subprocess.Popen(" ".join(["git", "diff", base, "-U0", noindex,
        args.left, args.right, "2>" + os.devnull, "|", args.clang_format_diff, 
        "-p1", formatFix]), shell=True, stdout=subprocess.PIPE)

    output,error = proc.communicate()
    if output != "":
      if args.print_diffs:
        sys.stdout.write(output)
      returncode = -1

  if returncode == -1:
    print "There were formatting errors. Rerun with --fix"
  return returncode

def main(argv):
  llvmBuild = os.environ.get("LLVMBUILD")
  parser = argparse.ArgumentParser(description=
                   "Report and optionally fix formatting errors in the LLILC "
                   "sources.",
                   formatter_class=argparse.ArgumentDefaultsHelpFormatter)
  parser.add_argument("--clang-tidy", metavar="PATH",
            default="clang-tidy", help="path to clang-tidy binary")
  parser.add_argument("--clang-format-diff", metavar="PATH",
            default="clang-format-diff.py",
            help="path to clang-format-diff tool")
  parser.add_argument("--clang-format", metavar="PATH",
            default="clang-format", help="path to clang-format binary")
  parser.add_argument("--compile-commands", metavar="PATH",
            help="path to a directory containing a JSON compile commands " \
                 "database used to determine clang-tidy options")
  parser.add_argument("--llvm-build", metavar="PATH",
            default=llvmBuild,
            help="path to LLVM builds. If not specified, defaults to the " \
                  "value of the LLVMBUILD environment variable. Only used " \
                  "when a compile commands database has not been specified.")
  parser.add_argument("--llvm-source", metavar="PATH",
            default=os.environ.get("LLVMSOURCE"),
            help="path to LLVM sources. If not specified, defaults to the " \
                  "value of the LLVMSOURCE environment variable. Only used " \
                  "when a compile commands database has not been specified.")
  parser.add_argument("--llilc-build", metavar="PATH",
            default=None if llvmBuild is None else \
                os.path.join(llvmBuild, "tools", "llilc"), \
            help="path to LLILC build. Only used when a compile commands " \
                 "database has not been specified.")
  parser.add_argument("--llilc-source", metavar="PATH",
            default=os.path.dirname(os.path.dirname(expandPath(__file__))),
            help="path to LLILC sources")
  parser.add_argument("--fix", action="store_true", default=False,
            help="fix failures when possible")
  parser.add_argument("--untidy", action="store_true", default=False,
            help="Don\'t run clang-tidy")
  parser.add_argument("--noformat", action="store_true", default=False,
            help="Don\'t run clang-format-diff")
  parser.add_argument("--checks", default="llvm*,misc*,microsoft*,"\
                      "-llvm-header-guard,-llvm-include-order",
            help="clang-tidy checks to run")
  group = parser.add_mutually_exclusive_group()
  group.add_argument("--base", metavar="BRANCH", default="origin",
            help="Base for obtaining diffs")
  group.add_argument("--noindex", action="store_true", default=False,
            help="Run git diff with --no-index to compare two paths")
  parser.add_argument("--formatall", action="store_true", default=False,
            help="Run clang-format on all files")
  parser.add_argument("--left", default="", 
            help="Path to compare against, used with --noindex")
  parser.add_argument("--right", default="", 
            help="Path to be compared, used with --noindex")
  parser.add_argument("--print-diffs", action="store_true", default=False,
            help="Print formatting diffs if there are diffs")
  args = parser.parse_args(argv)

  returncode=0
  if not args.untidy:
    returncode = runTidy(args)
    if returncode != 0:
      return returncode

  if not args.noformat:
    returncode = runFormat(args)
  
  return returncode

if __name__ == "__main__":
  returncode = main(sys.argv[1:])
  sys.exit(returncode)
