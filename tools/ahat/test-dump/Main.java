/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import dalvik.system.VMDebug;
import java.io.IOException;

/**
 * Program used to create a heap dump for test purposes.
 */
public class Main {
  public static DumpedStuff stuff;

  public static class DumpedStuff {
    public String basicString = "hello, world";
  }

  public static void main(String[] args) throws IOException {
    if (args.length < 1) {
      System.err.println("no output file specified");
      return;
    }
    String file = args[0];

    stuff = new DumpedStuff();

    System.err.println("Dumping hprof data to " + file);
    VMDebug.dumpHprofData(file);
  }
}
