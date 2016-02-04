/*
 * Copyright (C) 2016 The Android Open Source Project
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

public final class DerivedInSecondDex extends BaseInMainDex {
  DerivedInSecondDex() {
    super();
  }

  DerivedInSecondDex(int intValue) {
    // Not matched: Superclass in a different dex file has an IPUT.
    super(intValue);
  }

  DerivedInSecondDex(long dummy) {
    // Matched: Superclass in a different dex file has an IPUT that's pruned because we store 0.
    super(0);
  }
}
