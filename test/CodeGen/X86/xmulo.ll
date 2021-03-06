; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc -mtriple=x86_64-darwin-unknown < %s | FileCheck %s --check-prefix=SDAG
; RUN: llc -mtriple=x86_64-darwin-unknown -fast-isel -fast-isel-abort=1 < %s | FileCheck %s --check-prefix=FAST
; RUN: llc -mtriple=x86_64-darwin-unknown -mcpu=knl < %s | FileCheck %s --check-prefix=KNL

define {i64, i1} @t1() nounwind {
; SDAG-LABEL: t1:
; SDAG:       ## BB#0:
; SDAG-NEXT:    movl $8, %ecx
; SDAG-NEXT:    movl $9, %eax
; SDAG-NEXT:    mulq %rcx
; SDAG-NEXT:    seto %dl
; SDAG-NEXT:    retq
;
; FAST-LABEL: t1:
; FAST:       ## BB#0:
; FAST-NEXT:    movl $8, %ecx
; FAST-NEXT:    movl $9, %eax
; FAST-NEXT:    mulq %rcx
; FAST-NEXT:    seto %dl
; FAST-NEXT:    retq
;
; KNL-LABEL: t1:
; KNL:       ## BB#0:
; KNL-NEXT:    movl $8, %ecx
; KNL-NEXT:    movl $9, %eax
; KNL-NEXT:    mulq %rcx
; KNL-NEXT:    seto %dl
; KNL-NEXT:    retq
  %1 = call {i64, i1} @llvm.umul.with.overflow.i64(i64 9, i64 8)
  ret {i64, i1} %1
}

define {i64, i1} @t2() nounwind {
; SDAG-LABEL: t2:
; SDAG:       ## BB#0:
; SDAG-NEXT:    xorl %ecx, %ecx
; SDAG-NEXT:    movl $9, %eax
; SDAG-NEXT:    mulq %rcx
; SDAG-NEXT:    seto %dl
; SDAG-NEXT:    retq
;
; FAST-LABEL: t2:
; FAST:       ## BB#0:
; FAST-NEXT:    xorl %ecx, %ecx
; FAST-NEXT:    movl $9, %eax
; FAST-NEXT:    mulq %rcx
; FAST-NEXT:    seto %dl
; FAST-NEXT:    retq
;
; KNL-LABEL: t2:
; KNL:       ## BB#0:
; KNL-NEXT:    xorl %ecx, %ecx
; KNL-NEXT:    movl $9, %eax
; KNL-NEXT:    mulq %rcx
; KNL-NEXT:    seto %dl
; KNL-NEXT:    retq
  %1 = call {i64, i1} @llvm.umul.with.overflow.i64(i64 9, i64 0)
  ret {i64, i1} %1
}

define {i64, i1} @t3() nounwind {
; SDAG-LABEL: t3:
; SDAG:       ## BB#0:
; SDAG-NEXT:    movq $-1, %rcx
; SDAG-NEXT:    movl $9, %eax
; SDAG-NEXT:    mulq %rcx
; SDAG-NEXT:    seto %dl
; SDAG-NEXT:    retq
;
; FAST-LABEL: t3:
; FAST:       ## BB#0:
; FAST-NEXT:    movq $-1, %rcx
; FAST-NEXT:    movl $9, %eax
; FAST-NEXT:    mulq %rcx
; FAST-NEXT:    seto %dl
; FAST-NEXT:    retq
;
; KNL-LABEL: t3:
; KNL:       ## BB#0:
; KNL-NEXT:    movq $-1, %rcx
; KNL-NEXT:    movl $9, %eax
; KNL-NEXT:    mulq %rcx
; KNL-NEXT:    seto %dl
; KNL-NEXT:    retq
  %1 = call {i64, i1} @llvm.umul.with.overflow.i64(i64 9, i64 -1)
  ret {i64, i1} %1
}

