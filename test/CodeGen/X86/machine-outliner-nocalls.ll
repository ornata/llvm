; RUN: llc -enable-machine-outliner -march=x86-64 < %s | FileCheck %s

; Function Attrs: noinline noredzone nounwind ssp uwtable
define i32 @bar() #0 {
  ; CHECK-NOT: callq _OUTLINED_FUNCTION{{[0-9]+}}_0
  ret i32 1
}

; Function Attrs: noinline noredzone nounwind ssp uwtable
define i32 @foo() #0 {
  ; CHECK-NOT: callq _OUTLINED_FUNCTION{{[0-9]+}}_0
  ret i32 1
}

; Function Attrs: noinline noredzone nounwind ssp uwtable
define i32 @main() #0 {
  ; CHECK-LABEL: _main:
  %1 = alloca i32, align 4
  store i32 0, i32* %1, align 4
  ; CHECK-NOT: callq _OUTLINED_FUNCTION{{[0-9]+}}_0
  %2 = call i32 @bar() #1
  %3 = call i32 @foo() #1
  %4 = call i32 @bar() #1
  %5 = call i32 @foo() #1
  %6 = call i32 @bar() #1
  %7 = call i32 @foo() #1
  ret i32 0
}

; CHECK-NOT: _OUTLINED_FUNCTION{{[0-9]+}}_0: