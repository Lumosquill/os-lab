
# 成员C：LAB4 function3 2410699
# 只保存 callee-saved 的 ra、sp、s0~s11；按 struct context的字段顺序（每个 8 字节）存取，最后 ret 到 to->context.ra，完成“旧线程保存现场、新线程恢复现场”的切换。
# 页表切换和 TLB 刷新由 proc_run 负责。
#include <riscv.h>

    .text
    .globl switch_to
    .align 2
switch_to:
    # a0 = from, a1 = to
    # 保存 from 的被调用者保存寄存器：ra、sp、s0~s11
    STORE ra,   0*REGBYTES(a0)
    STORE sp,   1*REGBYTES(a0)
    STORE s0,   2*REGBYTES(a0)
    STORE s1,   3*REGBYTES(a0)
    STORE s2,   4*REGBYTES(a0)
    STORE s3,   5*REGBYTES(a0)
    STORE s4,   6*REGBYTES(a0)
    STORE s5,   7*REGBYTES(a0)
    STORE s6,   8*REGBYTES(a0)
    STORE s7,   9*REGBYTES(a0)
    STORE s8,  10*REGBYTES(a0)
    STORE s9,  11*REGBYTES(a0)
    STORE s10, 12*REGBYTES(a0)
    STORE s11, 13*REGBYTES(a0)

    # 恢复 to 的被调用者保存寄存器
    LOAD ra,   0*REGBYTES(a1)
    LOAD sp,   1*REGBYTES(a1)
    LOAD s0,   2*REGBYTES(a1)
    LOAD s1,   3*REGBYTES(a1)
    LOAD s2,   4*REGBYTES(a1)
    LOAD s3,   5*REGBYTES(a1)
    LOAD s4,   6*REGBYTES(a1)
    LOAD s5,   7*REGBYTES(a1)
    LOAD s6,   8*REGBYTES(a1)
    LOAD s7,   9*REGBYTES(a1)
    LOAD s8,  10*REGBYTES(a1)
    LOAD s9,  11*REGBYTES(a1)
    LOAD s10, 12*REGBYTES(a1)
    LOAD s11, 13*REGBYTES(a1)
    ret
