#!/usr/bin/env python
#
# title           : make_sln.py
# description     : Make a Visual Studio solution to debug a managed 
#                   application using LLILC jit.
#
# This script has been tested running on both Python 2.7 and Python 3.4.
#
# usage: make_sln.py [-h] [-d {summary,verbose}] [-x [EXTRA [EXTRA ...]]] [-n]
#                    [-p] [-v] [-r [CORERUN_ARGS [CORERUN_ARGS ...]]]
#                    [-j JIT_PATH] [-s SLN_PATH] [-i] [-f] -a APP_PATH -c
#                    CORECLR_RUNTIME_PATH
# 
# Make a solution to debug a managed application with LLILC as the JIT. If the
# application has any arguments, they must be appended to the end of the command
# line, preceded by "--".
# 
# optional arguments:
#   -h, --help            show this help message and exit
#   -d {summary,verbose}, --dump-level {summary,verbose}
#                         the dump level: summary, or verbose
#   -x [EXTRA [EXTRA ...]], --extra [EXTRA [EXTRA ...]]
#                         list of extra COMPlus settings. Each item is
#                         Name=Value, where Name does not have the
#                         COMPlus_AltJit prefix.
#   -n, --ngen            use ngened mscorlib
#   -p, --precise-gc      test with precise gc
#   -v, --verbose         echo commands
#   -r [CORERUN_ARGS [CORERUN_ARGS ...]], --corerun-args [CORERUN_ARGS [CORERUN_ARGS ...]]
#                         The command arguments to pass to CoreRun, e.g. /v for
#                         verbose.
#   -j JIT_PATH, --jit-path JIT_PATH
#                         full path to jit .dll. If given it is copied to
#                         coreclr directory. If not given we check that it
#                         exists in the coreclr directory.
#   -s SLN_PATH, --sln-path SLN_PATH
#                         full path to use for the solution that will be
#                         written. If not given solution is written to the same
#                         directory as the application and has the same name
#                         with '.sln' substituted for '.exe'.
#   -i, --invoke-vs       Invoke Visual Studio on the solution
#   -f, --force           Force overwrite existing solution
# 
# required arguments:
#   -a APP_PATH, --app-path APP_PATH
#                         full path to application to run with llilc.
#   -c CORECLR_RUNTIME_PATH, --coreclr-runtime-path CORECLR_RUNTIME_PATH
#                         full path to CoreCLR run-time binary directory


import argparse
import const
import os
import platform
import shutil
from string import Template
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

def GetPrintString(*args):
    return ' '.join(map(str, args))

def Print(*args):
    print (GetPrintString(*args))

def PrintError(*args):
    ''' Mimic the python 3.x print statement (should the community ever go to 3.4, we would not need to change much.)'''
    sys.stderr.write(GetPrintString(*args) + '\n')

