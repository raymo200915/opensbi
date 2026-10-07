OpenSBI Domain Support
======================

An OpenSBI domain is a system-level partition (subset) of underlying hardware
having its own memory regions (RAM and MMIO devices) and HARTs. The OpenSBI
will try to achieve secure isolation between domains using RISC-V platform
features such as PMP, ePMP, IOPMP, SiFive Shield, etc.

Important entities which help implement OpenSBI domain support are:

* **struct sbi_domain_memregion** - Representation of a domain memory region
* **struct sbi_hartmask** - Representation of domain HART set
* **struct sbi_domain** - Representation of a domain instance

Each HART of a RISC-V platform must have an OpenSBI domain assigned to it.
The OpenSBI platform support is responsible for populating domains and
providing HART id to domain mapping. The OpenSBI domain support will by
default assign **the ROOT domain** to all HARTs of a RISC-V platform, so
it is not mandatory for the OpenSBI platform support to populate domains.

Domain Memory Region
--------------------

A domain memory region is represented by **struct sbi_domain_memregion** in
OpenSBI and has following details:

* **order** - The size of a memory region is **2 ^ order** where **order**
  must be **3 <= order <= __riscv_xlen**
* **base** - The base address of a memory region is **2 ^ order**
  aligned start address
* **flags** - The flags of a memory region represent memory type (i.e.
  RAM or MMIO) and allowed accesses (i.e. READ, WRITE, EXECUTE, etc.)

Domain Instance
---------------

A domain instance is represented by **struct sbi_domain** in OpenSBI and
has following details:

* **index** - Logical index of this domain
* **name** - Name of this domain
* **init_order** - Initialization order of this domain. Domain with a
  lower initialization order will be booted first and two domains must
  not have same initialization order.
* **assigned_harts** - HARTs assigned to this domain
* **possible_harts** - HARTs possible in this domain
* **regions** - Array of memory regions terminated by a memory region
  with order zero
* **boot_hartid** - HART id of the HART booting this domain. The domain
  boot HART will be started at boot-time if boot HART is possible and
  assigned for this domain.
* **next_addr** - Address of the next booting stage for this domain
* **next_arg1** - Arg1 (or 'a1' register) of the next booting stage for
  this domain
* **next_mode** - Privilege mode of the next booting stage for this
  domain. This can be either S-mode or U-mode.
* **system_reset_allowed** - Is domain allowed to reset the system?
* **system_suspend_allowed** - Is domain allowed to suspend the system?

The memory regions represented by **regions** in **struct sbi_domain** have
following additional constraints to align with RISC-V PMP requirements:

* A memory region to protect OpenSBI firmware from S-mode and U-mode
  should always be present
* For two overlapping memory regions, one should be sub-region of another
* Two overlapping memory regions should not be of same size
* Two overlapping memory regions cannot have same flags
* Memory access checks on overlapping address should prefer smallest
  overlapping memory region flags.

ROOT Domain
-----------

**The ROOT domain** is the default OpenSBI domain which is assigned by
default to all HARTs of a RISC-V platform. The OpenSBI domain support
will hand-craft **the ROOT domain** very early at boot-time in the
following manner:

* **index** - Logical index of the ROOT domain is always zero
* **name** - Name of the ROOT domain is "root"
* **init_order** - Initialization order of the ROOT domain is always
  0xffffffff (aka maximum possible 32-bit value)
* **assigned_harts** - At boot-time all valid HARTs of a RISC-V platform
  are assigned the ROOT domain which changes later based on OpenSBI
  platform support
* **possible_harts** - All valid HARTs of a RISC-V platform are possible
  HARTs of the ROOT domain
* **regions** - Two memory regions available to the ROOT domain:
  **A)** A memory region to protect OpenSBI firmware from S-mode and U-mode
  **B)** A memory region of **order=__riscv_xlen** allowing S-mode and
  U-mode access to full memory address space
* **boot_hartid** - Coldboot HART is the HART booting the ROOT domain
* **next_addr** - Next booting stage address in coldboot HART scratch
  space is the next address for the ROOT domain
* **next_arg1** - Next booting stage arg1 in coldboot HART scratch space
  is the next arg1 for the ROOT domain
* **next_mode** - Next booting stage mode in coldboot HART scratch space
  is the next mode for the ROOT domain
* **system_reset_allowed** - The ROOT domain is allowed to reset the system
* **system_suspend_allowed** - The ROOT domain is allowed to suspend the system

Domain Effects
--------------

Few noteworthy effects of a system partitioned into domains are as follows:

* At any point in time, a HART is running in exactly one OpenSBI domain context
* The SBI IPI and RFENCE calls from HART A are restricted to the HARTs in
  domain assigned to HART A
* The SBI HSM calls which try to change/read state of HART B from HART A will
  only work if both HART A and HART B are assigned same domain
