import sys
import getopt
import os
import subprocess
import shutil

llilcSource = ""
llvmSource = ""
llvmBuild = ""
llilcLib = ""
llilcInc = ""
clrInc = ""
llvmInc = ""
formatFix = ""
tidyFix = ""
doFormat = True
tidy = True
base = "origin"
tidyChecks = "llvm*,misc*,microsoft*"

def checkEnvironment():
  if os.environ.get('LLVMSOURCE') == None:
    print "No LLVMSOURCE"
    return 2
  else:
    global llvmSource
    llvmSource = os.environ['LLVMSOURCE']

  if os.environ.get('LLVMBUILD') == None:
    print "No LLVMBUILD"
    return 2
  else:
    global llvmBuild
    llvmBuild = os.environ['LLVMBUILD']

  global llilcSource
  if os.environ.get('LLILCSOURCE') == None:
    llilcSource = os.environ['LLVMSOURCE'] + "\\tools\llilc"
  else:
    llilcSource = os.environ['LLILCSOURCE']

  global llilcLib 
  llilcLib = llilcSource + "\lib"
  global llilcInc
  llilcInc = llilcSource + "\include"
  global clrInc
  clrInc = llilcInc + "\clr"
  global llvmInc
  llvmInc = llvmSource + "\include"

  return 0

def parseArgs(argv):
  try:
    opts, args = getopt.getopt(argv, "h", ["fix", "checks=", "base=", "help",
        "untidy", "noformat"])
  except getopt.GetoptError as err:
    print err
    return 3

  for opt, arg in opts:
    if opt in ("--fix"):
      global formatFix
      formatFix = "-i"
      global tidyFix
      tidyFix = "-fix"
    elif opt in ("--untidy"):
      global tidy
      tidy = False
    elif opt in ("--noformat"):
      global doFormat
      doFormat = False
    elif opt in ("--checks"):
      global tidyChecks
      tidyChecks = arg
    elif opt in ("--base"):
      global base
      base = arg
    elif opt in ("-h", "--help"):
      return usage()
  
  return 0

def usage():
  print "Usage: python ccFormat.py [--checks] [--fix] [-h]"
  print "\t--fix\t\tFix failures when possible"
  print "\t--untidy\t\tDon't run clang-tidy"
  print "\t--noformat\t\tDon't run clang-format"
  print "\t--checks=<chk>\tClang Tidy checks to run"
  print "\t--base=<base>\tBase for obtaining diffs"
  print "\t--help\t\tDisplay this help message."

  print ""
  print "Requirements:"
  print "\tEnvirontment variables: LLVMSOURCE, LLVMBUILD"
  print "\tTools (on path): clang-format.exe, clang-tidy.exe " \
      + "clang-format-diff.py"
  print "\tTool dependencies: Python 2.7 or later"
  print ""
  return 4

def runTidy():
  if not tidy:
    return 0

  inc = ""
  tempinc = ""

  if  os.environ.get('INCLUDE') != None:
    tempinc = os.environ['INCLUDE']

  if tempinc != "":
    tempincList = tempinc.split(';')
    for item in tempincList:
      inc = inc + "-issytem " + item

  flags = "-target x86_64-pc-win32-msvc -fms-extensions -fms-compatibility \
      -fmsc-version=1800"
  inc = inc + " -I" + llilcLib + "\Jit -I" + llilcLib + "\MSILReader -I" \
      + llilcInc + " -I" + llvmInc + " -I" + clrInc + " -I" + llilcInc \
      + "\Driver -I" + llilcInc + "\Jit -I" + llilcInc + "\Reader -I" \
      + llilcInc + "\Pal -I" + llvmBuild + "\\tools\LLILC\lib\Reader -I" \
      + llvmBuild + "\\tools\LLILC\include -I" + llvmBuild + "\include"
  setDef = "-D_DEBUG -D_CRT_SECURE_NO_DEPRECATE -D_CRT_SECURE_NO_WARNINGS " \
      + "-D_CRT_NONSTDC_NO_DEPRECATE -D_CRT_NONSTDC_NO_WARNINGS " \
      + "-D_SCL_SECURE_NO_DEPRECATE -D_SCL_SECURE_NO_WARNINGS " \
      + "-D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS " \
      + "-D__STDC_LIMIT_MACROS -D_GNU_SOURCE -DLLILCJit_EXPORTS -D_WINDLL " \
      + "-D_MBCS -DNOMINMAX"

  currentDirectory = os.getcwd()
  os.chdir(llilcSource)

  basedir = os.getcwd()
  returncode = 0

  for dirname,subdir,files in os.walk(basedir):
    for filename in files:
      if filename.endswith(".c") or filename.endswith(".cpp"):
        errorlevel = subprocess.call("clang-tidy " + tidyFix + " -checks="
            + tidyChecks + " -header-filter=\"LLILC.*(Reader)|(Jit)|(Pal)\" "
            + dirname + "\\" + filename + " -- " + flags + " " + inc + " " 
            + setDef, shell=True)
        if errorlevel == 1:
          returncode = 1
  
  os.chdir(currentDirectory)
  return returncode

def runFormat():
  if not doFormat:
    return 0

  subprocess.call("git diff " + base + "-U0 2>nul | clang-format-diff.py -p1 "
      + formatFix, shell=True)
  return 0

def main(argv):
    returncode = parseArgs(argv)
    if returncode != 0:
        return returncode
    
    returncode = checkEnvironment()
    if returncode != 0:
        return returncode

    returncode = runTidy()
    runFormat()
    
    return returncode

if __name__ == "__main__":
  returncode = main(sys.argv[1:])

  if returncode == 4:
    # if we return 4, we printed the help message and should just quit,
    # but not fail
    returncode = 0

  sys.exit(returncode)
