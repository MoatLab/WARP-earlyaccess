#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "./nvme.h"

#define NVME_SPEC_VER (0x00010400)
#define NVME_DEFAULT_RU_SIZE (96 * MiB)


static void nvme_clear_ctrl(FemuCtrl *n, bool shutdown)
{
    int i;

    /* Coperd: pause nvme poller at earliest convenience */
    n->dataplane_started = false;

    if (shutdown) {
        femu_debug("shutting down NVMe Controller ...\n");
    } else {
        femu_debug("disabling NVMe Controller ...\n");
    }

    if (shutdown) {
        femu_debug("%s,clear_guest_notifier\n", __func__);
        nvme_clear_virq(n);
    }

    for (i = 0; i <= n->nr_io_queues; i++) {
        if (n->sq[i] != NULL) {
            nvme_free_sq(n->sq[i], n);
        }
    }
    for (i = 0; i <= n->nr_io_queues; i++) {
        if (n->cq[i] != NULL) {
            nvme_free_cq(n->cq[i], n);
        }
    }

    n->bar.cc = 0;
    n->features.temp_thresh = 0x14d;
    n->temp_warn_issued = 0;
    n->dbs_addr = 0;
    n->dbs_addr_hva = 0;
    n->eis_addr = 0;
    n->eis_addr_hva = 0;
}

static int nvme_start_ctrl(FemuCtrl *n)
{
    uint32_t page_bits = NVME_CC_MPS(n->bar.cc) + 12;
    uint32_t page_size = 1 << page_bits;

    if (n->cq[0] || n->sq[0] || !n->bar.asq || !n->bar.acq ||
        n->bar.asq & (page_size - 1) || n->bar.acq & (page_size - 1) ||
        NVME_CC_MPS(n->bar.cc) < NVME_CAP_MPSMIN(n->bar.cap) ||
        NVME_CC_MPS(n->bar.cc) > NVME_CAP_MPSMAX(n->bar.cap) ||
        NVME_CC_IOCQES(n->bar.cc) < NVME_CTRL_CQES_MIN(n->id_ctrl.cqes) ||
        NVME_CC_IOCQES(n->bar.cc) > NVME_CTRL_CQES_MAX(n->id_ctrl.cqes) ||
        NVME_CC_IOSQES(n->bar.cc) < NVME_CTRL_SQES_MIN(n->id_ctrl.sqes) ||
        NVME_CC_IOSQES(n->bar.cc) > NVME_CTRL_SQES_MAX(n->id_ctrl.sqes) ||
        !NVME_AQA_ASQS(n->bar.aqa) || NVME_AQA_ASQS(n->bar.aqa) > 4095 ||
        !NVME_AQA_ACQS(n->bar.aqa) || NVME_AQA_ACQS(n->bar.aqa) > 4095) {
        return -1;
    }

    n->page_bits = page_bits;
    n->page_size = 1 << n->page_bits;
    n->max_prp_ents = n->page_size / sizeof(uint64_t);
    n->cqe_size = 1 << NVME_CC_IOCQES(n->bar.cc);
    n->sqe_size = 1 << NVME_CC_IOSQES(n->bar.cc);

    nvme_init_cq(&n->admin_cq, n, n->bar.acq, 0, 0, NVME_AQA_ACQS(n->bar.aqa) +
                 1, 1, 1);
    nvme_init_sq(&n->admin_sq, n, n->bar.asq, 0, 0, NVME_AQA_ASQS(n->bar.aqa) +
                 1, NVME_Q_PRIO_HIGH, 1);

    /* Currently only used by FEMU ZNS extension */
    if (n->ext_ops.start_ctrl) {
        n->ext_ops.start_ctrl(n);
    }

    return 0;
}

static void nvme_write_bar(FemuCtrl *n, hwaddr offset, uint64_t data, unsigned size)
{
    switch (offset) {
    case 0xc:
        n->bar.intms |= data & 0xffffffff;
        n->bar.intmc = n->bar.intms;
        break;
    case 0x10:
        n->bar.intms &= ~(data & 0xffffffff);
        n->bar.intmc = n->bar.intms;
        break;
    case 0x14:
        /* If first sending data, then sending enable bit */
        if (!NVME_CC_EN(data) && !NVME_CC_EN(n->bar.cc) &&
                !NVME_CC_SHN(data) && !NVME_CC_SHN(n->bar.cc))
        {
            n->bar.cc = data;
        }

        if (NVME_CC_EN(data) && !NVME_CC_EN(n->bar.cc)) {
            n->bar.cc = data;
            if (nvme_start_ctrl(n)) {
                n->bar.csts = NVME_CSTS_FAILED;
            } else {
                n->bar.csts = NVME_CSTS_READY;
            }
        } else if (!NVME_CC_EN(data) && NVME_CC_EN(n->bar.cc)) {
            nvme_clear_ctrl(n, false);
            n->bar.csts &= ~NVME_CSTS_READY;
        }
        if (NVME_CC_SHN(data) && !(NVME_CC_SHN(n->bar.cc))) {
            nvme_clear_ctrl(n, true);
            n->bar.cc = data;
            n->bar.csts |= NVME_CSTS_SHST_COMPLETE;
        } else if (!NVME_CC_SHN(data) && NVME_CC_SHN(n->bar.cc)) {
            n->bar.csts &= ~NVME_CSTS_SHST_COMPLETE;
            n->bar.cc = data;
        }
        break;
    case 0x24:
        n->bar.aqa = data & 0xffffffff;
        break;
    case 0x28:
        n->bar.asq = data;
        break;
    case 0x2c:
        n->bar.asq |= data << 32;
        break;
    case 0x30:
        n->bar.acq = data;
        break;
    case 0x34:
        n->bar.acq |= data << 32;
        break;
    default:
        break;
    }
}

static uint64_t nvme_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    FemuCtrl *n = (FemuCtrl *)opaque;
    uint8_t *ptr = (uint8_t *)&n->bar;
    uint64_t val = 0;

    if (addr < sizeof(n->bar)) {
        memcpy(&val, ptr + addr, size);
    }

    return val;
}

static void nvme_process_db_admin(FemuCtrl *n, hwaddr addr, int val)
{
    uint32_t qid;
    uint16_t new_val = val & 0xffff;
    NvmeSQueue *sq;

    if (((addr - 0x1000) >> (2 + n->db_stride)) & 1) {
        NvmeCQueue *cq;

        qid = ((addr - (0x1000 + (1 << (2 + n->db_stride)))) >> (3 +
                                                                 n->db_stride));
        if (nvme_check_cqid(n, qid)) {
            return;
        }

        cq = n->cq[qid];
        //femu_debug("    femu-nvme nvme_process_db DONE cq = n->cq[qid]; \n");
        if (new_val >= cq->size) {
            return;
        }

        cq->head = new_val;

        if (cq->tail != cq->head) {
            nvme_isr_notify_admin(cq);
        }
    } else {
        qid = (addr - 0x1000) >> (3 + n->db_stride);
        if (nvme_check_sqid(n, qid)) {
            return;
        }
        sq = n->sq[qid];
        if (new_val >= sq->size) {
            return;
        }

        sq->tail = new_val;
        //femu_debug("\t nvme_process_sq_admin (nvme_admin_cmd)\n");
        nvme_process_sq_admin(sq);
    }
}

