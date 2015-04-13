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
# To exclude undesired test cases, please edit exclusion file.
#
# usage: llilc_runtest.py [-h] [-a {x86,x64}] [-b {debug,release}]
#                         [-d {summary,verbose}] [-r RESULT_PATH] -j JIT_PATH -c
#                         CORECLR_RUNTIME_PATH
# 
# optional arguments:
#   -h, --help            show this help message and exit
#   -a {x86,x64}, --arch {x86,x64}
#                         the target architure
#   -b {debug,release}, --build {debug,release}
#                         release or debug build of CoreCLR run-time used
#   -d {summary,verbose}, --dump-level {summary,verbose}
#                         the dump level: summary, or verbose
#   -r RESULT_PATH, --result-path RESULT_PATH
#                         the path to runtest result output directory
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
import subprocess
import applyfilter

# Return OS name used internally in paths
def OSName():
    if os.name == "nt":
        return "Windows_NT"
    else:
        return "Unknown"

# Return the path of built test path relavtive to coreclr tests directory
def BuiltTestPath(arch, build):
    built_test_directory = OSName() + "." + arch + "." + build
    built_test_path = os.path.join("..", "bin", "tests", built_test_directory)
    return built_test_path

# Exculde top level test directories
def ExcludeTopLevelTestDirectories():
    exclusion = os.path.join(os.path.dirname(__file__), "exclusion")
    with open(str(exclusion)) as excl:
        content = excl.readlines()
        os.environ["SkipTestAssemblies"] = content[1]

# Exclude individual test cases
def ExcludeIndividualTestCases(path):
    exclusion = os.path.join(os.path.dirname(__file__), "exclusion")
    with open(str(exclusion)) as excl:
        try:
            glob_files_to_delete = []
            content = excl.read().splitlines()
            for line in range(4, len(content)):
                all = content[line] + "*"
                files_to_delete = os.path.join(path, all) 
                glob_files_to_delete.extend(glob.glob(files_to_delete))
            for file_to_delete in glob_files_to_delete:
                os.remove(file_to_delete)
        except OSError:
            pass

# Remove unwanted test result files
def CleanUpResultFiles(path):
    try:
        for root, subdirs, files in os.walk(path):
            for file in files:
                if not file.endswith("error.txt"):
                    file_path = os.path.join(root, file)
                    os.remove(file_path)
    except OSError:
        pass

# Return default result path if result is generated
def DefaultResultPath():
    default_result_path = os.environ["TEMP"]
    default_result_path = os.path.join(default_result_path, "LLILCTestResult")
    return default_result_path

# Remove readonly files
def del_rw(action, name, exc):
    os.chmod(name, stat.S_IWRITE)
    os.remove(name)

def main(argv):
    # Parse the command line    
    parser = argparse.ArgumentParser()
    parser.add_argument("-a", "--arch", type=str, choices={"x86", "x64"},
                        default="x64", help="the target architure")
    parser.add_argument("-b", "--build", type=str, choices={"release", "debug"}, 
                        default="debug", help="release or debug build of CoreCLR run-time used")
    parser.add_argument("-d", "--dump-level", type=str, choices={"summary", "verbose"}, 
                        help="the dump level: summary, or verbose")
    parser.add_argument("-r", "--result-path", type=str, 
                        default=DefaultResultPath(), help="the path to runtest result output directory")
    parser.add_argument("-j", "--jit-path", required=True, 
                        help="full path to jit .dll")
    parser.add_argument("-c", "--coreclr-runtime-path", required=True, 
                        help="full path to CoreCLR run-time binary directory")
    args, unknown = parser.parse_known_args(argv)

    if unknown:
        print("Unknown argument(s): ", ", ".join(unknown))
        return -2
        
    # Ensure the command run from a CoreCLR tests directory
    current_work_directory = os.getcwd()
    path, folder = os.path.split(current_work_directory)
    parent_path, parent_folder = os.path.split(path)
    if (not folder == "tests" or
        not parent_folder == "coreclr"):
        print("This script is required to run from tests directory in a CoreCLR repository." )    
        return -1

    # Determine the built test location
    build_test_path = BuiltTestPath(str(args.arch), str(args.build))

    # Determine time stamp
    time_stamp = str(time.time()).split(".")[0]

    # Copy in llilcjit.dll with time stamp
    time_stamped_jit_name = "LLILCJit" + time_stamp + ".dll"
    time_stamped_jit_path = os.path.join(args.coreclr_runtime_path, time_stamped_jit_name)
    shutil.copy2(args.jit_path, time_stamped_jit_path)

    # Create llilctestenv.cmd with time stamp
    time_stamped_test_env_name = "LLILCTestEnv" + time_stamp + ".cmd"
    time_stamped_test_env_path = os.path.join(args.coreclr_runtime_path, time_stamped_test_env_name)

    # Todo: Test Env is right now only for Windows. Will expand when cross platform
    with open(time_stamped_test_env_path, "w") as test_env:
        test_env.write("set COMPlus_AltJit=*\n")
        test_env.write("set COMPlus_AltJitName=" + time_stamped_jit_name + "\n")
        test_env.write("set COMPlus_GCConservative=1\n")
        test_env.write("chcp 65001\n")
        if args.dump_level is not None:
            test_env.write("set COMPlus_DumpLLVMIR=" + args.dump_level + "\n")

    # Exclude undesired tests from running
    ExcludeTopLevelTestDirectories()
    ExcludeIndividualTestCases(build_test_path)

    # Run the test
    return_code = 0
    runtest_command = "runtest " + args.arch + " " + args.build 
    runtest_command = runtest_command + " Testenv " + time_stamped_test_env_path
    runtest_command = runtest_command + " " + args.coreclr_runtime_path 
    print(runtest_command)
    error_level = subprocess.call(runtest_command, shell=True)
    if error_level == 1:
        return_code = 1

    # Remove temporary time-stamped jit and test env files
    os.remove(time_stamped_jit_path)
    os.remove(time_stamped_test_env_path)

    # Copy out result if there is one, clean up undesired files, 
    # normalize it in case of verbose dump.
    if args.dump_level is not None:
        coreclr_result_path = os.path.join(build_test_path, "Reports")
        shutil.rmtree(args.result_path, onerror=del_rw)
        shutil.copytree(coreclr_result_path, args.result_path)
        ExcludeIndividualTestCases(args.result_path)
        CleanUpResultFiles(args.result_path)
        if args.dump_level == "verbose":
            print("Applying filter...")
            applyfilter.ApplyAll(args.result_path) 

    return return_code

if __name__ == "__main__":
    return_code = main(sys.argv[1:])
    sys.exit(return_code)
