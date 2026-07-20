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
* **next-arg1** (Optional) - The 64 bit next booting stage arg1 for the
  domain instance. If this DT property is not available and coldboot HART
  is not assigned to the domain instance then **next booting stage arg1 of coldboot HART**
  is used as default value.
* **next-addr** (Optional) - The 64 bit next booting stage address for the
  domain instance. If this DT property is not available and coldboot HART
  is not assigned to the domain instance then **0x0** is used as default
  value. If this DT property is not available and coldboot HART is assigned
  to the domain instance then **next booting stage address of coldboot HART**
  is used as default value.
* **next-mode** (Optional) - The 32 bit next booting stage mode for the
  domain instance. The possible values of this DT property are: **0x1**
  (S-mode), and **0x0** (U-mode). If this DT property is not available
  and coldboot HART is not assigned to the domain instance then **0x1**
  is used as default value. If this DT property is not available and
  coldboot HART is assigned to the domain instance then **next booting
  stage mode of coldboot HART** is used as default value.
* **system-reset-allowed** (Optional) - A boolean flag representing
  whether the domain instance is allowed to do system reset.
* **system-suspend-allowed** (Optional) - A boolean flag representing
  whether the domain instance is allowed to do system suspend.
* **hw-isolation** (Optional) - A child node used by platform-specific
  isolation mechanisms. The current OpenSBI WorldGuard implementation uses
  this node only as a container for per-domain WorldGuard execution metadata.

WorldGuard Device Tree Binding
------------------------------

The current OpenSBI WorldGuard support is built from:

* **sbi_hart_protection** for hart-local runtime reconfiguration during
  domain transitions
* **sbi_domain_data** for per-domain WorldGuard state
* platform-specific setup code that parses and programs WorldGuard checkers
  before domains are populated

The WorldGuard-specific DT state is split across two places:

* under a domain instance node, for per-domain execution metadata
* in the normal system DT topology, for CPU defaults and checker policy

### Domain-local WorldGuard Metadata

The optional **hw-isolation** child node under a domain instance may contain
one WorldGuard child node:

* **compatible = "sifive,wgchecker2"**
* **worldguard,wid** - Machine world ID selected when entering the domain
* **worldguard,widlist** - List of world IDs delegated to the domain

The current implementation looks up the domain node in the FDT, finds its
**hw-isolation** child, and parses the first child node compatible with
**sifive,wgchecker2**.

### System-level WorldGuard Metadata

The current implementation also parses the following system-level DT state:

* **/cpus**
  * **riscv,nworlds** - Total number of supported worlds
  * **sifive,trustedwid** (Optional) - Default trusted machine world ID.
    If omitted, the highest valid world ID is used.
* **/cpus/cpu@X**
  * **riscv,pmwid** - Default machine world ID for the hart
  * **riscv,pmwidlist** - Machine-visible world IDs allowed for the hart
  * **riscv,pmlwidlist** - World IDs that may be delegated by the hart
* **sifive,wgchecker2** nodes
  * **reg** - Checker MMIO base/size
  * **#access-controller-cells** - Must match the expected checker cell count
* protected resource nodes with **access-controllers** properties that point
  to a checker and encode protected address range, permission bitmap, and slot
  configuration data

For the current implementation, checker policy is parsed from
**access-controllers** references on protected resources and applied during
platform-specific WorldGuard setup.

### Runtime Behavior

At boot:

* platform setup detects whether checker programming and/or runtime WID
  switching are present in the DT
* if runtime WID support is present, OpenSBI registers a WorldGuard
  **sbi_hart_protection** mechanism and a **sbi_domain_data** provider
* per-hart default WID state is parsed from **/cpus**
* checker instances are parsed, validated, and programmed before domain
  population completes

During a domain transition:

* the WorldGuard **sbi_domain_data** provider supplies per-domain
  **worldguard,wid** and **worldguard,widlist** state
* the WorldGuard **sbi_hart_protection** implementation reprograms
  **MLWID**
* when **sswg** is present, it also reprograms **MWIDDELEG** and **SLWID**
* when a domain is quiesced, the hart is returned to its per-hart fallback
  machine world state

Runtime WID switching is enabled only when the CPU runtime properties are
present. A DT that only describes checker hardware does not, by itself,
enable hart-local WorldGuard reconfiguration.

WorldGuard Examples
-------------------

Domain instance with WorldGuard execution metadata:

