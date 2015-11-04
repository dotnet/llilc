#!/usr/bin/env python
#
#title           :llilc_checkpass.py
#description     :
# 
# llilc_checkpass checks one or two test results, no matter summary or verbose,
# and checks if any function successfully compiled by LLILC in base result (if any)
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
# methods that were only submitted to the target compiler are reported on
# stdout.
# 
# usage: llilc_checkpass.py [-h] -d DIFF_RESULT_PATH [-b BASE_RESULT_PATH] [-v]
#                           [--bs] [--ts]
# 
# Check the output of a LLILC test run (optionally against a prior baseline
# run), for each test looking to see which methods LLILC reported as passing or
# failing. Reports the reason for any failures and the failing methods and
# tests. Return code is the number of unexpected failures, i.e. failures which
# did not already occur in the baseline (if given).
# 
# optional arguments:
#   -h, --help            show this help message and exit
#   -b BASE_RESULT_PATH, --base-result-path BASE_RESULT_PATH
#                         full path to base result
#   -v, --verbose         Show target failures by test
#   --bs, --baseline-summary
#                         Show baseline failure summary
#   --ts, --target-summary
#                         Show target failure summary.  This has all target
#                         failures, even those that failed in the baseline (if
#                         any).
# 
# required arguments:
#   -d DIFF_RESULT_PATH, --diff-result-path DIFF_RESULT_PATH
#                         full path to target result
#
#==========================================================================================

import argparse
import re
import sys
import os
import const
import traceback

def collect_summary_files(root_dir):
    """ Collect a list of summary result file relative paths"""
    summary_files = []
    for root, dirs, files in os.walk(root_dir):
        relative_dir = os.path.relpath(root, root_dir)
        for file in files:
            if file.endswith('sum.txt'):
                relative_path = os.path.join(relative_dir, file)
                summary_files.append(relative_path)
    return summary_files

def analyze(file_path, pattern_reg_exp):
    """ Return the set of passing and failing methods and a map
        from failing methods to the reason they failed.
    """
    #
    # We pass in the pattern regular expression so we will not have
    # compile it repeatedly. For reference, here is its definition:
    #
    # r'^(Successfully read (.*))|(Failed to read (.*)\[(.*)\])$'
    #
    passing, failing, fail_reason_map = set(), set(), {}
    with open(file_path, 'r') as file:
        # passing and failing track the set of methods that were
        # successfully compiled and unsuccessfully compiled as reported by this file.
        for line in file:
            m = pattern_reg_exp.match(line)
            if m:
                if m.group(1):
                    passing.add(m.group(2))
                else:
                    method = m.group(4)
                    failing.add(method)
                    reason = m.group(5)
                    fail_reason_map[method] = reason
    return passing, failing, fail_reason_map

def get_fail_reason_diff(target_fail_reason_map, base_fail_reason_map):
    """Compute fail reason map for target, but not including failures from base"""
    diff_fail_reason_map = {}
    for method in target_fail_reason_map:
        if not method in base_fail_reason_map:
            reason = target_fail_reason_map[method]
            diff_fail_reason_map[method] = reason

    return diff_fail_reason_map

def print_failures(failed_set, caption):
    """Print methods that failed for given condition"""
    if failed_set:
        print('   ' + caption)
        for failed in failed_set:
            print('        ' + failed)

def report(value, description):
    print(('CheckPass: ' + description).format(value))

def tallyfailures(method_to_reason_to_tests, reason_to_method_to_tests, test, failure_reason_map):
    """ Record failures in summary maps.

        Given a test name and a map from failing methods to reasons for the failure,
        record the failure in two maps:
        - A map from method -> reason -> set(test)
        - A map from reason -> method -> set(test)
    """
    for method in failure_reason_map:
        reason = failure_reason_map[method]

        # Update method_to_reason_to_tests
        if not method in method_to_reason_to_tests:
            method_to_reason_to_tests[method] = {}
        reason_to_tests = method_to_reason_to_tests[method]
        if not reason in reason_to_tests:
            reason_to_tests[reason] = set()
        tests = reason_to_tests[reason]
        tests.add(test)

        # Update reason_to_method_to_tests
        if not reason in reason_to_method_to_tests:
            reason_to_method_to_tests[reason] = {}
        method_to_tests = reason_to_method_to_tests[reason]
        if not method in method_to_tests:
            method_to_tests[method] = set()
        tests = method_to_tests[method]
        tests.add(test)

