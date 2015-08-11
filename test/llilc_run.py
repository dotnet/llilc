#!/usr/bin/env python
#
# title           : llilcrun.py
# description     : Run a managed application using LLILC jit.
#
# This script expects to be run on Python 3.4 or later.
#
# usage: llilc_run.py [-h] [-d {verbose,summary}] [-x [EXTRA [EXTRA ...]]] [-n]
#                     [-p] [-v] [-r [CORERUN_AND_ARGS [CORERUN_AND_ARGS ...]]]
#                     [-j JIT_PATH] -a APP_PATH -c CORECLR_RUNTIME_PATH
#                     [ -- [APPLICATION_ARGUMENT ...] ]
# 
# Run a managed application with LLILC as the JIT. If the application has any
# arguments, they must be appended to the end of the command line, preceded by
# "--".
# 
# optional arguments:
#   -h, --help            show this help message and exit
#   -d {verbose,summary}, --dump-level {verbose,summary}
#                         the dump level: summary, or verbose
#   -x [EXTRA [EXTRA ...]], --extra [EXTRA [EXTRA ...]]
#                         list of extra COMPlus settings. Each item is
#                         Name=Value, where Name does not have the
#                         COMPlus_AltJit prefix.
#   -n, --ngen            use ngened mscorlib
#   -p, --precise-gc      test with precise gc
#   -v, --verbose         echo commands
#   -r [CORERUN_AND_ARGS [CORERUN_AND_ARGS ...]], --corerun-and-args [CORERUN_AND_ARGS [CORERUN_AND_ARGS ...]]
#                         If explicit CoreRun is needed (app is not
#                         CoreConsole), the CoreRun command and args to pass to
#                         CoreRun, e.g. /v for verbose.
#   -j JIT_PATH, --jit-path JIT_PATH
#                         full path to jit .dll. If given it is copied to
#                         coreclr directory. If not given we check that it
#                         exists in the coreclr directory.
# 
# required arguments:
#   -a APP_PATH, --app-path APP_PATH
#                         full path to application to run with llilc.
#   -c CORECLR_RUNTIME_PATH, --coreclr-runtime-path CORECLR_RUNTIME_PATH
#                         full path to CoreCLR run-time binary directory


import argparse
import os
import shutil
import subprocess
import sys

llilcverbose = False

def QuoteArg(arg):
    '''Strip off any enclosing single quotes and enclose in double quotes'''
    arg = '"' + arg.strip("'").strip('"') + '"'
    return arg

def UnquoteArg(arg):
    ''' Remove single and double quotes from front and back of arg'''
    return arg.strip("'").strip('"')

def log(*objs):
    '''Print log message to both stdout and stderr'''
    print("llilc-run: ", *objs)
    print("llilc-run: ", *objs, file=sys.stderr)
    
def RunCommand(command):
    ''' Run a command and return its exit code, optionally echoing it.'''
    global llilcverbose
    if llilcverbose:
        log ('About to execute: ', command)
    error_level = subprocess.call(command, shell=True)
    return error_level

def main(argv):
    '''
    main method of script. arguments are script path and remaining arguments.
    '''
    global llilcverbose
    parser = argparse.ArgumentParser(description='''Run a managed application with LLILC as the JIT.
                                     If the application has any arguments, they must be
                                     appended to the end of the command line, preceded by "--". 
                                     '''
                                     )
    parser.add_argument('-d', '--dump-level', type=str, choices={'summary', 'verbose'}, 
                        help='the dump level: summary, or verbose')
    parser.add_argument('-x', '--extra', type=str, default=[], nargs='*',
                        help='''list of extra COMPlus settings. Each item is Name=Value, where
                                Name does not have the COMPlus_AltJit prefix.
                             ''')
    parser.add_argument('-n', '--ngen', help='use ngened mscorlib', default=False, action="store_true")
    parser.add_argument('-p', '--precise-gc', help='test with precise gc', default=False, action="store_true")
    parser.add_argument('-v', '--verbose', help='echo commands', default=False, action="store_true")
    parser.add_argument('-r', '--corerun-and-args', type=str, nargs='*', default=[],
                        help='''If explicit CoreRun is needed (app is not CoreConsole),
                                the CoreRun command and args to pass to CoreRun, e.g. /v for verbose.
                             ''')
    parser.add_argument('-j', '--jit-path', type=str,
                        help='''full path to jit .dll. If given it is copied to coreclr directory.
                                If not given we check that it exists in the coreclr directory.
                             ''')
    required = parser.add_argument_group('required arguments')
    required.add_argument('-a', '--app-path', type=str, required=True, 
                        help='full path to application to run with llilc.')
    required.add_argument('-c', '--coreclr-runtime-path', required=True, 
                        help='full path to CoreCLR run-time binary directory')
    args, unknown = parser.parse_known_args(argv)
    llilcverbose = args.verbose
    if llilcverbose:
        log('Starting llilcrun.py')
        log('  argv=', argv)

    # Skip separating '--', if any.
    if unknown and (unknown[0] == '--'):
        unknown = unknown[1:]

    program_dir = os.path.dirname(args.app_path)
    jit_name = "llilcjit.dll"

    # jit_path is the path to where the jit would be in the CoreClr directory in order
    # to be used as the alternate jit.
    jit_path = os.path.join(args.coreclr_runtime_path, jit_name)

    if args.jit_path:
        # User specified a source path to the LLILC JIT. Copy it even if there
        # already is one, as it may be a revised version.
        shutil.copy2(args.jit_path, jit_path)
    elif not os.path.exists(jit_path):
        log("llilc jit not found at ", jit_path)
        return 1
    
    RunCommand('chcp 65001')
    os.environ["COMPlus_AltJit"]="*"
    os.environ["COMPlus_AltJitName"]=jit_name
    if (args.precise_gc):
        os.environ["COMPlus_InsertStatepoints"]="1"
    else:
        os.environ["COMPlus_GCConservative"]="1"
    if not args.ngen:
        os.environ["COMPlus_ZapDisable"]="1"
    if args.dump_level:
        os.environ["COMPlus_DumpLLVMIR"]=args.dump_level
    for arg in args.extra:
        pair = UnquoteArg(arg).split('=', maxsplit = 1)
        name = 'COMPLUS_AltJit' + pair[0]
        value = pair[1]
        os.environ[name] = value
    os.environ["CORE_ROOT"]=args.coreclr_runtime_path
    os.environ["CORE_LIBRARIES"]=program_dir
    if llilcverbose:
        RunCommand('set complus_')
        RunCommand('set CORE_ROOT')
        RunCommand('set CORE_LIBRARIES')
    command = ''
    if args.corerun_and_args:
        for arg in args.corerun_and_args:
            if command == '':
                # First of these will be the CoreRun.exe, so prefix with
                # the path.
                arg = os.path.join(args.coreclr_runtime_path, arg)
            arg = QuoteArg(arg)
            command += ' ' + arg
        
    if command != '':
        command += ' '
    command += QuoteArg(args.app_path)
    for arg in unknown:
        arg = QuoteArg(arg)
        command += ' ' + arg
    error_level = RunCommand(command)
    if llilcverbose:
        log ('Exiting llilc_run.py with exit code ', error_level)
    return error_level

if __name__ == '__main__':
    return_code = main(sys.argv[1:])
    sys.exit(return_code)
