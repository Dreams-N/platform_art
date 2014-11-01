/*
 * Copyright (C) 2006 The Android Open Source Project
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

/**
 * Miranda testing.
 */
public class MirandaClass extends MirandaAbstract {

    public MirandaClass() {}

    public boolean inInterface() {
        //System.out.println("    MirandaClass inInterface");
        return true;
    }

    public int inInterface2() {
        //System.out.println("    MirandaClass inInterface2");
        return 27;
    }

    public boolean inAbstract() {
        //System.out.println("    MirandaClass inAbstract");
        return false;
    }

    // Better not hit any of these...
    public void inInterfaceDummy1() {
        System.out.println("inInterfaceDummy1");
    }
    public void inInterfaceDummy2() {
        System.out.println("inInterfaceDummy2");
    }
    public void inInterfaceDummy3() {
        System.out.println("inInterfaceDummy3");
    }
    public void inInterfaceDummy4() {
        System.out.println("inInterfaceDummy4");
    }
    public void inInterfaceDummy5() {
        System.out.println("inInterfaceDummy5");
    }
}