; SMULO
define zeroext i1 @smuloi8(i8 %v1, i8 %v2, i8* %res) {
; SDAG-LABEL: smuloi8:
; SDAG:       ## BB#0:
; SDAG-NEXT:    movl %edi, %eax
; SDAG-NEXT:    imulb %sil
; SDAG-NEXT:    seto %cl
; SDAG-NEXT:    movb %al, (%rdx)
; SDAG-NEXT:    movl %ecx, %eax
; SDAG-NEXT:    retq
;
; FAST-LABEL: smuloi8:
; FAST:       ## BB#0:
; FAST-NEXT:    movl %edi, %eax
; FAST-NEXT:    imulb %sil
; FAST-NEXT:    seto %cl
; FAST-NEXT:    movb %al, (%rdx)
; FAST-NEXT:    andb $1, %cl
; FAST-NEXT:    movzbl %cl, %eax
; FAST-NEXT:    retq
;
; KNL-LABEL: smuloi8:
; KNL:       ## BB#0:
; KNL-NEXT:    movl %edi, %eax
; KNL-NEXT:    imulb %sil
; KNL-NEXT:    seto %cl
; KNL-NEXT:    movb %al, (%rdx)
; KNL-NEXT:    movl %ecx, %eax
; KNL-NEXT:    retq
  %t = call {i8, i1} @llvm.smul.with.overflow.i8(i8 %v1, i8 %v2)
  %val = extractvalue {i8, i1} %t, 0
  %obit = extractvalue {i8, i1} %t, 1
  store i8 %val, i8* %res
  ret i1 %obit
}

define zeroext i1 @smuloi16(i16 %v1, i16 %v2, i16* %res) {
; SDAG-LABEL: smuloi16:
; SDAG:       ## BB#0:
; SDAG-NEXT:    imulw %si, %di
; SDAG-NEXT:    seto %al
; SDAG-NEXT:    movw %di, (%rdx)
; SDAG-NEXT:    retq
;
; FAST-LABEL: smuloi16:
; FAST:       ## BB#0:
; FAST-NEXT:    imulw %si, %di
; FAST-NEXT:    seto %al
; FAST-NEXT:    movw %di, (%rdx)
; FAST-NEXT:    andb $1, %al
; FAST-NEXT:    movzbl %al, %eax
; FAST-NEXT:    retq
;
; KNL-LABEL: smuloi16:
; KNL:       ## BB#0:
; KNL-NEXT:    imulw %si, %di
; KNL-NEXT:    seto %al
; KNL-NEXT:    movw %di, (%rdx)
; KNL-NEXT:    retq
  %t = call {i16, i1} @llvm.smul.with.overflow.i16(i16 %v1, i16 %v2)
  %val = extractvalue {i16, i1} %t, 0
  %obit = extractvalue {i16, i1} %t, 1
  store i16 %val, i16* %res
  ret i1 %obit
}

define zeroext i1 @smuloi32(i32 %v1, i32 %v2, i32* %res) {
; SDAG-LABEL: smuloi32:
; SDAG:       ## BB#0:
; SDAG-NEXT:    imull %esi, %edi
; SDAG-NEXT:    seto %al
; SDAG-NEXT:    movl %edi, (%rdx)
; SDAG-NEXT:    retq
;
; FAST-LABEL: smuloi32:
; FAST:       ## BB#0:
; FAST-NEXT:    imull %esi, %edi
; FAST-NEXT:    seto %al
; FAST-NEXT:    movl %edi, (%rdx)
; FAST-NEXT:    andb $1, %al
; FAST-NEXT:    movzbl %al, %eax
; FAST-NEXT:    retq
;
; KNL-LABEL: smuloi32:
; KNL:       ## BB#0:
; KNL-NEXT:    imull %esi, %edi
; KNL-NEXT:    seto %al
; KNL-NEXT:    movl %edi, (%rdx)
; KNL-NEXT:    retq
  %t = call {i32, i1} @llvm.smul.with.overflow.i32(i32 %v1, i32 %v2)
  %val = extractvalue {i32, i1} %t, 0
  %obit = extractvalue {i32, i1} %t, 1
  store i32 %val, i32* %res
  ret i1 %obit
}

