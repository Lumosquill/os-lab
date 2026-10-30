# Lab 3 提示词（prompt.md）

2411433 李烁菲

```
[PROMPT]
任务：修改文件 kern/trap/trapentry.S，完成 Trap 上下文恢复与 Trap 入口功能模块。补全 RESTORE_ALL 宏（与已提供的 SAVE_ALL 配对，把被打断程序的执行状态从 trapframe 中完整恢复），并补全 __alltraps 中 SAVE_ALL 之后的部分（把保存好的 trapframe 地址按 RISC-V 调用约定传给 C 函数 trap()，trap() 返回后顺序进入 __trapret）。

操作要求：你必须在项目中修改实际文件，而不是仅展示代码。保留文件中的所有现有代码，只在两处标记 "#lab3 function1 YOUR CODE"补全。不得修改 SAVE_ALL、__trapret 标签、sret 指令、struct trapframe 与 trap() 的签名。

输出要求：直接进行文件操作，以工程真实代码为准，不要引入无关功能。

[RELY]
// ---- 来自 kern/trap/trap.h ----
struct pushregs {
    uintptr_t zero, ra, sp, gp, tp, t0, t1, t2, s0, s1, a0, a1, a2, a3, a4, a5, a6, a7,
              s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, t3, t4, t5, t6; 
};
struct trapframe {
    struct pushregs gpr; 
    uintptr_t status;       
    uintptr_t epc;          
    uintptr_t badvaddr;     
    uintptr_t cause;        
};
void trap(struct trapframe *tf);

// ---- 来自 kern/trap/trapentry.S 中已实现的 SAVE_ALL，其关键事实 ----
// 先 csrw sscratch, sp；再 addi sp, sp, -36*REGBYTES；之后 sp 指向 trapframe 起始地址。
//用 STORE 把 x0、x1、x3~x31 存到各自编号对应的槽（k*REGBYTES(sp)）；x2 不在此时保存。
//通过 csrrw s0, sscratch, x0 取回陷入前的 sp，随后存入槽 2（2*REGBYTES(sp)），并把 sscratch 清 0。因此槽 2 中保存的是"陷入前的 sp"，不是当前 sp。
// s1、s2、s3、s4 被用来读取 sstatus、sepc、sbadaddr、scause，并分别存入槽 32~35。
// 执行完 SAVE_ALL 后，当前 sp 指向 trapframe 起始地址。

// ---- 可用宏与约定 ----
// STORE / LOAD / REGBYTES 来自 <riscv.h>
// RISC-V 调用约定：第一个参数通过 a0 传递
// 特权指令 sret：从 sepc 返回，并按 sstatus 恢复特权级与中断使能

[GUARANTEE]
1.必须实现或补全的部分：
- RESTORE_ALL 宏体
- __alltraps 中 SAVE_ALL 之后到 __trapret 标签之前的代码
2.不允许修改：SAVE_ALL、__trapret、sret、struct trapframe、trap() 签名。
3.你可以根据需要添加：内部使用的注释。

[SPECIFICATION]
## RESTORE_ALL
Pre-Condition:
- 当前 sp 指向一个由 SAVE_ALL 保存的、完整且未被破坏的 trapframe 起始地址。
- 槽 2 保存的是陷入前的 sp 值；槽 32、33 分别保存 sstatus 与 sepc。

Post-Condition:
- sstatus 与 sepc 被写回为 trapframe 中保存的值（允许 trap() 通过修改 tf 来改变返回点与状态）。
- 除 x0 外的所有通用寄存器（含 sp）恢复为 trapframe 中对应槽的值；其中 sp 恢复为陷入前的 sp，
  即与 Trap 发生前的内核栈位置完全一致。
- 宏执行完毕后，下一条要执行的指令是 sret，且此后不再对 sp 做任何增减。
- badvaddr、cause 不需要写回任何寄存器；sscratch 保持为 0。

Case 1（trap() 未修改 tf）:
- 恢复后的寄存器、sstatus、sepc 与 SAVE_ALL 保存时一致，sret 回到被打断的位置。

Case 2（trap() 修改了 tf->epc 或 tf->status，如异常处理跳过指令）:
- 恢复使用的是 tf 当前内容，而不是 Trap 发生时的旧值，sret 回到被修改后的 epc。

Requirements:
- 写回 CSR 时需要借用通用寄存器，借用的寄存器自身也必须在最后被恢复为保存值，
  因此必须在恢复这些寄存器之前先读出槽 32、33 并写入 CSR。
- x2(sp) 必须是最后一个被恢复的寄存器，且其值来自槽 2，不能通过 addi 推算。
- 不得对 trapframe 内存的内容做修改。

## __alltraps
Pre-Condition:
- 由 stvec 指向并因 Trap 进入；SAVE_ALL 已执行完毕，sp 指向完整 trapframe。

Post-Condition:
- 以当前 trapframe 地址作为唯一参数调用 trap()，参数按 RISC-V 调用约定传递。
- trap() 返回后，sp 与调用前相同（调用前后栈平衡），控制流顺序进入 __trapret，由 RESTORE_ALL 与 sret 完成返回。
Case 1（Trap 为中断）/ Case 2（Trap 为异常）:
- 两类 Trap 走同一入口路径，区分工作由 trap() 内部完成，__alltraps 中不做区分。

Requirements:
- 不得在 __alltraps 末尾添加 ret 或到 __trapret 的跳转以外的额外控制流；
  __trapret 标签需保持紧跟在 __alltraps 之后。
- 不得在调用 trap() 前后破坏 trapframe 中任何槽位。

```

