#include "./nvme.h"

/**
void nvme_addr_read(FemuCtrl *n, hwaddr addr, void *buf, int size)
{
    if (n->cmbsz && addr >= n->ctrl_mem.addr &&
        addr < (n->ctrl_mem.addr + int128_get64(n->ctrl_mem.size)))
    {
        memcpy(buf, (void *)&n->cmbuf[addr - n->ctrl_mem.addr], size);
    }
    else
    {
        pci_dma_read(&n->parent_obj, addr, buf, size);
    }
}*/

static bool nvme_addr_is_cmb(FemuCtrl *n, hwaddr addr)
{
    hwaddr hi, lo;

    if (!n->cmb.cmse) {
        return false;
    }

    lo = n->params.legacy_cmb ? n->cmb.mem.addr : n->cmb.cba;
    hi = lo + int128_get64(n->cmb.mem.size);

    return addr >= lo && addr < hi;
}

static inline void *nvme_addr_to_cmb(FemuCtrl *n, hwaddr addr)
{
    hwaddr base = n->params.legacy_cmb ? n->cmb.mem.addr : n->cmb.cba;
    return &n->cmb.buf[addr - base];
}

static bool nvme_addr_is_pmr(FemuCtrl *n, hwaddr addr)
{
    hwaddr hi;

    if (!n->pmr.cmse) {
        return false;
    }

    hi = n->pmr.cba + int128_get64(n->pmr.dev->mr.size);

    return addr >= n->pmr.cba && addr < hi;
}
static inline void *nvme_addr_to_pmr(FemuCtrl *n, hwaddr addr)
{
    return memory_region_get_ram_ptr(&n->pmr.dev->mr) + (addr - n->pmr.cba);
}
 int nvme_addr_read(FemuCtrl *n, hwaddr addr, void *buf, int size)
{
    hwaddr hi = addr + size - 1;
    if (hi < addr) {
        return 1;
    }

    if (n->bar.cmbsz && nvme_addr_is_cmb(n, addr) && nvme_addr_is_cmb(n, hi)) {
        memcpy(buf, nvme_addr_to_cmb(n, addr), size);
        return 0;
    }

    if (nvme_addr_is_pmr(n, addr) && nvme_addr_is_pmr(n, hi)) {
        memcpy(buf, nvme_addr_to_pmr(n, addr), size);
        return 0;
    }

    return pci_dma_read(PCI_DEVICE(n), addr, buf, size);
}
/*
void nvme_addr_write(FemuCtrl *n, hwaddr addr, void *buf, int size)
{
    if (n->cmbsz && addr >= n->ctrl_mem.addr &&
        addr < (n->ctrl_mem.addr + int128_get64(n->ctrl_mem.size)))
    {
        memcpy((void *)&n->cmbuf[addr - n->ctrl_mem.addr], buf, size);
    }
    else
    {
        pci_dma_write(&n->parent_obj, addr, buf, size);
    }
}
*/

 int nvme_addr_write(FemuCtrl *n, hwaddr addr, const void *buf, int size)
{
    hwaddr hi = addr + size - 1;
    if (hi < addr) {
        return 1;
    }

    if (n->bar.cmbsz && nvme_addr_is_cmb(n, addr) && nvme_addr_is_cmb(n, hi)) {
        memcpy(nvme_addr_to_cmb(n, addr), buf, size);
        return 0;
    }

    if (nvme_addr_is_pmr(n, addr) && nvme_addr_is_pmr(n, hi)) {
        memcpy(nvme_addr_to_pmr(n, addr), buf, size);
        return 0;
    }

    return pci_dma_write(PCI_DEVICE(n), addr, buf, size);
}

static uint16_t nvme_map_addr_cmb(FemuCtrl *n, QEMUIOVector *iov, hwaddr addr,
                                  size_t len)
{
    if (!len)
    {
        return NVME_SUCCESS;
    }

    // trace_pci_nvme_map_addr_cmb(addr, len);

    if (!nvme_addr_is_cmb(n, addr) || !nvme_addr_is_cmb(n, addr + len - 1))
    {
        return NVME_DATA_TRAS_ERROR;
    }

    qemu_iovec_add(iov, (void * )nvme_addr_to_cmb(n, addr), len);

    return NVME_SUCCESS;
}

