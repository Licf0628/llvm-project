; 真实 wasm 合约场景：批量哈希验证
; 模拟区块链合约中常见的 Merkle 树验证或批量签名检查
target triple = "wasm32-unknown-unknown"

; 简化的哈希函数（模拟 SHA256/Keccak256 的一轮计算）
; 输入：data 指针，len 长度，seed 种子
; 输出：32位哈希值
define i32 @simple_hash(ptr nocapture readonly %data, i32 %len, i32 %seed) {
entry:
  %cmp = icmp eq i32 %len, 0
  br i1 %cmp, label %exit, label %loop_preheader

loop_preheader:
  br label %loop

loop:
  %i = phi i32 [ 0, %loop_preheader ], [ %i_next, %loop_body ]
  %hash = phi i32 [ %seed, %loop_preheader ], [ %hash_new, %loop_body ]
  
  ; 边界检查（真实合约必需）
  %in_bounds = icmp ult i32 %i, %len
  br i1 %in_bounds, label %loop_body, label %exit

loop_body:
  ; 读取一个字节
  %ptr = getelementptr inbounds i8, ptr %data, i32 %i
  %byte = load i8, ptr %ptr, align 1
  %byte_ext = zext i8 %byte to i32
  
  ; 哈希计算（模拟多轮混合运算）
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

; 批量验证函数：验证多个数据块的哈希是否匹配预期值
; 输入：
;   data_array: 数据块数组的起始地址
;   hash_array: 预期哈希值数组
;   lengths: 每个数据块的长度数组
;   count: 数据块数量
; 输出：0=全部通过，非0=失败的索引+1
define i32 @batch_verify(ptr nocapture readonly %data_array, 
                         ptr nocapture readonly %hash_array,
                         ptr nocapture readonly %lengths,
                         i32 %count) {
entry:
  %has_data = icmp ugt i32 %count, 0
  br i1 %has_data, label %loop_preheader, label %success

loop_preheader:
  br label %loop

loop:
  %idx = phi i32 [ 0, %loop_preheader ], [ %idx_next, %loop_continue ]
  %data_offset = phi i32 [ 0, %loop_preheader ], [ %data_offset_next, %loop_continue ]
  
  ; 读取当前数据块的长度
  %len_ptr = getelementptr inbounds i32, ptr %lengths, i32 %idx
  %len = load i32, ptr %len_ptr, align 4
  
  ; 读取预期的哈希值
  %expected_ptr = getelementptr inbounds i32, ptr %hash_array, i32 %idx
  %expected_hash = load i32, ptr %expected_ptr, align 4
  
  ; 计算当前数据块的起始地址
  %data_ptr = getelementptr inbounds i8, ptr %data_array, i32 %data_offset
  
  ; 计算实际哈希（使用索引作为 seed 增加变化）
  %seed = add i32 %idx, 12345
  %actual_hash = call i32 @simple_hash(ptr %data_ptr, i32 %len, i32 %seed)
  
  ; 验证哈希是否匹配
  %match = icmp eq i32 %actual_hash, %expected_hash
  br i1 %match, label %loop_continue, label %failure

loop_continue:
  ; 更新数据偏移（跳到下一个数据块）
  %data_offset_next = add i32 %data_offset, %len
  
  ; 更新索引
  %idx_next = add nuw nsw i32 %idx, 1
  %more = icmp ult i32 %idx_next, %count
  br i1 %more, label %loop, label %success

failure:
  ; 返回失败的索引+1（0 表示成功）
  %fail_code = add i32 %idx, 1
  ret i32 %fail_code

success:
  ret i32 0
}

; 热点场景：内存拷贝 + 校验和计算（常见于合约数据处理）
define i32 @memcpy_with_checksum(ptr nocapture %dst, 
                                  ptr nocapture readonly %src, 
                                  i32 %len) {
entry:
  %has_len = icmp ugt i32 %len, 0
  br i1 %has_len, label %loop, label %exit

loop:
  %i = phi i32 [ 0, %entry ], [ %i_next, %loop ]
  %checksum = phi i32 [ 0, %entry ], [ %checksum_new, %loop ]
  
  ; 读取源数据
  %src_ptr = getelementptr inbounds i8, ptr %src, i32 %i
  %byte = load i8, ptr %src_ptr, align 1
  
  ; 写入目标
  %dst_ptr = getelementptr inbounds i8, ptr %dst, i32 %i
  store i8 %byte, ptr %dst_ptr, align 1
  
  ; 更新校验和（累加 + 循环移位）
  %byte_ext = zext i8 %byte to i32
  %sum = add i32 %checksum, %byte_ext
  %rotl = shl i32 %sum, 1
  %rotr = lshr i32 %sum, 31
  %checksum_new = or i32 %rotl, %rotr
  
  %i_next = add nuw nsw i32 %i, 1
  %continue = icmp ult i32 %i_next, %len
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %checksum_new, %loop ]
  ret i32 %result
}
