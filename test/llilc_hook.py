#!/usr/bin/env python
#
# Title               :llilc_hook.py
# Description         :
#
# Hook is called from runjs which during its build step requires llilc specific
# setup. Currently all that is required is just a simple copy of llilcjit.dll. 
# The script requires two arguments to function the path of llilcjit.dll and the 
# bin path to copy into.
#
# Usage: llilc_hook.py <llilcjit.dll path> <run.js bin location>
#
################################################################################

import os
import shutil
import sys

################################################################################

def copydll(llilc_dll, drop_path):
    print "Copying %s into %s." % (llilc_dll, drop_path)
      
    shutil.copy2(llilc_dll, drop_path)
   
    print "Copied successfully."

def handle_args(args):
    if (len(args) < 2):
        print "Incorrect number of arguments."
      
        print_usage()
      
        return None, None
      
    if (os.path.isfile(args[1]) is not True):
        print "lliljit.dll must be a valid file."
      
        print_usage()
      
        return None, None
     
    if (os.path.isdir(args[2]) is not True):
        print "run.js bin must be a valid directory."
      
        print_usage()
      
        return None, None
      
    return args[1], args[2]
   
def print_usage():
   
    # User has run the script incorrectly
   
    print "Hook to copy llilcdll.jit during runjs run."
    print ""
    print "Usage: llilcHook.py <llilcjit.dll path> <runjs bin location>"
    print ""

if __name__ == "__main__":
   
    print "Starting LLILC Hook: Last Updated - 26-Jul-15"
  
    # Expect two arguments, the first is the path to
    # llilc and the other is the directory to copy into.
   
    llilc_path, drop_path = handle_args(sys.argv)
   
    if (llilc_path is None or drop_path is None):
        sys.exit(-1)
   
    # Copy the dll
    copydll(llilc_path, drop_path)
