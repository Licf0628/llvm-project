; 对指令调度高度敏感的测试负载
; 特点：长依赖链 + 频繁 load/store + 复杂分支
target triple = "wasm32-unknown-unknown"

; 密集计算：模拟密码学中的多轮混合运算
; 长依赖链，每条指令都依赖前一条的结果
define i32 @crypto_round(i32 %state, i32 %key) {
entry:
  ; 第1轮：依赖链长度 = 8
  %s1 = xor i32 %state, %key
  %s2 = mul i32 %s1, 1103515245
  %s3 = add i32 %s2, 12345
  %s4 = xor i32 %s3, %key
  %s5 = shl i32 %s4, 7
  %s6 = lshr i32 %s4, 25
  %s7 = or i32 %s5, %s6
  %s8 = add i32 %s7, %key
  
  ; 第2轮：继续依赖
  %s9 = mul i32 %s8, 69069
  %s10 = xor i32 %s9, 2147483647
  %s11 = add i32 %s10, 1
  %s12 = and i32 %s11, 4294967295
  
  ret i32 %s12
}

; 内存密集：不规则访问模式，对缓存敏感
define i32 @memory_intensive(ptr nocapture %data, i32 %len) {
entry:
  %cmp = icmp eq i32 %len, 0
  br i1 %cmp, label %exit, label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i_next, %loop_body ]
  %accum = phi i32 [ 0, %entry ], [ %accum_new, %loop_body ]
  
  %in_bounds = icmp ult i32 %i, %len
  br i1 %in_bounds, label %loop_body, label %exit

loop_body:
  ; 不规则访问：基于当前值计算下一个地址
  %hash = and i32 %accum, 255
  %offset = xor i32 %i, %hash
  %idx = and i32 %offset, 255
  
  ; Load
  %ptr1 = getelementptr inbounds i8, ptr %data, i32 %idx
  %val1 = load i8, ptr %ptr1, align 1
  %ext1 = zext i8 %val1 to i32
  
  ; 计算
  %tmp1 = add i32 %accum, %ext1
  %tmp2 = mul i32 %tmp1, 1103515245
  %tmp3 = add i32 %tmp2, 12345
  
  ; 再次 Load（依赖前面的计算）
  %idx2 = and i32 %tmp3, 255
  %ptr2 = getelementptr inbounds i8, ptr %data, i32 %idx2
  %val2 = load i8, ptr %ptr2, align 1
  %ext2 = zext i8 %val2 to i32
  
  ; Store（写回）
  %result = xor i32 %tmp3, %ext2
  %result_byte = trunc i32 %result to i8
  store i8 %result_byte, ptr %ptr1, align 1
  
  %accum_new = add i32 %tmp3, %ext2
  %i_next = add nuw nsw i32 %i, 1
  br label %loop

exit:
  %final = phi i32 [ 0, %entry ], [ %accum, %loop ]
  ret i32 %final
}

; 混合负载：计算 + 内存 + 分支
define i32 @mixed_workload(ptr nocapture %data, i32 %len, i32 %threshold) {
entry:
  %cmp = icmp eq i32 %len, 0
  br i1 %cmp, label %exit, label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i_next, %loop_continue ]
  %state = phi i32 [ 12345, %entry ], [ %state_new, %loop_continue ]
  %count = phi i32 [ 0, %entry ], [ %count_new, %loop_continue ]
  
  ; Load 数据
  %ptr = getelementptr inbounds i8, ptr %data, i32 %i
  %byte = load i8, ptr %ptr, align 1
  %val = zext i8 %byte to i32
  
  ; 密集计算（长依赖链）
  %key = xor i32 %state, %val
  %round1 = call i32 @crypto_round(i32 %state, i32 %key)
  %round2 = call i32 @crypto_round(i32 %round1, i32 %val)
  
  ; 分支判断
  %cond = icmp ugt i32 %round2, %threshold
  br i1 %cond, label %hot_path, label %cold_path

hot_path:
  ; 热路径：更多计算
  %hot1 = mul i32 %round2, 69069
  %hot2 = add i32 %hot1, 1
  %hot3 = xor i32 %hot2, %state
  
  ; Store 回去
  %hot_byte = trunc i32 %hot3 to i8
  store i8 %hot_byte, ptr %ptr, align 1
  
  %count_hot = add i32 %count, 1
  br label %loop_continue

cold_path:
  ; 冷路径：简单处理
  %cold1 = add i32 %round2, %state
  %cold_byte = trunc i32 %cold1 to i8
  store i8 %cold_byte, ptr %ptr, align 1
  br label %loop_continue

loop_continue:
  %state_new = phi i32 [ %hot3, %hot_path ], [ %cold1, %cold_path ]
  %count_new = phi i32 [ %count_hot, %hot_path ], [ %count, %cold_path ]
  
  %i_next = add nuw nsw i32 %i, 1
  %continue = icmp ult i32 %i_next, %len
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %count_new, %loop_continue ]
  ret i32 %result
}

; 基准测试主函数
define i32 @benchmark_sensitive(i32 %iterations) {
entry:
  %data = alloca [1024 x i8], align 1
  %data_ptr = getelementptr inbounds [1024 x i8], ptr %data, i32 0, i32 0
  
  ; 初始化
  br label %init_loop

init_loop:
  %init_i = phi i32 [ 0, %entry ], [ %init_i_next, %init_loop ]
  %init_ptr = getelementptr inbounds i8, ptr %data_ptr, i32 %init_i
  %init_val = and i32 %init_i, 255
  %init_byte = trunc i32 %init_val to i8
  store i8 %init_byte, ptr %init_ptr, align 1
  %init_i_next = add nuw nsw i32 %init_i, 1
  %init_continue = icmp ult i32 %init_i_next, 1024
  br i1 %init_continue, label %init_loop, label %bench_loop

bench_loop:
  %iter = phi i32 [ 0, %init_loop ], [ %iter_next, %bench_loop ]
  %accum = phi i32 [ 0, %init_loop ], [ %accum_new, %bench_loop ]
  
  ; 混合负载
  %result1 = call i32 @mixed_workload(ptr %data_ptr, i32 1024, i32 2147483647)
  
  ; 内存密集
  %result2 = call i32 @memory_intensive(ptr %data_ptr, i32 256)
  
  ; 组合结果
  %combined = xor i32 %result1, %result2
  %accum_new = add i32 %accum, %combined
  
  %iter_next = add nuw nsw i32 %iter, 1
  %continue = icmp ult i32 %iter_next, %iterations
  br i1 %continue, label %bench_loop, label %exit

exit:
  ret i32 %accum_new
}