static void nvme_process_db_io(FemuCtrl *n, hwaddr addr, int val)
{
    uint32_t qid;
    uint16_t new_val = val & 0xffff;
    NvmeSQueue *sq;

    if (n->dataplane_started) {
        //femu_debug("\t nvme_process_db_io (dataplane started, return)\n");
        return;
    }

    if (addr & ((1 << (2 + n->db_stride)) - 1)) {
        return;
    }

    if (((addr - 0x1000) >> (2 + n->db_stride)) & 1) {
        NvmeCQueue *cq;

        qid = ((addr - (0x1000 + (1 << (2 + n->db_stride)))) >> (3 +
                                                                 n->db_stride));
        if (nvme_check_cqid(n, qid)) {
            return;
        }

        cq = n->cq[qid];
        if (new_val >= cq->size) {
            return;
        }

        if (!cq->db_addr) {
            cq->head = new_val;
        }

        if (cq->tail != cq->head) {
            nvme_isr_notify_io(cq);
        }
    } else {
        qid = (addr - 0x1000) >> (3 + n->db_stride);
        if (nvme_check_sqid(n, qid)) {
            return;
        }
        sq = n->sq[qid];
        if (new_val >= sq->size) {
            return;
        }

        if (!sq->db_addr) {
            sq->tail = new_val;
        }
    }
}

static void nvme_mmio_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    FemuCtrl *n = (FemuCtrl *)opaque;
    //femu_debug("nvme_mmio_write ");
    if (addr < sizeof(n->bar)) {
        // femu_debug(" femu-nvme nvme_write_bar(addr 0x%lx, data 0x%lx, size 0x%x ) \n", addr, data, size);
        nvme_write_bar(n, addr, data, size);
        
    } else if (addr >= 0x1000 && addr < 0x1008) {
        // femu_debug(" femu-nvme nvme_process_db hwaddr : %lx val %lu size 0x%x -admin \n", addr, data, size);
        nvme_process_db_admin(n, addr, data);
    } else {
        // femu_debug(" femu-nvme nvme_process_db hwaddr : %lx val %lu size 0x%x -io \n", addr, data, size);
        nvme_process_db_io(n, addr, data);
    }
}

static void nvme_cmb_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    FemuCtrl *n = (FemuCtrl *)opaque;

    memcpy(&n->cmbuf[addr], &data, size);
}

static uint64_t nvme_cmb_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t val;
    FemuCtrl *n = (FemuCtrl *)opaque;

    memcpy(&val, &n->cmbuf[addr], size);

    return val;
}

static const MemoryRegionOps nvme_cmb_ops = {
    .read = nvme_cmb_read,
    .write = nvme_cmb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 2,
        .max_access_size = 8,
    },
};

static const MemoryRegionOps nvme_mmio_ops = {
    .read = nvme_mmio_read,
    .write = nvme_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 2,
        .max_access_size = 8,
    },
};

