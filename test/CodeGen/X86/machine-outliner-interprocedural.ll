; RUN: llc -enable-machine-outliner -march=x86-64 < %s | FileCheck %s

; Make sure the outliner can create simple calls across more than one function.

@x = global i32 0, align 4

define i32 @foo() #0 {
  ; CHECK-LABEL: _foo:
  %1 = alloca i32, align 4
  %2 = alloca i32, align 4
  %3 = alloca i32, align 4
  %4 = alloca i32, align 4
  %5 = alloca i32, align 4

  ; CHECK: callq l_OUTLINED_FUNCTION_0
  store i32 1, i32* %2, align 4
  store i32 2, i32* %3, align 4
  store i32 3, i32* %4, align 4
  store i32 4, i32* %5, align 4
  store i32 1, i32* @x, align 4

  ret i32 0
}

; Function Attrs: noinline noredzone nounwind ssp uwtable
define i32 @main() #0 {
  ; CHECK-LABEL: _main:
  %1 = alloca i32, align 4
  %2 = alloca i32, align 4
  %3 = alloca i32, align 4
  %4 = alloca i32, align 4
  %5 = alloca i32, align 4

  store i32 0, i32* %1, align 4
  store i32 0, i32* @x, align 4
  ; CHECK: callq l_OUTLINED_FUNCTION_0
  store i32 1, i32* %2, align 4
  store i32 2, i32* %3, align 4
  store i32 3, i32* %4, align 4
  store i32 4, i32* %5, align 4
  store i32 1, i32* @x, align 4
  ; CHECK: callq l_OUTLINED_FUNCTION_0
  store i32 1, i32* %2, align 4
  store i32 2, i32* %3, align 4
  store i32 3, i32* %4, align 4
  store i32 4, i32* %5, align 4
  ret i32 0
}

attributes #0 = { noredzone nounwind ssp uwtable "no-frame-pointer-elim"="true" }

; CHECK-LABEL: l_OUTLINED_FUNCTION_0:
; CHECK: movl  $1, -{{[0-9]+}}(%rbp)
; CHECK-NEXT: movl  $2, -{{[0-9]+}}(%rbp)
; CHECK-NEXT: movl  $3, -{{[0-9]+}}(%rbp)
; CHECK-NEXT: movl  $4, -{{[0-9]+}}(%rbp)
; CHECK-NEXT: retq
