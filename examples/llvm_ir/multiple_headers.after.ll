; ModuleID = 'hmr.module'
source_filename = "hmr.module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

%HmrRegexEntry = type { ptr, i32 }
%HmrModuleInfo = type { i32, ptr, i32, i32, ptr }

@.hmrstr = private unnamed_addr constant [6 x i8] c"Allow\00", align 1
@.hmrstr.1 = private unnamed_addr constant [30 x i8] c"INVITE,ACK,BYE,CANCEL,OPTIONS\00", align 1
@.hmrstr.2 = private unnamed_addr constant [10 x i8] c"Supported\00", align 1
@.hmrstr.3 = private unnamed_addr constant [13 x i8] c"100rel,timer\00", align 1
@.hmrstr.4 = private unnamed_addr constant [7 x i8] c"Server\00", align 1
@.hmrstr.5 = private unnamed_addr constant [11 x i8] c"User-Agent\00", align 1
@.hmrstr.6 = private unnamed_addr constant [8 x i8] c"SBC/1.0\00", align 1
@.hmrstr.7 = private unnamed_addr constant [16 x i8] c"X-Ingress-Realm\00", align 1
@.hmrstr.8 = private unnamed_addr constant [3 x i8] c".*\00", align 1
@hmr.regexes = private unnamed_addr constant [1 x %HmrRegexEntry] [%HmrRegexEntry { ptr @.hmrstr.8, i32 0 }]
@.hmrstr.9 = private unnamed_addr constant [16 x i8] c"MultipleHeaders\00", align 1
@hmr_module_info = local_unnamed_addr constant %HmrModuleInfo { i32 1, ptr @.hmrstr.9, i32 0, i32 1, ptr @hmr.regexes }, align 8

; Function Attrs: nounwind
define noundef i32 @hmr_apply(ptr %msg, ptr %ctx) local_unnamed_addr #0 {
entry:
  %0 = tail call i32 @hmr_rt_add_header(ptr %msg, ptr nonnull @.hmrstr, i32 5, ptr nonnull @.hmrstr.1, i32 29)
  %1 = tail call i32 @hmr_rt_add_header(ptr %msg, ptr nonnull @.hmrstr.2, i32 9, ptr nonnull @.hmrstr.3, i32 12)
  %2 = tail call i32 @hmr_rt_delete_header(ptr %msg, ptr nonnull @.hmrstr.4, i32 6)
  %3 = tail call { ptr, i32 } @hmr_rt_get_header(ptr %msg, ptr nonnull @.hmrstr.5, i32 10)
  %4 = extractvalue { ptr, i32 } %3, 0
  %5 = extractvalue { ptr, i32 } %3, 1
  %6 = tail call i32 @hmr_rt_regex_match(ptr %ctx, i32 0, ptr %4, i32 %5)
  %.not = icmp eq i32 %6, 0
  br i1 %.not, label %el.cont, label %match.ok

el.cont:                                          ; preds = %match.ok, %entry
  %7 = tail call { ptr, i32 } @hmr_rt_get_var(ptr %ctx, i32 6)
  %8 = extractvalue { ptr, i32 } %7, 0
  %9 = extractvalue { ptr, i32 } %7, 1
  %10 = tail call i32 @hmr_rt_add_header(ptr %msg, ptr nonnull @.hmrstr.7, i32 15, ptr %8, i32 %9)
  ret i32 0

match.ok:                                         ; preds = %entry
  %11 = tail call i32 @hmr_rt_set_header(ptr %msg, ptr nonnull @.hmrstr.5, i32 10, ptr nonnull @.hmrstr.6, i32 7)
  br label %el.cont
}

; Function Attrs: nounwind
declare i32 @hmr_rt_add_header(ptr, ptr, i32, ptr, i32) local_unnamed_addr #0

; Function Attrs: nounwind
declare i32 @hmr_rt_delete_header(ptr, ptr, i32) local_unnamed_addr #0

; Function Attrs: nounwind
declare { ptr, i32 } @hmr_rt_get_header(ptr, ptr, i32) local_unnamed_addr #0

; Function Attrs: nounwind
declare i32 @hmr_rt_regex_match(ptr, i32, ptr, i32) local_unnamed_addr #0

; Function Attrs: nounwind
declare i32 @hmr_rt_set_header(ptr, ptr, i32, ptr, i32) local_unnamed_addr #0

; Function Attrs: nounwind
declare { ptr, i32 } @hmr_rt_get_var(ptr, i32) local_unnamed_addr #0

attributes #0 = { nounwind }