static int nvme_check_constraints(FemuCtrl *n)
{
    if ((n->num_namespaces == 0 || n->num_namespaces > NVME_MAX_NUM_NAMESPACES)
        || (n->nr_io_queues < 1 || n->nr_io_queues > NVME_MAX_QS) ||
        (n->db_stride > NVME_MAX_STRIDE) ||
        (n->max_q_ents < 1) ||
        (n->max_sqes > NVME_MAX_QUEUE_ES || n->max_cqes > NVME_MAX_QUEUE_ES ||
         n->max_sqes < NVME_MIN_SQUEUE_ES || n->max_cqes < NVME_MIN_CQUEUE_ES) ||
        (n->vwc > 1 || n->intc > 1 || n->cqr > 1 || n->extended > 1) ||
        (n->nlbaf > 16) ||
        (n->lba_index >= n->nlbaf) ||
        (n->meta && !n->mc) ||
        (n->extended && !(NVME_ID_NS_MC_EXTENDED(n->mc))) ||
        (!n->extended && n->meta && !(NVME_ID_NS_MC_SEPARATE(n->mc))) ||
        (n->dps && n->meta < 8) ||
        (n->dps && ((n->dps & DPS_FIRST_EIGHT) &&
                    !NVME_ID_NS_DPC_FIRST_EIGHT(n->dpc))) ||
        (n->dps && !(n->dps & DPS_FIRST_EIGHT) &&
         !NVME_ID_NS_DPC_LAST_EIGHT(n->dpc)) ||
        (n->dps & DPS_TYPE_MASK && !((n->dpc & NVME_ID_NS_DPC_TYPE_MASK) &
                                     (1 << ((n->dps & DPS_TYPE_MASK) - 1)))) ||
        (n->mpsmax > 0xf || n->mpsmax > n->mpsmin) ||
        (n->oacs & ~(NVME_OACS_FORMAT)) ||
        (n->oncs & ~(NVME_ONCS_COMPARE | NVME_ONCS_WRITE_UNCORR |
                     NVME_ONCS_DSM | NVME_ONCS_WRITE_ZEROS))) {
                         return -1;
     }

    return 0;
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
uint16_t femu_map_dptr(FemuCtrl *n, NvmeSg *sg, size_t len,
                       NvmeCmd *cmd)
{
    uint64_t prp1, prp2;

    switch (NVME_CMD_FLAGS_PSDT(cmd->flags)) {
    case NVME_PSDT_PRP:
        prp1 = le64_to_cpu(cmd->dptr.prp1);
        prp2 = le64_to_cpu(cmd->dptr.prp2);
        return nvme_map_prp(n, sg, prp1, prp2, len);
        //uint16_t nvme_map_prp(QEMUSGList *qsg, QEMUIOVector *iov, uint64_t prp1, uint64_t prp2, uint32_t len, FemuCtrl *n)
        //../hw/femu/femu.c:336:29: error: passing argument 1 of ‘nvme_map_prp’ from incompatible pointer type [-Werror=incompatible-pointer-types]
    case NVME_PSDT_SGL_MPTR_CONTIGUOUS:
    case NVME_PSDT_SGL_MPTR_SGL:
        return nvme_map_sgl(n, sg, cmd->dptr.sgl, len, cmd);
    default:
        return NVME_INVALID_FIELD;
    }
}

/*

static uint16_t nvme_map_mdata(FemuCtrl *n, uint32_t nlb, NvmeRequest *req)
{
    NvmeNamespace *ns = req->ns;
    size_t len = nvme_m2b(ns, nlb);
    uint16_t status;

    if (nvme_ns_ext(ns)) {
        NvmeSg sg;

        len += nvme_l2b(ns, nlb);

        status = femu_map_dptr(n, &sg, len, &req->cmd);
        if (status) {
            return status;
        }

        nvme_sg_init(n, &req->sg, sg.flags & NVME_SG_DMA);
        nvme_sg_split(&sg, ns, NULL, &req->sg);
        nvme_sg_unmap(&sg);

        return NVME_SUCCESS;
    }

    return nvme_map_mptr(n, &req->sg, len, &req->cmd);
}


static uint16_t nvme_tx_interleaved(FemuCtrl *n, NvmeSg *sg, uint8_t *ptr,
                                    uint32_t len, uint32_t bytes,
                                    int32_t skip_bytes, int64_t offset,
                                    NvmeTxDirection dir)
{
    hwaddr addr;
    uint32_t trans_len, count = bytes;
    bool dma = sg->flags & NVME_SG_DMA;
    int64_t sge_len;
    int sg_idx = 0;
    int ret;

    assert(sg->flags & NVME_SG_ALLOC);

    while (len) {
        sge_len = dma ? sg->qsg.sg[sg_idx].len : sg->iov.iov[sg_idx].iov_len;

        if (sge_len - offset < 0) {
            offset -= sge_len;
            sg_idx++;
            continue;
        }

        if (sge_len == offset) {
            offset = 0;
            sg_idx++;
            continue;
        }

        trans_len = MIN(len, count);
        trans_len = MIN(trans_len, sge_len - offset);

        if (dma) {
            addr = sg->qsg.sg[sg_idx].base + offset;
        } else {
            addr = (hwaddr)(uintptr_t)sg->iov.iov[sg_idx].iov_base + offset;
        }

        if (dir == NVME_TX_DIRECTION_TO_DEVICE) {
            ret = nvme_addr_read(n, addr, ptr, trans_len);
        } else {
            ret = nvme_addr_write(n, addr, ptr, trans_len);
        }

        if (ret) {
            return NVME_DATA_TRAS_ERROR;
        }

        ptr += trans_len;
        len -= trans_len;
        count -= trans_len;
        offset += trans_len;

        if (count == 0) {
            count = bytes;
            offset += skip_bytes;
        }
    }

    return NVME_SUCCESS;
}*/
/** hw/nvme/subsys.c */
/*
 * QEMU NVM Express Subsystem: nvme-subsys
 *
 * Copyright (c) 2021 Minwoo Im <minwoo.im.dev@gmail.com>
 *
 * This code is licensed under the GNU GPL v2.  Refer COPYING.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"

#include "nvme.h"


static void nvme_attach_ns(FemuCtrl *n, NvmeNamespace *ns)
{
    //uint32_t nsid = ns->params.nsid;
    //assert(nsid && nsid <= NVME_MAX_NAMESPACES);
    //n->namespaces[nsid] = ns;
    n->namespaces = ns; 
    ns->attached++;

    n->dmrsl = MIN_NON_ZERO(n->dmrsl,
                            BDRV_REQUEST_MAX_BYTES / nvme_l2b(ns, 1));
}

static int nvme_subsys_reserve_cntlids(FemuCtrl *n, int start, int num)
{
    NvmeSubsystem *subsys = n->subsys;
    NvmeSecCtrlList *list = &n->sec_ctrl_list;
    NvmeSecCtrlEntry *sctrl;
    int i, cnt = 0;

    for (i = start; i < ARRAY_SIZE(subsys->ctrls) && cnt < num; i++) {
        if (!subsys->ctrls[i]) {
            sctrl = &list->sec[cnt];
            sctrl->scid = cpu_to_le16(i);
            subsys->ctrls[i] = SUBSYS_SLOT_RSVD;
            cnt++;
        }
    }

    return cnt;
}

static void nvme_subsys_unreserve_cntlids(FemuCtrl *n)
{
    NvmeSubsystem *subsys = n->subsys;
    NvmeSecCtrlList *list = &n->sec_ctrl_list;
    NvmeSecCtrlEntry *sctrl;
    int i, cntlid;

    for (i = 0; i < n->params.sriov_max_vfs; i++) {
        sctrl = &list->sec[i];
        cntlid = le16_to_cpu(sctrl->scid);

        if (cntlid) {
            assert(subsys->ctrls[cntlid] == SUBSYS_SLOT_RSVD);
            subsys->ctrls[cntlid] = NULL;
            sctrl->scid = 0;
        }
    }
}

//int nvme_subsys_register_ctrl(FemuCtrl *n)
int femu_subsys_register_ctrl(FemuCtrl *n)
{
    NvmeSubsystem *subsys = n->subsys;
    NvmeSecCtrlEntry *sctrl = nvme_sctrl(n);
    int cntlid, nsid, num_rsvd, num_vfs = n->params.sriov_max_vfs;

    if (pci_is_vf(&n->parent_obj)) {
        cntlid = le16_to_cpu(sctrl->scid);
    } else {
        for (cntlid = 0; cntlid < ARRAY_SIZE(subsys->ctrls); cntlid++) {
            if (!subsys->ctrls[cntlid]) {
                break;
            }
        }

        if (cntlid == ARRAY_SIZE(subsys->ctrls)) {
            //error_setg(errp, "no more free controller id");
            return -1;
        }

        num_rsvd = nvme_subsys_reserve_cntlids(n, cntlid + 1, num_vfs);
        if (num_rsvd != num_vfs) {
            nvme_subsys_unreserve_cntlids(n);
            //error_setg(errp,
            //           "no more free controller ids for secondary controllers");
            return -1;
        }
    }

    if (!subsys->serial) {
        subsys->serial = g_strdup(n->params.serial);
    } else if (strcmp(subsys->serial, n->params.serial)) {
        //error_setg(errp, "invalid controller serial");
        return -1;
    }

    subsys->ctrls[cntlid] = n;

    for (nsid = 1; nsid < ARRAY_SIZE(subsys->namespaces); nsid++) {
        NvmeNamespace *ns = subsys->namespaces[nsid];
        if (ns && ns->params.shared && !ns->params.detached) {
            nvme_attach_ns(n, ns);
        }
    }

    return cntlid;
}
//void nvme_subsys_unregister_ctrl(NvmeSubsystem *subsys, FemuCtrl *n)
void femu_subsys_unregister_ctrl(NvmeSubsystem *subsys, FemuCtrl *n)
{
    if (pci_is_vf(&n->parent_obj)) {
        subsys->ctrls[n->cntlid] = SUBSYS_SLOT_RSVD;
    } else {
        subsys->ctrls[n->cntlid] = NULL;
        nvme_subsys_unreserve_cntlids(n);
    }

    n->cntlid = -1;
}

static bool nvme_calc_rgif(uint16_t nruh, uint16_t nrg, uint8_t *rgif)
{
    uint16_t val;
    unsigned int i;

    if (unlikely(nrg == 1)) {
        /* PIDRG_NORGI scenario, all of pid is used for PHID */
        *rgif = 0;
        return true;
    }

    val = nrg;
    i = 0;
    while (val) {
        val >>= 1;
        i++;
    }
    *rgif = i;

    /* ensure remaining bits suffice to represent number of phids in a RG */
    if (unlikely((UINT16_MAX >> i) < nruh)) {
        *rgif = 0;
        return false;
    }

    return true;
}