define zeroext i1 @smuloi64(i64 %v1, i64 %v2, i64* %res) {
; SDAG-LABEL: smuloi64:
; SDAG:       ## BB#0:
; SDAG-NEXT:    imulq %rsi, %rdi
; SDAG-NEXT:    seto %al
; SDAG-NEXT:    movq %rdi, (%rdx)
; SDAG-NEXT:    retq
;
; FAST-LABEL: smuloi64:
; FAST:       ## BB#0:
; FAST-NEXT:    imulq %rsi, %rdi
; FAST-NEXT:    seto %al
; FAST-NEXT:    movq %rdi, (%rdx)
; FAST-NEXT:    andb $1, %al
; FAST-NEXT:    movzbl %al, %eax
; FAST-NEXT:    retq
;
; KNL-LABEL: smuloi64:
; KNL:       ## BB#0:
; KNL-NEXT:    imulq %rsi, %rdi
; KNL-NEXT:    seto %al
; KNL-NEXT:    movq %rdi, (%rdx)
; KNL-NEXT:    retq
  %t = call {i64, i1} @llvm.smul.with.overflow.i64(i64 %v1, i64 %v2)
  %val = extractvalue {i64, i1} %t, 0
  %obit = extractvalue {i64, i1} %t, 1
  store i64 %val, i64* %res
  ret i1 %obit
}

; UMULO
define zeroext i1 @umuloi8(i8 %v1, i8 %v2, i8* %res) {
; SDAG-LABEL: umuloi8:
; SDAG:       ## BB#0:
; SDAG-NEXT:    movl %edi, %eax
; SDAG-NEXT:    mulb %sil
; SDAG-NEXT:    seto %cl
; SDAG-NEXT:    movb %al, (%rdx)
; SDAG-NEXT:    movl %ecx, %eax
; SDAG-NEXT:    retq
;
; FAST-LABEL: umuloi8:
; FAST:       ## BB#0:
; FAST-NEXT:    movl %edi, %eax
; FAST-NEXT:    mulb %sil
; FAST-NEXT:    seto %cl
; FAST-NEXT:    movb %al, (%rdx)
; FAST-NEXT:    andb $1, %cl
; FAST-NEXT:    movzbl %cl, %eax
; FAST-NEXT:    retq
;
; KNL-LABEL: umuloi8:
; KNL:       ## BB#0:
; KNL-NEXT:    movl %edi, %eax
; KNL-NEXT:    mulb %sil
; KNL-NEXT:    seto %cl
; KNL-NEXT:    movb %al, (%rdx)
; KNL-NEXT:    movl %ecx, %eax
; KNL-NEXT:    retq
  %t = call {i8, i1} @llvm.umul.with.overflow.i8(i8 %v1, i8 %v2)
  %val = extractvalue {i8, i1} %t, 0
  %obit = extractvalue {i8, i1} %t, 1
  store i8 %val, i8* %res
  ret i1 %obit
}

define zeroext i1 @umuloi16(i16 %v1, i16 %v2, i16* %res) {
; SDAG-LABEL: umuloi16:
; SDAG:       ## BB#0:
; SDAG-NEXT:    movq %rdx, %rcx
; SDAG-NEXT:    movl %edi, %eax
; SDAG-NEXT:    mulw %si
; SDAG-NEXT:    seto %dl
; SDAG-NEXT:    movw %ax, (%rcx)
; SDAG-NEXT:    movl %edx, %eax
; SDAG-NEXT:    retq
;
; FAST-LABEL: umuloi16:
; FAST:       ## BB#0:
; FAST-NEXT:    movq %rdx, %rcx
; FAST-NEXT:    movl %edi, %eax
; FAST-NEXT:    mulw %si
; FAST-NEXT:    seto %dl
; FAST-NEXT:    movw %ax, (%rcx)
; FAST-NEXT:    andb $1, %dl
; FAST-NEXT:    movzbl %dl, %eax
; FAST-NEXT:    retq
;
; KNL-LABEL: umuloi16:
; KNL:       ## BB#0:
; KNL-NEXT:    movq %rdx, %rcx
; KNL-NEXT:    movl %edi, %eax
; KNL-NEXT:    mulw %si
; KNL-NEXT:    seto %dl
; KNL-NEXT:    movw %ax, (%rcx)
; KNL-NEXT:    movl %edx, %eax
; KNL-NEXT:    retq
  %t = call {i16, i1} @llvm.umul.with.overflow.i16(i16 %v1, i16 %v2)
  %val = extractvalue {i16, i1} %t, 0
  %obit = extractvalue {i16, i1} %t, 1
  store i16 %val, i16* %res
  ret i1 %obit
}

