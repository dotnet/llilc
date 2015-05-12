#!/usr/bin/env python
#
#title           :llilc_runtest.py
#description     :
#
# This script runs CoreCLR test with specified LLILC JIT and pre-built 
# CoreCLR runtime. If verbose level is specified, either summary or
# verbose, a result will be created in default location or a place
# that is specified. In verbose case, the result will be normalized
# with filter.
#
# It is required to run this script from tests directory in a CoreCLR
# repository.
#
# To exclude undesired test cases, please edit exclusion.targets file.
#
# usage: llilc_runtest.py [-h] [-a {x64,x86}] [-b {debug,release}]
#                         [-d {summary,verbose}] [-r RESULT_PATH] -j JIT_PATH -c
#                         CORECLR_RUNTIME_PATH
# 
# optional arguments:
#   -h, --help            show this help message and exit
#   -a {x64,x86}, --arch {x64,x86}
#                         the target architure
#   -b {debug,release}, --build {debug,release}
#                         release or debug build of CoreCLR run-time used
#   -d {summary,verbose}, --dump-level {summary,verbose}
#                         the dump level: summary, or verbose
#   -r RESULT_PATH, --result-path RESULT_PATH
#                         the path to runtest result output directory
# 
# required arguments:
#   -j JIT_PATH, --jit-path JIT_PATH
#                         full path to jit .dll
#   -c CORECLR_RUNTIME_PATH, --coreclr-runtime-path CORECLR_RUNTIME_PATH
#                         full path to CoreCLR run-time binary directory
# 
#==========================================================================================

import argparse
import time
import os
import shutil
import glob
import sys
import stat
import subprocess
import applyfilter
import const

# Return OS name used internally in paths
def OSName():
    if os.name == 'nt':
        return 'Windows_NT'
    else:
        return 'Unknown'

# Return the path of built test path relavtive to coreclr tests directory
def BuiltTestPath(arch, build):
    built_test_directory = OSName() + '.' + arch + '.' + build
    built_test_path = os.path.join('..', 'bin', 'tests', built_test_directory)
    return built_test_path

# Exculde top level test directories
def ExcludeTopLevelTestDirectories():
    os.environ['SkipTestAssemblies'] = 'Common;Exceptions;GC;Loader;managed;packages;Regressions;runtime;Tests;TestWrappers_x64_release;Threading'

# Remove unwanted test result files
def CleanUpResultFiles(path):
    try:
        for root, subdirs, files in os.walk(path):
            for file in files:
                if not file.endswith('error.txt'):
                    file_path = os.path.join(root, file)
                    os.remove(file_path)
    except OSError:
        pass

# Return default result path if result is generated
def DefaultResultPath():
    default_result_path = os.environ['TEMP']
    default_result_path = os.path.join(default_result_path, 'LLILCTestResult')
    return default_result_path

# Remove readonly files
def del_rw(action, name, exc):
    os.chmod(name, stat.S_IWRITE)
    os.remove(name)

# Count files in a directory
def CountFiles(path, suffix):
    total = 0
    for root, sub_dirs, files in os.walk(path):
        for file_name in files:
            if file_name.endswith(suffix):
                total = total + 1
    return total