def print_map_to_set(the_map, description1, description2):
    """ Print a map whose range values are sets.

        the_map is the map to print.

        description1 and description1 are format descriptors.

        description1 is used to print the key and can reference
        the key number (key_num), the key (key), and the
        number of elements in the corresponding set (value).

        description2 is used to print the elements of the set and
        can reference the item number (item_num), the item (item),
        as well as the key_num and key.
    """
    key_num = -1
    for key in the_map:
        key_num += 1
        the_set = the_map[key]
        print(description1.format(key_num=key_num, key=key, value=len(the_set)))
        item_num = -1
        for item in the_set:
            item_num += 1
            print(description2.format(key_num=key_num, key=key, item_num=item_num, item=item))

def print_method_to_reason_to_tests(method_to_reason_to_tests):
    """ Print a map that is a map from method -> reason -> set(tests) """
    method_num = -1
    for method in method_to_reason_to_tests:
        method_num += 1
        reason_to_tests = method_to_reason_to_tests[method]
        print((' '*4 + 'method[{method_num}]={method}').format(method=method, method_num=method_num))
        print_map_to_set(reason_to_tests, 
                         (' '*8 + 'reason[{key_num}]={key}, len(tests)={value}'),
                         (' '*12 + 'test[{item_num}]={item}'))

def print_reason_to_method_to_tests(reason_to_method_to_tests):
    """ Print a map that is a map from reason-> method -> set(tests) """
    reason_num = -1
    for reason in reason_to_method_to_tests:
        reason_num += 1
        method_to_tests = reason_to_method_to_tests[reason]
        print((' '*4 + 'reason[{reason_num}]={reason}').format(reason=reason, reason_num=reason_num))
        print_map_to_set(method_to_tests, 
                         (' '*8 + 'method[{key_num}]={key}'),
                         (' '*12 + 'test[{item_num}]={item}'))

def get_union_over_keys(map_to_set):
    """Compute the union of the map range values.
    
       For a map whose range is a set, union the range
       over keys in the map return the resulting set.
    """
    result = set()
    for key in map_to_set:
        value_set = map_to_set[key]
        result |= value_set;
    return result

def get_reason_to_tests(reason_to_method_to_tests):
    """ Union over methods to get tests failing for a reason."""
    reason_to_tests = {}
    for reason in reason_to_method_to_tests:
        method_to_tests = reason_to_method_to_tests[reason]
        tests = get_union_over_keys(method_to_tests)
        reason_to_tests[reason] = tests
    return reason_to_tests

def get_tests(reason_to_tests):
    """ Get set of failing tests from a failure_to_tests map.

        Union over failure reasons given a map from reasons to test sets,
        returning the set of failing tests.
    """
    return get_union_over_keys(reason_to_tests)

def print_reason_to_tests(reason_to_tests):
    """ Print a map from reasons to sets of tests failing for that reason."""
    print_map_to_set(reason_to_tests,
                     (' '*4 + 'reason[{key_num}]={key}, len(tests)={value}'),
                         (' '*8 + 'test[{item_num}]={item}'))

def print_set(the_set, description):
    """ Print a set, using the given descriptor to caption each item.

        Description can reference the item number (item_num) and the
        item (item).
    """
    item_num = -1
    for item in the_set:
        item_num += 1
        print(description.format(item_num=item_num, item=item))

