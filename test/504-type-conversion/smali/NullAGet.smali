# Copyright (C) 2015 The Android Open Source Project
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

.class public LNullAGet;

.super Ljava/lang/Object;

.method public static method()B
   .registers 2
   const/4 v0, 0
   # Do a aget-object on a null constant. The verifier
   # used to consider the result of that instruction to be zero.
   aget-object v0, v0, v0
   # By having v0 be zero, the verifier used to consider the
   # following instruction properly typed.
   int-to-byte v0, v0
   return v0
.end method