static inline bool nvme_addr_is_iomem(FemuCtrl *n, hwaddr addr)
{
    hwaddr hi, lo;

    /*
     * The purpose of this check is to guard against invalid "local" access to
     * the iomem (i.e. controller registers). Thus, we check against the range
     * covered by the 'bar0' MemoryRegion since that is currently composed of
     * two subregions (the NVMe "MBAR" and the MSI-X table/pba). Note, however,
     * that if the device model is ever changed to allow the CMB to be located
     * in BAR0 as well, then this must be changed.
     */
    lo = n->bar0.addr;
    hi = lo + int128_get64(n->bar0.size);

    return addr >= lo && addr < hi;
}


static uint16_t nvme_map_addr_pmr(FemuCtrl *n, QEMUIOVector *iov, hwaddr addr,
                                  size_t len)
{
    if (!len)
    {
        return NVME_SUCCESS;
    }

    if (!nvme_addr_is_pmr(n, addr) || !nvme_addr_is_pmr(n, addr + len - 1))
    {
        return NVME_DATA_TRAS_ERROR;
    }

    qemu_iovec_add(iov, nvme_addr_to_pmr(n, addr), len);

    return NVME_SUCCESS;
}

uint16_t nvme_tx(FemuCtrl *n, NvmeSg *sg, void *ptr, uint32_t len,
                        NvmeTxDirection dir)
{
    assert(sg->flags & NVME_SG_ALLOC);

    if (sg->flags & NVME_SG_DMA) {
        const MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
        dma_addr_t residual;

        if (dir == NVME_TX_DIRECTION_TO_DEVICE) {
            dma_buf_write(ptr, len, &residual, &sg->qsg, attrs);
        } else {
            dma_buf_read(ptr, len, &residual, &sg->qsg, attrs);
        }

        if (unlikely(residual)) {
            //trace_pci_nvme_err_invalid_dma();
            return NVME_INVALID_FIELD | NVME_DNR;
        }
    } else {
        size_t bytes;

        if (dir == NVME_TX_DIRECTION_TO_DEVICE) {
            bytes = qemu_iovec_to_buf(&sg->iov, 0, ptr, len);
        } else {
            bytes = qemu_iovec_from_buf(&sg->iov, 0, ptr, len);
        }

        if (unlikely(bytes != len)) {
            //trace_pci_nvme_err_invalid_dma();
            femu_err("pci nvme err invalid dma\n");
            return NVME_INVALID_FIELD | NVME_DNR;
        }
    }

    return NVME_SUCCESS;
}

uint16_t nvme_c2h(FemuCtrl *n, void *ptr, uint32_t len,
                                NvmeRequest *req)
{
    uint16_t status;

    status = femu_map_dptr(n, &req->sg, len, &req->cmd);
    if (status) {
        return status;
    }

    return nvme_tx(n, &req->sg, ptr, len, NVME_TX_DIRECTION_FROM_DEVICE);
}

uint16_t nvme_h2c(FemuCtrl *n, void *ptr, uint32_t len,
                                NvmeRequest *req)
{
    uint16_t status;

    status = femu_map_dptr(n, &req->sg, len, &req->cmd);
    if (status) {
        return status;
    }

    return nvme_tx(n, &req->sg, ptr, len, NVME_TX_DIRECTION_TO_DEVICE);
}

static inline void nvme_sg_init(FemuCtrl *n, NvmeSg *sg, bool dma)
{
    if (dma) {
        pci_dma_sglist_init(&sg->qsg, PCI_DEVICE(n), 0);
        sg->flags = NVME_SG_DMA;
    } else {
        qemu_iovec_init(&sg->iov, 0);
    }

    sg->flags |= NVME_SG_ALLOC;
}

static inline void nvme_sg_unmap(NvmeSg *sg)
{
    if (!(sg->flags & NVME_SG_ALLOC)) {
        return;
    }

    if (sg->flags & NVME_SG_DMA) {
        qemu_sglist_destroy(&sg->qsg);
    } else {
        qemu_iovec_destroy(&sg->iov);
    }

    memset(sg, 0x0, sizeof(*sg));
}

