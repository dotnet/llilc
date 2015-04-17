#!/usr/bin/env python
#
#title           :llilc_checkpass.py
#description     :
# 
# llilc_checkpass compares two test results, no matter summary or verbose,
# and checks if any function successfully jitted by LLILC in base result
# failed jitting in diff result.
#
# If the check passed, the script return 0. In case of 0, it might have
# jitted more functions in diff result than in base result. The two results
# are not necessary the same in this case. Counting result is reported.
# 
# If the check failed due to newly faily routine, return 1.
#     
# If the check failed due to unexpected reason, return negative numbers.
#
# usage: llilc_checkpass.py [-h] -b BASE_RESULT_PATH -d DIFF_RESULT_PATH
# 
# optional arguments:
#   -h, --help            show this help message and exit
# 
# required arguments:
#   -b BASE_RESULT_PATH, --base-result-path BASE_RESULT_PATH
#                         full path to base result
#   -d DIFF_RESULT_PATH, --diff-result-path DIFF_RESULT_PATH
#                         full path to diff result
#
#==========================================================================================

import argparse
import re
import sys
import os
import difflib
import const

def main(argv):
    # define return code const value
    const.CheckPassFail = 1
    const.CheckPassOK = 0
    const.GeneralError = -1
    const.UnknownArguments = -2
    const.MissingResult = -3

    # Parse the command line    
    parser = argparse.ArgumentParser()
    required = parser.add_argument_group('required arguments')
    required.add_argument('-b', '--base-result-path', type=str, required=True,
                        help='full path to base result')
    required.add_argument('-d', '--diff-result-path', type=str, required=True,
                        help='full path to diff result')
    args, unknown = parser.parse_known_args(argv)

    if unknown:
        print('Unknown argument(s): ', ', '.join(unknown))
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
 
        # Collect a list of summary result file relative path in diff result    
        diff_files = []
        for root, dirs, files in os.walk(args.diff_result_path):
            for file in files:
                if file.endswith('sum.txt'):
                    relative_path = os.path.relpath(root, args.diff_result_path)
                    relative_path = os.path.join(relative_path, file)
                    diff_files.append(relative_path)
    except:
        e = sys.exc_info()[0]
        print('Error: CheckPass failed due to ', e)
        return const.GeneralError

    # Check if results are empty
    if len(base_files) == 0:
        print('Error: base result is empty')
        return const.MissingResult

    if len(diff_files) == 0:
        print('Error: diff result is empty')
        return const.MissingResult

    # Counting the newly failed or passed test cases
    count_new_failed = 0
    count_new_passed = 0

    print('Checking started.')    
    for file in base_files:
        if file in diff_files:
            try:
                base_file_path = os.path.join(args.base_result_path, file)
                diff_file_path = os.path.join(args.diff_result_path, file)
                with open(base_file_path, 'r') as bases, open(diff_file_path, 'r') as diffs:
                    diff = difflib.ndiff(bases.readlines(),diffs.readlines())
                    for line in diff:
                        if re.search('- Successfully read ', line):
                            count_new_failed = count_new_failed + 1
                        if re.search('\+ Successfully read ', line):
                           count_new_passed = count_new_passed + 1
            except:
                e = sys.exc_info()[0]
                print('Error: CheckPass failed due to ', e)
                return const.GeneralError
        else:
            missing_result_file = os.path.join(args.diff_result_path, file)
            print('Error: diff result does not result file ', missing_result_file)
            return const.MissingResult

    print('CheckPass: ', count_new_failed, ' test cases passed in base result but failed in diff result.')
    print('CheckPass: ', count_new_passed, ' test cases failed in base result but passed in diff result.') 
    if count_new_failed == 0:
        print('CheckPass Passed.')
        return const.CheckPassOK
    else:
        print('CheckPass Failed.\n')
        return const.CheckPassFail

if __name__ == '__main__':
    return_code = main(sys.argv[1:])
    sys.exit(return_code)