define zeroext i1 @umuloi32(i32 %v1, i32 %v2, i32* %res) {
; SDAG-LABEL: umuloi32:
; SDAG:       ## BB#0:
; SDAG-NEXT:    movq %rdx, %rcx
; SDAG-NEXT:    movl %edi, %eax
; SDAG-NEXT:    mull %esi
; SDAG-NEXT:    seto %dl
; SDAG-NEXT:    movl %eax, (%rcx)
; SDAG-NEXT:    movl %edx, %eax
; SDAG-NEXT:    retq
;
; FAST-LABEL: umuloi32:
; FAST:       ## BB#0:
; FAST-NEXT:    movq %rdx, %rcx
; FAST-NEXT:    movl %edi, %eax
; FAST-NEXT:    mull %esi
; FAST-NEXT:    seto %dl
; FAST-NEXT:    movl %eax, (%rcx)
; FAST-NEXT:    andb $1, %dl
; FAST-NEXT:    movzbl %dl, %eax
; FAST-NEXT:    retq
;
; KNL-LABEL: umuloi32:
; KNL:       ## BB#0:
; KNL-NEXT:    movq %rdx, %rcx
; KNL-NEXT:    movl %edi, %eax
; KNL-NEXT:    mull %esi
; KNL-NEXT:    seto %dl
; KNL-NEXT:    movl %eax, (%rcx)
; KNL-NEXT:    movl %edx, %eax
; KNL-NEXT:    retq
  %t = call {i32, i1} @llvm.umul.with.overflow.i32(i32 %v1, i32 %v2)
  %val = extractvalue {i32, i1} %t, 0
  %obit = extractvalue {i32, i1} %t, 1
  store i32 %val, i32* %res
  ret i1 %obit
}

define zeroext i1 @umuloi64(i64 %v1, i64 %v2, i64* %res) {
; SDAG-LABEL: umuloi64:
; SDAG:       ## BB#0:
; SDAG-NEXT:    movq %rdx, %rcx
; SDAG-NEXT:    movq %rdi, %rax
; SDAG-NEXT:    mulq %rsi
; SDAG-NEXT:    seto %dl
; SDAG-NEXT:    movq %rax, (%rcx)
; SDAG-NEXT:    movl %edx, %eax
; SDAG-NEXT:    retq
;
; FAST-LABEL: umuloi64:
; FAST:       ## BB#0:
; FAST-NEXT:    movq %rdx, %rcx
; FAST-NEXT:    movq %rdi, %rax
; FAST-NEXT:    mulq %rsi
; FAST-NEXT:    seto %dl
; FAST-NEXT:    movq %rax, (%rcx)
; FAST-NEXT:    andb $1, %dl
; FAST-NEXT:    movzbl %dl, %eax
; FAST-NEXT:    retq
;
; KNL-LABEL: umuloi64:
; KNL:       ## BB#0:
; KNL-NEXT:    movq %rdx, %rcx
; KNL-NEXT:    movq %rdi, %rax
; KNL-NEXT:    mulq %rsi
; KNL-NEXT:    seto %dl
; KNL-NEXT:    movq %rax, (%rcx)
; KNL-NEXT:    movl %edx, %eax
; KNL-NEXT:    retq
  %t = call {i64, i1} @llvm.umul.with.overflow.i64(i64 %v1, i64 %v2)
  %val = extractvalue {i64, i1} %t, 0
  %obit = extractvalue {i64, i1} %t, 1
  store i64 %val, i64* %res
  ret i1 %obit
}

