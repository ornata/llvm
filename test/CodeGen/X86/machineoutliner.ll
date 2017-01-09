; RUN: llc -enable-machine-outliner -march=x86-64 < %s | FileCheck %s

; Test if the outliner can create outlined functions, and if it obeys basic
; block boundaries.
;
; Currently, there should never be labels inside outlined functions.

; Function Attrs: noredzone nounwind ssp uwtable
define void @main() #0 {
; CHECK-LABEL: _main:
entry:
  %a = alloca i32, align 4
  %b = alloca i32, align 4

  store i32 0, i32* %a, align 4
  store i32 1, i32* %b, align 4
  %0 = load i32, i32* %a, align 4
  %1 = load i32, i32* %b, align 4
  %cmp = icmp sgt i32 %0, %1
  br i1 %cmp, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  ; CHECK: callq  l_OUTLINED_FUNCTION0
  ; CHECK: jmp  LBB0_3

  store i32 1, i32* %a, align 4
  store i32 2, i32* %b, align 4
  br label %if.end

if.else:                                          ; preds = %entry
  ; CHECK: callq  l_OUTLINED_FUNCTION1
  ; CHECK: LBB0_3:

  store i32 2, i32* %a, align 4
  store i32 3, i32* %b, align 4
  br label %if.end

if.end:                                           ; preds = %if.else, %if.then
  %2 = load i32, i32* %a, align 4
  %3 = load i32, i32* %b, align 4
  %cmp1 = icmp sgt i32 %2, %3
  br i1 %cmp1, label %if.then2, label %if.else3

if.then2:                                         ; preds = %if.end
  ; CHECK: jle  LBB0_5
  ; CHECK: callq  l_OUTLINED_FUNCTION0
  ; CHECK: jmp  LBB0_6

  store i32 1, i32* %a, align 4
  store i32 2, i32* %b, align 4
  br label %if.end4

if.else3:                                         ; preds = %if.end
  ; CHECK: l_OUTLINED_FUNCTION1

  store i32 2, i32* %a, align 4
  store i32 3, i32* %b, align 4
  br label %if.end4

if.end4:                                          ; preds = %if.else3, %if.then2
  ret void
}


; CHECK-LABEL: l_OUTLINED_FUNCTION0:
; CHECK:  movl  $1, -4(%rbp)
; CHECK:  movl  $2, -8(%rbp)
; CHECK:  retq

; CHECK-LABEL: l_OUTLINED_FUNCTION1:
; CHECK: movl  $2, -4(%rbp)
; CHECK: movl  $3, -8(%rbp)
; CHECK: retq


attributes #0 = { noredzone nounwind ssp uwtable "no-frame-pointer-elim"="true" }