def log(*objs):
    '''Print log message to both stdout and stderr'''
    Print("llilc_run\stdout: ", *objs)
    PrintError("llilc_run\stderr: ", *objs)
    
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

    const.Success = 0
    const.GeneralError = -1
    const.VS2015Missing = -2
    const.SolutionAlreadyExists = -3
    const.BadSolutionPath = -4

    parser = argparse.ArgumentParser(description='''Make a solution to debug a managed 
                                     application with LLILC as the JIT.
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
    parser.add_argument('-r', '--corerun-args', type=str, nargs='*', default=[],
                        help='''The command arguments to pass to CoreRun, e.g. /v for verbose.
                             ''')
    parser.add_argument('-j', '--jit-path', type=str,
                        help='''full path to jit .dll. If given it is copied to coreclr directory.
                                If not given we check that it exists in the coreclr directory.
                             ''')
    parser.add_argument('-s', '--sln-path', type=str,
                        help='''full path to use for the solution that will be written.
                                If not given solution is written to the same directory
                                as the application and has the same name with '.sln'
                                substituted for '.exe'.
                             ''')
    parser.add_argument('-i', '--invoke-vs', default=False, action="store_true",
                        help='Invoke Visual Studio on the solution')

    parser.add_argument('-f', '--force', default=False, action="store_true",
                        help='Force overwrite existing solution')

    required = parser.add_argument_group('required arguments')
    required.add_argument('-a', '--app-path', type=str, required=True, 
                        help='full path to application to run with llilc.')
    required.add_argument('-c', '--coreclr-runtime-path', required=True, 
                        help='full path to CoreCLR run-time binary directory')

    sln_template = Template('''
Microsoft Visual Studio Solution File, Format Version 12.00
# Visual Studio 14
VisualStudioVersion = 14.0.23107.0
MinimumVisualStudioVersion = 10.0.40219.1
Project("{911E67C6-3D85-4FCE-B560-20A9C3E3FF48}") = "$project", "$app_exe", "{3BE0C1EC-A093-408F-982D-6D50F5D9146C}"
	ProjectSection(DebuggerProjectSystem) = preProject
		PortSupplier = 00000000-0000-0000-0000-000000000000
		Executable = $corerun
		RemoteMachine = $current_machine
		StartingDirectory = $app_dir
		Arguments = $arguments
		Environment = $tabbed_app_environment
		LaunchingEngine = 2e36f1d4-b23c-435d-ab41-18e608940038
		UseLegacyDebugEngines = No
		LaunchSQLEngine = No
		AttachLaunchAction = No
	EndProjectSection
EndProject
''')

    current_machine = platform.node()

    args, unknown = parser.parse_known_args(argv)
    llilcverbose = args.verbose
    if llilcverbose:
        log('Starting make_sln.py')
        log('  argv=', argv)

    # Skip separating '--', if any.
    if unknown and (unknown[0] == '--'):
        unknown = unknown[1:]

    app_path = args.app_path
    (app_dir, app_exe) = os.path.split(app_path)
    (project, extension) = os.path.splitext(app_exe)
    if args.sln_path:
        sln_path = os.path.abspath(args.sln_path)
        (sln_base, sln_ext) = os.path.splitext(sln_path)
        if sln_ext != '.sln':
            log('Specified solution file: ', args.sln_path, ' does not end with .sln')
            return const.BadSolutionPath
    else:
        sln_path= os.path.join(app_dir, (project + ".sln"))
    if os.path.exists(sln_path):
        if not args.force:
            log("File ", sln_path, " already exists, use --force to overwrite")
            return const.SolutionAlreadyExists

    corerun = os.path.join(args.coreclr_runtime_path, "CoreRun.exe")

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

    # Compute desired environment.

    # First initialize empty values for variables known to LLILC.
    env = {}
    env["complus_altjit"] = ""
    env["complus_altjitname"] = ""
    env["complus_gcconservative"] = ""
    env["complus_insertstatepoints"] = ""
    env["complus_zapdisable"] = ""
    env["complus_dumpllvmir"] = ""
    env["complus_jitgcinfologging"] = ""
    env["complus_altjitexclude"] = ""
    env["complus_altjitbreakatjitstart"] = ""
    env["complus_altjitmsildump"] = ""
    env["complus_altjitllvmdump"] = ""
    env["complus_altjitcoderangedump"] = ""
    env["complus_simdintrinc"] = ""
    env["complus_altjitoptions"] = ""

    # Now initialized based on desired values and options.
    env["complus_altjit"]="*"
    env["complus_altjitname"]=jit_name
    if (args.precise_gc):
        env["complus_insertstatepoints"]="1"
    else:
        env["complus_gcconservative"]="1"
    if not args.ngen:
        env["complus_zapdisable"]="1"
    if args.dump_level:
        env["complus_dumpllvmir"]=args.dump_level
    for arg in args.extra:
        pair = UnquoteArg(arg).split('=', 1)
        name = 'complus_altjit' + pair[0]
        name = name.lower()
        value = pair[1]
        env[name] = value
    env["core_root"]=args.coreclr_runtime_path
    env["core_libraries"]=app_dir

    # Convert environment into tab-separated string.
    tabbed_app_environment = ''
    keys = list(env.keys())
    keys.sort()
    for key in keys:
        value = env[key]
        tabbed_app_environment += (key + '=' + value + "\t")

    arguments = ''
    if args.corerun_args:
        for arg in args.corerun_args:
            arg = QuoteArg(arg)
            arguments += ' ' + arg
        
    if arguments != '':
        arguments += ' '
    arguments += QuoteArg(args.app_path)
    for arg in unknown:
        arg = QuoteArg(arg)
        arguments += ' ' + arg

    sln = sln_template.substitute(locals())
    try:
        with open(sln_path, 'w') as sln_file:
            sln_file.writelines(sln)
    except:
        e = sys.exc_info()[0]
        print('Error: make_sln.py failed due to ', e)
        return const.GeneralError

    error_level = const.Success
    if args.invoke_vs:
        vstools = os.environ['VS140COMNTOOLS']
        if not vstools:
            log ("make_sln.py requires Visual Studio 2015")
            return const.VS2015Missing

        vspath = os.path.abspath(os.path.join(vstools, "..", "IDE", "devenv.exe"))
        command = "start"
        command += ' '
        command += QuoteArg(vspath)
        command += ' '
        command += QuoteArg(sln_path)
        error_level = RunCommand(command)

    if llilcverbose:
        log ('Exiting make_sln.py with exit code ', error_level)
    return error_level

if __name__ == '__main__':
    return_code = main(sys.argv[1:])
    sys.exit(return_code)
