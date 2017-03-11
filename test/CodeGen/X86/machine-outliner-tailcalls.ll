; ModuleID = '/Users/easter/Desktop/tailcalls.c'
source_filename = "/Users/easter/Desktop/tailcalls.c"
target datalayout = "e-m:o-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx10.12.0"

@x = common local_unnamed_addr global i32 0, align 4

; Function Attrs: noredzone nounwind ssp uwtable
define i32 @foo0(i32) local_unnamed_addr #0 {
  store i32 0, i32* @x, align 4, !tbaa !2
  %2 = tail call i32 @ext(i32 1) #2
  ret i32 undef
}

; Function Attrs: noredzone
declare i32 @ext(i32) local_unnamed_addr #1

; Function Attrs: noredzone nounwind ssp uwtable
define i32 @foo1(i32) local_unnamed_addr #0 {
  store i32 0, i32* @x, align 4, !tbaa !2
  %2 = tail call i32 @ext(i32 1) #2
  ret i32 undef
}

attributes #0 = { noredzone nounwind ssp uwtable "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-jump-tables"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="false" "stack-protector-buffer-size"="8" "target-cpu"="penryn" "target-features"="+cx16,+fxsr,+mmx,+sse,+sse2,+sse3,+sse4.1,+ssse3,+x87" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { noredzone "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="false" "stack-protector-buffer-size"="8" "target-cpu"="penryn" "target-features"="+cx16,+fxsr,+mmx,+sse,+sse2,+sse3,+sse4.1,+ssse3,+x87" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #2 = { noredzone nounwind }

!llvm.module.flags = !{!0}
!llvm.ident = !{!1}

!0 = !{i32 1, !"PIC Level", i32 2}
!1 = !{!"clang version 5.0.0 (https://github.com/llvm-mirror/clang.git 21af18162eb21419072e982fefc3c99a80ca1670) (https://github.com/llvm-mirror/llvm.git 09b6785a09403803ed99ff92654a7871038c06ec)"}
!2 = !{!3, !3, i64 0}
!3 = !{!"int", !4, i64 0}
!4 = !{!"omnipotent char", !5, i64 0}
!5 = !{!"Simple C/C++ TBAA"}
