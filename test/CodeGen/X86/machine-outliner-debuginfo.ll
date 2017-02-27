; RUN: llc -enable-machine-outliner -mtriple=x86_64-apple-darwin < %s | FileCheck %s

@x = global i32 0, align 4, !dbg !0

; Function Attrs: noinline noredzone nounwind ssp uwtable
define i32 @main() #0 !dbg !11 {
  %1 = alloca i32, align 4
  %2 = alloca i32, align 4
  %3 = alloca i32, align 4
  %4 = alloca i32, align 4
  %5 = alloca i32, align 4
  store i32 0, i32* %1, align 4
  store i32 0, i32* @x, align 4, !dbg !23
  ; CHECK: callq l_OUTLINED_FUNCTION_0
  store i32 1, i32* %2, align 4, !dbg !24
  call void @llvm.dbg.declare(metadata i32* %2, metadata !14, metadata !15), !dbg !16
  store i32 2, i32* %3, align 4, !dbg !25
  store i32 3, i32* %4, align 4, !dbg !26
  call void @llvm.dbg.declare(metadata i32* %3, metadata !17, metadata !15), !dbg !18
  store i32 4, i32* %5, align 4, !dbg !27
  store i32 1, i32* @x, align 4, !dbg !28
  ; CHECK: callq l_OUTLINED_FUNCTION_0
  store i32 1, i32* %2, align 4, !dbg !29
  call void @llvm.dbg.declare(metadata i32* %4, metadata !19, metadata !15), !dbg !20
  store i32 2, i32* %3, align 4, !dbg !30
  store i32 3, i32* %4, align 4, !dbg !31
  call void @llvm.dbg.declare(metadata i32* %5, metadata !21, metadata !15), !dbg !22
  store i32 4, i32* %5, align 4, !dbg !32
  ret i32 0, !dbg !33
}

; Function Attrs: nounwind readnone
declare void @llvm.dbg.declare(metadata, metadata, metadata) #1

attributes #0 = { noredzone nounwind ssp uwtable "no-frame-pointer-elim"="true" }
attributes #1 = { nounwind readnone }

; CHECK-LABEL: l_OUTLINED_FUNCTION_0:
; CHECK-NOT:  .loc  {{[0-9]+}} {{[0-9]+}} {{[0-9]+}} {{^(is_stmt)}}
; CHECK:      movl  $1, -{{[0-9]+}}(%rbp)
; CHECK-NEXT: movl  $2, -{{[0-9]+}}(%rbp)
; CHECK-NEXT: movl  $3, -{{[0-9]+}}(%rbp)
; CHECK-NEXT: movl  $4, -{{[0-9]+}}(%rbp)
; CHECK-NEXT: retq

!llvm.dbg.cu = !{!2}
!llvm.module.flags = !{!7, !8, !9}
!llvm.ident = !{!10}

!0 = !DIGlobalVariableExpression(var: !1)
!1 = distinct !DIGlobalVariable(name: "x", scope: !2, file: !3, line: 2, type: !6, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C99, file: !3, producer: "clang version 5.0.0", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, enums: !4, globals: !5)
!3 = !DIFile(filename: "debug-info-test.c", directory: "dir")
!4 = !{}
!5 = !{!0}
!6 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!7 = !{i32 2, !"Dwarf Version", i32 4}
!8 = !{i32 2, !"Debug Info Version", i32 3}
!9 = !{i32 1, !"PIC Level", i32 2}
!10 = !{!"clang version 5.0.0"}
!11 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 4, type: !12, isLocal: false, isDefinition: true, scopeLine: 4, flags: DIFlagPrototyped, isOptimized: false, unit: !2, variables: !4)
!12 = !DISubroutineType(types: !13)
!13 = !{!6}
!14 = !DILocalVariable(name: "a", scope: !11, file: !3, line: 5, type: !6)
!15 = !DIExpression()
!16 = !DILocation(line: 5, column: 6, scope: !11)
!17 = !DILocalVariable(name: "b", scope: !11, file: !3, line: 5, type: !6)
!18 = !DILocation(line: 5, column: 9, scope: !11)
!19 = !DILocalVariable(name: "c", scope: !11, file: !3, line: 5, type: !6)
!20 = !DILocation(line: 5, column: 12, scope: !11)
!21 = !DILocalVariable(name: "d", scope: !11, file: !3, line: 5, type: !6)
!22 = !DILocation(line: 5, column: 15, scope: !11)
!23 = !DILocation(line: 7, column: 4, scope: !11)
!24 = !DILocation(line: 9, column: 4, scope: !11)
!25 = !DILocation(line: 10, column: 4, scope: !11)
!26 = !DILocation(line: 11, column: 4, scope: !11)
!27 = !DILocation(line: 12, column: 4, scope: !11)
!28 = !DILocation(line: 14, column: 4, scope: !11)
!29 = !DILocation(line: 16, column: 4, scope: !11)
!30 = !DILocation(line: 17, column: 4, scope: !11)
!31 = !DILocation(line: 18, column: 4, scope: !11)
!32 = !DILocation(line: 19, column: 4, scope: !11)
!33 = !DILocation(line: 21, column: 2, scope: !11)