;
; Check the use of the overflow bit in combination with a select instruction.
;
define i32 @smuloselecti32(i32 %v1, i32 %v2) {
; SDAG-LABEL: smuloselecti32:
; SDAG:       ## BB#0:
; SDAG-NEXT:    movl %edi, %eax
; SDAG-NEXT:    imull %esi, %eax
; SDAG-NEXT:    cmovol %edi, %esi
; SDAG-NEXT:    movl %esi, %eax
; SDAG-NEXT:    retq
;
; FAST-LABEL: smuloselecti32:
; FAST:       ## BB#0:
; FAST-NEXT:    movl %edi, %eax
; FAST-NEXT:    imull %esi, %eax
; FAST-NEXT:    cmovol %edi, %esi
; FAST-NEXT:    movl %esi, %eax
; FAST-NEXT:    retq
;
; KNL-LABEL: smuloselecti32:
; KNL:       ## BB#0:
; KNL-NEXT:    movl %edi, %eax
; KNL-NEXT:    imull %esi, %eax
; KNL-NEXT:    cmovol %edi, %esi
; KNL-NEXT:    movl %esi, %eax
; KNL-NEXT:    retq
  %t = call {i32, i1} @llvm.smul.with.overflow.i32(i32 %v1, i32 %v2)
  %obit = extractvalue {i32, i1} %t, 1
  %ret = select i1 %obit, i32 %v1, i32 %v2
  ret i32 %ret
}

define i64 @smuloselecti64(i64 %v1, i64 %v2) {
; SDAG-LABEL: smuloselecti64:
; SDAG:       ## BB#0:
; SDAG-NEXT:    movq %rdi, %rax
; SDAG-NEXT:    imulq %rsi, %rax
; SDAG-NEXT:    cmovoq %rdi, %rsi
; SDAG-NEXT:    movq %rsi, %rax
; SDAG-NEXT:    retq
;
; FAST-LABEL: smuloselecti64:
; FAST:       ## BB#0:
; FAST-NEXT:    movq %rdi, %rax
; FAST-NEXT:    imulq %rsi, %rax
; FAST-NEXT:    cmovoq %rdi, %rsi
; FAST-NEXT:    movq %rsi, %rax
; FAST-NEXT:    retq
;
; KNL-LABEL: smuloselecti64:
; KNL:       ## BB#0:
; KNL-NEXT:    movq %rdi, %rax
; KNL-NEXT:    imulq %rsi, %rax
; KNL-NEXT:    cmovoq %rdi, %rsi
; KNL-NEXT:    movq %rsi, %rax
; KNL-NEXT:    retq
  %t = call {i64, i1} @llvm.smul.with.overflow.i64(i64 %v1, i64 %v2)
  %obit = extractvalue {i64, i1} %t, 1
  %ret = select i1 %obit, i64 %v1, i64 %v2
  ret i64 %ret
}

define i32 @umuloselecti32(i32 %v1, i32 %v2) {
; SDAG-LABEL: umuloselecti32:
; SDAG:       ## BB#0:
; SDAG-NEXT:    movl %edi, %eax
; SDAG-NEXT:    mull %esi
; SDAG-NEXT:    cmovol %edi, %esi
; SDAG-NEXT:    movl %esi, %eax
; SDAG-NEXT:    retq
;
; FAST-LABEL: umuloselecti32:
; FAST:       ## BB#0:
; FAST-NEXT:    movl %edi, %eax
; FAST-NEXT:    mull %esi
; FAST-NEXT:    cmovol %edi, %esi
; FAST-NEXT:    movl %esi, %eax
; FAST-NEXT:    retq
;
; KNL-LABEL: umuloselecti32:
; KNL:       ## BB#0:
; KNL-NEXT:    movl %edi, %eax
; KNL-NEXT:    mull %esi
; KNL-NEXT:    cmovol %edi, %esi
; KNL-NEXT:    movl %esi, %eax
; KNL-NEXT:    retq
  %t = call {i32, i1} @llvm.umul.with.overflow.i32(i32 %v1, i32 %v2)
  %obit = extractvalue {i32, i1} %t, 1
  %ret = select i1 %obit, i32 %v1, i32 %v2
  ret i32 %ret
}

