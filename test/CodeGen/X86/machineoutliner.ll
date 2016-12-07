; RUN: llc -enable-machine-outliner -march=x86-64 < %s | FileCheck %s
; Test that the outliner can outline a simple function.

; Function Attrs: noredzone nounwind ssp uwtable
define void @foo() #0 {
entry:
  ; CHECK-LABEL: foo:
  %x = alloca i32, align 4
  %y = alloca i32, align 4
  %z = alloca i32, align 4
  store i32 1, i32* %x, align 4
  store i32 2, i32* %y, align 4
  store i32 3, i32* %z, align 4
  ret void
  ; CHECK:	subq	$12, %rsp
  ; CHECK:	callq	l_OUTLINED_FUNCTION0
  ; CHECK:	addq	$12, %rsp
}

; Function Attrs: noredzone nounwind ssp uwtable
define i32 @main() #0 {
entry:
  ; CHECK-LABEL: main:
  %retval = alloca i32, align 4
  %x = alloca i32, align 4
  %y = alloca i32, align 4
  %z = alloca i32, align 4
  store i32 0, i32* %retval, align 4
  store i32 1, i32* %x, align 4
  store i32 2, i32* %y, align 4
  store i32 3, i32* %z, align 4
  ret i32 0
  ; CHECK:  movl	$0, -16(%rbp)
  ; CHECK:  callq	l_OUTLINED_FUNCTION0
  ; CHECK:  xorl	%eax, %eax

}

attributes #0 = { noredzone }
; CHECK:	movl	$1, -12(%rbp)
; CHECK: 	movl	$2, -8(%rbp)
; CHECK:	movl	$3, -4(%rbp)
; CHECK:	retq