* A HART running in S-mode or U-mode can only access memory based on the
  memory regions of the domain assigned to the HART

Domain Device Tree Bindings
---------------------------

The OpenSBI domains can be described in the **device tree (DT) blob** (or
flattened device tree) passed to the OpenSBI firmwares by the previous
booting stage. This allows OpenSBI platform support to parse and populate
OpenSBI domains from the device tree blob (or flattened device tree).

### Domain Configuration Node

All OpenSBI domain description related DT nodes should be under the domain
configuration DT node. The **/chosen** DT node is the preferred parent of
the domain configuration DT node.

The DT properties of a domain configuration DT node are as follows:

* **compatible** (Mandatory) - The compatible string of the domain
  configuration. This DT property should have value *"opensbi,domain,config"*

### Domain Memory Region Node

The domain memory region DT node describes details of a memory region and
can be pointed by multiple domain instance DT nodes. The access permissions
of the memory region are specified separately in domain instance node.

The DT properties of a domain memory region DT node are as follows:

* **compatible** (Mandatory) - The compatible string of the domain memory
  region. This DT property should have value *"opensbi,domain,memregion"*
* **base** (Mandatory) - The base address of the domain memory region. This
  DT property should have a **2 ^ order** aligned 64 bit address (i.e. two
  DT cells).
* **order** (Mandatory) - The order of the domain memory region. This DT
  property should have a 32 bit value (i.e. one DT cell) in the range
  **3 <= order <= __riscv_xlen**.
* **mmio** (Optional) - A boolean flag representing whether the domain
  memory region is a memory-mapped I/O (MMIO) region.
* **devices** (Optional) - The list of device DT node phandles for devices
  which fall under this domain memory region.

### Domain Instance Node

The domain instance DT node describes set of possible HARTs, set of memory
regions, and other details of a domain instance.

The DT properties of a domain instance DT node are as follows:

* **compatible** (Mandatory) - The compatible string of the domain instance.
  This DT property should have value *"opensbi,domain,instance"*
* **init-order** (Optional) - The 32-bit initialization order for the domain
  instance. If this DT property is not available then domain instance DT node
  offset is used as default value.
* **possible-harts** (Optional) - The list of CPU DT node phandles for the
  the domain instance. This list represents the possible HARTs of the
  domain instance.
* **root-regions-inheritance** (Optional) - A string property controlling
  how memory regions are inherited from **the ROOT domain**, which are then
  overlaid with regions specified in the **regions** property for additional
  restrictions. The allowed values are:
  * "all" - inherit all memory regions from **the ROOT domain**
  * "m-only" - inherit M-mode only memory regions from **the ROOT domain**
  If this DT property is absent, behavior is the same as "m-only".
* **regions** (Optional) - The list of domain memory region DT node phandle
  and access permissions for the domain instance. Each list entry is a pair
  of DT node phandle and access permissions. The access permissions are
  represented as a 32bit bitmask having bits: **M readable** (BIT[0]),
  **M writeable** (BIT[1]), **M executable** (BIT[2]), **SU readable**
  (BIT[3]), **SU writable** (BIT[4]), and **SU executable** (BIT[5]).
  The enforce permission bit (BIT[6]), if set, will lock the permissions
  in the PMP. This will enforce the permissions on M-mode as well which
  otherwise will have unrestricted access. This bit must be used with
  caution because no changes can be made to a PMP entry once its locked
  until the hart is reset.
  Any region of a domain defined in DT node cannot have only M-bits set
  in access permissions i.e. it cannot be an m-mode only accessible region.
* **boot-hart** (Optional) - The DT node phandle of the HART booting the
  domain instance. If not specified, defaults to the coldboot HART. Note that
  if the coldboot HART is assigned to this domain, it will be forced as
  the boot HART regardless of this property.
* **next-arg1** (Optional) - The 64 bit next booting stage arg1
  for the domain instance. If this DT property is not available
  then **next booting stage arg1 of coldboot HART** is used as default value.
* **next-addr** (Optional) - The 64 bit next booting stage address
  for the domain instance. If this DT property is not available
  then **next booting stage address of coldboot HART** is used as default value.
* **next-mode** (Optional) - The 32 bit next booting stage mode for the
  domain instance. The possible values of this DT property are: **0x1**
  (S-mode), and **0x0** (U-mode). If this DT property is not available
  then **next booting stage mode of coldboot HART** is used as default value.
* **system-reset-allowed** (Optional) - A boolean flag representing
  whether the domain instance is allowed to do system reset.
* **system-suspend-allowed** (Optional) - A boolean flag representing
  whether the domain instance is allowed to do system suspend.

### Assigning HART To Domain Instance

