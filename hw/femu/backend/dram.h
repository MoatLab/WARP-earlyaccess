#ifndef __FEMU_MEM_BACKEND
#define __FEMU_MEM_BACKEND

#include <stdint.h>

/* DRAM backend SSD address space */
typedef struct SsdDramBackend {
    void    *logical_space;
    int64_t size; /* in bytes */
    int64_t actual_size ; /* Actual amount of DRAM for cylon in bytes */
    int64_t emulated_target_size ; /* Guest OS capture in bytes */
    int     femu_mode;
} SsdDramBackend;

int init_dram_backend(SsdDramBackend **mbe, int64_t nbytes);
int init_dram_backend_scale(SsdDramBackend **mbe, int64_t nbytes_actual, int64_t nbytes_to_emulate);

void free_dram_backend(SsdDramBackend *);

int backend_rw(SsdDramBackend *, QEMUSGList *, uint64_t *, bool);

#endif