def main(argv):
    # define return code const value
    const.RunTestFail = 1
    const.RunTestOK = 0
    const.GeneralError = -1
    const.UnknownArguments = -2
    const.NormalizationFail = -3
    const.WrongRunDir = -4

    # Parse the command line    
    parser = argparse.ArgumentParser()
    parser.add_argument('-a', '--arch', type=str, choices={'x86', 'x64'},
                        default='x64', help='the target architure')
    parser.add_argument('-b', '--build', type=str, choices={'release', 'debug'}, 
                        default='debug', help='release or debug build of CoreCLR run-time used')
    parser.add_argument('-d', '--dump-level', type=str, choices={'summary', 'verbose'}, 
                        help='the dump level: summary, or verbose')
    parser.add_argument('-r', '--result-path', type=str, 
                        default=DefaultResultPath(), help='the path to runtest result output directory')
    required = parser.add_argument_group('required arguments')
    required.add_argument('-j', '--jit-path', required=True, 
                        help='full path to jit .dll')
    required.add_argument('-c', '--coreclr-runtime-path', required=True, 
                        help='full path to CoreCLR run-time binary directory')
    args, unknown = parser.parse_known_args(argv)

    if unknown:
        print('Unknown argument(s): ', ', '.join(unknown))
        return const.UnknowArguments
        
    # Ensure the command run from a CoreCLR tests directory
    current_work_directory = os.getcwd()
    path, folder = os.path.split(current_work_directory)
    parent_path, parent_folder = os.path.split(path)
    if (not folder == 'tests' or
        not parent_folder == 'coreclr'):
        print('This script is required to run from tests directory in a CoreCLR repository.' )    
        return const.WrongRunDir

    try:
        # Determine the built test location
        build_test_path = BuiltTestPath(str(args.arch), str(args.build))

        # Determine time stamp
        time_stamp = str(time.time()).split('.')[0]

        # Copy in llilcjit.dll with time stamp
        time_stamped_jit_name = 'LLILCJit' + time_stamp + '.dll'
        time_stamped_jit_path = os.path.join(args.coreclr_runtime_path, time_stamped_jit_name)
        shutil.copy2(args.jit_path, time_stamped_jit_path)

        # Create llilctestenv.cmd with time stamp
        time_stamped_test_env_name = 'LLILCTestEnv' + time_stamp + '.cmd'
        time_stamped_test_env_path = os.path.join(args.coreclr_runtime_path, time_stamped_test_env_name)

        # Todo: Test Env is right now only for Windows. Will expand when cross platform
        with open(time_stamped_test_env_path, 'w') as test_env:
            test_env.write('set COMPlus_AltJit=*\n')
            test_env.write('set COMPlus_AltJitName=' + time_stamped_jit_name + '\n')
            test_env.write('set COMPlus_GCConservative=1\n')
            test_env.write('set COMPlus_INSERTSTATEPOINTS=1\n')
            test_env.write('set COMPlus_ZapDisable=1\n')
            test_env.write('chcp 65001\n')
            if args.dump_level is not None:
                test_env.write('set COMPlus_DumpLLVMIR=' + args.dump_level + '\n')

        # Exclude undesired tests from running
        ExcludeTopLevelTestDirectories()
    except:
        e = sys.exc_info()[0]
        print('Error: RunTest failed due to ', e)
        return const.GeneralError

    # Run the test
    return_code = const.RunTestOK
    exclusion = os.path.join(os.path.dirname(__file__), 'exclusion.targets')
    runtest_command = 'runtest ' + args.arch + ' ' + args.build 
    runtest_command = runtest_command + ' Exclude ' + exclusion
    runtest_command = runtest_command + ' Testenv ' + time_stamped_test_env_path
    runtest_command = runtest_command + ' ' + args.coreclr_runtime_path 
    print(runtest_command)
    error_level = subprocess.call(runtest_command, shell=True)
    if error_level == 1:
        return_code = const.RunTestFail

    # Remove temporary time-stamped jit and test env files
    try:
        os.remove(time_stamped_jit_path)
        os.remove(time_stamped_test_env_path)
    except:
        e = sys.exc_info()[0]
        print('Error: RunTest failed due to ', e)
        return const.GeneralError

    # Copy out result if there is one, clean up undesired files, 
    # In case of verbose result, normalize it and extract out summary.
    # In case of summary result, rename all result file name.
    if args.dump_level is not None:
        try:
            coreclr_result_path = os.path.join(build_test_path, 'Reports')
            if os.path.exists(args.result_path):
                shutil.rmtree(args.result_path, onerror=del_rw)            
            shutil.copytree(coreclr_result_path, args.result_path)
            CleanUpResultFiles(args.result_path)
            total = CountFiles(args.result_path, 'error.txt')
            if args.dump_level == 'verbose':
                print('Verbose-mode post-processing started')
                print('found ', total, 'valid raw test outputs (error.txt) under ', str(coreclr_result_path))
                print('creating normalized outputs (error.txt) under ', str(args.result_path))
                print('creating summary outputs (sum.txt) under ', str(args.result_path))
                applyfilter.ApplyAll(args.result_path)
                print('Verbose-mode post-processing finished')
            if args.dump_level == 'summary':
                print('Summary-mode post-processing started')
                print('found ', total, 'valid raw test outputs (error.txt) under ', os.path.abspath(coreclr_result_path))
                print('creating summary outputs (sum.txt) under ', str(args.result_path))
                applyfilter.SummaryRenameAll(args.result_path) 
                print('Summary-mode post-processing finished')
        except:
            e = sys.exc_info()[0]
            print('Error: Test result normalization failed due to ', e)
            return const.NormalizationFail
    else:
        print('No post-processing needed.')

    return return_code

if __name__ == '__main__':
    return_code = main(sys.argv[1:])
    sys.exit(return_code)