static bool nvme_subsys_setup_fdp(NvmeSubsystem *subsys, Error **errp)  //setting nrg and runs
{
    NvmeEnduranceGroup *endgrp = &subsys->endgrp;
    uint64_t tt_nru = subsys->params.fdp.nru; 

    if (!subsys->params.fdp.runs) {
        error_setg(errp, "fdp.runs must be non-zero");
        return false;
    }

    endgrp->fdp.runs = subsys->params.fdp.runs;
    endgrp->fdp.nru = subsys->params.fdp.nru;
    femu_log(" endgrp->fdp.runs = %lu\n", endgrp->fdp.runs);
    //ru ns = reclaim unit normal "size" 

    if (!subsys->params.fdp.nrg) {
        error_setg(errp, "fdp.nrg must be non-zero");
        return false;
    }

    endgrp->fdp.nrg = subsys->params.fdp.nrg;
    femu_log(" endgrp->fdp.nrg = %u\n", endgrp->fdp.nrg);

    if (!subsys->params.fdp.nruh) {
        error_setg(errp, "fdp.nruh must be non-zero");
        return false;
    }

    endgrp->fdp.nruh = subsys->params.fdp.nruh;
    femu_log(" endgrp->fdp.nruh = %u\n", endgrp->fdp.nruh);

    if (!nvme_calc_rgif(endgrp->fdp.nruh, endgrp->fdp.nrg, &endgrp->fdp.rgif)) {
        error_setg(errp,
                   "cannot derive a valid rgif (nruh %"PRIu16" nrg %"PRIu32")",
                   endgrp->fdp.nruh, endgrp->fdp.nrg);
        return false;
    }

    endgrp->fdp.rus = g_new(NvmeReclaimUnit *, endgrp->fdp.nrg);
    for(int i=0 ; i < endgrp->fdp.nrg; i++){
        endgrp->fdp.rus[i] = g_new(NvmeReclaimUnit, tt_nru);
        femu_log("      endgrp->fdp.rus[%d] = new %ld NvmeReclaimUnits\n", i, tt_nru);
        // INHO
        //For sure , but qemu fdp implementation cleary does not consider 1ruh -> N rus situation.
        //Thinking about the figures such as rus inside the reclaim group in SNIA, 
        //figures from samsung that describe how the ruh use and write to the multiple rus by their own color(actually ruhid which is stream id)
        // ruh.rus has JUST ONE RECLAIM UNIT per reclaim group is totally NONSENSE. 
        // So what I am gonna do is 1. setup rus and rgs 2. and make ruh points to them by their pointers.
        //This means g_new statment inside the ruh initialization code is no longer valid.

    }
    

    endgrp->fdp.ruhs = g_new(NvmeRuHandle, endgrp->fdp.nruh);

    for (uint16_t ruhid = 0; ruhid < endgrp->fdp.nruh; ruhid++) {
        endgrp->fdp.ruhs[ruhid] = (NvmeRuHandle) {
            .ruht = NVME_RUHT_INITIALLY_ISOLATED,
            .ruha = NVME_RUHA_UNUSED,
        };
        //Prev Anot : This means g_new statment inside the ruh initialization code is no longer valid.
        //endgrp->fdp.ruhs[ruhid].rus = g_new(NvmeReclaimUnit, endgrp->fdp.nrg);
        endgrp->fdp.ruhs[ruhid].rus = g_new(NvmeReclaimUnit *, endgrp->fdp.nrg);
        for(int rg=0 ; rg < endgrp->fdp.nrg; ++rg){
            //endgrp->fdp.ruhs[ruhid].rus = &endgrp->fdp.rus[rg][ruhid];
            endgrp->fdp.ruhs[ruhid].rus[rg] = &endgrp->fdp.rus[rg][ruhid];
        }
    }

    endgrp->fdp.enabled = true;
    femu_log("fdp.enabled = true");

    return true;
}

static bool nvme_subsys_setup(NvmeSubsystem *subsys, Error **errp)  //OK
{
    const char *nqn = subsys->params.nqn ?
        subsys->params.nqn : subsys->parent_obj.id;

    snprintf((char *)subsys->subnqn, sizeof(subsys->subnqn),
             "nqn.2019-08.org.qemu:%s", nqn);

    if (subsys->params.fdp.enabled && !nvme_subsys_setup_fdp(subsys, errp)) {
        return false;
    }

    return true;
}

static void nvme_subsys_realize(DeviceState *dev, Error **errp)
{
    NvmeSubsystem *subsys = NVME_SUBSYS(dev);

    qbus_init(&subsys->bus, sizeof(NvmeBus), TYPE_NVME_BUS, dev, dev->id);

    nvme_subsys_setup(subsys, errp);
}

static Property nvme_subsystem_props[] = {
    DEFINE_PROP_STRING("nqn", NvmeSubsystem, params.nqn),
    DEFINE_PROP_BOOL("fdp", NvmeSubsystem, params.fdp.enabled, false),
    DEFINE_PROP_SIZE("fdp.runs", NvmeSubsystem, params.fdp.runs,
                     NVME_DEFAULT_RU_SIZE),
    DEFINE_PROP_UINT32("fdp.nrg", NvmeSubsystem, params.fdp.nrg, 1),
    DEFINE_PROP_UINT16("fdp.nruh", NvmeSubsystem, params.fdp.nruh, 0),
    DEFINE_PROP_UINT64("fdp.nru", NvmeSubsystem, params.fdp.nru, 128),
    DEFINE_PROP_END_OF_LIST(),
};

static void nvme_ns_init_identify(FemuCtrl *n, NvmeIdNs *id_ns)
{
    int npdg;
    int i;

    /* NSFEAT Bit 3: Support the Deallocated or Unwritten Logical Block error */
    id_ns->nsfeat        |= (0x4 | 0x10);
    id_ns->nlbaf         = n->nlbaf - 1;
    id_ns->flbas         = n->lba_index | (n->extended << 4);
    id_ns->mc            = n->mc;
    id_ns->dpc           = n->dpc;
    id_ns->dps           = n->dps;
    id_ns->dlfeat        = 0x9;
    id_ns->lbaf[0].lbads = 9;               //512?
    id_ns->lbaf[0].ms    = 0;

    npdg = 1;
    id_ns->npda = id_ns->npdg = npdg - 1;

    for (i = 0; i < n->nlbaf; i++) {
        id_ns->lbaf[i].lbads = BDRV_SECTOR_BITS + i;
        id_ns->lbaf[i].ms    = cpu_to_le16(n->meta);
    }
}