static uint16_t nvme_map_addr(FemuCtrl *n, NvmeSg *sg, hwaddr addr, size_t len)
{
    bool cmb = false, pmr = false;

    if (!len)
    {
        return NVME_SUCCESS;
    }

    //trace_pci_nvme_map_addr(addr, len);

    if (nvme_addr_is_iomem(n, addr))
    {
        return NVME_DATA_TRAS_ERROR;
    }

    if (nvme_addr_is_cmb(n, addr))
    {
        cmb = true;
    }
    else if (nvme_addr_is_pmr(n, addr))
    {
        pmr = true;
    }

    if (cmb || pmr)
    {
        if (sg->flags & NVME_SG_DMA)
        {
            return NVME_INVALID_USE_OF_CMB | NVME_DNR;
        }

        if (sg->iov.niov + 1 > IOV_MAX)
        {
            goto max_mappings_exceeded;
        }

        if (cmb)
        {
            return nvme_map_addr_cmb(n, &sg->iov, addr, len);
        }
        else
        {
            return nvme_map_addr_pmr(n, &sg->iov, addr, len);
        }
    }

    if (!(sg->flags & NVME_SG_DMA))
    {
        return NVME_INVALID_USE_OF_CMB | NVME_DNR;
    }

    if (sg->qsg.nsg + 1 > IOV_MAX)
    {
        goto max_mappings_exceeded;
    }

    qemu_sglist_add(&sg->qsg, addr, len);

    return NVME_SUCCESS;

max_mappings_exceeded:
    //NVME_GUEST_ERR(pci_nvme_ub_too_many_mappings,
    //               "number of mappings exceed 1024");
    femu_err("number of mappings exceed 1024");
    return NVME_INTERNAL_DEV_ERROR | NVME_DNR;
}

static inline bool nvme_addr_is_dma(FemuCtrl *n, hwaddr addr)
{
    return !(nvme_addr_is_cmb(n, addr) || nvme_addr_is_pmr(n, addr));
}

uint16_t nvme_map_prp(FemuCtrl *n, NvmeSg *sg, uint64_t prp1,
                      uint64_t prp2, uint32_t len)
{
    hwaddr trans_len = n->page_size - (prp1 % n->page_size);
    trans_len = MIN(len, trans_len);
    int num_prps = (len >> n->page_bits) + 1;
    uint16_t status;
    int ret;

    //trace_pci_nvme_map_prp(trans_len, len, prp1, prp2, num_prps);
    femu_log("nvme_map_prp %d\n", num_prps);
    nvme_sg_init(n, sg, nvme_addr_is_dma(n, prp1));

    status = nvme_map_addr(n, sg, prp1, trans_len);
    if (status)
    {
        goto unmap;
    }

    len -= trans_len;
    if (len)
    {
        if (len > n->page_size)
        {
            uint64_t prp_list[n->max_prp_ents];
            uint32_t nents, prp_trans;
            int i = 0;

            /*
             * The first PRP list entry, pointed to by PRP2 may contain offset.
             * Hence, we need to calculate the number of entries in based on
             * that offset.
             */
            nents = (n->page_size - (prp2 & (n->page_size - 1))) >> 3;
            prp_trans = MIN(n->max_prp_ents, nents) * sizeof(uint64_t);
            ret = nvme_addr_read(n, prp2, (void *)prp_list, prp_trans);
            if (ret)
            {
                //trace_pci_nvme_err_addr_read(prp2);
                status = NVME_DATA_TRAS_ERROR;
                goto unmap;
            }
            while (len != 0)
            {
                uint64_t prp_ent = le64_to_cpu(prp_list[i]);

                if (i == nents - 1 && len > n->page_size)
                {
                    if (unlikely(prp_ent & (n->page_size - 1)))
                    {
                        //trace_pci_nvme_err_invalid_prplist_ent(prp_ent);
                        status = NVME_INVALID_PRP_OFFSET | NVME_DNR;
                        goto unmap;
                    }

                    i = 0;
                    nents = (len + n->page_size - 1) >> n->page_bits;
                    nents = MIN(nents, n->max_prp_ents);
                    prp_trans = nents * sizeof(uint64_t);
                    ret = nvme_addr_read(n, prp_ent, (void *)prp_list,
                                         prp_trans);
                    if (ret)
                    {
                        //trace_pci_nvme_err_addr_read(prp_ent);
                        status = NVME_DATA_TRAS_ERROR;
                        goto unmap;
                    }
                    prp_ent = le64_to_cpu(prp_list[i]);
                }

                if (unlikely(prp_ent & (n->page_size - 1)))
                {
                    //trace_pci_nvme_err_invalid_prplist_ent(prp_ent);
                    status = NVME_INVALID_PRP_OFFSET | NVME_DNR;
                    goto unmap;
                }

                trans_len = MIN(len, n->page_size);
                status = nvme_map_addr(n, sg, prp_ent, trans_len);
                if (status)
                {
                    goto unmap;
                }

                len -= trans_len;
                i++;
            }
        }
        else
        {
            if (unlikely(prp2 & (n->page_size - 1)))
            {
                //trace_pci_nvme_err_invalid_prp2_align(prp2);
                status = NVME_INVALID_PRP_OFFSET | NVME_DNR;
                goto unmap;
            }
            status = nvme_map_addr(n, sg, prp2, len);
            if (status)
            {
                goto unmap;
            }
        }
    }

    return NVME_SUCCESS;

unmap:
    nvme_sg_unmap(sg);
    return status;
}

