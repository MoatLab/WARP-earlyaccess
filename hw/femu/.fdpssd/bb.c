#include "../nvme.h"
#include "./ftl.h"

static void bb_init_ctrl_str(FemuCtrl *n)
{
    static int fsid_vbb = 0;
    const char *vbbssd_mn = "FEMU Cylon NVMe Flexible Data Placement SSD Controller";
    const char *vbbssd_sn = "vSSD";

    nvme_set_ctrl_name(n, vbbssd_mn, vbbssd_sn, &fsid_vbb);
}

/* bb <=> black-box */
static void bb_init(FemuCtrl *n, Error **errp)
{
    struct ssd *ssd = n->ssd = g_malloc0(sizeof(struct ssd));

    bb_init_ctrl_str(n);

    ssd->dataplane_started_ptr = &n->dataplane_started;
    ssd->ssdname = (char *)n->devname;
    femu_debug("Starting FEMU in FEMU Cylon NVMe Flexible Data Placement SSD mode ...\n");
    ssd_init(n);
}

static void bb_flip(FemuCtrl *n, NvmeCmd *cmd)
{
    struct ssd *ssd = n->ssd;
    int64_t cdw10 = le64_to_cpu(cmd->cdw10);

    switch (cdw10) {
    case FEMU_ENABLE_GC_DELAY:
        ssd->sp.enable_gc_delay = true;
        femu_log("%s,FEMU GC Delay Emulation [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_GC_DELAY:
        ssd->sp.enable_gc_delay = false;
        femu_log("%s,FEMU GC Delay Emulation [Disabled]!\n", n->devname);
        break;
    case FEMU_ENABLE_DELAY_EMU:
        ssd->sp.pg_rd_lat = NAND_READ_LATENCY;
        ssd->sp.pg_wr_lat = NAND_PROG_LATENCY;
        ssd->sp.blk_er_lat = NAND_ERASE_LATENCY;
        ssd->sp.ch_xfer_lat = 0;
        femu_log("%s,FEMU Delay Emulation [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_DELAY_EMU:
        ssd->sp.pg_rd_lat = 0;
        ssd->sp.pg_wr_lat = 0;
        ssd->sp.blk_er_lat = 0;
        ssd->sp.ch_xfer_lat = 0;
        femu_log("%s,FEMU Delay Emulation [Disabled]!\n", n->devname);
        break;
    case FEMU_RESET_ACCT:
        n->nr_tt_ios = 0;
        n->nr_tt_late_ios = 0;
        femu_log("%s,Reset tt_late_ios/tt_ios,%lu/%lu\n", n->devname,
                n->nr_tt_late_ios, n->nr_tt_ios);
        break;
    case FEMU_ENABLE_LOG:
        n->print_log = true;
        femu_log("%s,Log print [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_LOG:
        n->print_log = false;
        femu_log("%s,Log print [Disabled]!\n", n->devname);
        break;
    // case FEMU_ENABLE_FDP:
    //        n->print_log = true;
    //        femu_log("%s,Log print [Enabled]!\n", n->devname);
    // case FEMU_DISALBLE_FDP:
    //       n->print_log = true;
    //       femu_log("%s,Log print [Disabled]!\n", n->devname);

    default:
        printf("FEMU:%s,Not implemented flip cmd (%lu)\n", n->devname, cdw10);
    }
}

static uint16_t bb_nvme_rw(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    return nvme_rw(n, ns, cmd, req);
}

static uint16_t bb_io_cmd(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                          NvmeRequest *req)
{
    switch (cmd->opcode) {
    case NVME_CMD_READ:
        return bb_nvme_rw(n, ns, cmd, req);
    case NVME_CMD_WRITE:
        /** 
        if(ns->endgrp && ns->endgrp->fdp.enabled) {
            femu_debug("bb_io_cmd fdp write here \n");
             * 
             * If more ns is in the device, then "nvme_check_bounds()" 
             * have to be called before nvme_do_write_fdp
             * to check whether the slba and nlb is valid for the namespace 
             * that gonna write
             * 
             * 
            nvme_do_write_fdp(n, req, req->slba, req->nlb);
        }*/
        return bb_nvme_rw(n, ns, cmd, req);
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static uint16_t bb_admin_cmd(FemuCtrl *n, NvmeCmd *cmd)
{

    // fdp_debug("   typedef struct NvmeCmd {\n");
    // fdp_debug("       uint8_t    opcode       %d\n", cmd->opcode);
    // fdp_debug("       uint8_t     flags       %d\n", cmd->flags);
    // fdp_debug("       uint16_t    cid         %d\n", cmd->cid);
    // fdp_debug("       uint32_t    nsid        %u\n", cmd->nsid);
    // fdp_debug("       uint64_t    res2        %lu\n",cmd->res2);
    // fdp_debug("       uint64_t    mptr        %lu\n",cmd->mptr);
    // fdp_debug("       NvmeCmdDptr dptr            \n");
    // fdp_debug("           uint64_t    prp1        %lu\n", cmd->dptr.prp1);
    // fdp_debug("           uint64_t    prp2        %lu\n",cmd->dptr.prp2);
    // fdp_debug("           NvmeSglDescriptor sgl           \n");
    // fdp_debug("                   uint64_t addr           %lu\n",cmd->dptr.sgl.addr);
    // fdp_debug("                   uint32_t len            %u\n",cmd->dptr.sgl.len);
    // fdp_debug("                   uint8_t  rsvd[3]        %d %d %d\n",cmd->dptr.sgl.rsvd[0],cmd->dptr.sgl.rsvd[1],cmd->dptr.sgl.rsvd[2]);
    // fdp_debug("                   uint8_t  type           %d\n",cmd->dptr.sgl.type);  
    // fdp_debug("       uint32_t    cdw10       %u\n",cmd->cdw10);
    // fdp_debug("       uint32_t    cdw11       %u\n",cmd->cdw11);
    // fdp_debug("       uint32_t    cdw12       %u\n",cmd->cdw12);
    // fdp_debug("       uint32_t    cdw13       %u\n",cmd->cdw13);
    // fdp_debug("       uint32_t    cdw14       %u\n",cmd->cdw14);
    // fdp_debug("       uint32_t    cdw15       %u\n",cmd->cdw15);

    switch (cmd->opcode) {
    case NVME_ADM_CMD_FEMU_FLIP:
        bb_flip(n, cmd);
        return NVME_SUCCESS;
    case NVME_ADM_CMD_ASYNC_EV_REQ:
        femu_debug("\t\tNVME_ADM_CMD_ASYNC_EV_REQ %d\n", cmd->opcode);
        //return nvme_aer(n, req);
        return NVME_SUCCESS;
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

int nvme_register_bbssd(FemuCtrl *n)
{
    n->ext_ops = (FemuExtCtrlOps) {
        .state            = NULL,
        .init             = bb_init,
        .exit             = NULL,
        .rw_check_req     = NULL,
        .admin_cmd        = bb_admin_cmd,
        .io_cmd           = bb_io_cmd,
        .get_log          = NULL,
    };

    return 0;
}