static NvmeRuHandle *nvme_find_ruh_by_attr(NvmeEnduranceGroup *endgrp,
                                           uint8_t ruha, uint16_t *ruhid)
{
    for (uint16_t i = 0; i < endgrp->fdp.nruh; i++) {
        NvmeRuHandle *ruh = &endgrp->fdp.ruhs[i];

        if (ruh->ruha == ruha) {
            *ruhid = i;
            return ruh;
        }
    }

    return NULL;
}
static bool nvme_ns_init_fdp(NvmeNamespace *ns, Error **errp)
{
    NvmeEnduranceGroup *endgrp = ns->endgrp;
    NvmeRuHandle *ruh;
    uint8_t lbafi = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    g_autofree unsigned int *ruhids = NULL;
    unsigned int *ruhid;
    char *r, *p, *token;
    uint16_t *ph;
    uint64_t tt_nru = endgrp->fdp.nru;
    femu_log("  nvme_ns_init_fdp here \n");
    //TODO-inho 1
    if (!ns->params.fdp.ruhs && !endgrp->fdp.ruhs) {
        femu_err("      (!ns->params.fdp.ruhs) ns->params.fdp.ruhs was NULL (setting )\n");
        
        ns->fdp.nphs = 1;
        ph = ns->fdp.phs = g_new(uint16_t, 1);

        ruh = nvme_find_ruh_by_attr(endgrp, NVME_RUHA_CTRL, ph);
        if (!ruh) {
            ruh = nvme_find_ruh_by_attr(endgrp, NVME_RUHA_UNUSED, ph);
            if (!ruh) {
                error_setg(errp, "no unused reclaim unit handles left");
                return false;
            }

            ruh->ruha = NVME_RUHA_CTRL;
            ruh->lbafi = lbafi;
            //ruh->ruamw = endgrp->fdp.runs >> ns->lbaf.ds;
            ruh->ruamw = endgrp->fdp.runs >> ns->lbaf.lbads;

            femu_err("     nvme_ns_init_fdp - endgrp->fdp.rus[rg][i].ruamw update; \n");
            for (uint16_t rg = 0; rg < endgrp->fdp.nrg; rg++) {
                for(uint16_t i = 0; i < tt_nru; i++){
                    endgrp->fdp.rus[rg][i].ruamw = ruh->ruamw;
                }
            }
            // for (uint16_t rg = 0; rg < endgrp->fdp.nrg; rg++) {
            //     //ruh->rus[rg].ruamw = ruh->ruamw;
            //     if(ruh->rus[rg]->ruamw != ruh->ruamw){
            //         femu_err("      if(ruh->rus[%d]->ruamw%lu != ruh->ruamw %lu); fin \n",rg,ruh->rus[rg]->ruamw, ruh->ruamw);
            //         ruh->rus[rg]->ruamw = ruh->ruamw;
            //     }
            // }
            
        } else if (ruh->lbafi != lbafi) {
            error_setg(errp, "lba format index of controller assigned "
                       "reclaim unit handle does not match namespace lba "
                       "format index");
            return false;
        }else{
            femu_err("     FIND ruh = nvme_find_ruh_by_attr(endgrp, NVME_RUHA_CTRL, ph); \n");
        }

        return true;
    }
    femu_log("      ruhid (char *) initialized as char[%d] \n", endgrp->fdp.nruh);
    if(endgrp->fdp.ruhs){
        ruhid = ruhids = g_new0(unsigned int, endgrp->fdp.nruh);
        ns->fdp.nphs = endgrp->fdp.nruh;
        femu_log("      ns->fdp.nphs : %d \n",ns->fdp.nphs);
        ph = ns->fdp.phs = g_new(uint16_t, ns->fdp.nphs);
        
        for (unsigned int i = 0; i < ns->fdp.nphs; i++, ruhid++, ph++) {
            if (*ruhid >= endgrp->fdp.nruh) {
                femu_err("      (*ruhid >= endgrp->fdp.nruh) invalid reclaim unit handle identifier \n");
                error_setg(errp, "invalid reclaim unit handle identifier");
                return false;
            }
            ruh = &endgrp->fdp.ruhs[*ruhid];
            
            switch (ruh->ruha) {
                case NVME_RUHA_UNUSED:
                    femu_log("      NVME_RUHA_UNUSED. set as ruh->ruha = NVME_RUHA_HOST;\n");
                    ruh->ruha = NVME_RUHA_HOST;
                    ruh->lbafi = lbafi;
                    //ruh->ruamw = endgrp->fdp.runs >> ns->lbaf.ds;
                    ruh->ruamw = endgrp->fdp.runs >> ns->lbaf.lbads;

                    for (uint16_t rg = 0; rg < endgrp->fdp.nrg; rg++) {
                        //ruh->rus[rg].ruamw = ruh->ruamw;
                        ruh->rus[rg]->ruamw = ruh->ruamw;
                    }

                    break;

                case NVME_RUHA_HOST:
                    if (ruh->lbafi != lbafi) {
                        femu_err("      (ruh->lbafi != lbafi) NVME_RUHA_HOST  reclaim unit handle does not match namespace \n");

                        error_setg(errp, "lba format index of host assigned"
                                "reclaim unit handle does not match namespace "
                                "lba format index");
                        return false;
                    }

                    break;

                case NVME_RUHA_CTRL:
                    femu_err("      NVME_RUHA_CTRL reclaim unit handle is controller assigned \n");
                    error_setg(errp, "reclaim unit handle is controller assigned");
                    return false;

                default:
                    femu_err("      no such type for ruh->ruha - init error? \n");
                    abort();
            }
            *ph = *ruhid;

        }
        return true;    //instead of tokenizing 1;2;3;4, allocate sequentially
    }
    
    ruhid = ruhids = g_new0(unsigned int, endgrp->fdp.nruh);
    femu_log("          r = p = strdup(ns->params.fdp.ruhs %s);\n", ns->params.fdp.ruhs);
    r = p = strdup(ns->params.fdp.ruhs);
    femu_log("          (token = qemu_strsep(&p, ;)) != NULL\n");
    /* parse the placement handle identifiers */
    while ((token = qemu_strsep(&p, ";")) != NULL) {
        ns->fdp.nphs += 1;
        if (ns->fdp.nphs > NVME_FDP_MAXPIDS ||
            ns->fdp.nphs == endgrp->fdp.nruh) {
            error_setg(errp, "too many placement handles");
            free(r);
            return false;
        }

        if (qemu_strtoui(token, NULL, 0, ruhid++) < 0) {
            error_setg(errp, "cannot parse reclaim unit handle identifier");
            free(r);
            return false;
        }
    }
    femu_err("ns->fdp.nphs : %d \n",ns->fdp.nphs);

    free(r);
    ph = ns->fdp.phs = g_new(uint16_t, ns->fdp.nphs);

    ruhid = ruhids;

    /* verify the identifiers */
    for (unsigned int i = 0; i < ns->fdp.nphs; i++, ruhid++, ph++) {
        if (*ruhid >= endgrp->fdp.nruh) {
            femu_err("      (*ruhid >= endgrp->fdp.nruh) invalid reclaim unit handle identifier \n");
            error_setg(errp, "invalid reclaim unit handle identifier");
            return false;
        }

        ruh = &endgrp->fdp.ruhs[*ruhid];

        switch (ruh->ruha) {
        case NVME_RUHA_UNUSED:
            femu_log("      NVME_RUHA_UNUSED. set as ruh->ruha = NVME_RUHA_HOST;\n");
            ruh->ruha = NVME_RUHA_HOST;
            ruh->lbafi = lbafi;
            //ruh->ruamw = endgrp->fdp.runs >> ns->lbaf.ds;
            ruh->ruamw = endgrp->fdp.runs >> ns->lbaf.lbads;

            for (uint16_t rg = 0; rg < endgrp->fdp.nrg; rg++) {
                //ruh->rus[rg].ruamw = ruh->ruamw;
                ruh->rus[rg]->ruamw = ruh->ruamw;
            }

            break;

        case NVME_RUHA_HOST:
            if (ruh->lbafi != lbafi) {
                femu_err("      (ruh->lbafi != lbafi) NVME_RUHA_HOST  reclaim unit handle does not match namespace \n");

                error_setg(errp, "lba format index of host assigned"
                           "reclaim unit handle does not match namespace "
                           "lba format index");
                return false;
            }

            break;

        case NVME_RUHA_CTRL:
            femu_err("      NVME_RUHA_CTRL reclaim unit handle is controller assigned \n");
            error_setg(errp, "reclaim unit handle is controller assigned");
            return false;

        default:
            femu_err("      no such type for ruh->ruha - init error? \n");
            abort();
        }

        *ph = *ruhid;
    }

    return true;
}

