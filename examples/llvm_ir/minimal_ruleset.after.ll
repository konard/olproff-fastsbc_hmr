; ModuleID = 'hmr.module'
source_filename = "hmr.module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

%HmrModuleInfo = type { i32, ptr, i32, i32, ptr }

@.hmrstr = private unnamed_addr constant [10 x i8] c"Supported\00", align 1
@.hmrstr.1 = private unnamed_addr constant [7 x i8] c"100rel\00", align 1
@.hmrstr.2 = private unnamed_addr constant [15 x i8] c"MinimalRuleset\00", align 1
@hmr_module_info = local_unnamed_addr constant %HmrModuleInfo { i32 1, ptr @.hmrstr.2, i32 0, i32 0, ptr null }, align 8

; Function Attrs: nounwind
define noundef i32 @hmr_apply(ptr %msg, ptr nocapture readnone %ctx) local_unnamed_addr #0 {
entry:
  %0 = tail call i32 @hmr_rt_add_header(ptr %msg, ptr nonnull @.hmrstr, i32 9, ptr nonnull @.hmrstr.1, i32 6)
  ret i32 0
}

; Function Attrs: nounwind
declare i32 @hmr_rt_add_header(ptr, ptr, i32, ptr, i32) local_unnamed_addr #0

attributes #0 = { nounwind }
