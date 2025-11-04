# Faulty Oops Breakdown

## Summary of Faulty Oops
Writing to /dev/faulty intentionally triggers a kernel Oops via a NULL pointer dereference inside the LDD3 faulty module. The crash occurs in the module’s write path (faulty_write) where the driver performs a deliberate bad memory access (classic LDD3 example: *(int *)0 = 0;). The Oops confirms a data abort on a write to virtual address 0x0.

---
### Reproduction 
---
```bash
# trigger and capture the oops
dmesg -C
echo hello_world > /dev/faulty
dmesg | tee /root/faulty-oops.txt
```

---
### Oops Output Log
---
```bash
$ echo hello_world > /dev/faulty
[  508.034318] Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
[  508.034579] Mem abort info:
[  508.034946]   ESR = 0x0000000096000045
[  508.035084]   EC = 0x25: DABT (current EL), IL = 32 bits
[  508.035161]   SET = 0, FnV = 0
[  508.035254]   EA = 0, S1PTW = 0
[  508.035379]   FSC = 0x05: level 1 translation fault
[  508.035530] Data abort info:
[  508.035619]   ISV = 0, ISS = 0x00000045
[  508.035822]   CM = 0, WnR = 1
[  508.036017] user pgtable: 4k pages, 39-bit VAs, pgdp=0000000041a4a000
[  508.036219] [0000000000000000] pgd=0000000000000000, p4d=0000000000000000, pud=0000000000000000
[  508.036612] Internal error: Oops: 0000000096000045 [#1] PREEMPT SMP
[  508.036893] Modules linked in: scull(O) silly(O) sleepy(O) hello(O) faulty(O)
[  508.037251] CPU: 1 PID: 335 Comm: sh Tainted: G           O      5.15.194-yocto-standard #1
[  508.037445] Hardware name: linux,dummy-virt (DT)
[  508.037672] pstate: 80000005 (Nzcv daif -PAN -UAO -TCO -DIT -SSBS BTYPE=--)
[  508.037841] pc : faulty_write+0x18/0x20 [faulty]
[  508.038207] lr : vfs_write+0xf8/0x2a0
[  508.038309] sp : ffffffc0096e3d80
[  508.038398] x29: ffffffc0096e3d80 x28: ffffff8003642940 x27: 0000000000000000
[  508.038690] x26: 0000000000000000 x25: 0000000000000000 x24: 0000000000000000
[  508.038986] x23: 0000000000000000 x22: ffffffc0096e3dc0 x21: 0000005576747a80
[  508.039138] x20: ffffff8003680c00 x19: 000000000000000c x18: 0000000000000000
[  508.039291] x17: 0000000000000000 x16: 0000000000000000 x15: 0000000000000000
[  508.039461] x14: 0000000000000000 x13: 0000000000000000 x12: 0000000000000000
[  508.039607] x11: 0000000000000000 x10: 0000000000000000 x9 : ffffffc008272108
[  508.039802] x8 : 0000000000000000 x7 : 0000000000000000 x6 : 0000000000000000
[  508.039976] x5 : 0000000000000001 x4 : ffffffc000ba0000 x3 : ffffffc0096e3dc0
[  508.040141] x2 : 000000000000000c x1 : 0000000000000000 x0 : 0000000000000000
[  508.040416] Call trace:
[  508.040503]  faulty_write+0x18/0x20 [faulty]
[  508.040626]  ksys_write+0x74/0x110
[  508.040726]  __arm64_sys_write+0x24/0x30
[  508.040823]  invoke_syscall+0x5c/0x130
[  508.040927]  el0_svc_common.constprop.0+0x4c/0x100
[  508.041029]  do_el0_svc+0x4c/0xc0
[  508.041112]  el0_svc+0x28/0x80
[  508.041218]  el0t_64_sync_handler+0xa4/0x130
[  508.041301]  el0t_64_sync+0x1a0/0x1a4
[  508.041484] Code: d2800001 d2800000 d503233f d50323bf (b900003f)
[  508.041792] ---[ end trace 51a8efa03d3826b8 ]---
Segmentation fault
```

---

### Fault Interpretation Breakdown
---
- **Fault type:** ```Unable to handle kernel NULL pointer dereference``` at VA ```0x0```
The access was to address zero.

- **ESR/EC:** ```ESR = 0x96000045```, ```EC = 0x25: DABT (current EL)```
A **Data ABorT** occurred in the kernel (current exception level).

- **Write vs read:** ```WnR = 1```
The fault happened on a write access.

- **Faulting instruction:** ```pc : faulty_write+0x18/0x20 [faulty]```
The program counter points inside the ```faulty``` module’s write routine.

- **Backtrace:**
```faulty_write → ksys_write → __arm64_sys_write → ...```
This shows a user-space write to ```/dev/faulty``` reaching the driver’s write method and faulting there.

- **Registers at fault:** ```x0 = 0x0```, ```x1 = 0x0```, …
Consistent with a store to a NULL address. The disassembly word ```b900003f``` is an AArch64 store instruction; in the LDD example the driver effectively executes a store to address 0, e.g. ```*(int *)0 = 0;```.

- **Tainted flag:** ```Tainted: ... O```
```O``` means an out-of-tree module was loaded; expected for LDD modules.

- **Modules linked in:** includes ```faulty(O)```
Confirms the correct module was present when the Oops occurred.

---

### Locating the Exact Source Line
---
You can map the trace to source lines using either Yocto’s kernel scripts or addr2line.

#### Debug Route 1) **decode_stacktrace.sh**
This annotates each frame with ```file:line```. The ```faulty_write``` frame will point to the line in ```faulty.c``` that does the bad store.
```bash
KSRCDIR=build/tmp/work-shared/qemuarm64/kernel-source
KBUILDDIR=build/tmp/work-shared/qemuarm64/kernel-build-artifacts

$KSRCDIR/scripts/decode_stacktrace.sh \
  $KBUILDDIR/vmlinux \
  $KSRCDIR \
  < /path/to/faulty-oops.txt
```
#### Debug Route 2) **addr2line with module offset**
On the Yocto target run:
```bash
cat /sys/module/faulty/sections/.text
```

Take the ```pc``` from the Oops (```faulty_write+0x18...```) and compute the address offset from ```.text```. Then on the host:
```bash
aarch64-poky-linux-addr2line -e \
  build/tmp/work/qemuarm64-poky-linux/misc-modules/*/git/misc-modules/faulty.ko \
  0x<OFFSET>
```

This prints ```faulty.c:<line>```.
> In the LDD3 template, this is the line with *(int *)0 = 0; inside the driver’s read/write path.

---

### Root Cause
---
The driver deliberately dereferences a NULL pointer in its write path to demonstrate kernel oops handling:
```c
/* LDD3 faulty example */
*(int *)0 = 0;   /* intentional NULL write */
```
This causes a **data abort on write to VA 0x0**, producing the Oops seen above.

---

### Fixes and Notes
---

- To ensure you always load your ```faulty.ko``` and not the md/RAID “faulty”:

    - Prefer ```insmod /lib/modules/$(uname -r)/extra/faulty.ko```, or

    - Disable ```CONFIG_MD_FAULTY``` via a kernel config fragment so ```modprobe faulty``` resolves to your module.

- Make ```/dev/faulty``` automatically:

    - Have your init script parse ```/proc/devices``` and ```mknod /dev/faulty c <major> 0``` on boot.

---