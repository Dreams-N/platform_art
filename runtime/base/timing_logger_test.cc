/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "timing_logger.h"

#include "common_runtime_test.h"

namespace art {

class TimingLoggerTest : public CommonRuntimeTest {};

// TODO: Negative test cases (improper pairing of EndSplit, etc.)

TEST_F(TimingLoggerTest, StartEnd) {
  const char* split1name = "First Split";
  TimingLogger logger("StartEnd", true, false);
  logger.StartTiming(split1name);
  logger.EndTiming();  // Ends split1.
  const auto& timings = logger.GetTimings();
  EXPECT_EQ(2U, timings.size());  // Start, End splits
  EXPECT_TRUE(timings[0].IsStartTiming());
  EXPECT_STREQ(timings[0].GetName(), split1name);
  EXPECT_TRUE(timings[1].IsEndTiming());
}


TEST_F(TimingLoggerTest, StartNewEnd) {
  const char* split1name = "First Split";
  const char* split2name = "Second Split";
  const char* split3name = "Third Split";
  TimingLogger logger("StartNewEnd", true, false);
  logger.StartTiming(split1name);
  logger.NewTiming(split2name);
  logger.NewTiming(split3name);
  logger.EndTiming();
  // Get the timings and verify that they are sane.
  const auto& timings = logger.GetTimings();
  // 6 timings in the timing logger at this point.
  EXPECT_EQ(6U, timings.size());
  EXPECT_TRUE(timings[0].IsStartTiming());
  EXPECT_STREQ(timings[0].GetName(), split1name);
  EXPECT_TRUE(timings[1].IsEndTiming());
  EXPECT_TRUE(timings[2].IsStartTiming());
  EXPECT_STREQ(timings[2].GetName(), split2name);
  EXPECT_TRUE(timings[3].IsEndTiming());
  EXPECT_TRUE(timings[4].IsStartTiming());
  EXPECT_STREQ(timings[4].GetName(), split3name);
  EXPECT_TRUE(timings[5].IsEndTiming());
}

TEST_F(TimingLoggerTest, StartNewEndNested) {
  const char* name1 = "First Split";
  const char* name2 = "Second Split";
  const char* name3 = "Third Split";
  const char* name4 = "Fourth Split";
  const char* name5 = "Fifth Split";
  TimingLogger logger("StartNewEndNested", true, false);
  logger.StartTiming(name1);
  logger.NewTiming(name2);  // Ends timing1.
  logger.StartTiming(name3);
  logger.StartTiming(name4);
  logger.NewTiming(name5);  // Ends timing4.
  logger.EndTiming();  // Ends timing5.
  logger.EndTiming();  // Ends timing3.
  logger.EndTiming();  // Ends timing2.
  const auto& timings = logger.GetTimings();
  EXPECT_EQ(10U, timings.size());
  size_t idx_1 = logger.FindTimingIndex(name1, 0);
  size_t idx_2 = logger.FindTimingIndex(name2, 0);
  size_t idx_3 = logger.FindTimingIndex(name3, 0);
  size_t idx_4 = logger.FindTimingIndex(name4, 0);
  size_t idx_5 = logger.FindTimingIndex(name5, 0);
  size_t idx_6 = logger.FindTimingIndex("Not found", 0);
  EXPECT_NE(idx_1, TimingLogger::kIndexNotFound);
  EXPECT_NE(idx_2, TimingLogger::kIndexNotFound);
  EXPECT_NE(idx_3, TimingLogger::kIndexNotFound);
  EXPECT_NE(idx_4, TimingLogger::kIndexNotFound);
  EXPECT_NE(idx_5, TimingLogger::kIndexNotFound);
  EXPECT_EQ(idx_6, TimingLogger::kIndexNotFound);
  // EXPECT_STREQ(splits[0].second, split1name);
  // EXPECT_STREQ(splits[1].second, split4name);
  // EXPECT_STREQ(splits[2].second, split5name);
  // EXPECT_STREQ(splits[3].second, split3name);
  // EXPECT_STREQ(splits[4].second, split2name);
}


TEST_F(TimingLoggerTest, Scoped) {
  const char* outersplit = "Outer Split";
  const char* innersplit1 = "Inner Split 1";
  const char* innerinnersplit1 = "Inner Inner Split 1";
  const char* innersplit2 = "Inner Split 2";
  TimingLogger logger("Scoped", true, false);
  {
    TimingLogger::ScopedTiming outer(outersplit, &logger);
    {
      TimingLogger::ScopedTiming inner1(innersplit1, &logger);
      {
        TimingLogger::ScopedTiming innerinner1(innerinnersplit1, &logger);
      }  // Ends innerinnersplit1.
    }  // Ends innersplit1.
    {
      TimingLogger::ScopedTiming inner2(innersplit2, &logger);
    }  // Ends innersplit2.
  }  // Ends outersplit.
  const auto& timings = logger.GetTimings();
  EXPECT_EQ(8U, timings.size());  // 4 start timings and 4 end timings.
  // EXPECT_STREQ(timings[0].second, innerinnersplit1);
  // EXPECT_STREQ(timings[1].second, innersplit1);
  // EXPECT_STREQ(timings[2].second, innersplit2);
  // EXPECT_STREQ(timings[3].second, outersplit);
}


TEST_F(TimingLoggerTest, ScopedAndExplicit) {
  const char* outersplit = "Outer Split";
  const char* innersplit = "Inner Split";
  const char* innerinnersplit1 = "Inner Inner Split 1";
  const char* innerinnersplit2 = "Inner Inner Split 2";
  TimingLogger logger("Scoped", true, false);
  logger.StartTiming(outersplit);
  {
    TimingLogger::ScopedTiming inner(innersplit, &logger);
    logger.StartTiming(innerinnersplit1);
    logger.NewTiming(innerinnersplit2);  // Ends innerinnersplit1.
    logger.EndTiming();
  }  // Ends innerinnersplit2, then innersplit.
  logger.EndTiming();  // Ends outersplit.
  const auto& timings = logger.GetTimings();
  EXPECT_EQ(8U, timings.size());
  // EXPECT_STREQ(timings[0].second, innerinnersplit1);
  // EXPECT_STREQ(timings[1].second, innerinnersplit2);
  // EXPECT_STREQ(timings[2].second, innersplit);
  // EXPECT_STREQ(timings[3].second, outersplit);
}

}  // namespace art
