# edk2-ruyi

EDK2 platform support for [ruyifirmware](https://github.com/openRuyi-Project/ruyifirmware).

Currently, only SpacemiT K3 Pico ITX is supported.

UEFI Setup → CPU Configuration → CPU mode selects one of three profiles:

- X100 only: eight X100 cores with the original CPU features (default).
- X100 + A100: sixteen cores without vector or hypervisor extensions.
- A100 only: eight A100 cores with vectors and without hypervisor support.

Save and restart to apply. A100-only mode transfers the UEFI boot hart to
A100 before handing its CPU description to Linux. Firmware updates restore
X100 only; UEFI variables are preserved. CPU selection is stored separately
and is persistent even when changed from RAM recovery.

Mixed mode needs userspace that can run without V, such as RV64GC. An
RVA23U64 root filesystem is unsuitable for that mode.

The OS handoff DT identifies the selected profile to userspace: `k3` for
X100 only, `k3-x100-a100` for mixed mode, and `k3-a100` for A100 only.
Firmware generates these descriptions without Linux source or binding changes.
