#pragma once
#include "riscv.h"

/* What is on the PCI bus. Discovery only: nothing here drives a device. */
#define PCI_MAX 16

int    pci_init(void);                          /* -> how many were found */
int    pci_count(void);
uint64 pci_ecam(void);
int    pci_name(int i, char *out, int cap);     /* "00:01.0" */
int    pci_summary(int i, char *out, int cap);  /* one line */
int    pci_render(int i, char *out, int cap);   /* the details, as text */