2411570 何美璇

````
[PROMPT]
**任务**：修改文件 kern/trap/trap.c，在标有 "//lab3 function2" 的位置，实现内核在 S 模式下的同步异常处理功能（exception_handler）：当 trap 的原因是异常而不是中断时，由它根据异常类型输出提示信息，并保证处理返回后程序能够继续向下执行而不是反复触发同一个异常。本任务只负责异常处理，不负责中断处理（interrupt_handler 由其他成员实现）、不负责 trap_dispatch 的分发逻辑、不负责 kern/trap/trapentry.S 中现场的保存与恢复。

**操作要求**：你必须在项目中修改实际文件，而不是仅展示代码。保留文件中的现有代码，不修改 trap.c 中 exception_handler 以外的已有函数（包括 print_trapframe、trap_dispatch、trap、idt_init 等），只在标记位置补全。可以添加辅助函数和内部使用的宏。

**输出要求**：直接进行文件操作，使用下面 [RELY]、[GUARANTEE]、[SPECIFICATION] 中的信息，以工程真实代码为准，不要引入无关功能。

[RELY]
// 异常原因编号（libs/riscv.h）
#define CAUSE_ILLEGAL_INSTRUCTION 0x2   // 非法指令异常
#define CAUSE_BREAKPOINT          0x3   // 断点异常

// 数据结构（kern/trap/trap.h），所有字段类型均为 uintptr_t
struct trapframe {
    struct pushregs gpr;   // 32 个通用寄存器
    uintptr_t status;      // sstatus
    uintptr_t epc;         // sepc，触发异常的那条指令的地址
    uintptr_t badvaddr;    // stval
    uintptr_t cause;       // scause，异常时最高位为 0，低位是异常编号
};

// 可用函数
void print_trapframe(struct trapframe *tf);   // kern/trap/trap.c，已实现，输出完整 trapframe，其中包含 "cause" 一行
int cprintf(const char *fmt, ...);            // 格式化输出，现有代码中用 "0x%08x" 输出 tf 中的地址

// 调用关系（kern/trap/trap.c）
// trap_dispatch() 在 tf->cause 最高位为 0 时调用 exception_handler(tf)。
// exception_handler 返回后，trapentry.S 会用 tf 中的内容恢复现场并执行 sret，
// 因此 tf->epc 的值决定处理结束后从哪条指令继续执行。
// RISC-V 对非法指令、断点等同步异常，保存的 epc 都是触发异常的那条指令本身，硬件不会自动指向下一条。

// 指令编码事实
// RISC-V 指令的最低两位为 11 时是 4 字节指令，否则是 2 字节压缩指令。
// 工程可能启用压缩指令，因此 ebreak 这类指令的长度不一定是 4 字节。

[GUARANTEE]
必须实现的接口：
```c
void exception_handler(struct trapframe *tf);   // 必须具有外部链接，不能声明为 static
```
该函数必须定义在 trap_dispatch 之前，使 trap_dispatch 能正常调用。测试代码在其他源文件中通过 extern 引用它，所以不能改函数名、参数和返回值类型。

你可以根据需要添加：
- 辅助函数（建议设为 static，只在 trap.c 内部使用）
- 内部使用的宏定义

[SPECIFICATION]
## exception_handler
**Pre-Condition**:
- 由 trap_dispatch 在异常（cause 最高位为 0）时调用，tf 非空，指向 trapentry.S 保存好的有效 trapframe。
- 对于非法指令和断点异常，tf->epc 是触发异常的那条指令的地址，该地址在内核中可读。
- 也可能被测试代码用一个手工构造的 trapframe 调用，其 epc 不一定指向有效内存。

**Post-Condition**:
- 返回后，除 tf->epc 之外，tf 中的其他字段（包括所有通用寄存器）保持调用前的值不变。
- 函数返回后不会再次触发同一个异常；对于已识别的异常类型，返回后程序从触发异常的指令的下一条指令继续执行。
- 函数本身不 panic、不死循环，调用后系统可以继续运行。

  **Case 1**（cause 是非法指令异常）:
  - 输出包含 "Illegal instruction at" 字样的提示信息，并带出触发异常的指令地址；将 tf->epc 更新为下一条指令的地址，下一条指令的位置由触发异常那条指令自身的编码长度决定。

  **Case 2**（cause 是断点异常）:
  - 输出包含 "Breakpoint at" 字样的提示信息，并带出触发断点的指令地址；将 tf->epc 更新为下一条指令的地址，下一条指令的位置由触发断点的那条指令自身的编码长度决定。

  **Case 3**（其他任何异常类型）:
  - 不识别的异常不做特殊处理：调用 print_trapframe 输出完整现场，不修改 tf->epc，不访问 tf->epc 指向的内存，函数正常返回。

**Requirements**:
- 更新 epc 时不得假定指令长度固定为 4 字节，2 字节和 4 字节的指令都必须能正确越过。
- Case 3 中不得读取 tf->epc 所指的内存，因为测试会传入 epc 为 0 的手工 trapframe。
- 输出的提示信息格式需保证评分脚本能匹配到 "Illegal instruction at" 和 "Breakpoint at"，Case 3 的输出需包含 print_trapframe 打印的 "cause    0x%08x" 一行。
````