At boot-time, a HART is assigned to a non-ROOT domain on first come
first serve basis if the HART is listed as a possible HART of the
non-ROOT domain. If no non-ROOT domain list a HART as possible HART
then the HART is assigned to **the ROOT domain**.

At runtime, the assignment of a HART can change from one domain to
another domain as long as the HART is listed in possible HARTs of
both domains.

### Domain Configuration Only Accessible to OpenSBI

The software running inside a domain instance should only be aware of
devices and hardware resources accessible to itself.

To hide domain configuration from domain instances, the following should
be done:

* The previous booting stage should preferably provide a separate device
  tree for each domain instance and mention location of device tree in
  respective domain instance DT nodes using **next-arg1** DT property.
* If domain assigned to a HART does not have separate device tree then
  OpenSBI platform support should remove all domain configuration details
  from the device tree passed by previous booting stage before passing it
  to the next booting stage.

### Example

```
    chosen {
        opensbi-domains {
            compatible = "opensbi,domain,config";

            tmem: tmem {
                compatible = "opensbi,domain,memregion";
                base = <0x0 0x80100000>;
                order = <20>;
            };

            tuart: tuart {
                compatible = "opensbi,domain,memregion";
                base = <0x0 0x10011000>;
                order = <12>;
                mmio;
                devices = <&uart1>;
            };

            allmem: allmem {
                compatible = "opensbi,domain,memregion";
                base = <0x0 0x0>;
                order = <64>;
            };

            tdomain: trusted-domain {
                compatible = "opensbi,domain,instance";
                possible-harts = <&cpu0>;
                regions = <&tmem 0x3f>, <&tuart 0x3f>;
                boot-hart = <&cpu0>;
                next-arg1 = <0x0 0x0>;
                next-addr = <0x0 0x80100000>;
                next-mode = <0x0>;
                system-reset-allowed;
                system-suspend-allowed;
            };

            udomain: untrusted-domain {
                compatible = "opensbi,domain,instance";
                possible-harts = <&cpu1 &cpu2 &cpu3 &cpu4>;
                regions = <&tmem 0x0>, <&tuart 0x0>, <&allmem 0x3f>;
            };
        };
    };

    cpus {
        #address-cells = <1>;
        #size-cells = <0>;
        timebase-frequency = <10000000>;

        cpu0: cpu@0 {
            device_type = "cpu";
            reg = <0x00>;
            compatible = "riscv";
            ...
        };

        cpu1: cpu@1 {
            device_type = "cpu";
            reg = <0x01>;
            compatible = "riscv";
            ...
        };

        cpu2: cpu@2 {
            device_type = "cpu";
            reg = <0x02>;
            compatible = "riscv";
            ...
        };

        cpu3: cpu@3 {
            device_type = "cpu";
            reg = <0x03>;
            compatible = "riscv";
            ...
        };

        cpu4: cpu@4 {
            device_type = "cpu";
            reg = <0x04>;
            compatible = "riscv";
            ...
        };
    };

    uart1: serial@10011000 {
        ...
    };
```

### RPMI SYSTEM_IRQ Delivery Through MPXY

The domain configuration can contain SYSIRQ nodes, often labeled
`rpmi_sysirq_intc` in DTS overlays. OpenSBI matches the compatible string,
not the node name or label. The binding properties are:

| Property | Requirement | Meaning |
|:---------|:------------|:--------|
| `compatible` | Mandatory | `"opensbi,mpxy-sysirq"` |
| `interrupt-controller` | For interrupt-controller references | Marks the virtual interrupt controller |
| `#interrupt-cells` | For interrupt-controller references | `<1>`: the VIRQ index |
| `interrupts-extended` | Mandatory | Physical sources as `<&irqchip hwirq flags>` |
| `opensbi,mpxy-channel-id` | Mandatory | Unique MPXY channel and VIRQ namespace |
| `opensbi,domain` | Mandatory | Phandle to the destination domain |

An entry's position assigns its zero-based VIRQ number in the channel.
The referenced irqchip identifies the physical controller; `opensbi,domain`
selects the destination for every entry in that node. Thus the mapping is
`(channel-id, VIRQ index) -> (irqchip, HWIRQ)`, while the routing selects the
destination domain and its boot hart.

OpenSBI initializes VIRQ on demand, establishes all channel mappings and
routing rules, and then registers courier handlers using the physical IRQ
flags. Physical delivery targets the owning domain's boot hart. The current
registration path supports wired M-mode interrupt controllers with masking
and affinity operations. Delegated sources and unsupported controller paths
are rejected rather than silently redirected. This initialization does not
depend on a QEMU demo build flag.

An `opensbi,mpxy-sysirq` node under the domain configuration registers a
domain-owned SBI MPXY channel backed by OpenSBI's local VIRQ queues. This
channel does not require an RPMI mailbox or a remote platform controller.
For example:

