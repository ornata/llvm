; RUN: llc -enable-machine-outliner -march=x86-64 < %s | FileCheck %s

; Make sure the outliner doesn't outline instructions that aren't in the same
; basic block.

@x = global i32 0, align 4

; Function Attrs: noinline noredzone nounwind ssp uwtable
define i32 @main() #0 {
  ; CHECK-LABEL: _main:
  %1 = alloca i32, align 4
  %2 = alloca i32, align 4
  %3 = alloca i32, align 4
  %4 = alloca i32, align 4
  %5 = alloca i32, align 4
  store i32 0, i32* %1, align 4
  store i32 0, i32* %2, align 4
  %6 = load i32, i32* %2, align 4
  %7 = icmp ne i32 %6, 0
  br i1 %7, label %9, label %8

; <label>:8:                                      ; preds = %0
  ; CHECK: callq _OUTLINED_FUNCTION{{[0-9]+}}_0
  ; CHECK: cmpl  $0, -{{[0-9]+}}(%rbp)
  ; CHECK: jne  LBB0_{{[0-9]+}}
  store i32 1, i32* %2, align 4
  store i32 2, i32* %3, align 4
  store i32 3, i32* %4, align 4
  store i32 4, i32* %5, align 4
  br label %10

; <label>:9:                                      ; preds = %0
  store i32 1, i32* %4, align 4
  br label %10

; <label>:10:                                     ; preds = %9, %8
  %11 = load i32, i32* %2, align 4
  %12 = icmp ne i32 %11, 0
  br i1 %12, label %14, label %13

; <label>:13:                                     ; preds = %10
  ; CHECK: callq _OUTLINED_FUNCTION{{[0-9]+}}_0
  ; CHECK: LBB0_6:
  store i32 1, i32* %2, align 4
  store i32 2, i32* %3, align 4
  store i32 3, i32* %4, align 4
  store i32 4, i32* %5, align 4
  br label %15

; <label>:14:                                     ; preds = %10
  store i32 1, i32* %4, align 4
  br label %15

; <label>:15:                                     ; preds = %14, %13
  ret i32 0
}

attributes #0 = { noredzone nounwind ssp uwtable "no-frame-pointer-elim"="true" }

; CHECK-LABEL: _OUTLINED_FUNCTION{{[0-9]+}}_0:
; CHECK: movl  $1, -{{[0-9]+}}(%rbp)
; CHECK: movl  $2, -{{[0-9]+}}(%rbp)
; CHECK: movl  $3, -{{[0-9]+}}(%rbp)
; CHECK: movl  $4, -{{[0-9]+}}(%rbp)
; CHECK: retq