define i64 @umuloselecti64(i64 %v1, i64 %v2) {
; SDAG-LABEL: umuloselecti64:
; SDAG:       ## BB#0:
; SDAG-NEXT:    movq %rdi, %rax
; SDAG-NEXT:    mulq %rsi
; SDAG-NEXT:    cmovoq %rdi, %rsi
; SDAG-NEXT:    movq %rsi, %rax
; SDAG-NEXT:    retq
;
; FAST-LABEL: umuloselecti64:
; FAST:       ## BB#0:
; FAST-NEXT:    movq %rdi, %rax
; FAST-NEXT:    mulq %rsi
; FAST-NEXT:    cmovoq %rdi, %rsi
; FAST-NEXT:    movq %rsi, %rax
; FAST-NEXT:    retq
;
; KNL-LABEL: umuloselecti64:
; KNL:       ## BB#0:
; KNL-NEXT:    movq %rdi, %rax
; KNL-NEXT:    mulq %rsi
; KNL-NEXT:    cmovoq %rdi, %rsi
; KNL-NEXT:    movq %rsi, %rax
; KNL-NEXT:    retq
  %t = call {i64, i1} @llvm.umul.with.overflow.i64(i64 %v1, i64 %v2)
  %obit = extractvalue {i64, i1} %t, 1
  %ret = select i1 %obit, i64 %v1, i64 %v2
  ret i64 %ret
}

;
; Check the use of the overflow bit in combination with a branch instruction.
;
define zeroext i1 @smulobri32(i32 %v1, i32 %v2) {
; SDAG-LABEL: smulobri32:
; SDAG:       ## BB#0:
; SDAG-NEXT:    imull %esi, %edi
; SDAG-NEXT:    jo LBB15_1
; SDAG-NEXT:  ## BB#2: ## %continue
; SDAG-NEXT:    movb $1, %al
; SDAG-NEXT:    retq
; SDAG-NEXT:  LBB15_1: ## %overflow
; SDAG-NEXT:    xorl %eax, %eax
; SDAG-NEXT:    retq
;
; FAST-LABEL: smulobri32:
; FAST:       ## BB#0:
; FAST-NEXT:    imull %esi, %edi
; FAST-NEXT:    jo LBB15_1
; FAST-NEXT:  ## BB#2: ## %continue
; FAST-NEXT:    movb $1, %al
; FAST-NEXT:    andb $1, %al
; FAST-NEXT:    movzbl %al, %eax
; FAST-NEXT:    retq
; FAST-NEXT:  LBB15_1: ## %overflow
; FAST-NEXT:    xorl %eax, %eax
; FAST-NEXT:    andb $1, %al
; FAST-NEXT:    movzbl %al, %eax
; FAST-NEXT:    retq
;
; KNL-LABEL: smulobri32:
; KNL:       ## BB#0:
; KNL-NEXT:    imull %esi, %edi
; KNL-NEXT:    jo LBB15_1
; KNL-NEXT:  ## BB#2: ## %continue
; KNL-NEXT:    movb $1, %al
; KNL-NEXT:    retq
; KNL-NEXT:  LBB15_1: ## %overflow
; KNL-NEXT:    xorl %eax, %eax
; KNL-NEXT:    retq
  %t = call {i32, i1} @llvm.smul.with.overflow.i32(i32 %v1, i32 %v2)
  %val = extractvalue {i32, i1} %t, 0
  %obit = extractvalue {i32, i1} %t, 1
  br i1 %obit, label %overflow, label %continue, !prof !0

overflow:
  ret i1 false

continue:
  ret i1 true
}

