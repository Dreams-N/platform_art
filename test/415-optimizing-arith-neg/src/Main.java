/*
 * Copyright (C) 2014 The Android Open Source Project
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

// Note that $opt$ is a marker for the optimizing compiler to ensure
// it does compile the method.
public class Main {

  public static void expectEquals(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void expectEquals(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void expectEquals(float expected, float result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void expectEquals(double expected, double result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void expectNaN(float result) {
    if (!Float.isNaN(result)) {
      throw new Error("Expected NaN: " + result);
    }
  }

  public static void expectNaN(double result) {
    if (!Double.isNaN(result)) {
      throw new Error("Expected NaN: " + result);
    }
  }

  public static void main(String[] args) {
    negInt();
    $opt$InplaceNegOneInt(1);

    negLong();
    $opt$InplaceNegOneLong(1L);

    negFloat();
    negDouble();
  }

  private static void negInt() {
    expectEquals(-1, $opt$NegInt(1));
    expectEquals(1, $opt$NegInt(-1));
    expectEquals(0, $opt$NegInt(0));
    expectEquals(51, $opt$NegInt(-51));
    expectEquals(-51, $opt$NegInt(51));
    expectEquals(2147483647, $opt$NegInt(-2147483647));  // (2^31 - 1)
    expectEquals(-2147483647, $opt$NegInt(2147483647));  // -(2^31 - 1)
    // From the Java 7 SE Edition specification:
    // http://docs.oracle.com/javase/specs/jls/se7/html/jls-15.html#jls-15.15.4
    //
    //   For integer values, negation is the same as subtraction from
    //   zero.  The Java programming language uses two's-complement
    //   representation for integers, and the range of two's-complement
    //   values is not symmetric, so negation of the maximum negative
    //   int or long results in that same maximum negative number.
    //   Overflow occurs in this case, but no exception is thrown.
    //   For all integer values x, -x equals (~x)+1.''
    expectEquals(-2147483648, $opt$NegInt(-2147483648)); // -(2^31)
  }

  private static void $opt$InplaceNegOneInt(int a) {
    a = -a;
    expectEquals(-1, a);
  }

  private static void negLong() {
    expectEquals(-1L, $opt$NegLong(1L));
    expectEquals(1L, $opt$NegLong(-1L));
    expectEquals(0L, $opt$NegLong(0L));
    expectEquals(51L, $opt$NegLong(-51L));
    expectEquals(-51L, $opt$NegLong(51L));

    expectEquals(2147483647L, $opt$NegLong(-2147483647L));  // (2^31 - 1)
    expectEquals(-2147483647L, $opt$NegLong(2147483647L));  // -(2^31 - 1)
    expectEquals(2147483648L, $opt$NegLong(-2147483648L));  // 2^31
    expectEquals(-2147483648L, $opt$NegLong(2147483648L));  // -(2^31)

    expectEquals(9223372036854775807L, $opt$NegLong(-9223372036854775807L));  // (2^63 - 1)
    expectEquals(-9223372036854775807L, $opt$NegLong(9223372036854775807L));  // -(2^63 - 1)
    // See remark regarding the negation of the maximum negative
    // (long) value in negInt().
    expectEquals(-9223372036854775808L, $opt$NegLong(-9223372036854775808L)); // -(2^63)
  }

  private static void $opt$InplaceNegOneLong(long a) {
    a = -a;
    expectEquals(-1L, a);
  }

  private static void negFloat() {
     expectEquals(-1F, $opt$NegFloat(1F));
     expectEquals(1F, $opt$NegFloat(-1F));
     expectEquals(0F, $opt$NegFloat(0F));
     expectEquals(51F, $opt$NegFloat(-51F));
     expectEquals(-51F, $opt$NegFloat(51F));

     expectEquals(-0.1F, $opt$NegFloat(0.1F));
     expectEquals(0.1F, $opt$NegFloat(-0.1F));
     expectEquals(343597.38362F, $opt$NegFloat(-343597.38362F));
     expectEquals(-343597.38362F, $opt$NegFloat(343597.38362F));

     expectEquals(-Float.MIN_NORMAL, $opt$NegFloat(Float.MIN_NORMAL));
     expectEquals(Float.MIN_NORMAL, $opt$NegFloat(-Float.MIN_NORMAL));
     expectEquals(-Float.MIN_VALUE, $opt$NegFloat(Float.MIN_VALUE));
     expectEquals(Float.MIN_VALUE, $opt$NegFloat(-Float.MIN_VALUE));
     expectEquals(-Float.MAX_VALUE, $opt$NegFloat(Float.MAX_VALUE));
     expectEquals(Float.MAX_VALUE, $opt$NegFloat(-Float.MAX_VALUE));

     expectEquals(Float.NEGATIVE_INFINITY, $opt$NegFloat(Float.POSITIVE_INFINITY));
     expectEquals(Float.POSITIVE_INFINITY, $opt$NegFloat(Float.NEGATIVE_INFINITY));
     expectNaN($opt$NegFloat(Float.NaN));
  }

  private static void negDouble() {
     expectEquals(-1D, $opt$NegDouble(1D));
     expectEquals(1D, $opt$NegDouble(-1D));
     expectEquals(0D, $opt$NegDouble(0D));
     expectEquals(51D, $opt$NegDouble(-51D));
     expectEquals(-51D, $opt$NegDouble(51D));

     expectEquals(-0.1D, $opt$NegDouble(0.1D));
     expectEquals(0.1D, $opt$NegDouble(-0.1D));
     expectEquals(343597.38362D, $opt$NegDouble(-343597.38362D));
     expectEquals(-343597.38362D, $opt$NegDouble(343597.38362D));

     expectEquals(-Double.MIN_NORMAL, $opt$NegDouble(Double.MIN_NORMAL));
     expectEquals(Double.MIN_NORMAL, $opt$NegDouble(-Double.MIN_NORMAL));
     expectEquals(-Double.MIN_VALUE, $opt$NegDouble(Double.MIN_VALUE));
     expectEquals(Double.MIN_VALUE, $opt$NegDouble(-Double.MIN_VALUE));
     expectEquals(-Double.MAX_VALUE, $opt$NegDouble(Double.MAX_VALUE));
     expectEquals(Double.MAX_VALUE, $opt$NegDouble(-Double.MAX_VALUE));

     expectEquals(Double.NEGATIVE_INFINITY, $opt$NegDouble(Double.POSITIVE_INFINITY));
     expectEquals(Double.POSITIVE_INFINITY, $opt$NegDouble(Double.NEGATIVE_INFINITY));
     expectNaN($opt$NegDouble(Double.NaN));
  }

  static int $opt$NegInt(int a){
    return -a;
  }

  static long $opt$NegLong(long a){
    return -a;
  }

  static float $opt$NegFloat(float a){
    return -a;
  }

  static double $opt$NegDouble(double a){
    return -a;
  }
}
