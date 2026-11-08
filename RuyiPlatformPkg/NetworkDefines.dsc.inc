## SPDX-License-Identifier: BSD-2-Clause-Patent
[Defines]
  # Drivers are always present. Setup controls their runtime support PCDs.
  DEFINE NETWORK_PXE_BOOT_ENABLE = TRUE
  DEFINE NETWORK_HTTP_BOOT_ENABLE = TRUE
  DEFINE NETWORK_ENABLE = TRUE
  # Platform Ethernet drivers provide SNP directly.
  DEFINE NETWORK_SNP_ENABLE = FALSE
  DEFINE NETWORK_IP4_ENABLE = TRUE
  DEFINE NETWORK_IP6_ENABLE = TRUE
  DEFINE NETWORK_ALLOW_HTTP_CONNECTIONS = TRUE
  DEFINE NETWORK_ISCSI_ENABLE = FALSE
  # The timer RNG provides network IDs, not cryptographic entropy.
  DEFINE NETWORK_TLS_ENABLE = FALSE
