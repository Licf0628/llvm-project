; test.wasm.ll
target triple = "wasm32-unknown-unknown"

; 一个热点循环：大量 load/store，便于观察缓存/距离相关的调度
define void @hot_loop(ptr nocapture %ptr, i32 %n, i32 %val) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %body ]
  ; 计算地址：ptr + i*4
  %idx = mul nuw nsw i32 %i, 4
  %p   = getelementptr inbounds i8, ptr %ptr, i32 %idx

  ; load + 简单计算 + store，制造数据依赖和内存依赖
  %old = load i8, ptr %p, align 1
  %ext = zext i8 %old to i32
  %sum = add i32 %ext, %val
  %new = trunc i32 %sum to i8
  store i8 %new, ptr %p, align 1

  ; 分支：制造控制依赖和热点路径
  %cond = icmp slt i32 %sum, 1024
  br i1 %cond, label %body, label %exit_slow

body:
  %i.next = add nuw nsw i32 %i, 1
  %cmp    = icmp ult i32 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit_slow:
  ; 再做一次 load/store，当“冷路径”与热点路径混在一起时，看调度如何处理
  %old2 = load i8, ptr %ptr, align 1
  %ext2 = zext i8 %old2 to i32
  %sum2 = add i32 %ext2, 1
  %new2 = trunc i32 %sum2 to i8
  store i8 %new2, ptr %ptr, align 1
  br label %exit

exit:
  ret void
}