def main(argv):
    """ Main program for checking one or two test runs. """

    # Define return code constants
    const.GeneralError = -1
    const.UnknownArguments = -2
    const.MissingResult = -3

    # Parse the command line    
    parser = argparse.ArgumentParser(description='''Check the output of a LLILC test run
    (optionally against a prior baseline run), for each test looking to see which methods LLILC
    reported as passing or failing. Reports the reason for any failures and the failing
    methods and tests. Return code is the number of unexpected failures, i.e.
    failures which did not already occur in the baseline (if given).''')
    required = parser.add_argument_group('required arguments')
    required.add_argument('-d', '--diff-result-path', type=str, required=True,
                        help='full path to target result')
    parser.add_argument('-b', '--base-result-path', type=str, 
                        help='full path to base result')
    parser.add_argument('-v', '--verbose', default=False, action="store_true",
                        help='Show target failures by test')
    parser.add_argument('--bs', '--baseline-summary', default=False, action="store_true",
                        help='Show baseline failure summary')
    parser.add_argument('--ts', '--target-summary', default=False, action="store_true",
                        help='''Show target failure summary. This has all target failures,
                        even those that failed in the baseline (if any).''')

    args, unknown = parser.parse_known_args(argv)

    if unknown:
        print('Unknown argument(s): ', ', '.join(unknown))
        return const.UnknownArguments
    verbose = args.verbose
    base_summary = args.bs
    target_summary = args.ts
    have_base = args.base_result_path != None
    try:
        # Collect a list of summary result file relative path in base result  
        if have_base:
            base_files = collect_summary_files(args.base_result_path)
        else:
            base_files = []
 
        # Collect a list of summary result file relative path in target result
        target_files = collect_summary_files(args.diff_result_path)
    except:
        e = sys.exc_info()[0]
        print('Error: CheckPass failed due to ', e)
        traceback.print_exc()
        return const.GeneralError

    # Check if results are empty
    if len(base_files) == 0:
        print('Warning: no base files, will analyze only the target')

    if len(target_files) == 0:
        print('Error: target result is empty')
        return const.MissingResult

    # We are comparing two test runs, the baseline and the "target".
    # For a given test, let
    # - bp be the set of passing methods for the baseline, i.e. those successfully read.
    # - bf be the set of failing methods for the baseline, i.e. those that failed to be read.
    # - tp be the set of passing methods for the target, i.e. those successfully read.
    # - tf be the set of failing methods for the target, i.e. those that failed to be read.
    # 
    # Then the universe, u, of methods for the test is given by
    #
    # u = bp | bf | tp | tf
    #
    # Let
    # - bm be methods of the universe missing from the baseline, i.e. bm = u - bp - bf.
    # - tm be methods of the universe missing from the target, i.e. tm = u - tp - tf.
    #
    # Then for the baseline {bp, bf, bm} partitions the universe, and
    # for the target {tp, tf, tm} partitions the universe.
    #
    # We can then categorize the methods by taking the cross-intersections of these partitions
    # as shown by this diagram:
    #
    #      +--------------+--------------+--------------+
    #      | bp           | bf           | bm           |
    #  +---+--------------+--------------+--------------+
    #  |tp | pp = bp & tp | fp = bf & tp | mp = bm & tp |
    #  +---+--------------+--------------+--------------+
    #  |tf | pf = bp & tf | ff = bf & tf | mf = bm & tf |
    #  +---+--------------+--------------+--------------+
    #  |tm | pm = bp & tm | fm = bf & tm | mm = bm & tm |
    #  +---+--------------+--------------+--------------+
    #
    # The set mm is the methods missing from both the baseline and the
    # target, so they will also be missing from the universe, so
    # mm is empty. We here describe the remaining sets and give them
    # short nicknames
    #
    # 1. pp passed in baseline and target: "old pass"
    # 2. fp failed in baseline but passed in target: "fix pass"
    # 3. mp was missing in baseline but passed in target: "new pass"
    # 4. pf passed in baseline but failed in target: "regressing fail"
    # 5. ff failed in baseline and target: "old fail"
    # 6. mf was missing in baseline but failed in target: "new failure"
    # 7. pm passed in baseline but is missing in target: "missing pass"
    # 8. fm failed in baseline but is missing in target: "missing fail"
    # 
    # The sum of categories 4 to 8 is the number of unexpected failures.

    # Clear counters for each category.
    num_pp_methods = 0
    num_fp_methods = 0
    num_mp_methods = 0
    num_pf_methods = 0
    num_ff_methods = 0
    num_mf_methods = 0
    num_pm_methods = 0
    num_fm_methods = 0

    # Maps to summarize failure tests and reasons.

    # method -> (reason -> set(tests))
    base_method_to_reason_to_tests = {}
    target_method_to_reason_to_tests = {}
    diff_method_to_reason_to_tests = {}

    # Maps reason -> (method -> set(tests))
    base_reason_to_method_to_tests = {}
    target_reason_to_method_to_tests = {}
    diff_reason_to_method_to_tests = {}

    # reason -> set(tests), summed over methods.
    base_reason_to_tests = {}
    target_reason_to_tests = {}
    diff_reason_to_tests = {}

    # This script expects input that contains lines of the form:
    # - "Successfully read <method name>" to indicate successsful compilation of
    #   a method
    # - "Failed to read <method name>[<failure reason>]" to indicate that a
    #   method failed to compile
    #
    # All other lines are ignored.
    pattern_reg_exp = re.compile(r'^(Successfully read (.*))|(Failed to read (.*)\[(.*)\])$')

    print('Checking started.')
    all_files = {}
    for file in base_files:
        all_files[file] = 1
    for file in target_files:
        if file in all_files:
            all_files[file] += 2
        else:
            all_files[file] = 2
    for file in all_files:
        try:
            # Every test now has its own result directory, so use that directory to
            # identify the test.
            test = os.path.dirname(file)
            in_which = all_files[file]
            if in_which == 1:
                # Only in base files.
                missing_result_file = os.path.join(args.diff_result_path, file)
                print('Warning: diff directory does not have result file ' + missing_result_file)
                continue

            # If we get here in_which must be 2 or 3, which means we have the target file.

            target_file_path = os.path.join(args.diff_result_path, file)
            target_passing_methods, target_failing_methods, target_fail_reason_map = \
                analyze(target_file_path, pattern_reg_exp)

            if in_which == 3:
                # file processed by both old and new.
                base_file_path = os.path.join(args.base_result_path, file)
                base_passing_methods, base_failing_methods, base_fail_reason_map = \
                    analyze(base_file_path, pattern_reg_exp)
            else:
                # in_which must be 2. No base file, new result file.
                if have_base:
                    # If we do not have a base, no point in printing this out.
                    print('Info: diff directory has new result file ', file)
                base_passing_methods, base_failing_methods = set(), set()
                base_fail_reason_map = {}

            tallyfailures(target_method_to_reason_to_tests, 
                          target_reason_to_method_to_tests,
                          test,
                          target_fail_reason_map)

            if have_base:
                diff_fail_reason_map = get_fail_reason_diff(target_fail_reason_map, 
                                                            base_fail_reason_map)
                # Compute failure summary maps.
                tallyfailures(base_method_to_reason_to_tests, 
                              base_reason_to_method_to_tests,
                              test,
                              base_fail_reason_map)

                tallyfailures(diff_method_to_reason_to_tests, 
                              diff_reason_to_method_to_tests,
                              test,
                              diff_fail_reason_map)

            # All methods processed in base, whether passing or failing.
            base_processed_methods = base_passing_methods | base_failing_methods

            # All methods processed in target, whether passing or failing.
            target_processed_methods = target_passing_methods | target_failing_methods

            universe_methods = base_processed_methods | target_processed_methods

            base_missing_methods = universe_methods - base_processed_methods
            target_missing_methods = universe_methods - target_processed_methods

            # Compute the grid
            pp_methods = base_passing_methods & target_passing_methods 
            fp_methods = base_failing_methods & target_passing_methods 
            mp_methods = base_missing_methods & target_passing_methods 
            pf_methods = base_passing_methods & target_failing_methods 
            ff_methods = base_failing_methods & target_failing_methods 
            mf_methods = base_missing_methods & target_failing_methods 
            pm_methods = base_passing_methods & target_missing_methods 
            fm_methods = base_failing_methods & target_missing_methods 

            pp_methods_len = len(pp_methods)
            fp_methods_len = len(fp_methods)
            mp_methods_len = len(mp_methods)
            pf_methods_len = len(pf_methods)
            ff_methods_len = len(ff_methods)
            mf_methods_len = len(mf_methods)
            pm_methods_len = len(pm_methods)
            fm_methods_len = len(fm_methods)

            test_unexpected_failures = (pf_methods_len + mf_methods_len +
                                        pm_methods_len + fm_methods_len)
            if (test_unexpected_failures > 0) & verbose:
                print('Unexpected failure(s) in test: ' + test)
                if have_base:
                    print_failures(pf_methods, ' regressing fail methods: passed in baseline but failed in target')
                    print_failures(mf_methods, ' new failure methods: was missing in baseline but failed in target')
                    print_failures(pm_methods, ' missing pass methods: passed in baseline but is missing in target')
                    print_failures(fm_methods, ' missing fail methods: failed in baseline but is missing in target')
                else:
                    print_failures(mf_methods, ' failed in target')

            num_pp_methods += pp_methods_len
            num_fp_methods += fp_methods_len
            num_mp_methods += mp_methods_len
            num_pf_methods += pf_methods_len
            num_ff_methods += ff_methods_len
            num_mf_methods += mf_methods_len
            num_pm_methods += pm_methods_len
            num_fm_methods += fm_methods_len

        except:
            e = sys.exc_info()[0]
            print('Error: CheckPass failed due to ', e)
            traceback.print_exc()
            return const.GeneralError

    num_target_failures = num_pf_methods + num_ff_methods + num_mf_methods
    target_reason_to_tests = get_reason_to_tests(target_reason_to_method_to_tests)
    target_failed_tests = get_tests(target_reason_to_tests)

    if ((not have_base) or target_summary) and (num_target_failures > 0):
        print("Target failures summaries")
        print("")
        print("  method -> reason -> set(test)")
        print_method_to_reason_to_tests(target_method_to_reason_to_tests)
        print("")
        print("  reason -> method -> set(test)")
        print_reason_to_method_to_tests(target_reason_to_method_to_tests)
        print("  reason -> set(test)")
        print_reason_to_tests(target_reason_to_tests)
        print("   Failing tests")
        print_set(target_failed_tests, (' '*4 + 'test[{item_num}]={item}'))

    if have_base:
        num_base_failures = num_fp_methods + num_ff_methods + num_fm_methods
        num_diff_failures = num_pf_methods + num_mf_methods

        base_reason_to_tests = get_reason_to_tests(base_reason_to_method_to_tests)
        base_failed_tests = get_tests(base_reason_to_tests)
        diff_reason_to_tests = get_reason_to_tests(diff_reason_to_method_to_tests)
        diff_failed_tests = get_tests(diff_reason_to_tests)

        if base_summary & (num_base_failures > 0):
            print("Base failures summaries")
            print("")
            print("  method -> reason -> set(test)")
            print_method_to_reason_to_tests(base_method_to_reason_to_tests)
            print("")
            print("  reason -> method -> set(test)")
            print_reason_to_method_to_tests(base_reason_to_method_to_tests)
            print("  reason -> set(test)")
            print_reason_to_tests(base_reason_to_tests)
            print("   Failing tests")
            print_set(base_failed_tests, (' '*4 + 'test[{item_num}]={item}'))


        if num_diff_failures > 0:
            print("Diff failures summaries")
            print("")
            print("  method -> reason -> set(test)")
            print_method_to_reason_to_tests(diff_method_to_reason_to_tests)
            print("")
            print("  reason -> method -> set(test)")
            print_reason_to_method_to_tests(diff_reason_to_method_to_tests)
            print("  reason -> set(test)")
            print_reason_to_tests(diff_reason_to_tests)
            print("   Failing tests")
            print_set(diff_failed_tests, (' '*4 + 'test[{item_num}]={item}'))

    if have_base:
        report(num_pp_methods, '{0} old pass methods: passed in baseline and target')
        report(num_fp_methods, '{0} fix pass methods: failed in baseline but passed in target')
        report(num_mp_methods, '{0} new pass methods: was missing in baseline but passed in target')
        report(num_pf_methods, '{0} regressing fail methods: passed in baseline but failed in target')
        report(num_ff_methods, '{0} old fail methods: failed in baseline and target')
        report(num_mf_methods, '{0} new failure methods: was missing in baseline but failed in target')
        report(num_pm_methods, '{0} missing pass methods: passed in baseline but is missing in target')
        report(num_fm_methods, '{0} missing fail methods: failed in baseline but is missing in target')
    else:
        report(num_mp_methods, '{0} pass methods')
        report(num_mf_methods, '{0} failure methods')

    unexpected_failures = (num_pf_methods + num_mf_methods)
    if have_base:
        report(unexpected_failures, '{0} total failing methods excluding old fails')

    if unexpected_failures == 0:
        print('There were no unexpected failures.')
    else:
        print('There were unexpected failures.')

    return unexpected_failures

if __name__ == '__main__':
    return_code = main(sys.argv[1:])
    sys.exit(return_code)