static int nvme_init_namespace(FemuCtrl *n, NvmeNamespace *ns, Error **errp)
{
    NvmeIdNs *id_ns = &ns->id_ns;
    uint64_t num_blks;
    int lba_index;

    nvme_ns_init_identify(n, id_ns);

    lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    num_blks = n->ns_size / ((1 << id_ns->lbaf[lba_index].lbads));
    id_ns->nuse = id_ns->ncap = id_ns->nsze = cpu_to_le64(num_blks);

    n->csi = NVME_CSI_NVM;
    ns->ctrl = n;
    ns->ns_blks = ns_blks(ns, lba_index);
    ns->util = bitmap_new(num_blks);
    ns->uncorrectable = bitmap_new(num_blks);

    if(!n->subsys){
        femu_err("  nvme_init_namespace (!n->subsys) n->subsys is NULL \n");
        ns->params.shared = false;
    }
    else{
        ns->subsys = n->subsys;
        ns->endgrp = &n->subsys->endgrp;
        if(!nvme_ns_init_fdp(ns, errp)){
            femu_err("Something wrong in nvme_ns_init_fdp(ns, errp)\n");
            return -1;
        }
        femu_log("  nvme_init_namespace initialized ns->subsys and ns->endgrp \n");
    }

    

    return 0;
}

static int nvme_init_namespaces(FemuCtrl *n, Error **errp)
{
    int i;

    /* FIXME: FEMU only supports 1 namesapce now */
    assert(n->num_namespaces == 1);

    for (i = 0; i < n->num_namespaces; i++) {
        NvmeNamespace *ns = &n->namespaces[i];
        ns->size = n->ns_size;
        ns->start_block = i * n->ns_size >> BDRV_SECTOR_BITS;
        ns->id = i + 1;

        if (nvme_init_namespace(n, ns, errp)) {
            return 1;
        }

    }

    return 0;
}

static void nvme_init_ctrl(FemuCtrl *n)
{
    NvmeIdCtrl *id = &n->id_ctrl;
    uint8_t *pci_conf = n->parent_obj.config;
    uint32_t ctratt;

    char *subnqn;
    int i;

    id->vid = cpu_to_le16(pci_get_word(pci_conf + PCI_VENDOR_ID));
    id->ssvid = cpu_to_le16(pci_get_word(pci_conf + PCI_SUBSYSTEM_VENDOR_ID));
    ctratt = NVME_CTRATT_ELBAS;

    id->rab          = 6;
    id->ieee[0]      = 0x00;
    id->ieee[1]      = 0x02;
    id->ieee[2]      = 0xb3;
    id->cmic         = 0;
    id->mdts         = n->mdts;
    id->ver          = 0x00010300;
    /* TODO: NVME_OACS_NS_MGMT */
    id->oacs         = cpu_to_le16(n->oacs | NVME_OACS_DBBUF);
    id->acl          = n->acl;
    id->aerl         = n->aerl;
    id->frmw         = 7 << 1 | 1;
    id->lpa          = NVME_LPA_NS_SMART | NVME_LPA_CSE | NVME_LPA_EXTENDED;
    id->elpe         = n->elpe;
    id->npss         = 0;
    id->sqes         = (n->max_sqes << 4) | 0x6;
    id->cqes         = (n->max_cqes << 4) | 0x4;
    id->nn           = cpu_to_le32(n->num_namespaces);
    id->oncs         = cpu_to_le16(n->oncs);
    subnqn           = g_strdup_printf("nqn.2019-08.org.qemu:%s", n->serial);
    strpadcpy((char *)id->subnqn, sizeof(id->subnqn), subnqn, '\0');
    id->fuses        = cpu_to_le16(0);
    id->fna          = 0;
    id->vwc          = n->vwc;
    id->awun         = cpu_to_le16(0);
    id->awupf        = cpu_to_le16(0);
    id->psd[0].mp    = cpu_to_le16(0x9c4);
    id->psd[0].enlat = cpu_to_le32(0x10);
    id->psd[0].exlat = cpu_to_le32(0x4);
    id->cntlid = cpu_to_le16(n->cntlid);

    if (n->subsys) {
        id->cmic |= NVME_CMIC_MULTI_CTRL;
        ctratt |= NVME_CTRATT_ENDGRPS;

        id->endgidmax = cpu_to_le16(0x1);       //Both QEMU and FEMU supports only 1 endurance group at the moment

        if (n->subsys->endgrp.fdp.enabled) {
            ctratt |= NVME_CTRATT_FDPS;
            femu_log("FEMU NVMe : \"I'm NVMe fdp enabled device!\" ctratt hex:%x \n",ctratt);
                // AUDIT sprintf(filename0, "write_log.csv");
                //fp = fopen(filename0, "w");
                //fprintf(fp, "test write\n");
                //fprintf(fp, "start(s),\t\tend(s),\t\tstart(us),\t\tend(us),\t\ttime(s),\t\ttime(us),\t\tpid,\t\truhid,\t\tslba,\t\tnlb,\t\tru->ruamw,\t\truh_action\n");
                //fclose(fp);
        }else{
            femu_log("FEMU NVMe : \"NVMe fdp disabled device!\" ctratt hex:%x \n",ctratt);
        }
    }else{
        femu_log("FEMU NVMe : \"n->subsys NULL in this device!\" ctratt hex:%x \n",ctratt);
    }


    id->ctratt = cpu_to_le32(ctratt);
    n->features.arbitration     = 0x1f0f0706;
    n->features.power_mgmt      = 0;
    n->features.temp_thresh     = 0x14d;
    n->features.err_rec         = 0;
    n->features.volatile_wc     = n->vwc;
    n->features.nr_io_queues   = ((n->nr_io_queues - 1) | ((n->nr_io_queues -
                                                              1) << 16));
    n->features.int_coalescing  = n->intc_thresh | (n->intc_time << 8);
    n->features.write_atomicity = 0;
    n->features.async_config    = 0x0;
    n->features.sw_prog_marker  = 0;

    for (i = 0; i <= n->nr_io_queues; i++) {
        n->features.int_vector_config[i] = i | (n->intc << 16);
    }

    n->bar.cap = 0;
    NVME_CAP_SET_MQES(n->bar.cap, n->max_q_ents);
    NVME_CAP_SET_CQR(n->bar.cap, n->cqr);
    NVME_CAP_SET_AMS(n->bar.cap, 1);
    NVME_CAP_SET_TO(n->bar.cap, 0xf);
    NVME_CAP_SET_DSTRD(n->bar.cap, n->db_stride);
    NVME_CAP_SET_NSSRS(n->bar.cap, 0);
    NVME_CAP_SET_CSS(n->bar.cap, 1);
    NVME_CAP_SET_CSS(n->bar.cap, NVME_CAP_CSS_CSI_SUPP);
    NVME_CAP_SET_CSS(n->bar.cap, NVME_CAP_CSS_ADMIN_ONLY);

    NVME_CAP_SET_MPSMIN(n->bar.cap, n->mpsmin);
    NVME_CAP_SET_MPSMAX(n->bar.cap, n->mpsmax);

    n->bar.vs = NVME_SPEC_VER;
    n->bar.intmc = n->bar.intms = 0;
    n->temperature = NVME_TEMPERATURE;
}