define zeroext i1 @smulobri64(i64 %v1, i64 %v2) {
; SDAG-LABEL: smulobri64:
; SDAG:       ## BB#0:
; SDAG-NEXT:    imulq %rsi, %rdi
; SDAG-NEXT:    jo LBB16_1
; SDAG-NEXT:  ## BB#2: ## %continue
; SDAG-NEXT:    movb $1, %al
; SDAG-NEXT:    retq
; SDAG-NEXT:  LBB16_1: ## %overflow
; SDAG-NEXT:    xorl %eax, %eax
; SDAG-NEXT:    retq
;
; FAST-LABEL: smulobri64:
; FAST:       ## BB#0:
; FAST-NEXT:    imulq %rsi, %rdi
; FAST-NEXT:    jo LBB16_1
; FAST-NEXT:  ## BB#2: ## %continue
; FAST-NEXT:    movb $1, %al
; FAST-NEXT:    andb $1, %al
; FAST-NEXT:    movzbl %al, %eax
; FAST-NEXT:    retq
; FAST-NEXT:  LBB16_1: ## %overflow
; FAST-NEXT:    xorl %eax, %eax
; FAST-NEXT:    andb $1, %al
; FAST-NEXT:    movzbl %al, %eax
; FAST-NEXT:    retq
;
; KNL-LABEL: smulobri64:
; KNL:       ## BB#0:
; KNL-NEXT:    imulq %rsi, %rdi
; KNL-NEXT:    jo LBB16_1
; KNL-NEXT:  ## BB#2: ## %continue
; KNL-NEXT:    movb $1, %al
; KNL-NEXT:    retq
; KNL-NEXT:  LBB16_1: ## %overflow
; KNL-NEXT:    xorl %eax, %eax
; KNL-NEXT:    retq
  %t = call {i64, i1} @llvm.smul.with.overflow.i64(i64 %v1, i64 %v2)
  %val = extractvalue {i64, i1} %t, 0
  %obit = extractvalue {i64, i1} %t, 1
  br i1 %obit, label %overflow, label %continue, !prof !0

overflow:
  ret i1 false

continue:
  ret i1 true
}

define zeroext i1 @umulobri32(i32 %v1, i32 %v2) {
; SDAG-LABEL: umulobri32:
; SDAG:       ## BB#0:
; SDAG-NEXT:    movl %edi, %eax
; SDAG-NEXT:    mull %esi
; SDAG-NEXT:    jo LBB17_1
; SDAG-NEXT:  ## BB#2: ## %continue
; SDAG-NEXT:    movb $1, %al
; SDAG-NEXT:    retq
; SDAG-NEXT:  LBB17_1: ## %overflow
; SDAG-NEXT:    xorl %eax, %eax
; SDAG-NEXT:    retq
;
; FAST-LABEL: umulobri32:
; FAST:       ## BB#0:
; FAST-NEXT:    movl %edi, %eax
; FAST-NEXT:    mull %esi
; FAST-NEXT:    jo LBB17_1
; FAST-NEXT:  ## BB#2: ## %continue
; FAST-NEXT:    movb $1, %al
; FAST-NEXT:    andb $1, %al
; FAST-NEXT:    movzbl %al, %eax
; FAST-NEXT:    retq
; FAST-NEXT:  LBB17_1: ## %overflow
; FAST-NEXT:    xorl %eax, %eax
; FAST-NEXT:    andb $1, %al
; FAST-NEXT:    movzbl %al, %eax
; FAST-NEXT:    retq
;
; KNL-LABEL: umulobri32:
; KNL:       ## BB#0:
; KNL-NEXT:    movl %edi, %eax
; KNL-NEXT:    mull %esi
; KNL-NEXT:    jo LBB17_1
; KNL-NEXT:  ## BB#2: ## %continue
; KNL-NEXT:    movb $1, %al
; KNL-NEXT:    retq
; KNL-NEXT:  LBB17_1: ## %overflow
; KNL-NEXT:    xorl %eax, %eax
; KNL-NEXT:    retq
  %t = call {i32, i1} @llvm.umul.with.overflow.i32(i32 %v1, i32 %v2)
  %val = extractvalue {i32, i1} %t, 0
  %obit = extractvalue {i32, i1} %t, 1
  br i1 %obit, label %overflow, label %continue, !prof !0

overflow:
  ret i1 false

continue:
  ret i1 true
}

