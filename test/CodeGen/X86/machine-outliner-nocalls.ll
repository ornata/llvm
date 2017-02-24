; RUN: llc -enable-machine-outliner -march=x86-64 < %s | FileCheck %s

; Make sure the outliner never outlines call instructions.

; Function Attrs: noinline noredzone nounwind ssp uwtable
define i32 @bar() #0 {
  ; CHECK-NOT: l_OUTLINED_FUNCTION_{{[0-9]+}}
  ret i32 1
}

; Function Attrs: noinline noredzone nounwind ssp uwtable
define i32 @foo() #0 {
  ; CHECK-NOT: l_OUTLINED_FUNCTION_{{[0-9]+}}
  ret i32 1
}

; Function Attrs: noinline noredzone nounwind ssp uwtable
define i32 @main() #0 {
  ; CHECK-LABEL: _main:
  %1 = alloca i32, align 4
  store i32 0, i32* %1, align 4
  ; CHECK-NOT: callq l_OUTLINED_FUNCTION_{{[0-9]+}}
  %2 = call i32 @bar() #1
  %3 = call i32 @foo() #1
  %4 = call i32 @bar() #1
  %5 = call i32 @foo() #1
  %6 = call i32 @bar() #1
  %7 = call i32 @foo() #1
  ret i32 0
}

attributes #0 = { noredzone nounwind ssp uwtable "no-frame-pointer-elim"="true" }

; CHECK-NOT: l_OUTLINED_FUNCTION_{{[0-9]+}}:
