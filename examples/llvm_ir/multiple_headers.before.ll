; ModuleID = 'hmr.module'
source_filename = "hmr.module"
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
@hmr_module_info = constant %HmrModuleInfo { i32 1, ptr @.hmrstr.9, i32 0, i32 1, ptr @hmr.regexes }, align 8

define i32 @hmr_apply(ptr %msg, ptr %ctx) {
entry:
  %0 = call i32 @hmr_rt_add_header(ptr %msg, ptr @.hmrstr, i32 5, ptr @.hmrstr.1, i32 29)
  br label %rule.cont

ret.ok:                                           ; preds = %rule.cont4
  ret i32 0

rule.cont:                                        ; preds = %entry
  %1 = call i32 @hmr_rt_add_header(ptr %msg, ptr @.hmrstr.2, i32 9, ptr @.hmrstr.3, i32 12)
  br label %rule.cont1

rule.cont1:                                       ; preds = %rule.cont
  %2 = call i32 @hmr_rt_delete_header(ptr %msg, ptr @.hmrstr.4, i32 6)
  br label %rule.cont2

rule.cont2:                                       ; preds = %rule.cont1
  %3 = call { ptr, i32 } @hmr_rt_get_header(ptr %msg, ptr @.hmrstr.5, i32 10)
  %4 = extractvalue { ptr, i32 } %3, 0
  %5 = extractvalue { ptr, i32 } %3, 1
  %6 = call i32 @hmr_rt_regex_match(ptr %ctx, i32 0, ptr %4, i32 %5)
  %7 = icmp ne i32 %6, 0
  br i1 %7, label %match.ok, label %el.cont

rule.cont3:                                       ; preds = %el.cont
  %8 = call { ptr, i32 } @hmr_rt_get_var(ptr %ctx, i32 6)
  %9 = extractvalue { ptr, i32 } %8, 0
  %10 = extractvalue { ptr, i32 } %8, 1
  %11 = call i32 @hmr_rt_add_header(ptr %msg, ptr @.hmrstr.7, i32 15, ptr %9, i32 %10)
  br label %rule.cont4

el.cont:                                          ; preds = %match.ok, %rule.cont2
  br label %rule.cont3

match.ok:                                         ; preds = %rule.cont2
  %12 = call i32 @hmr_rt_set_header(ptr %msg, ptr @.hmrstr.5, i32 10, ptr @.hmrstr.6, i32 7)
  br label %el.cont

rule.cont4:                                       ; preds = %rule.cont3
  br label %ret.ok
}

declare i32 @hmr_rt_add_header(ptr, ptr, i32, ptr, i32)

declare i32 @hmr_rt_delete_header(ptr, ptr, i32)

declare { ptr, i32 } @hmr_rt_get_header(ptr, ptr, i32)

declare i32 @hmr_rt_regex_match(ptr, i32, ptr, i32)

declare i32 @hmr_rt_set_header(ptr, ptr, i32, ptr, i32)

declare { ptr, i32 } @hmr_rt_get_var(ptr, i32)
