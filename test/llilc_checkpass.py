#!/usr/bin/env python
#
#title           :llilc_checkpass.py
#description     :
# 
# llilc_checkpass compares two test results, no matter summary or verbose,
# and checks if any function successfully compiled by LLILC in base result
# failed to compile in the target result or if any function that was newly
# submitted for compilation failed to compile. Either of these situations
# is considered an unexpected failure. The script returns the number of
# unexpected failures, where 0 indicates that all tests passed. If the script
# fails due to an unexpected environmental issue, the return value will be
# negative, and has no relation to the number of unexpected failures.
#
# Whether or not all tests passed, the set of methods submitted for compilation
# may differ between the base result and the target result. The number of
# methods that were only submitted to the base compiler and the number of
# methods that were onty submitted to the target compiler are reported on
# stdout.
# 
# usage: llilc_checkpass.py [-h] -b BASE_RESULT_PATH -d TARGET_RESULT_PATH
# 
# optional arguments:
#   -h, --help            show this help message and exit
# 
# required arguments:
#   -b BASE_RESULT_PATH, --base-result-path BASE_RESULT_PATH
#                         full path to base result
#   -d TARGET_RESULT_PATH, --diff-result-path TARGET_RESULT_PATH
#                         full path to target result
#
#==========================================================================================

import argparse
import re
import sys
import os
import const
import traceback

def main(argv):
    # Define return code constants
    const.GeneralError = -1
    const.UnknownArguments = -2
    const.MissingResult = -3

    # Parse the command line    
    parser = argparse.ArgumentParser()
    required = parser.add_argument_group('required arguments')
    required.add_argument('-b', '--base-result-path', type=str, required=True,
                        help='full path to base result')
    required.add_argument('-d', '--diff-result-path', type=str, required=True,
                        help='full path to target result')
    args, unknown = parser.parse_known_args(argv)

    if unknown:
        print 'Unknown argument(s): ', ', '.join(unknown)
        return const.UnknownArguments

    try:
        # Collect a list of summary result file relative path in base result    
        base_files = []
        for root, dirs, files in os.walk(args.base_result_path):
            for file in files:
                if file.endswith('sum.txt'):
                    relative_path = os.path.relpath(root, args.base_result_path)
                    relative_path = os.path.join(relative_path, file)
                    base_files.append(relative_path)
 
        # Collect a list of summary result file relative path in target result
        target_files = []
        for root, dirs, files in os.walk(args.diff_result_path):
            for file in files:
                if file.endswith('sum.txt'):
                    relative_path = os.path.relpath(root, args.diff_result_path)
                    relative_path = os.path.join(relative_path, file)
                    target_files.append(relative_path)
    except:
        e = sys.exc_info()[0]
        print 'Error: CheckPass failed due to ', e
        traceback.print_exc()
        return const.GeneralError

    # Check if results are empty
    if len(base_files) == 0:
        print 'Error: base result is empty'
        return const.MissingResult

    if len(target_files) == 0:
        print 'Error: target result is empty'
        return const.MissingResult

    # Track the number of methods meeting any of the following criteria:
    # 1. Compiled cleanly in the baseline and are now failing to compile
    # 2. Failed to compile in the baseline and are now compiling cleanly
    # 3. Processed in the baseline but not processed now
    # 4. Not processed in the baseline, processed now, and compiling cleanly
    # 5. Not processed in the baseline, processed now, and failing to compile
    #
    # The sum of categories (1) and (5) is the number of unexpected compilation
    # failures.

    target_failure_diff = 0
    target_success_diff = 0
    missing_base_methods = 0
    new_target_successes = 0
    new_target_failures = 0

    # This script expects input that contains lines of the form:
    # - "Successfully read <method name>" to indicate successsful compilation of
    #   a method
    # - "Failed to read <method name>[<failure reason>]" to indicate that a
    #   method failed to compile
    #
    # All other lines are ignored.
    prog = re.compile(r'^(Successfully read (.*))|(Failed to read (.*)\[.*\])$')

    print 'Checking started.'
    for file in base_files:
        if file in target_files:
            try:
                base_file_path = os.path.join(args.base_result_path, file)
                target_file_path = os.path.join(args.diff_result_path, file)
                with open(base_file_path, 'r') as base, open(target_file_path, 'r') as target:
                    # base_passing and base_failing track the set of methods that were
                    # successfully compiled and unsuccessfully compiled by the baseline
                    # JIT, respectively. The value for each entry is used to track whether
                    # or not a method that was processed by the baseline JIT was also
                    # processed by the new JIT.
                    base_passing, base_failing = {}, {}
                    for line in base:
                        m = prog.match(line)
                        if m is not None:
                            if m.group(1) is not None:
                                base_passing[m.group(2)] = False
                            else:
                                base_failing[m.group(4)] = False

                    base_matched_methods = 0
                    for line in target:
                        m = prog.match(line)
                        if m is not None:
                            def update(method, good_set, bad_set, matched_count, new_count, bad_count):
                                if method in good_set:
                                    if not good_set[method]:
                                        good_set[method] = True
                                        matched_count += 1
                                elif method in bad_set:
                                    if not bad_set[method]:
                                        bad_set[method] = True
                                        matched_count, bad_count = matched_count + 1, bad_count + 1
                                else:
                                    new_count += 1

                                return matched_count, new_count, bad_count

                            if m.group(1) is not None:
                                base_matched_methods, new_target_successes, target_failure_diff = update(m.group(2), base_passing, base_failing, base_matched_methods, new_target_successes, target_failure_diff)
                            else:
                                base_matched_methods, new_target_failures, target_success_diff = update(m.group(4), base_failing, base_passing, base_matched_methods, new_target_failures, target_success_diff)

                    missing_base_methods += len(base_passing) + len(base_failing) - base_matched_methods
            except:
                e = sys.exc_info()[0]
                print 'Error: CheckPass failed due to ', e
                traceback.print_exc()
                return const.GeneralError
        else:
            missing_result_file = os.path.join(args.diff_result_path, file)
            print 'Error: diff directory does not have result file ', missing_result_file
            return const.MissingResult

    print 'CheckPass:', target_failure_diff, 'method(s) compiled successfully in base result that failed in target result.'
    print 'CheckPass:', target_success_diff, 'method(s) failed in base result that compiled successfully in target result.'
    print 'CheckPass:', missing_base_methods, 'method(s) processed in base result that were not processed in target result.'
    print 'CheckPass:', new_target_successes, 'method(s) not processed in base result that compiled successfully in target result.'
    print 'CheckPass:', new_target_failures, 'method(s) not processed in base result that failed to compile in target result.'

    unexpected_failures = target_failure_diff + new_target_failures

    if unexpected_failures == 0:
        print 'There were no unexpected failures.'
    else:
        print 'There were unexpected failures.'

    return unexpected_failures

if __name__ == '__main__':
    return_code = main(sys.argv[1:])
    sys.exit(return_code)
