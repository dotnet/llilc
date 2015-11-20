#!/usr/bin/env python

import sys
import argparse
import os
import subprocess
import platform
import io
import string
import urllib2
import shutil
import stat

def run(args):
  nugetFolder = os.path.join(args.target, ".nuget")
  print("\nEnsuring folder: %s" % nugetFolder )
  if not os.path.exists(nugetFolder):
    os.makedirs(nugetFolder)

  nugetExe = os.path.join(nugetFolder, "nuget.exe")
  if not os.path.exists(nugetExe):
    nugetOrg = "http://nuget.org/nuget.exe"
    print("Downloading... %s" % nugetOrg )
    response = urllib2.urlopen(nugetOrg)
    output = open(nugetExe,'wb')
    output.write(response.read())
    output.close()
    # Ensure it's executable
    st = os.stat(nugetExe)
    os.chmod(nugetExe, st.st_mode | stat.S_IEXEC)

  if (sys.platform != "win32"):
    # shutil.which can be used for python 3.3 or later, instead.
    for mono in ["/usr/bin/mono", "/usr/local/bin/mono"]:
      if os.path.exists(mono):
        monopath = mono
    if not monopath:
      raise "mono is required to run nuget.exe"
    nugetExe = monopath + " " + nugetExe

  nugetSpec = os.path.join(nugetFolder, os.path.basename(args.nuspec))
  if args.nuspec != nugetSpec:
    print("\nCopying " + args.nuspec + " to " + nugetSpec)
    shutil.copyfile(args.nuspec, nugetSpec)

  if args.json != None:
    nugetJson = os.path.join(nugetFolder, os.path.basename(args.json))
    if args.json != nugetJson:
      print("\nCopying " + args.json + " to " + nugetJson)
      shutil.copyfile(args.json, nugetJson)

  nugetCommand = nugetExe + " pack " + nugetSpec \
                 + " -NoPackageAnalysis -NoDefaultExcludes" \
                 " -OutputDirectory %s" % nugetFolder

  ret = os.system(nugetCommand)
  return ret

def main(argv):
  parser = argparse.ArgumentParser(description=
   "Download nuget and run it to create a package using the given nuspec. " \
   "Example: make_package.py " \
   "--target f:\llilc-rel\\bin\Release " \
   "--nuspec f:\llilc\lib\ObjWriter\.nuget\Microsoft.Dotnet.ObjectWriter.nuspec",
   formatter_class=argparse.ArgumentDefaultsHelpFormatter)
  parser.add_argument("--target", metavar="PATH",
            default=None,
            help="path to a target directory that contains files that will " \
                 "packaged")
  parser.add_argument("--nuspec", metavar="PATH",
            default=None,
            help="path to a nuspec file. This file is assumed to be under " \
                 "a child directory (.nuget) of the target by convetion")
  parser.add_argument("--json", metavar="PATH",
            default=None,
            help="path to a json file. This file is used to create " \
                 "a redirection package")
  args,unknown = parser.parse_known_args(argv)

  if unknown:
    print("Unknown argument(s): ", ", ".join(unknown))
    return -3

  returncode=0
  if args.target == None:
    print("--target is not specified.")
    return -3

  if args.nuspec == None:
    print("--nuspec is not specified")
    return -3

  returncode = run(args)

  return returncode


if __name__ == "__main__":
  returncode = main(sys.argv[1:])
  sys.exit(returncode)
