#!/bin/bash
#
# Copyright (C) 2014 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

if [ ! -d libcore ]; then
  echo "Script needs to be run at the root of the android tree"
  exit 1
fi

# Jar containing jsr166 tests.
jsr166_test_jar=${OUT_DIR-out}/target/common/obj/JAVA_LIBRARIES/jsr166-tests_intermediates/javalib.jar

# Jar containing all the other tests.
test_jar=${OUT_DIR-out}/target/common/obj/JAVA_LIBRARIES/core-tests_intermediates/javalib.jar


if [ ! -f $test_jar ]; then
  echo "Before running, you must build core-tests, jsr166-tests and vogar: \
        make core-tests jsr166-tests vogar vogar.jar"
  exit 1
fi

emulator="no"
if [ "$ANDROID_SERIAL" = "emulator-5554" ]; then
  emulator="yes"
fi

# Packages that currently work correctly with the expectation files.
working_packages=("dalvik.system"
                  "libcore.icu"
                  "libcore.io"
                  "libcore.java.lang"
                  "libcore.java.math"
                  "libcore.java.text"
                  "libcore.java.util"
                  "libcore.javax.crypto"
                  "libcore.javax.security"
                  "libcore.javax.sql"
                  "libcore.javax.xml"
                  "libcore.net"
                  "libcore.reflect"
                  "libcore.util"
                  "org.apache.harmony.annotation"
                  "org.apache.harmony.crypto"
                  "org.apache.harmony.luni"
                  "org.apache.harmony.nio"
                  "org.apache.harmony.regex"
                  "org.apache.harmony.security"
                  "org.apache.harmony.testframework"
                  "org.apache.harmony.tests.java.io"
                  "org.apache.harmony.tests.java.lang"
                  "org.apache.harmony.tests.java.math"
                  "org.apache.harmony.tests.java.util"
                  "org.apache.harmony.tests.java.text"
                  "org.apache.harmony.tests.javax.security"
                  "tests.java.lang.String"
                  "jsr166")
packages=${working_packages[@]}

# Options passed to Vogar when `--mode=device` is passed to this script.
#
# Use the ART instance located in path /data/local/tmp/ on device, and
# a boot image built with the Optimizing compiler.
vogar_mode_device_args="--device-dir=/data/local/tmp"
vogar_mode_device_args="$vogar_mode_device_args --vm-command=/data/local/tmp/system/bin/art"
vogar_mode_device_args="$vogar_mode_device_args --vm-arg -Ximage:/data/art-test/core-optimizing.art"

# Options passed to Vogar when `--mode=host` is passed to this script.
#
# We explicitly give a wrong path for the image, to ensure Vogar
# will create a boot image with the default compiler. Note that
# giving an existing image on host does not work because of
# classpath/resources differences when compiling the boot image.
vogar_mode_host_args="--vm-arg -Ximage:/non/existent"

vogar_args=$@
while true; do
  if [[ "$1" == "--mode=device" ]]; then
    # Option `--mode=device` is passed to Vogar, as well as options in `$vogar_mode_device_args`.
    vogar_args="$vogar_args $vogar_mode_device_args"
    shift
  elif [[ "$1" == "--mode=host" ]]; then
    # Option `--mode=host` is passed to Vogar, as well as options in `$vogar_mode_host_args`.
    vogar_args="$vogar_args $vogar_mode_host_args"
    shift
  elif [[ "$1" == --packages=* ]]; then
    # Initialize array `packages` from a space-separated list of packages.
    packages_list=$(echo "$1" | sed 's/--packages=//')
    packages=($packages_list)
    # Remove the --package=* option from the list of Vogar arguments.
    vogar_args=${vogar_args/$1}
    shift
  elif [[ "$1" == "--show-default-packages" ]]; then
    echo ${working_packages[@]} | tr " " "\n"
    exit
  elif [[ "$1" == "--debug" ]]; then
    # Remove the --debug option from the list of Vogar arguments.
    vogar_args=${vogar_args/$1}
    vogar_args="$vogar_args --vm-arg -XXlib:libartd.so"
    shift
  elif [[ "$1" == "--help" ]]; then
    cat <<EOF
Usage: $0 [OPTION]...
Run libcore tests using Vogar.

Options:
  --packages=PACKAGE_LIST     Use the space-separated PACKAGE_LIST instead of
                                the default package list.
  --show-default-packages     Show the default list of packages used and exit.
  --debug                     Use the runtime's debug mode (i.e. use libartd.so
                                instead of libart.so).
  --help                      Display this help and exit.

Other options are passed to Vogar as-is, e.g. '--mode=device', '--mode=host',
'--variant=X32', etc.  See 'vogar --help' for more information.

When running on a device ('--mode=device'), Vogar is also passed these options:
  $vogar_mode_device_args

When running on host ('--mode=host'), Vogar is also passed these options:
  $vogar_mode_host_args

EOF
    exit
  elif [[ "$1" == "" ]]; then
    # End of parameter list.
    break
  else
    # Untouched option passed to Vogar.
    shift
  fi
done

# Increase the timeout, as vogar cannot set individual test
# timeout when being asked to run packages, and some tests go above
# the default timeout.
vogar_args="$vogar_args --timeout 480"

# Run the tests using vogar.
echo "Running tests for the following test packages:"
echo ${packages[@]} | tr " " "\n"
vogar $vogar_args --expectations art/tools/libcore_failures.txt \
  --classpath $jsr166_test_jar --classpath $test_jar ${packages[@]}
