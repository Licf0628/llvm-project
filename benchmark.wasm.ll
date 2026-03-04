; 完整的可执行 wasm 合约测试模块
target triple = "wasm32-unknown-unknown"

define i32 @simple_hash(ptr nocapture readonly %data, i32 %len, i32 %seed) {
entry:
  %cmp = icmp eq i32 %len, 0
  br i1 %cmp, label %exit, label %loop_preheader

loop_preheader:
  br label %loop

loop:
  %i = phi i32 [ 0, %loop_preheader ], [ %i_next, %loop_body ]
  %hash = phi i32 [ %seed, %loop_preheader ], [ %hash_new, %loop_body ]
  %in_bounds = icmp ult i32 %i, %len
  br i1 %in_bounds, label %loop_body, label %exit

loop_body:
  %ptr = getelementptr inbounds i8, ptr %data, i32 %i
  %byte = load i8, ptr %ptr, align 1
  %byte_ext = zext i8 %byte to i32
  %xor1 = xor i32 %hash, %byte_ext
  %mul1 = mul i32 %xor1, 1103515245
  %add1 = add i32 %mul1, 12345
  %shr1 = lshr i32 %add1, 16
  %and1 = and i32 %shr1, 32767
  %xor2 = xor i32 %add1, %and1
  %mul2 = mul i32 %xor2, 69069
  %hash_new = add i32 %mul2, 1
  %i_next = add nuw nsw i32 %i, 1
  %continue = icmp ult i32 %i_next, %len
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ %seed, %entry ], [ %hash, %loop ], [ %hash_new, %loop_body ]
  ret i32 %result
}

define i32 @benchmark_hash(i32 %iterations) {
entry:
  %data = alloca [256 x i8], align 1
  %data_ptr = getelementptr inbounds [256 x i8], ptr %data, i32 0, i32 0
  br label %init_loop

init_loop:
  %init_i = phi i32 [ 0, %entry ], [ %init_i_next, %init_loop ]
  %init_ptr = getelementptr inbounds i8, ptr %data_ptr, i32 %init_i
  %init_byte = trunc i32 %init_i to i8
  store i8 %init_byte, ptr %init_ptr, align 1
  %init_i_next = add nuw nsw i32 %init_i, 1
  %init_continue = icmp ult i32 %init_i_next, 256
  br i1 %init_continue, label %init_loop, label %bench_loop

bench_loop:
  %iter = phi i32 [ 0, %init_loop ], [ %iter_next, %bench_loop ]
  %accum = phi i32 [ 0, %init_loop ], [ %accum_new, %bench_loop ]
  %hash = call i32 @simple_hash(ptr %data_ptr, i32 256, i32 %iter)
  %accum_new = xor i32 %accum, %hash
  %iter_next = add nuw nsw i32 %iter, 1
  %continue = icmp ult i32 %iter_next, %iterations
  br i1 %continue, label %bench_loop, label %exit

exit:
  ret i32 %accum_new
}