```dts
sysirq {
    compatible = "opensbi,mpxy-sysirq";
    interrupt-controller;
    #interrupt-cells = <1>;
    opensbi,mpxy-channel-id = <4>;
    opensbi,domain = <&tdomain>;
    interrupts-extended = <&aplic_m 10 4>;
};
```

The channel ID must be unique. The referenced domain owns the channel, and
each `interrupts-extended` entry maps its physical interrupt to a zero-based
SYS_IRQ_INDEX (VIRQ) in entry order. Index zero is valid. Interrupt delivery
to M-mode still depends on the interrupt-controller configuration; this node
does not change hardware privilege-level routing.

The channel advertises the RPMI message protocol and SYSTEM_IRQ service group
`0x000f`, version 2.0, as defined by the
[SYSTEM_IRQ proposal](https://github.com/riscv-non-isa/riscv-rpmi/pull/150).
The implemented services are GET_ATTRIBUTES (`0x02`), GET_TARGET_ATTRIBUTES
(`0x03`), GET_IRQ_ATTRIBUTES (`0x04`), and SYSIRQ_CLAIM (`0x09`). Other service
IDs return `RPMI_ERR_NOTSUPP`; interrupt state and routing are configured by
OpenSBI rather than through SET services.

SYS_TARGET_INDEX enumerates the owning domain's `possible-harts` in increasing
OpenSBI hart-index order. It is not a hart ID. GET_TARGET_ATTRIBUTES reports
the corresponding hart ID. CLAIM is restricted to the calling hart's target;
claiming another target returns `RPMI_ERR_DENIED`.

The S-mode payload must configure an SBI MPXY shared-memory region, discover
the channel and its protocol attributes, and invoke MPXY
SEND_MESSAGE_WITH_RESPONSE with message ID `0x09`. All numeric payload fields
are little-endian:

| Payload | Fields |
|:--------|:-------|
| Request | `u32 SYS_TARGET_INDEX` |
| Successful response | `s32 STATUS`, `u32 RETURNED`, `u32 SYS_IRQ_INDEX[RETURNED]` |
| Error response | `s32 STATUS` |

CLAIM removes pending interrupts for this channel from the calling hart's
domain queue but leaves their physical sources masked. It returns a
batch of up to 63 indices. There is no separate COMPLETE service. The payload
must handle every returned interrupt and clear its device's interrupt source,
then call CLAIM again until RETURNED is zero. The next valid CLAIM for the same
channel and target acknowledges/unmasks the previous batch's physical sources
before collecting new pending interrupts. The optional Yield ECALL can instead
complete handled batches explicitly. This OpenSBI-specific rearm convention
prevents a level-triggered source from being delivered again before the payload
has cleared it; it does not add a COMPLETE service to the RPMI wire protocol.

OpenSBI clears the S-mode notification when the domain queue is empty. A
nonempty CLAIM response does not return to a previously interrupted domain:
the payload must first process the returned batch. A subsequent CLAIM with
RETURNED zero requests the deferred return switch, if one is needed and all
channels in the domain queue are drained and all claimed sources are rearmed.
This empty-CLAIM return convention
is specific to OpenSBI's domain courier, not part of the RPMI wire protocol.

The previous private VIRQ ECALL extension (`0x0900524d`) and its POP/COMPLETE
operations are no longer registered. Existing payloads using those calls must
migrate to the MPXY CLAIM loop described above.

### Experimental Yield ECALL

`CONFIG_SBI_ECALL_YIELD` enables the project-local experimental extension
EID `0x0800594c`, FID `0`. This is not an official SBI assignment. The extension
is registered when local SYSIRQ channels exist and can be discovered with
SBI BASE PROBE_EXTENSION. It takes no arguments and ignores argument registers.
On success, `a0` is zero and `a1` is the number of IRQs completed by the call.

After handling every IRQ in a claimed batch and clearing its device source,
the payload may invoke Yield instead of relying on the next CLAIM to complete
that batch. Yield completes all outstanding batches belonging to the current
domain and calling hart, using the same completion helper as CLAIM. Each
successful completion removes its record, so mixing Yield and CLAIM or
repeating Yield without a new batch cannot duplicate EOI/unmask operations.
Yield does not require an MPXY shared-memory buffer and does not dequeue IRQs.

If pending IRQs remain, Yield returns without switching and the payload must
continue claiming them. If all pending and claimed IRQs are drained and a VIRQ
courier return is pending, Yield requests a switch at the end of the ECALL trap.
The ECALL return state is saved before switching. When the domain is entered
again, execution resumes after Yield. No previous domain means no switch.
On a completion failure, Yield returns an SBI error without requesting a
switch, retaining the failed and remaining completion records for retry.
Already completed IRQs are not rolled back; the completed count in `a1`
reports progress even on error.
