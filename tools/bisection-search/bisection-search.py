#!/usr/bin/env python2.7
#
# Copyright (C) 2016 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Performs bisection bug search on methods and optimizations.

See README.

Example usage:
./art/tools/bisection-search.py -cp classes.dex --correct-output out Test
"""

from __future__ import division
from __future__ import print_function
import argparse
import re
import sys
from test_utils import DeviceTestEnv
from test_utils import FatalError
from test_utils import GetAndroidTop
from test_utils import HostTestEnv

# Passes which are always run in order for compilation to complete successfully.
MANDATORY_PASSES = ["dex_cache_array_fixups_arm",
                    "dex_cache_array_fixups_mips",
                    "instruction_simplifier$before_codegen",
                    "pc_relative_fixups_x86",
                    "pc_relative_fixups_mips",
                    "x86_memory_operand_generation"]
# Passes which are not optimizations. They are not affected by the --run-passes
# mechanism and will run regardless.
NON_PASSES = ["builder", "prepare_for_register_allocation",
              "liveness", "register"]


class Dex2OatWrapperTestable(object):
  """Class representing a testable compilation.

  Accepts filters on compiled methods and optimization passes.
  """

  def __init__(self, base_cmd, test_env, class_name, correct_output=None,
               verbose=False):
    """Constructor.

    Args:
      base_cmd: string. base command to run.
      test_env: ITestEnv.
      class_name: string, name of class to run.
      correct_output: string, correct output to compare against or None.
      verbose: bool, enable verbose output.
    """
    self._base_cmd = base_cmd
    self._test_env = test_env
    self._class_name = class_name
    self._correct_output = correct_output
    self._compiled_methods_path = self._test_env.CreateFile("compiled_methods")
    self._passes_to_run_path = self._test_env.CreateFile("run_passes")
    self._verbose = verbose

  def Test(self, compiled_methods, passes_to_run=None):
    """Tests compilation with compiled_methods and run_passes switches active.

    If compiled_methods is None then compiles all methods.
    If passes_to_run is None then runs default passes.

    Args:
      compiled_methods: List of strings representing methods to compile or None.
      passes_to_run: List of strings representing passes to run or None.

    Returns:
      True if test passes with given settings. False otherwise.
    """
    if self._verbose:
      print("Testing methods: {0} passes:{1}.".format(
          compiled_methods, passes_to_run))
    cmd = self._PrepareCmd(compiled_methods=compiled_methods,
                           passes_to_run=passes_to_run,
                           verbose_compiler=True)
    (output, _, ret_code) = self._test_env.RunCommand(cmd)
    res = ret_code != 0 or output == self._correct_output
    if self._verbose:
      print("Test passed: {0}.".format(res))
    return res

  def GetAllMethods(self):
    """Get methods compiled during the test.

    Returns:
      List of strings representing methods compiled during the test.

    Raises:
      FatalError: An error occurred when retrieving methods list.
    """
    cmd = self._PrepareCmd(verbose_compiler=True)
    (_, err_output, _) = self._test_env.RunCommand(cmd)
    match_methods = re.findall(r"Building ([^\n]+)\n", err_output)
    if not match_methods:
      raise FatalError("Failed to retrieve methods list. "
                       "Not recognized output format.")
    return match_methods

  def GetAllPassesForMethod(self, compiled_method):
    """Get all optimization passes ran for a method during the test.

    Args:
      compiled_method: String representing method to compile.

    Returns:
      List of strings representing passes ran for compiled_method during test.

    Raises:
      FatalError: An error occurred when retrieving passes list.
    """
    cmd = self._PrepareCmd(compiled_methods=[compiled_method],
                           verbose_compiler=True)
    (_, err_output, _) = self._test_env.RunCommand(cmd)
    match_passes = re.findall(r"Starting pass: ([^\n]+)\n", err_output)
    if not match_passes:
      raise FatalError("Failed to retrieve passes list. "
                       "Not recognized output format.")
    return [p for p in match_passes if p not in NON_PASSES]

  def _PrepareCmd(self, compiled_methods=None, passes_to_run=None,
                  verbose_compiler=False):
    """Prepare command to run."""
    cmd = self._base_cmd
    if compiled_methods is not None:
      self._test_env.WriteLines(self._compiled_methods_path, compiled_methods)
      cmd += " -Xcompiler-option --compiled-methods={0}".format(
          self._compiled_methods_path)
    if passes_to_run is not None:
      self._test_env.WriteLines(self._passes_to_run_path, passes_to_run)
      cmd += " -Xcompiler-option --run-passes={0}".format(
          self._passes_to_run_path)
    if verbose_compiler:
      cmd += (" -Xcompiler-option --runtime-arg -Xcompiler-option"
              " -verbose:compiler")
    cmd += " -classpath {0} {1}".format(self._test_env.classpath,
                                        self._class_name)
    return cmd


def BinarySearch(start, end, test):
  """Binary search integers using test function to guide the process."""
  while start < end:
    mid = (start + end) // 2
    if test(mid):
      start = mid + 1
    else:
      end = mid
  return start


def FilterPasses(passes, cutoff_idx):
  """Filters passes list according to cutoff_idx but keeps mandatory passes."""
  return [opt_pass for idx, opt_pass in enumerate(passes)
          if opt_pass in MANDATORY_PASSES or idx < cutoff_idx]


def BugSearch(testable):
  """Find buggy (method, optimization pass) pair for a given testable.

  Args:
    testable: Dex2OatWrapperTestable.

  Returns:
    (string, string) tuple. First element is name of method which when compiled
    exposes test failure. Second element is name of optimization pass such that
    for aforementioned method running all passes up to and excluding the pass
    results in test passing but running all passes up to and including the pass
    results in test failing.

    (None, None) if test passes when compiling all methods.
    (string, None) if a method is found which exposes the failure, but the
      failure happens even when running just mandatory passes.
  """
  all_methods = testable.GetAllMethods()
  faulty_method_idx = BinarySearch(
      0,
      len(all_methods),
      lambda mid: testable.Test(all_methods[0:mid]))
  if faulty_method_idx == len(all_methods):
    return (None, None)
  if faulty_method_idx == 0:
    raise FatalError("Testable fails with no methods compiled. "
                     "Perhaps issue lies outside of compiler.")
  faulty_method = all_methods[faulty_method_idx - 1]
  all_passes = testable.GetAllPassesForMethod(faulty_method)
  faulty_pass_idx = BinarySearch(
      0,
      len(all_passes),
      lambda mid: testable.Test([faulty_method],
                                FilterPasses(all_passes, mid)))
  if faulty_pass_idx == 0:
    return (faulty_method, None)
  assert faulty_pass_idx != len(all_passes), "Method must fail for some passes."
  faulty_pass = all_passes[faulty_pass_idx - 1]
  return (faulty_method, faulty_pass)


def PrepareParser():
  """Prepares argument parser."""
  parser = argparse.ArgumentParser()
  parser.add_argument(
      "-cp", "--classpath", required=True, type=str, help="classpath")
  parser.add_argument("--correct-output",
                      type=str,
                      help="file containing correct output")
  parser.add_argument(
      "--host", action="store_true", default=False, help="run on host")
  parser.add_argument("classname", help="name of class to run")
  parser.add_argument("--64", dest="x64", action="store_true",
                      default=False, help="x64 mode")
  parser.add_argument("-d", dest="lib_debug", action="store_true",
                      default=False, help="use libartd.so")
  parser.add_argument("--dalvikvm-option", dest="extra", metavar="OPTION",
                      nargs="*", help="additional dalvikvm argument")
  parser.add_argument("--verbose", action="store_true",
                      default=False, help="enable verbose output")
  return parser


def main():
  # Parse arguments
  parser = PrepareParser()
  args = parser.parse_args()

  # Prepare environment
  if args.correct_output is not None:
    with open(args.correct_output, "r") as myfile:
      correct_output = myfile.read()
  else:
    correct_output = None
  if args.host:
    run_cmd = "dalvikvm" if args.x64 else "dalvikvm32"
    run_cmd += " -XXlib:{0}".format(
        "libartd.so" if args.lib_debug else "libart.so")
    run_cmd += " -Xnorelocate"
    run_cmd += (" -Ximage:{0}/out/host/linux-x86/framework/core-optimizing-pic"
                ".art").format(GetAndroidTop())
    if args.extra:
      run_cmd += " " + " ".join(["{0}".format(arg) for arg in args.extra])
    test_env = HostTestEnv(args.classpath, args.x64)
  else:
    run_cmd = "dalvikvm" if args.x64 else "dalvikvm32"
    test_env = DeviceTestEnv(args.classpath)

  # Perform the search
  try:
    testable = Dex2OatWrapperTestable(run_cmd, test_env, args.classname,
                                      correct_output, args.verbose)
    (method, opt_pass) = BugSearch(testable)
  except Exception, e:
    print("Error. Refer to logfile: {0}".format(test_env.logfile.name))
    test_env.logfile.write("Exception: {0}\n".format(e))
    raise

  # Report results
  if method is None:
    print("Couldn't find any bugs.")
  elif opt_pass is None:
    print("Faulty method: {0}. Fails with just mandatory passes.".format(
        method))
  else:
    print("Faulty method and pass: {0}, {1}.".format(method, opt_pass))
  print("Logfile: {0}".format(test_env.logfile.name))
  sys.exit(0)


if __name__ == "__main__":
  main()
