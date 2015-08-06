#!/usr/bin/env python
#
# title           : llilcrun.py
# description     : Run a managed application using LLILC jit.
#   

import argparse
import os
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

def RunCommand(command):
    ''' Run a command and return its exit code, optionally echoing it.'''
    global llilcverbose
    if llilcverbose:
        sys.stderr.write (command + '\n')
        sys.stdout.write (command + '\n')
    error_level = subprocess.call(command, shell=True)
    return error_level

def main(argv):
    '''
    main method of script. arguments are script path and remaining arguments.
    '''
    global llilcverbose
    parser = argparse.ArgumentParser()
    parser.add_argument('--llilc-dump-level', type=str, choices={'summary', 'verbose'}, 
                        help='the dump level: summary, or verbose')
    parser.add_argument('--llilc-extra', type=str, default=[], nargs='*',
                        help='''list of extra COMPlus settings. Each item is Name=Value, where
                                Name does not have the COMPlus_AltJit prefix.
                             ''')
    parser.add_argument('--llilc-ngen', help='use ngened mscorlib', default=False, action="store_true")
    parser.add_argument('--llilc-run-verbose', help='echo commands', default=False, action="store_true")
    parser.add_argument('--llilc-corerun-and-args', type=str, nargs='*',
                        default=[], help='If explicit CoreRun is needed (app is not CoreConsole), the CoreRun command and args to pass to CoreRun, e.g. /v for verbose.')
    required = parser.add_argument_group('required arguments')
    required.add_argument('--llilc-app-path', type=str, required=True, 
                        help='full path to application to run with llilc.')
    required.add_argument('--llilc-coreclr-runtime-path', required=True, 
                        help='full path to CoreCLR run-time binary directory')
    args, unknown = parser.parse_known_args(argv)
    llilcverbose = args.llilc_run_verbose
    if llilcverbose:
        sys.stderr.write('Hello from llilcrun.py\n')

    program_dir = os.path.dirname(args.llilc_app_path)

    RunCommand('chcp')
    RunCommand('chcp 65001')
    RunCommand('chcp')
    os.environ["COMPlus_AltJit"]="*"
    os.environ["COMPlus_AltJitName"]="llilcjit.dll"
    os.environ["COMPlus_GCConservative"]="1"
    if not args.llilc_ngen:
        os.environ["COMPlus_ZapDisable"]="1"
    if args.llilc_dump_level:
        os.environ["COMPlus_DumpLLVMIR"]=args.llilc_dump_level
    for arg in args.llilc_extra:
        pair = UnquoteArg(arg).split('=', maxsplit = 1)
        name = 'COMPLUS_AltJit' + pair[0]
        value = pair[1]
        os.environ[name] = value
    os.environ["CORE_ROOT"]=args.llilc_coreclr_runtime_path
    os.environ["CORE_LIBRARIES"]=program_dir
    if llilcverbose:
        RunCommand('set complus_')
        RunCommand('set CORE_ROOT')
        RunCommand('set CORE_LIBRARIES')
    command = ''
    if args.llilc_corerun_and_args:
        for arg in args.llilc_corerun_and_args:
            if command == '':
                # First of these will be the CoreRun.exe, so prefix with
                # the path.
                arg = os.path.join(args.llilc_coreclr_runtime_path, arg)
            arg = QuoteArg(arg)
            command += ' ' + arg
        
    if command != '':
        command += ' '
    command += QuoteArg(args.llilc_app_path)
    for arg in unknown:
        arg = QuoteArg(arg)
        command += ' ' + arg
    error_level = RunCommand(command)
    if llilcverbose:
        sys.stdout.write ('Exiting llilc_run.py with exit code ' + str(error_level) + '\n')
        sys.stderr.write ('Exiting llilc_run.py with exit code ' + str(error_level) + '\n')
    return error_level

if __name__ == '__main__':
    return_code = main(sys.argv[1:])
    sys.exit(return_code)