define zeroext i1 @umulobri64(i64 %v1, i64 %v2) {
; SDAG-LABEL: umulobri64:
; SDAG:       ## BB#0:
; SDAG-NEXT:    movq %rdi, %rax
; SDAG-NEXT:    mulq %rsi
; SDAG-NEXT:    jo LBB18_1
; SDAG-NEXT:  ## BB#2: ## %continue
; SDAG-NEXT:    movb $1, %al
; SDAG-NEXT:    retq
; SDAG-NEXT:  LBB18_1: ## %overflow
; SDAG-NEXT:    xorl %eax, %eax
; SDAG-NEXT:    retq
;
; FAST-LABEL: umulobri64:
; FAST:       ## BB#0:
; FAST-NEXT:    movq %rdi, %rax
; FAST-NEXT:    mulq %rsi
; FAST-NEXT:    jo LBB18_1
; FAST-NEXT:  ## BB#2: ## %continue
; FAST-NEXT:    movb $1, %al
; FAST-NEXT:    andb $1, %al
; FAST-NEXT:    movzbl %al, %eax
; FAST-NEXT:    retq
; FAST-NEXT:  LBB18_1: ## %overflow
; FAST-NEXT:    xorl %eax, %eax
; FAST-NEXT:    andb $1, %al
; FAST-NEXT:    movzbl %al, %eax
; FAST-NEXT:    retq
;
; KNL-LABEL: umulobri64:
; KNL:       ## BB#0:
; KNL-NEXT:    movq %rdi, %rax
; KNL-NEXT:    mulq %rsi
; KNL-NEXT:    jo LBB18_1
; KNL-NEXT:  ## BB#2: ## %continue
; KNL-NEXT:    movb $1, %al
; KNL-NEXT:    retq
; KNL-NEXT:  LBB18_1: ## %overflow
; KNL-NEXT:    xorl %eax, %eax
; KNL-NEXT:    retq
  %t = call {i64, i1} @llvm.umul.with.overflow.i64(i64 %v1, i64 %v2)
  %val = extractvalue {i64, i1} %t, 0
  %obit = extractvalue {i64, i1} %t, 1
  br i1 %obit, label %overflow, label %continue, !prof !0

overflow:
  ret i1 false

continue:
  ret i1 true
}

define i1 @bug27873(i64 %c1, i1 %c2) {
; SDAG-LABEL: bug27873:
; SDAG:       ## BB#0:
; SDAG-NEXT:    movl $160, %ecx
; SDAG-NEXT:    movq %rdi, %rax
; SDAG-NEXT:    mulq %rcx
; SDAG-NEXT:    seto %al
; SDAG-NEXT:    orb %sil, %al
; SDAG-NEXT:    retq
;
; FAST-LABEL: bug27873:
; FAST:       ## BB#0:
; FAST-NEXT:    movl $160, %ecx
; FAST-NEXT:    movq %rdi, %rax
; FAST-NEXT:    mulq %rcx
; FAST-NEXT:    seto %al
; FAST-NEXT:    orb %sil, %al
; FAST-NEXT:    retq
;
; KNL-LABEL: bug27873:
; KNL:       ## BB#0:
; KNL-NEXT:    andl $1, %esi
; KNL-NEXT:    kmovw %esi, %k0
; KNL-NEXT:    movl $160, %ecx
; KNL-NEXT:    movq %rdi, %rax
; KNL-NEXT:    mulq %rcx
; KNL-NEXT:    seto %al
; KNL-NEXT:    andl $1, %eax
; KNL-NEXT:    kmovw %eax, %k1
; KNL-NEXT:    korw %k1, %k0, %k0
; KNL-NEXT:    kmovw %k0, %eax
; KNL-NEXT:    ## kill: %AL<def> %AL<kill> %EAX<kill>
; KNL-NEXT:    retq
  %mul = call { i64, i1 } @llvm.umul.with.overflow.i64(i64 %c1, i64 160)
  %mul.overflow = extractvalue { i64, i1 } %mul, 1
  %x1 = or i1 %c2, %mul.overflow
  ret i1 %x1
}

declare {i8,  i1} @llvm.smul.with.overflow.i8 (i8,  i8 ) nounwind readnone
declare {i16, i1} @llvm.smul.with.overflow.i16(i16, i16) nounwind readnone
declare {i32, i1} @llvm.smul.with.overflow.i32(i32, i32) nounwind readnone
declare {i64, i1} @llvm.smul.with.overflow.i64(i64, i64) nounwind readnone
declare {i8,  i1} @llvm.umul.with.overflow.i8 (i8,  i8 ) nounwind readnone
declare {i16, i1} @llvm.umul.with.overflow.i16(i16, i16) nounwind readnone
declare {i32, i1} @llvm.umul.with.overflow.i32(i32, i32) nounwind readnone
declare {i64, i1} @llvm.umul.with.overflow.i64(i64, i64) nounwind readnone

!0 = !{!"branch_weights", i32 0, i32 2147483647}
