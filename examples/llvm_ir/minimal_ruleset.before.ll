; ModuleID = 'hmr.module'
source_filename = "hmr.module"
target triple = "x86_64-pc-linux-gnu"

%HmrModuleInfo = type { i32, ptr, i32, i32, ptr }

@.hmrstr = private unnamed_addr constant [10 x i8] c"Supported\00", align 1
@.hmrstr.1 = private unnamed_addr constant [7 x i8] c"100rel\00", align 1
@.hmrstr.2 = private unnamed_addr constant [15 x i8] c"MinimalRuleset\00", align 1
@hmr_module_info = constant %HmrModuleInfo { i32 1, ptr @.hmrstr.2, i32 0, i32 0, ptr null }, align 8

define i32 @hmr_apply(ptr %msg, ptr %ctx) {
entry:
  %0 = call i32 @hmr_rt_add_header(ptr %msg, ptr @.hmrstr, i32 9, ptr @.hmrstr.1, i32 6)
  br label %rule.cont

ret.ok:                                           ; preds = %rule.cont
  ret i32 0

rule.cont:                                        ; preds = %entry
  br label %ret.ok
}

declare i32 @hmr_rt_add_header(ptr, ptr, i32, ptr, i32)