```text
    chosen {
        opensbi-domains {
            compatible = "opensbi,domain,config";

            example_domain: domain@1 {
                compatible = "opensbi,domain,instance";
                possible-harts = <&cpu2>;
                regions = <&mem0 0x3f>;
                boot-hart = <&cpu2>;
                next-addr = <0x00000000 0x80200000>;
                next-mode = <0x1>;

                hw-isolation {
                    worldguard {
                        compatible = "sifive,wgchecker2";
                        worldguard,wid = <0x0 0x1>;
                        worldguard,widlist = <1 3>;
                    };
                };
            };
        };
    };
```

CPU default state, checker, and protected resource example:

```text
    cpus {
        riscv,nworlds = <4>;
        sifive,trustedwid = <3>;

        cpu0: cpu@0 {
            reg = <0>;
            riscv,pmwid = <0x0 0x3>;
            riscv,pmwidlist = <0x0 0xb>;
            riscv,pmlwidlist = <0x0 0xb>;
        };
    };

    memory0: memory@80000000 {
        reg = <0x0 0x80000000 0x0 0x80000000>;
        access-controllers =
            <0x100
             0x00000000 0x80000000 0x00000000 0x40000000
             0x00000000 0x000000cf 0x0f>,
            <0x100
             0x00000000 0xc0000000 0x00000000 0x01000000
             0x00000000 0x000000cc 0x0f>,
            <0x100
             0x00000000 0xc1000000 0x00000000 0x3f000000
             0x00000000 0x000000cf 0x0f>;
    };

    flash0: flash@20000000 {
        reg = <0x0 0x20000000 0x0 0x04000000>;
        access-controllers =
            <0x101
             0x00000000 0x20000000 0x00000000 0x04000000
             0x00000000 0x000000c3 0x0f>;
    };

    uart0: serial@10000000 {
        reg = <0x0 0x10000000 0x0 0x00001000>;
        access-controllers =
            <0x102
             0x00000000 0x10000000 0x00000000 0x00001000
             0x00000000 0x000000c0 0x0f>;
    };

    wgchecker0: wgchecker@6000000 {
        compatible = "sifive,wgchecker2";
        reg = <0x0 0x06000000 0x0 0x1000>;
        #access-controller-cells = <7>;
        phandle = <0x100>;
    };

    wgchecker1: wgchecker@6001000 {
        compatible = "sifive,wgchecker2";
        reg = <0x0 0x06001000 0x0 0x1000>;
        #access-controller-cells = <7>;
        phandle = <0x101>;
    };

    wgchecker2: wgchecker@6002000 {
        compatible = "sifive,wgchecker2";
        reg = <0x0 0x06002000 0x0 0x1000>;
        #access-controller-cells = <7>;
        phandle = <0x102>;
    };
```

The test overlay used in this tree is at:

* **platform/generic/virt/qemu-virt-wg-overlay.dts**

That overlay supplies the current QEMU virt test metadata for OpenSBI
domains and protected resources. The base DTB must still provide the checker
nodes and the CPU-level WorldGuard properties consumed by the runtime code.

### Assigning HART To Domain Instance

By default, all HARTs are assigned to **the ROOT domain**. The OpenSBI
platform support can provide the HART to domain instance assignment using
platform specific callback.

The HART to domain instance assignment can be parsed from the device tree
using optional DT property **opensbi-domain** in each CPU DT node. The
value of DT property **opensbi-domain** is the DT phandle of the domain
instance DT node. If **opensbi-domain** DT property is not specified then
corresponding HART is assigned to **the ROOT domain**.

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
            opensbi-domain = <&tdomain>;
            ...
        };

        cpu1: cpu@1 {
            device_type = "cpu";
            reg = <0x01>;
            compatible = "riscv";
            opensbi-domain = <&udomain>;
            ...
        };

        cpu2: cpu@2 {
            device_type = "cpu";
            reg = <0x02>;
            compatible = "riscv";
            opensbi-domain = <&udomain>;
            ...
        };

        cpu3: cpu@3 {
            device_type = "cpu";
            reg = <0x03>;
            compatible = "riscv";
            opensbi-domain = <&udomain>;
            ...
        };

        cpu4: cpu@4 {
            device_type = "cpu";
            reg = <0x04>;
            compatible = "riscv";
            opensbi-domain = <&udomain>;
            ...
        };
    };

    uart1: serial@10011000 {
        ...
    };
```