// uint16_t nvme_map_prp(QEMUSGList *qsg, QEMUIOVector *iov, uint64_t prp1,
//                       uint64_t prp2, uint32_t len, FemuCtrl *n)
// {
//     hwaddr trans_len = n->page_size - (prp1 % n->page_size);
//     trans_len = MIN(len, trans_len);
//     int num_prps = (len >> n->page_bits) + 1;
//     bool cmb = false;

//     if (!prp1) {
//         return NVME_INVALID_FIELD | NVME_DNR;
//     } else if (n->cmbsz && prp1 >= n->ctrl_mem.addr &&
//                prp1 < n->ctrl_mem.addr + int128_get64(n->ctrl_mem.size)) {
//         cmb = true;
//         qsg->nsg = 0;
//         qemu_iovec_init(iov, num_prps);
//         qemu_iovec_add(iov, (void *)&n->cmbuf[prp1-n->ctrl_mem.addr], trans_len);
//     } else {
//         pci_dma_sglist_init(qsg, &n->parent_obj, num_prps);
//         qemu_sglist_add(qsg, prp1, trans_len);
//     }

//     len -= trans_len;
//     if (len) {
//         if (!prp2) {
//             goto unmap;
//         }
//         if (len > n->page_size) {
//             uint64_t prp_list[n->max_prp_ents];
//             uint32_t nents, prp_trans;

//             int i = 0;
//             nents = (len + n->page_size - 1) >> n->page_bits;
//             prp_trans = MIN(n->max_prp_ents, nents) * sizeof(uint64_t);
//             nvme_addr_read(n, prp2, (void *)prp_list, prp_trans);
//             while (len != 0) {
//                 uint64_t prp_ent = le64_to_cpu(prp_list[i]);

//                 if (i == n->max_prp_ents - 1 && len > n->page_size) {
//                     if (!prp_ent || prp_ent & (n->page_size - 1)) {
//                         goto unmap;
//                     }

//                     i = 0;
//                     nents = (len + n->page_size - 1) >> n->page_bits;
//                     prp_trans = MIN(n->max_prp_ents, nents) * sizeof(uint64_t);
//                     nvme_addr_read(n, prp_ent, (void *)prp_list,
//                                    prp_trans);
//                     prp_ent = le64_to_cpu(prp_list[i]);
//                 }

//                 if (!prp_ent || prp_ent & (n->page_size - 1)) {
//                     goto unmap;
//                 }

//                 trans_len = MIN(len, n->page_size);
//                 if (!cmb){
//                     qemu_sglist_add(qsg, prp_ent, trans_len);
//                 } else {
//                     uint64_t off = prp_ent - n->ctrl_mem.addr;
//                     qemu_iovec_add(iov, (void *)&n->cmbuf[off], trans_len);
//                 }
//                 len -= trans_len;
//                 i++;
//             }
//         } else {
//             if (prp2 & (n->page_size - 1)) {
//                 goto unmap;
//             }
//             if (!cmb) {
//                 qemu_sglist_add(qsg, prp2, len);
//             } else {
//                 uint64_t off = prp2 - n->ctrl_mem.addr;
//                 qemu_iovec_add(iov, (void *)&n->cmbuf[off], trans_len);
//             }
//         }
//     }

//     return NVME_SUCCESS;

// unmap:
//     if (!cmb) {
//         qemu_sglist_destroy(qsg);
//     } else {
//         qemu_iovec_destroy(iov);
//     }

//     return NVME_INVALID_FIELD | NVME_DNR;
// }