static int nvme_init_subsys(FemuCtrl *n)
{
    int cntlid;

    if (!n->subsys) {
        return 0;
    }

    //cntlid = nvme_subsys_register_ctrl(n);
    cntlid = femu_subsys_register_ctrl(n);
    if (cntlid < 0) {
        return -1;
    }

    n->cntlid = cntlid;

    return 0;
}

static void nvme_subsys_class_init(ObjectClass *oc, void *data) //OK
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);

    dc->realize = nvme_subsys_realize;
    dc->desc = "Virtual NVMe subsystem";
    dc->hotpluggable = false;

    device_class_set_props(dc, nvme_subsystem_props);
}

static const TypeInfo nvme_subsys_info = {
    .name = TYPE_NVME_SUBSYS,
    .parent = TYPE_DEVICE,
    .class_init = nvme_subsys_class_init,
    .instance_size = sizeof(NvmeSubsystem),
};

static void nvme_init_cmb(FemuCtrl *n)
{
    n->bar.cmbloc = n->cmbloc;
    n->bar.cmbsz  = n->cmbsz;

    n->cmbuf = g_malloc0(NVME_CMBSZ_GETSIZE(n->bar.cmbsz));
    memory_region_init_io(&n->ctrl_mem, OBJECT(n), &nvme_cmb_ops, n, "nvme-cmb",
                          NVME_CMBSZ_GETSIZE(n->bar.cmbsz));
    pci_register_bar(&n->parent_obj, NVME_CMBLOC_BIR(n->bar.cmbloc),
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &n->ctrl_mem);
}

static void nvme_init_pci(FemuCtrl *n)
{
    uint8_t *pci_conf = n->parent_obj.config;

    pci_conf[PCI_INTERRUPT_PIN] = 1;
    /* Coperd: QEMU-OCSSD(0x1d1d,0x1f1f), QEMU-NVMe(0x8086,0x5845) */
    pci_config_set_prog_interface(pci_conf, 0x2);
    pci_config_set_vendor_id(pci_conf, n->vid);
    pci_config_set_device_id(pci_conf, n->did);
    pci_config_set_class(pci_conf, PCI_CLASS_STORAGE_EXPRESS);
    pcie_endpoint_cap_init(&n->parent_obj, 0x80);

    memory_region_init_io(&n->iomem, OBJECT(n), &nvme_mmio_ops, n, "nvme",
                          n->reg_size);
    pci_register_bar(&n->parent_obj, 0, PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &n->iomem);
    if (msix_init_exclusive_bar(&n->parent_obj, n->nr_io_queues + 1, 4, NULL)) {
        return;
    }
    msi_init(&n->parent_obj, 0x50, 32, true, false, NULL);

    if (n->cmbsz) {
        nvme_init_cmb(n);
    }
}

static int nvme_register_extensions(FemuCtrl *n)
{
    if (OCSSD(n)) {
        switch (n->lver) {
        case OCSSD12:
            nvme_register_ocssd12(n);
            break;
        case OCSSD20:
            nvme_register_ocssd20(n);
            break;
        default:
            break;
        }
    } else if (NOSSD(n)) {
        nvme_register_nossd(n);
    } else if (BBSSD(n)) {
        nvme_register_bbssd(n);
    } else if (ZNSSD(n)) {
        nvme_register_znssd(n);
    } else {
        /* TODO: For future extensions */
    }

    return 0;
}

static void femu_realize(PCIDevice *pci_dev, Error **errp)
{
    FemuCtrl *n = FEMU(pci_dev);
    int64_t bs_size;

    nvme_check_size();

    if (nvme_check_constraints(n)) {
        return;
    }

    bs_size = ((int64_t)n->memsz) * 1024 * 1024;

    init_dram_backend(&n->mbe, bs_size);
    n->mbe->femu_mode = n->femu_mode;

    n->completed = 0;
    n->start_time = time(NULL);
    n->reg_size = pow2ceil(0x1004 + 2 * (n->nr_io_queues + 1) * 4);
    n->ns_size = bs_size / (uint64_t)n->num_namespaces;

    /* Coperd: [1..nr_io_queues] are used as IO queues */
    n->sq = g_malloc0(sizeof(*n->sq) * (n->nr_io_queues + 1));
    n->cq = g_malloc0(sizeof(*n->cq) * (n->nr_io_queues + 1));
    n->namespaces = g_malloc0(sizeof(*n->namespaces) * n->num_namespaces);
    n->elpes = g_malloc0(sizeof(*n->elpes) * (n->elpe + 1));
    n->aer_reqs = g_malloc0(sizeof(*n->aer_reqs) * (n->aerl + 1));
    n->features.int_vector_config = g_malloc0(sizeof(*n->features.int_vector_config) * (n->nr_io_queues + 1));

    nvme_init_pci(n);
    femu_log("\t nvme_init_subsys start \n");
    if(!n->subsys)
        nvme_init_subsys(n);

    nvme_init_ctrl(n);

    nvme_init_namespaces(n, errp);

    nvme_register_extensions(n);

    if (n->ext_ops.init) {
        n->ext_ops.init(n, errp);
    }
    femu_log("\t femu_realize fin \n");

}

static void nvme_destroy_poller(FemuCtrl *n)
{
    int i;
    femu_debug("Destroying NVMe poller !!\n");

    for (i = 1; i <= n->nr_pollers; i++) {
        qemu_thread_join(&n->poller[i]);
    }

    for (i = 1; i <= n->nr_pollers; i++) {
        pqueue_free(n->pq[i]);
        femu_ring_free(n->to_poller[i]);
        femu_ring_free(n->to_ftl[i]);
    }

    g_free(n->should_isr);
}

static void femu_exit(PCIDevice *pci_dev)
{
    FemuCtrl *n = FEMU(pci_dev);

    femu_debug("femu_exit starting!\n");

    if (n->ext_ops.exit) {
        n->ext_ops.exit(n);
    }

    nvme_clear_ctrl(n, true);
    nvme_destroy_poller(n);
    free_dram_backend(n->mbe);

    g_free(n->namespaces);
    g_free(n->features.int_vector_config);
    g_free(n->aer_reqs);
    g_free(n->elpes);
    g_free(n->cq);
    g_free(n->sq);
    msix_uninit_exclusive_bar(pci_dev);
    memory_region_unref(&n->iomem);
    if (n->cmbsz) {
        memory_region_unref(&n->ctrl_mem);
    }
}

