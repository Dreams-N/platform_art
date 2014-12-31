public class Inliner {

  // CHECK-START: void Inliner.InlineVoid() inliner (before)
  // CHECK-DAG:     [[Const42:i##]] IntConstant 42
  // CHECK-DAG:                     InvokeStaticOrDirect
  // CHECK-DAG:                     InvokeStaticOrDirect [ [[Const42]] ]

  // CHECK-START: void Inliner.InlineVoid() inliner (after)
  // CHECK-NOT:                     InvokeStaticOrDirect

  public static void InlineVoid() {
    returnVoid();
    returnVoidWithOneParameter(42);
  }

  // CHECK-START: int Inliner.InlineParameter(int) inliner (before)
  // CHECK-DAG:     [[Param:i##]]  ParameterValue
  // CHECK-DAG:     [[Result:i##]] InvokeStaticOrDirect [ [[Param]] ]
  // CHECK-DAG:                    Return [ [[Result]] ]

  // CHECK-START: int Inliner.InlineParameter(int) inliner (after)
  // CHECK-DAG:     [[Param:i##]]  ParameterValue
  // CHECK-DAG:                    Return [ [[Param]] ]

  public static int InlineParameter(int a) {
    return returnParameter(a);
  }

  // CHECK-START: long Inliner.InlineWideParameter(long) inliner (before)
  // CHECK-DAG:     [[Param:j##]]  ParameterValue
  // CHECK-DAG:     [[Result:j##]] InvokeStaticOrDirect [ [[Param]] ]
  // CHECK-DAG:                    Return [ [[Result]] ]

  // CHECK-START: long Inliner.InlineWideParameter(long) inliner (after)
  // CHECK-DAG:     [[Param:j##]]  ParameterValue
  // CHECK-DAG:                    Return [ [[Param]] ]

  public static long InlineWideParameter(long a) {
    return returnWideParameter(a);
  }

  // CHECK-START: java.lang.Object Inliner.InlineReferenceParameter(java.lang.Object) inliner (before)
  // CHECK-DAG:     [[Param:l##]]  ParameterValue
  // CHECK-DAG:     [[Result:l##]] InvokeStaticOrDirect [ [[Param]] ]
  // CHECK-DAG:                    Return [ [[Result]] ]

  // CHECK-START: java.lang.Object Inliner.InlineReferenceParameter(java.lang.Object) inliner (after)
  // CHECK-DAG:     [[Param:l##]]  ParameterValue
  // CHECK-DAG:                    Return [ [[Param]] ]

  public static Object InlineReferenceParameter(Object o) {
    return returnReferenceParameter(o);
  }

  // CHECK-START: int Inliner.InlineInt() inliner (before)
  // CHECK-DAG:     [[Result:i##]] InvokeStaticOrDirect
  // CHECK-DAG:                    Return [ [[Result]] ]

  // CHECK-START: int Inliner.InlineInt() inliner (after)
  // CHECK-DAG:     [[Const4:i##]] IntConstant 4
  // CHECK-DAG:                    Return [ [[Const4]] ]

  public static int InlineInt() {
    return returnInt();
  }

  // CHECK-START: long Inliner.InlineWide() inliner (before)
  // CHECK-DAG:     [[Result:j##]] InvokeStaticOrDirect
  // CHECK-DAG:                    Return [ [[Result]] ]

  // CHECK-START: long Inliner.InlineWide() inliner (after)
  // CHECK-DAG:     [[Const8:j##]] LongConstant 8
  // CHECK-DAG:                    Return [ [[Const8]] ]

  public static long InlineWide() {
    return returnWide();
  }

  // CHECK-START: int Inliner.InlineAdd() inliner (before)
  // CHECK-DAG:     [[Const3:i##]] IntConstant 3
  // CHECK-DAG:     [[Const5:i##]] IntConstant 5
  // CHECK-DAG:     [[Result:i##]] InvokeStaticOrDirect
  // CHECK-DAG:                    Return [ [[Result]] ]

  // CHECK-START: int Inliner.InlineAdd() inliner (after)
  // CHECK-DAG:     [[Const3:i##]] IntConstant 3
  // CHECK-DAG:     [[Const5:i##]] IntConstant 5
  // CHECK-DAG:     [[Add:i##]]    Add [ [[Const3]] [[Const5]] ]
  // CHECK-DAG:                    Return [ [[Add]] ]

  public static int InlineAdd() {
    return returnAdd(3, 5);
  }

  // CHECK-START: int Inliner.InlineFieldAccess() inliner (before)
  // CHECK-DAG:     [[After:i##]]  InvokeStaticOrDirect
  // CHECK-DAG:                    Return [ [[After]] ]

  // CHECK-START: int Inliner.InlineFieldAccess() inliner (after)
  // CHECK-DAG:     [[Const1:i##]] IntConstant 1
  // CHECK-DAG:     [[Before:i##]] StaticFieldGet
  // CHECK-DAG:     [[After:i##]]  Add [ [[Before]] [[Const1]] ]
  // CHECK-DAG:                    StaticFieldSet [ {{l##}} [[After]] ]
  // CHECK-DAG:                    Return [ [[After]] ]

  // CHECK-START: int Inliner.InlineFieldAccess() inliner (after)
  // CHECK-NOT:                    InvokeStaticOrDirect

  public static int InlineFieldAccess() {
    return incCounter();
  }

  // CHECK-START: int Inliner.InlineWithControlFlow(boolean) inliner (before)
  // CHECK-DAG:     [[Const1:i##]] IntConstant 1
  // CHECK-DAG:     [[Const3:i##]] IntConstant 3
  // CHECK-DAG:     [[Const5:i##]] IntConstant 5
  // CHECK-DAG:     [[Add:i##]]    InvokeStaticOrDirect [ [[Const1]] [[Const3]] ]
  // CHECK-DAG:     [[Sub:i##]]    InvokeStaticOrDirect [ [[Const5]] [[Const3]] ]
  // CHECK-DAG:     [[Phi:i##]]    Phi [ [[Add]] [[Sub]] ]
  // CHECK-DAG:                    Return [ [[Phi]] ]

  // CHECK-START: int Inliner.InlineWithControlFlow(boolean) inliner (after)
  // CHECK-DAG:     [[Const1:i##]] IntConstant 1
  // CHECK-DAG:     [[Const3:i##]] IntConstant 3
  // CHECK-DAG:     [[Const5:i##]] IntConstant 5
  // CHECK-DAG:     [[Add:i##]]    Add [ [[Const1]] [[Const3]] ]
  // CHECK-DAG:     [[Sub:i##]]    Sub [ [[Const5]] [[Const3]] ]
  // CHECK-DAG:     [[Phi:i##]]    Phi [ [[Add]] [[Sub]] ]
  // CHECK-DAG:                    Return [ [[Phi]] ]

  public static int InlineWithControlFlow(boolean cond) {
    int x, const1, const3, const5;
    const1 = 1;
    const3 = 3;
    const5 = 5;
    if (cond) {
      x = returnAdd(const1, const3);
    } else {
      x = returnSub(const5, const3);
    }
    return x;
  }


  private static void returnVoid() {
    return;
  }

  private static void returnVoidWithOneParameter(int a) {
    return;
  }

  private static int returnParameter(int a) {
    return a;
  }

  private static long returnWideParameter(long a) {
    return a;
  }

  private static Object returnReferenceParameter(Object o) {
    return o;
  }

  private static int returnInt() {
    return 4;
  }

  private static long returnWide() {
    return 8L;
  }

  private static int returnAdd(int a, int b) {
    return a + b;
  }

  private static int returnSub(int a, int b) {
    return a - b;
  }

  private static int counter = 42;

  private static int incCounter() {
    return ++counter;
  }
}