uint16_t dma_write_prp(FemuCtrl *n, uint8_t *ptr, uint32_t len, uint64_t prp1,
                       uint64_t prp2)
{
    //QEMUSGList qsg;
    //QEMUIOVector iov;
    NvmeSg  sg;
    uint16_t status = NVME_SUCCESS;

    //if (nvme_map_prp(&qsg, &iov, prp1, prp2, len, n))
    if(nvme_map_prp(n, &sg, prp1,prp2, len))
    {
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (sg.qsg.nsg > 0)
    {
        if (dma_buf_write(ptr, len, NULL, &sg.qsg, MEMTXATTRS_UNSPECIFIED))
        {
            status = NVME_INVALID_FIELD | NVME_DNR;
        }
        qemu_sglist_destroy(&sg.qsg);
    }
    else
    {
        if (qemu_iovec_from_buf(&sg.iov, 0, ptr, len) != len)
        {
            status = NVME_INVALID_FIELD | NVME_DNR;
        }
        qemu_iovec_destroy(&sg.iov);
    }

    return status;
}

uint16_t dma_read_prp(FemuCtrl *n, uint8_t *ptr, uint32_t len, uint64_t prp1,
                      uint64_t prp2)
{
    NvmeSg  sg;
    uint16_t status = NVME_SUCCESS;

    //if (nvme_map_prp(&qsg, &iov, prp1, prp2, len, n))
    if(nvme_map_prp(n, &sg, prp1,prp2, len))
    {
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (sg.qsg.nsg > 0)
    {
        if (dma_buf_read(ptr, len, NULL, &sg.qsg, MEMTXATTRS_UNSPECIFIED))
        {
            status = NVME_INVALID_FIELD | NVME_DNR;
        }
        qemu_sglist_destroy(&sg.qsg);
    }
    else
    {
        if (qemu_iovec_to_buf(&sg.iov, 0, ptr, len) != len)
        {
            status = NVME_INVALID_FIELD | NVME_DNR;
        }
        qemu_iovec_destroy(&sg.iov);
    }

    return status;
}

/*
 * Map 'nsgld' data descriptors from 'segment'. The function will subtract the
 * number of bytes mapped in len.
 */
static uint16_t nvme_map_sgl_data(FemuCtrl *n, NvmeSg *sg,
                                  NvmeSglDescriptor *segment, uint64_t nsgld,
                                  size_t *len, NvmeCmd *cmd)
{
    dma_addr_t addr, trans_len;
    uint32_t dlen;
    uint16_t status;

    for (int i = 0; i < nsgld; i++)
    {
        uint8_t type = NVME_SGL_TYPE(segment[i].type);

        switch (type)
        {
        case NVME_SGL_DESCR_TYPE_DATA_BLOCK:
            break;
        case NVME_SGL_DESCR_TYPE_SEGMENT:
        case NVME_SGL_DESCR_TYPE_LAST_SEGMENT:
            return NVME_INVALID_NUM_SGL_DESCRS | NVME_DNR;
        default:
            return NVME_SGL_DESCR_TYPE_INVALID | NVME_DNR;
        }

        dlen = le32_to_cpu(segment[i].len);

        if (!dlen)
        {
            continue;
        }

        if (*len == 0)
        {
            /*
             * All data has been mapped, but the SGL contains additional
             * segments and/or descriptors. The controller might accept
             * ignoring the rest of the SGL.
             */
            uint32_t sgls = le32_to_cpu(n->id_ctrl.sgls);
            if (sgls & NVME_CTRL_SGLS_EXCESS_LENGTH)
            {
                break;
            }

            // trace_pci_nvme_err_invalid_sgl_excess_length(dlen);
            return NVME_DATA_SGL_LEN_INVALID | NVME_DNR;
        }

        trans_len = MIN(*len, dlen);

        addr = le64_to_cpu(segment[i].addr);

        if (UINT64_MAX - addr < dlen)
        {
            return NVME_DATA_SGL_LEN_INVALID | NVME_DNR;
        }

        status = nvme_map_addr(n, sg, addr, trans_len);
        if (status)
        {
            return status;
        }

        *len -= trans_len;
    }

    return NVME_SUCCESS;
}

uint16_t nvme_map_sgl(FemuCtrl *n, NvmeSg *sg, NvmeSglDescriptor sgl,
                      size_t len, NvmeCmd *cmd)
{
    /*
     * Read the segment in chunks of 256 descriptors (one 4k page) to avoid
     * dynamically allocating a potentially huge SGL. The spec allows the SGL
     * to be larger (as in number of bytes required to describe the SGL
     * descriptors and segment chain) than the command transfer size, so it is
     * not bounded by MDTS.
     */
    const int SEG_CHUNK_SIZE = 256;

    NvmeSglDescriptor segment[SEG_CHUNK_SIZE], *sgld, *last_sgld;
    uint64_t nsgld;
    uint32_t seg_len;
    uint16_t status;
    hwaddr addr;
    int ret;

    sgld = &sgl;
    addr = le64_to_cpu(sgl.addr);

    // trace_pci_nvme_map_sgl(NVME_SGL_TYPE(sgl.type), len);

    nvme_sg_init(n, sg, nvme_addr_is_dma(n, addr));

    /*
     * If the entire transfer can be described with a single data block it can
     * be mapped directly.
     */
    if (NVME_SGL_TYPE(sgl.type) == NVME_SGL_DESCR_TYPE_DATA_BLOCK)
    {
        status = nvme_map_sgl_data(n, sg, sgld, 1, &len, cmd);
        if (status)
        {
            goto unmap;
        }

        goto out;
    }

    for (;;)
    {
        switch (NVME_SGL_TYPE(sgld->type))
        {
        case NVME_SGL_DESCR_TYPE_SEGMENT:
        case NVME_SGL_DESCR_TYPE_LAST_SEGMENT:
            break;
        default:
            return NVME_INVALID_SGL_SEG_DESCR | NVME_DNR;
        }

        seg_len = le32_to_cpu(sgld->len);

        /* check the length of the (Last) Segment descriptor */
        if (!seg_len || seg_len & 0xf)
        {
            return NVME_INVALID_SGL_SEG_DESCR | NVME_DNR;
        }

        if (UINT64_MAX - addr < seg_len)
        {
            return NVME_DATA_SGL_LEN_INVALID | NVME_DNR;
        }

        nsgld = seg_len / sizeof(NvmeSglDescriptor);

        while (nsgld > SEG_CHUNK_SIZE)
        {
            if (nvme_addr_read(n, addr, segment, sizeof(segment)))
            {
                //trace_pci_nvme_err_addr_read(addr);
                status = NVME_DATA_TRAS_ERROR;
                goto unmap;
            }

            status = nvme_map_sgl_data(n, sg, segment, SEG_CHUNK_SIZE,
                                       &len, cmd);
            if (status)
            {
                goto unmap;
            }

            nsgld -= SEG_CHUNK_SIZE;
            addr += SEG_CHUNK_SIZE * sizeof(NvmeSglDescriptor);
        }

        ret = nvme_addr_read(n, addr, segment, nsgld * sizeof(NvmeSglDescriptor));
        if (ret)
        {
            //trace_pci_nvme_err_addr_read(addr);
            status = NVME_DATA_TRAS_ERROR;
            goto unmap;
        }

        last_sgld = &segment[nsgld - 1];

        /*
         * If the segment ends with a Data Block, then we are done.
         */
        if (NVME_SGL_TYPE(last_sgld->type) == NVME_SGL_DESCR_TYPE_DATA_BLOCK)
        {
            status = nvme_map_sgl_data(n, sg, segment, nsgld, &len, cmd);
            if (status)
            {
                goto unmap;
            }

            goto out;
        }

        /*
         * If the last descriptor was not a Data Block, then the current
         * segment must not be a Last Segment.
         */
        if (NVME_SGL_TYPE(sgld->type) == NVME_SGL_DESCR_TYPE_LAST_SEGMENT)
        {
            status = NVME_INVALID_SGL_SEG_DESCR | NVME_DNR;
            goto unmap;
        }

        sgld = last_sgld;
        addr = le64_to_cpu(sgld->addr);

        /*
         * Do not map the last descriptor; it will be a Segment or Last Segment
         * descriptor and is handled by the next iteration.
         */
        status = nvme_map_sgl_data(n, sg, segment, nsgld - 1, &len, cmd);
        if (status)
        {
            goto unmap;
        }
    }

out:
    /* if there is any residual left in len, the SGL was too short */
    if (len)
    {
        status = NVME_DATA_SGL_LEN_INVALID | NVME_DNR;
        goto unmap;
    }

    return NVME_SUCCESS;

unmap:
    nvme_sg_unmap(sg);
    return status;
}