static Property femu_props[] = {
    DEFINE_PROP_STRING("serial", FemuCtrl, serial),
    DEFINE_PROP_UINT32("devsz_mb", FemuCtrl, memsz, 1024), /* in MB */
    DEFINE_PROP_UINT32("namespaces", FemuCtrl, num_namespaces, 1),
    DEFINE_PROP_UINT32("queues", FemuCtrl, nr_io_queues, 8),
    DEFINE_PROP_UINT32("entries", FemuCtrl, max_q_ents, 0x7ff),
    DEFINE_PROP_UINT8("multipoller_enabled", FemuCtrl, multipoller_enabled, 0),
    DEFINE_PROP_UINT8("max_cqes", FemuCtrl, max_cqes, 0x4),
    DEFINE_PROP_UINT8("max_sqes", FemuCtrl, max_sqes, 0x6),
    DEFINE_PROP_UINT8("stride", FemuCtrl, db_stride, 0),
    DEFINE_PROP_UINT8("aerl", FemuCtrl, aerl, 3),
    DEFINE_PROP_UINT8("acl", FemuCtrl, acl, 3),
    DEFINE_PROP_UINT8("elpe", FemuCtrl, elpe, 3),
    DEFINE_PROP_UINT8("mdts", FemuCtrl, mdts, 10),
    DEFINE_PROP_UINT8("cqr", FemuCtrl, cqr, 1),
    DEFINE_PROP_UINT8("vwc", FemuCtrl, vwc, 0),
    DEFINE_PROP_UINT8("intc", FemuCtrl, intc, 0),
    DEFINE_PROP_UINT8("intc_thresh", FemuCtrl, intc_thresh, 0),
    DEFINE_PROP_UINT8("intc_time", FemuCtrl, intc_time, 0),
    DEFINE_PROP_UINT8("ms", FemuCtrl, ms, 16),
    DEFINE_PROP_UINT8("ms_max", FemuCtrl, ms_max, 64),
    DEFINE_PROP_UINT8("dlfeat", FemuCtrl, dlfeat, 1),
    DEFINE_PROP_UINT8("mpsmin", FemuCtrl, mpsmin, 0),
    DEFINE_PROP_UINT8("mpsmax", FemuCtrl, mpsmax, 0),
    DEFINE_PROP_UINT8("nlbaf", FemuCtrl, nlbaf, 5),
    DEFINE_PROP_UINT8("lba_index", FemuCtrl, lba_index, 0),
    DEFINE_PROP_UINT8("extended", FemuCtrl, extended, 0),
    DEFINE_PROP_UINT8("dpc", FemuCtrl, dpc, 0),
    DEFINE_PROP_UINT8("dps", FemuCtrl, dps, 0),
    DEFINE_PROP_UINT8("mc", FemuCtrl, mc, 0),
    DEFINE_PROP_UINT8("meta", FemuCtrl, meta, 0),
    DEFINE_PROP_UINT32("cmbsz", FemuCtrl, cmbsz, 0),
    DEFINE_PROP_UINT32("cmbloc", FemuCtrl, cmbloc, 0),
    DEFINE_PROP_UINT16("oacs", FemuCtrl, oacs, NVME_OACS_FORMAT),
    DEFINE_PROP_UINT16("oncs", FemuCtrl, oncs, NVME_ONCS_DSM),
    DEFINE_PROP_UINT16("vid", FemuCtrl, vid, 0x1d1d),
    DEFINE_PROP_UINT16("did", FemuCtrl, did, 0x1f1f),
    DEFINE_PROP_UINT8("femu_mode", FemuCtrl, femu_mode, FEMU_NOSSD_MODE),
    DEFINE_PROP_UINT8("flash_type", FemuCtrl, flash_type, MLC),
    DEFINE_PROP_UINT8("lver", FemuCtrl, lver, 0x2),
    DEFINE_PROP_UINT16("lsec_size", FemuCtrl, oc_params.sec_size, 4096),
    DEFINE_PROP_UINT8("lsecs_per_pg", FemuCtrl, oc_params.secs_per_pg, 4),
    DEFINE_PROP_UINT16("lpgs_per_blk", FemuCtrl, oc_params.pgs_per_blk, 512),
    DEFINE_PROP_UINT8("lmax_sec_per_rq", FemuCtrl, oc_params.max_sec_per_rq, 64),
    DEFINE_PROP_UINT8("lnum_ch", FemuCtrl, oc_params.num_ch, 2),
    DEFINE_PROP_UINT8("lnum_lun", FemuCtrl, oc_params.num_lun, 8),
    DEFINE_PROP_UINT8("lnum_pln", FemuCtrl, oc_params.num_pln, 2),
    DEFINE_PROP_UINT16("lmetasize", FemuCtrl, oc_params.sos, 16),
    DEFINE_PROP_UINT8("zns_num_ch", FemuCtrl, zns_params.zns_num_ch, 2),
    DEFINE_PROP_UINT8("zns_num_lun", FemuCtrl, zns_params.zns_num_lun, 4),
    DEFINE_PROP_UINT64("zns_read", FemuCtrl, zns_params.zns_read, 40000),
    DEFINE_PROP_UINT64("zns_write", FemuCtrl, zns_params.zns_write, 200000),
    DEFINE_PROP_INT32("secsz", FemuCtrl, bb_params.secsz, 512),
    DEFINE_PROP_INT32("secs_per_pg", FemuCtrl, bb_params.secs_per_pg, 8),
    DEFINE_PROP_INT32("pgs_per_blk", FemuCtrl, bb_params.pgs_per_blk, 256),
    DEFINE_PROP_INT32("blks_per_pl", FemuCtrl, bb_params.blks_per_pl, 256),
    DEFINE_PROP_INT32("pls_per_lun", FemuCtrl, bb_params.pls_per_lun, 1),
    DEFINE_PROP_INT32("luns_per_ch", FemuCtrl, bb_params.luns_per_ch, 8),
    DEFINE_PROP_INT32("nchs", FemuCtrl, bb_params.nchs, 8),
    DEFINE_PROP_INT32("pg_rd_lat", FemuCtrl, bb_params.pg_rd_lat, 40000),
    DEFINE_PROP_INT32("pg_wr_lat", FemuCtrl, bb_params.pg_wr_lat, 200000),
    DEFINE_PROP_INT32("blk_er_lat", FemuCtrl, bb_params.blk_er_lat, 2000000),
    DEFINE_PROP_INT32("ch_xfer_lat", FemuCtrl, bb_params.ch_xfer_lat, 0),
    DEFINE_PROP_INT32("gc_thres_pcent", FemuCtrl, bb_params.gc_thres_pcent, 75),
    DEFINE_PROP_INT32("gc_thres_pcent_high", FemuCtrl, bb_params.gc_thres_pcent_high, 95),
    DEFINE_PROP_STRING("fdp.ruhs", NvmeNamespace, params.fdp.ruhs),
    DEFINE_PROP_LINK("subsys", FemuCtrl, subsys, TYPE_NVME_SUBSYS,
                     NvmeSubsystem *),    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription femu_vmstate = {
    .name = "femu",
    .unmigratable = 1,
};

static void femu_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->realize = femu_realize;
    pc->exit = femu_exit;
    pc->class_id = PCI_CLASS_STORAGE_EXPRESS;
    pc->vendor_id = PCI_VENDOR_ID_INTEL;
    pc->device_id = 0x5845;
    pc->revision = 2;

    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    dc->desc = "FEMU Non-Volatile Memory Express";
    device_class_set_props(dc, femu_props);
    dc->vmsd = &femu_vmstate;
}

static const TypeInfo femu_info = {
    .name          = "femu",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(FemuCtrl),
    .class_init    = femu_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { }
    },
};

static void femu_register_types(void)
{
    type_register_static(&femu_info);
}

type_init(femu_register_types)

static void nvme_subsys_register_types(void)
{
    type_register_static(&nvme_subsys_info);
}

type_init(nvme_subsys_register_types)   //Note Does this affects the "type_init(femu_register_types)" in the last line of this file?
