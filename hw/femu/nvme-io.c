#include "./nvme.h"

//FILE *fp, *fp2, *fp3, *fp4, *fp5, *fp6;
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>

static uint16_t nvme_io_cmd(FemuCtrl *n, NvmeCmd *cmd, NvmeRequest *req);
static inline uint64_t nvme_get_timestamp(FemuCtrl *n);

static inline void nvme_set_timestamp(FemuCtrl *n, uint64_t ts)
{
    //trace_pci_nvme_setfeat_timestamp(ts);

    n->host_timestamp = le64_to_cpu(ts);
    n->timestamp_set_qemu_clock_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
}

static inline uint64_t nvme_get_timestamp(FemuCtrl *n)
{
    uint64_t current_time = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed_time = current_time - n->timestamp_set_qemu_clock_ms;

    union nvme_timestamp {
        struct {
            uint64_t timestamp:48;
            uint64_t sync:1;
            uint64_t origin:3;
            uint64_t rsvd1:12;
        };
        uint64_t all;
    };

    union nvme_timestamp ts;
    ts.all = 0;
    ts.timestamp = n->host_timestamp + elapsed_time;

    /* If the host timestamp is non-zero, set the timestamp origin */
    ts.origin = n->host_timestamp ? 0x01 : 0x00;

    //trace_pci_nvme_getfeat_timestamp(ts.all);

    return cpu_to_le64(ts.all);
}
static void nvme_update_sq_eventidx(const NvmeSQueue *sq)
{
    if (sq->eventidx_addr_hva) {
        *((uint32_t *)(sq->eventidx_addr_hva)) = sq->tail;
        return;
    }

    if (sq->eventidx_addr) {
        nvme_addr_write(sq->ctrl, sq->eventidx_addr, (void *)&sq->tail,
                        sizeof(sq->tail));
    }
}

static inline void nvme_copy_cmd(NvmeCmd *dst, NvmeCmd *src)
{
#if defined(__AVX__)
    __m256i *d256 = (__m256i *)dst;
    const __m256i *s256 = (const __m256i *)src;

    _mm256_store_si256(&d256[0], _mm256_load_si256(&s256[0]));
    _mm256_store_si256(&d256[1], _mm256_load_si256(&s256[1]));
#elif defined(__SSE2__)
    __m128i *d128 = (__m128i *)dst;
    const __m128i *s128 = (const __m128i *)src;

    _mm_store_si128(&d128[0], _mm_load_si128(&s128[0]));
    _mm_store_si128(&d128[1], _mm_load_si128(&s128[1]));
    _mm_store_si128(&d128[2], _mm_load_si128(&s128[2]));
    _mm_store_si128(&d128[3], _mm_load_si128(&s128[3]));
#else
    *dst = *src;
#endif
}

static void nvme_process_sq_io(void *opaque, int index_poller)
{
    NvmeSQueue *sq = opaque;
    FemuCtrl *n = sq->ctrl;

    uint16_t status;
    hwaddr addr;
    NvmeCmd cmd;
    NvmeRequest *req;
    int processed = 0;

    nvme_update_sq_tail(sq);
    while (!(nvme_sq_empty(sq))) {
        if (sq->phys_contig) {
            addr = sq->dma_addr + sq->head * n->sqe_size;
            nvme_copy_cmd(&cmd, (void *)&(((NvmeCmd *)sq->dma_addr_hva)[sq->head]));
        } else {
            addr = nvme_discontig(sq->prp_list, sq->head, n->page_size,
                                  n->sqe_size);
            nvme_addr_read(n, addr, (void *)&cmd, sizeof(cmd));
        }
        nvme_inc_sq_head(sq);
        //femu_debug("nvme_process_sq_io here \n");
        req = QTAILQ_FIRST(&sq->req_list);
        QTAILQ_REMOVE(&sq->req_list, req, entry);
        memset(&req->cqe, 0, sizeof(req->cqe));
        /* Coperd: record req->stime at earliest convenience */
        req->expire_time = req->stime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        req->cqe.cid = cmd.cid;
        req->cmd_opcode = cmd.opcode;
        memcpy(&req->cmd, &cmd, sizeof(NvmeCmd));

        if (n->print_log) {
            femu_debug("%s,cid:%d\n", __func__, cmd.cid);
        }

        status = nvme_io_cmd(n, &cmd, req);
        if (1 && status == NVME_SUCCESS) {
            req->status = status;

            int rc = femu_ring_enqueue(n->to_ftl[index_poller], (void *)&req, 1);
            if (rc != 1) {
                femu_err("enqueue failed, ret=%d\n", rc);
            }
        } else if (status == NVME_SUCCESS) {
            /* Normal I/Os that don't need delay emulation */
            req->status = status;
        } else {
            femu_err("Error IO processed!\n");
        }

        processed++;
    }

    nvme_update_sq_eventidx(sq);
    sq->completed += processed;
}

static void nvme_post_cqe(NvmeCQueue *cq, NvmeRequest *req)
{
    FemuCtrl *n = cq->ctrl;
    NvmeSQueue *sq = req->sq;
    NvmeCqe *cqe = &req->cqe;
    uint8_t phase = cq->phase;
    hwaddr addr;

    if (n->print_log) {
        femu_debug("%s,req,lba:%lu,lat:%lu\n", n->devname, req->slba, req->reqlat);
    }
    cqe->status = cpu_to_le16((req->status << 1) | phase);
    cqe->sq_id = cpu_to_le16(sq->sqid);
    cqe->sq_head = cpu_to_le16(sq->head);

    if (cq->phys_contig) {
        addr = cq->dma_addr + cq->tail * n->cqe_size;
        ((NvmeCqe *)cq->dma_addr_hva)[cq->tail] = *cqe;
    } else {
        addr = nvme_discontig(cq->prp_list, cq->tail, n->page_size, n->cqe_size);
        nvme_addr_write(n, addr, (void *)cqe, sizeof(*cqe));
    }

    nvme_inc_cq_tail(cq);
}

static void nvme_process_cq_cpl(void *arg, int index_poller)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    NvmeCQueue *cq = NULL;
    NvmeRequest *req = NULL;
    struct rte_ring *rp = n->to_ftl[index_poller];
    pqueue_t *pq = n->pq[index_poller];
    uint64_t now;
    int processed = 0;
    int rc;
    int i;

    if (BBSSD(n)) {
        rp = n->to_poller[index_poller];
    }

    while (femu_ring_count(rp)) {
        req = NULL;
        rc = femu_ring_dequeue(rp, (void *)&req, 1);
        if (rc != 1) {
            femu_err("dequeue from to_poller request failed\n");
        }
        assert(req);

        pqueue_insert(pq, req);
    }

    while ((req = pqueue_peek(pq))) {
        now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (now < req->expire_time) {
            break;
        }
        if(req == NULL){
            femu_debug(" req is NULL.  \n ");
            assert(req!=NULL);
        }
        if(req->sq == NULL){
            femu_debug(" req->opcode %d req->sq NULL\n", req->cmd.opcode );
            assert(req->sq!=NULL);
        }

        cq = n->cq[req->sq->sqid];
        if (!cq->is_active)
            continue;
        nvme_post_cqe(cq, req);
        QTAILQ_INSERT_TAIL(&req->sq->req_list, req, entry);
        pqueue_pop(pq);
        processed++;
        n->nr_tt_ios++;
        if (now - req->expire_time >= 20000) {
            n->nr_tt_late_ios++;
            if (n->print_log) {
                femu_debug("%s,diff,pq.count=%lu,%" PRId64 ", %lu/%lu\n",
                           n->devname, pqueue_size(pq), now - req->expire_time,
                           n->nr_tt_late_ios, n->nr_tt_ios);
            }
        }
        n->should_isr[req->sq->sqid] = true;
    }

    if (processed == 0)
        return;

    switch (n->multipoller_enabled) {
    case 1:
        nvme_isr_notify_io(n->cq[index_poller]);
        break;
    default:
        for (i = 1; i <= n->nr_io_queues; i++) {
            if (n->should_isr[i]) {
                nvme_isr_notify_io(n->cq[i]);
                n->should_isr[i] = false;
            }
        }
        break;
    }
}

void *nvme_poller(void *arg)
{
    FemuCtrl *n = ((NvmePollerThreadArgument *)arg)->n;
    int index = ((NvmePollerThreadArgument *)arg)->index;
    int i;

    switch (n->multipoller_enabled) {
    case 1:
        while (1) {
            if ((!n->dataplane_started)) {
                usleep(1000);
                continue;
            }
            NvmeSQueue *sq = n->sq[index];
            NvmeCQueue *cq = n->cq[index];
            if (sq && sq->is_active && cq && cq->is_active) {
                nvme_process_sq_io(sq, index);
            }
            nvme_process_cq_cpl(n, index);
        }
        break;
    default:
        while (1) {
            if ((!n->dataplane_started)) {
                usleep(1000);
                continue;
            }

            for (i = 1; i <= n->nr_io_queues; i++) {
                NvmeSQueue *sq = n->sq[i];
                NvmeCQueue *cq = n->cq[i];
                if (sq && sq->is_active && cq && cq->is_active) {
                    //femu_err("nvme_poller : nvme_process_sq_io(sq, index %d\n);",index);
                    nvme_process_sq_io(sq, index);
                }
            }
            nvme_process_cq_cpl(n, index);
        }
        break;
    }

    return NULL;
}

uint16_t nvme_rw(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint16_t ctrl = le16_to_cpu(rw->control);
    uint32_t nlb  = le16_to_cpu(rw->nlb) + 1;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint64_t prp1 = le64_to_cpu(rw->prp1);
    uint64_t prp2 = le64_to_cpu(rw->prp2);
    const uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    const uint16_t ms = le16_to_cpu(ns->id_ns.lbaf[lba_index].ms);
    const uint8_t data_shift = ns->id_ns.lbaf[lba_index].lbads;
    uint64_t data_size = (uint64_t)nlb << data_shift;
    uint64_t data_offset = slba << data_shift;
    uint64_t meta_size = nlb * ms;
    uint64_t elba = slba + nlb;
    uint16_t err;
    int ret;

    req->is_write = (rw->opcode == NVME_CMD_WRITE) ? 1 : 0;

    err = femu_nvme_rw_check_req(n, ns, cmd, req, slba, elba, nlb, ctrl,
                                 data_size, meta_size);
    if (err)
        return err;
    
    //if (nvme_map_prp(&req->qsg, &req->iov, prp1, prp2, data_size, n)) {
    if (nvme_map_prp(n, &req->sg, prp1, prp2, data_size)){
        nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
                            offsetof(NvmeRwCmd, prp1), 0, ns->id);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    assert((nlb << data_shift) == req->sg.qsg.size);

    req->slba = slba;
    req->status = NVME_SUCCESS;
    req->nlb = nlb;
    //ret = backend_rw(n->mbe, &req->qsg, &data_offset, req->is_write); // deceprecated 
    ret = backend_rw(n->mbe, &req->sg.qsg, &data_offset, req->is_write);
    if (!ret) {
        return NVME_SUCCESS;
    }

    return NVME_DNR;
}

static uint16_t nvme_dsm(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                         NvmeRequest *req)
{
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t dw11 = le32_to_cpu(cmd->cdw11);
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    int i;

    if (dw11 & NVME_DSMGMT_AD) {
        uint16_t nr = (dw10 & 0xff) + 1;

        uint64_t slba;
        uint32_t nlb;
        NvmeDsmRange range[nr];

        if (dma_write_prp(n, (uint8_t *)range, sizeof(range), prp1, prp2)) {
            nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
                                offsetof(NvmeCmd, dptr.prp1), 0, ns->id);
            return NVME_INVALID_FIELD | NVME_DNR;
        }

        req->status = NVME_SUCCESS;
        for (i = 0; i < nr; i++) {
            slba = le64_to_cpu(range[i].slba);
            nlb = le32_to_cpu(range[i].nlb);
            if (slba + nlb > le64_to_cpu(ns->id_ns.nsze)) {
                nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_LBA_RANGE,
                                    offsetof(NvmeCmd, cdw10), slba + nlb, ns->id);
                return NVME_LBA_RANGE | NVME_DNR;
            }

            bitmap_clear(ns->util, slba, nlb);
        }
    }

    return NVME_SUCCESS;
}

static uint16_t nvme_compare(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                             NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint32_t nlb  = le16_to_cpu(rw->nlb) + 1;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint64_t prp1 = le64_to_cpu(rw->prp1);
    uint64_t prp2 = le64_to_cpu(rw->prp2);
    int i;

    uint64_t elba = slba + nlb;
    uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    uint8_t data_shift = ns->id_ns.lbaf[lba_index].lbads;
    uint64_t data_size = nlb << data_shift;
    uint64_t offset  = ns->start_block + (slba << data_shift);

    if ((slba + nlb) > le64_to_cpu(ns->id_ns.nsze)) {
        nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_LBA_RANGE,
                            offsetof(NvmeRwCmd, nlb), elba, ns->id);
        return NVME_LBA_RANGE | NVME_DNR;
    }
    if (n->id_ctrl.mdts && data_size > n->page_size * (1 << n->id_ctrl.mdts)) {
        nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
                            offsetof(NvmeRwCmd, nlb), nlb, ns->id);
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    //if (nvme_map_prp(&req->qsg, &req->iov, prp1, prp2, data_size, n)) {
    if(nvme_map_prp(n , &req->sg, prp1, prp2, data_size)){
        nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
                            offsetof(NvmeRwCmd, prp1), 0, ns->id);
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (find_next_bit(ns->uncorrectable, elba, slba) < elba) {
        return NVME_UNRECOVERED_READ;
    }

    for (i = 0; i < req->qsg.nsg; i++) {
        uint32_t len = req->qsg.sg[i].len;
        uint8_t tmp[2][len];

        nvme_addr_read(n, req->qsg.sg[i].base, tmp[1], len);
        if (memcmp(tmp[0], tmp[1], len)) {
            qemu_sglist_destroy(&req->qsg);
            return NVME_CMP_FAILURE;
        }
        offset += len;
    }

    qemu_sglist_destroy(&req->qsg);

    return NVME_SUCCESS;
}

static uint16_t nvme_flush(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    return NVME_SUCCESS;
}

static uint16_t nvme_write_zeros(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                                 NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb  = le16_to_cpu(rw->nlb) + 1;

    if ((slba + nlb) > ns->id_ns.nsze) {
        nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_LBA_RANGE,
                            offsetof(NvmeRwCmd, nlb), slba + nlb, ns->id);
        return NVME_LBA_RANGE | NVME_DNR;
    }

    return NVME_SUCCESS;
}

static uint16_t nvme_write_uncor(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                                 NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb  = le16_to_cpu(rw->nlb) + 1;

    if ((slba + nlb) > ns->id_ns.nsze) {
        nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_LBA_RANGE,
                            offsetof(NvmeRwCmd, nlb), slba + nlb, ns->id);
        return NVME_LBA_RANGE | NVME_DNR;
    }

    bitmap_set(ns->uncorrectable, slba, nlb);

    return NVME_SUCCESS;
}
static inline uint16_t nvme_make_pid(NvmeNamespace *ns, uint16_t rg,
                                     uint16_t ph)
{
    uint16_t rgif = ns->endgrp->fdp.rgif;

    if (!rgif) {
        return ph;
    }

    return (rg << (16 - rgif)) | ph;
}

static inline bool nvme_ph_valid(NvmeNamespace *ns, uint16_t ph)
{
    return ph < ns->fdp.nphs;
}

static inline bool nvme_rg_valid(NvmeEnduranceGroup *endgrp, uint16_t rg)
{
    return rg < endgrp->fdp.nrg;
}

uint16_t nvme_pid2ph(NvmeNamespace *ns, uint16_t pid)
{
    uint16_t rgif = ns->endgrp->fdp.rgif;

    if (!rgif) {
        return pid;
    }

    return pid & ((1 << (15 - rgif)) - 1);
}

uint16_t nvme_pid2rg(NvmeNamespace *ns, uint16_t pid)
{
    uint16_t rgif = ns->endgrp->fdp.rgif;

    if (!rgif) {
        return 0;
    }

    return pid >> (16 - rgif);
}

bool nvme_parse_pid(NvmeNamespace *ns, uint16_t pid,
                                  uint16_t *ph, uint16_t *rg)
{
    *rg = nvme_pid2rg(ns, pid);
    *ph = nvme_pid2ph(ns, pid);

    return nvme_ph_valid(ns, *ph) && nvme_rg_valid(ns->endgrp, *rg);
}

static NvmeFdpEvent *nvme_fdp_alloc_event(FemuCtrl *n, NvmeFdpEventBuffer *ebuf)
{
    NvmeFdpEvent *ret = NULL;
    bool is_full = ebuf->next == ebuf->start && ebuf->nelems;

    ret = &ebuf->events[ebuf->next++];
    if (unlikely(ebuf->next == NVME_FDP_MAX_EVENTS)) {
        ebuf->next = 0;
    }
    if (is_full) {
        ebuf->start = ebuf->next;
    } else {
        ebuf->nelems++;
    }

    memset(ret, 0, sizeof(NvmeFdpEvent));
    ret->timestamp = nvme_get_timestamp(n);

    return ret;
}
static inline int log_event(NvmeRuHandle *ruh, uint8_t event_type)
{
    //return (ruh->event_filter >> nvme_fdp_evf_shifts[event_type]) & 0x1;
    return (ruh->event_filter >> nvme_fdp_evf_shifts[event_type]) & 0xff;

}

 bool nvme_update_ruh(FemuCtrl *n, NvmeNamespace *ns, uint16_t pid)
{
    NvmeEnduranceGroup *endgrp = ns->endgrp;
    NvmeRuHandle *ruh;
    NvmeReclaimUnit *ru;
    NvmeFdpEvent *e = NULL;
    uint16_t ph, rg, ruhid;

    if (!nvme_parse_pid(ns, pid, &ph, &rg)) {
        return false;
    }

    ruhid = ns->fdp.phs[ph];

    ruh = &endgrp->fdp.ruhs[ruhid];
    //ru = &ruh->rus[rg];
    ru = ruh->rus[rg];
    femu_log("ruh->event_filter %lu (>> nvme_fdp_evf_shifts[NFW]%u &0xff = %u) \n", \
                                            ruh->event_filter,nvme_fdp_evf_shifts[FDP_EVT_RU_NOT_FULLY_WRITTEN],\
                                            nvme_fdp_evf_shifts[FDP_EVT_RU_NOT_FULLY_WRITTEN] & 0xff);
    femu_log("ruh->event_filter %lu (>> nvme_fdp_evf_shifts[MRE]%u &0xff = %u) \n", \
                                            ruh->event_filter,nvme_fdp_evf_shifts[FDP_EVT_MEDIA_REALLOC],\
                                            nvme_fdp_evf_shifts[FDP_EVT_MEDIA_REALLOC] & 0xff);

    if (ru->ruamw) {
        //FDP_EVT_RU_NOT_FULLY_WRITTEN: 0                              //FDP_EVT_MEDIA_REALLOC:32
        if (log_event(ruh, FDP_EVT_RU_NOT_FULLY_WRITTEN) || log_event(ruh, FDP_EVT_MEDIA_REALLOC)) {
            e = nvme_fdp_alloc_event(n, &endgrp->fdp.host_events);
            
            if( log_event(ruh, FDP_EVT_MEDIA_REALLOC)){
                e->type = FDP_EVT_MEDIA_REALLOC;
                //fprintf(fp, "%u\n",FDP_EVT_MEDIA_REALLOC);
            
            }
            else{
                //fprintf(fp, "%u\n",FDP_EVT_RU_NOT_FULLY_WRITTEN);
                e->type = FDP_EVT_RU_NOT_FULLY_WRITTEN;
            
            }
            
            e->flags = FDPEF_PIV | FDPEF_NSIDV | FDPEF_LV;
            e->pid = cpu_to_le16(pid);
            e->nsid = cpu_to_le32(ns->params.nsid);
            e->rgid = cpu_to_le16(rg);
            e->ruhid = cpu_to_le16(ruhid);
           // fprintf(fp, "%u\n",FDP_EVT_RU_NOT_FULLY_WRITTEN);
        }

        /* log (eventual) GC overhead of prematurely swapping the RU */
        //mbmw stands for mb of media written, which contains both write from the host and device itself

        //femu - call gc func here
        //nvme_fdp_stat_inc(&endgrp->fdp.mbmw, nvme_l2b(ns, ru->ruamw));//?
    }

    ru->ruamw = ruh->ruamw; //keep using different RU so the ru->ruamw is shifted or erased..

    return true;
}

static uint16_t nvme_io_mgmt_recv_ruhs(FemuCtrl *n, NvmeRequest *req,
                                       size_t len)
{
    NvmeNamespace *ns = req->ns;
    NvmeEnduranceGroup *endgrp;
    NvmeRuhStatus *hdr;
    NvmeRuhStatusDescr *ruhsd;
    unsigned int nruhsd;
    uint16_t rg, ph, *ruhid;
    size_t trans_len;
    g_autofree uint8_t *buf = NULL;
    femu_debug("        femu-nvme nvme_io_mgmt_recv_ruhs    \n");

    if (!n->subsys) {
        femu_debug("  REPORT : NVME_INVALID_FIELD | NVME_DNR;   (!n->subsys) n->subsys is NULL  \n");
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    //if (ns->params.nsid == 0 || ns->params.nsid == 0xffffffff) {
    if(ns->id == 0 || ns->id == 0xffffffff) {
        femu_debug("  REPORT : NVME_INVALID_FIELD | NVME_DNR;   ns->id == 0 || ns->id == 0xffffffff  \n");
        return NVME_INVALID_NSID | NVME_DNR;
    }

    if (!n->subsys->endgrp.fdp.enabled) {
        femu_debug("  REPORT : NVME_INVALID_FIELD | NVME_DNR;  (!n->subsys->endgrp.fdp.enabled) n->subsys->endgrp.fdp.enabled FALSE \n");
        return NVME_FDP_DISABLED | NVME_DNR;
    }
    if (!ns->endgrp) {
        femu_debug("  REPORT : NVME_INVALID_FIELD | NVME_DNR;   (!ns->endgrp) ns->endgrp is NULL  \n");
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    
    femu_debug("\t\t\t  endgrp = ns->endgrp(enabled : %d) \n", ns->endgrp->fdp.enabled);
    endgrp = ns->endgrp;
    femu_debug("\t\t\t  ns->fdp.nphs %d ",ns->fdp.nphs);            ///0 ???
    femu_debug("\t\t\t  endgrp->fdp.nrg %d; \n",endgrp->fdp.nrg);        

    nruhsd = ns->fdp.nphs * endgrp->fdp.nrg;                          //HERE. but still dn't understand
    femu_debug("\t\t\t      1 nruhsd = ns->fdp.nphs * endgrp->fdp.nrg; fin\n");
    trans_len = sizeof(NvmeRuhStatus) + nruhsd * sizeof(NvmeRuhStatusDescr);
    femu_debug("\t\t\t      2 trans_len = sizeof(NvmeRuhStatus) + nruhsd * sizeof(NvmeRuhStatusDescr); fin\n");
    buf = g_malloc(trans_len);
    femu_debug("\t\t\t      3  buf = g_malloc(trans_len); fin\n");

    trans_len = MIN(trans_len, len);
    femu_debug("\t\t\t      4  trans_len = MIN(trans_len, len); fin\n");

    hdr = (NvmeRuhStatus *)buf;
    ruhsd = (NvmeRuhStatusDescr *)(buf + sizeof(NvmeRuhStatus));
    femu_debug("\t\t\thdr->nruhsd = cpu_to_le16(nruhsd); hdr->nruhsd : %u\n",cpu_to_le16(nruhsd));
    hdr->nruhsd = cpu_to_le16(nruhsd);
    femu_debug("\t\t\t hdr->nruhsd fin\n" );

    if(!ns->fdp.phs){
        femu_err("\t\t\t\t (!ns->fdp.phs) ns->fdp.phs is NULL");
    }
    femu_debug("\t\t\t  ruhid = ns->fdp.phs; ");
    ruhid = ns->fdp.phs;
    femu_debug("\t\t\t start for (ph = 0; ph < ns->fdp.nphs; ph++, ruhid++). ruhid %u \n",*ruhid);

    for (ph = 0; ph < ns->fdp.nphs; ph++, ruhid++) {
        NvmeRuHandle *ruh = &endgrp->fdp.ruhs[*ruhid];

        for (rg = 0; rg < endgrp->fdp.nrg; rg++, ruhsd++) {
            uint16_t pid = nvme_make_pid(ns, rg, ph);
            NvmeReclaimUnit *ru = ruh->rus[rg];
            ruhsd->pid = cpu_to_le16(pid);
            ruhsd->ruhid = *ruhid;
            ruhsd->earutr = 0;
            ruhsd->ruamw = cpu_to_le64(ru->ruamw);
        }
    }
    femu_debug("\t\t\tfin for (ph = 0; ph < ns->fdp.nphs; ph++, ruhid++). \n nvme_c2h(n, buf, trans_len, req);\n");
    NvmeCmd *cmd = &req->cmd;
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    return dma_read_prp(n, (uint8_t *)buf, trans_len, prp1, prp2);
    //return nvme_c2h(n, buf, trans_len, req);
}

static uint16_t nvme_io_mgmt_recv(FemuCtrl *n, NvmeRequest *req)
{
    NvmeCmd *cmd = &req->cmd;
    uint32_t cdw10 = le32_to_cpu(cmd->cdw10);
    uint32_t numd = le32_to_cpu(cmd->cdw11);
    uint8_t mo = (cdw10 & 0xff);
    size_t len = (numd + 1) << 2;

    switch (mo) {
    case NVME_IOMR_MO_NOP:
        femu_debug("    femu-nvme nvme_io_mgmt_recv NVME_IOMR_MO_NOP return 0;\n");
        return 0;
    case NVME_IOMR_MO_RUH_STATUS:
        femu_debug("    femu-nvme nvme_io_mgmt_recv NVME_IOMR_MO_RUH_STATUS nvme_io_mgmt_recv_ruhs(n, req, len);\n");
        return nvme_io_mgmt_recv_ruhs(n, req, len);
    default:
        femu_debug("    femu-nvme nvme_io_mgmt_recv NVME_INVALID_FIELD | NVME_DNR;\n");
        return NVME_INVALID_FIELD | NVME_DNR;
    };
}

static uint16_t nvme_io_mgmt_send_ruh_update(FemuCtrl *n, NvmeRequest *req)
{
    NvmeCmd *cmd = &req->cmd;
    NvmeNamespace *ns = req->ns;
    uint32_t cdw10 = le32_to_cpu(cmd->cdw10);
    uint16_t ret = NVME_SUCCESS;
    uint32_t npid = (cdw10 >> 1) + 1;
    unsigned int i = 0;
    g_autofree uint16_t *pids = NULL;
    uint32_t maxnpid = n->subsys->endgrp.fdp.nrg * n->subsys->endgrp.fdp.nruh;

    if (unlikely(npid >= MIN(NVME_FDP_MAXPIDS, maxnpid))) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    pids = g_new(uint16_t, npid);

    ret = nvme_h2c(n, pids, npid * sizeof(uint16_t), req);
    if (ret) {
        return ret;
    }

    for (; i < npid; i++) {
        if (!nvme_update_ruh(n, ns, pids[i])) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }
    }

    return ret;
}

static uint16_t nvme_io_mgmt_send(FemuCtrl *n, NvmeRequest *req)
{
    NvmeCmd *cmd = &req->cmd;
    uint32_t cdw10 = le32_to_cpu(cmd->cdw10);
    uint8_t mo = (cdw10 & 0xff);

    switch (mo) {
    case NVME_IOMS_MO_NOP:
        return 0;
    case NVME_IOMS_MO_RUH_UPDATE:
        return nvme_io_mgmt_send_ruh_update(n, req);
    default:
        return NVME_INVALID_FIELD | NVME_DNR;
    };
}
static uint16_t nvme_io_cmd(FemuCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    NvmeNamespace *ns;
    uint32_t nsid = le32_to_cpu(cmd->nsid);

    if (nsid == 0 || nsid > n->num_namespaces) {
        femu_err("%s, NVME_INVALID_NSID %" PRIu32 "\n", __func__, nsid);
        return NVME_INVALID_NSID | NVME_DNR;
    }

    req->ns = ns = &n->namespaces[nsid - 1];

    switch (cmd->opcode) {
    case NVME_CMD_FLUSH:
        if (!n->id_ctrl.vwc || !n->features.volatile_wc) {
            femu_debug(" nvme_io_cmd NVME_CMD_FLUSH NVME_SUCCESS\n");
            return NVME_SUCCESS;
        }
        femu_debug(" nvme_io_cmd NVME_CMD_FLUSH nvme_flush(n, ns, cmd, req);\n");
        return nvme_flush(n, ns, cmd, req);
    case NVME_CMD_DSM:
        if (NVME_ONCS_DSM & n->oncs) {
            femu_debug(" nvme_io_cmd NVME_CMD_DSM\n");
            return nvme_dsm(n, ns, cmd, req);
        }
        return NVME_INVALID_OPCODE | NVME_DNR;
    case NVME_CMD_COMPARE:
        femu_debug(" nvme_io_cmd NVME_CMD_COMPARE\n");
        if (NVME_ONCS_COMPARE & n->oncs) {
            return nvme_compare(n, ns, cmd, req);
        }
        return NVME_INVALID_OPCODE | NVME_DNR;
    case NVME_CMD_WRITE_ZEROES:
        femu_debug(" nvme_io_cmd NVME_CMD_WRITE_ZEROES\n");
        if (NVME_ONCS_WRITE_ZEROS & n->oncs) {
            return nvme_write_zeros(n, ns, cmd, req);
        }
        return NVME_INVALID_OPCODE | NVME_DNR;
    case NVME_CMD_WRITE_UNCOR:
        femu_debug(" nvme_io_cmd NVME_CMD_WRITE_UNCOR\n");
        if (NVME_ONCS_WRITE_UNCORR & n->oncs) {
            return nvme_write_uncor(n, ns, cmd, req);
        }
        return NVME_INVALID_OPCODE | NVME_DNR;
    case NVME_CMD_IO_MGMT_SEND:
        femu_debug(" nvme_io_cmd NVME_CMD_IO_MGMT_SEND\n");
        return nvme_io_mgmt_send(n, req);
    case NVME_CMD_IO_MGMT_RECV:
        femu_debug(" nvme_io_cmd NVME_CMD_IO_MGMT_RECV\n");
        return nvme_io_mgmt_recv(n, req);
    case NVME_CMD_IO_WAF_PER_RUH : 
        femu_debug(" nvme_io_cmd NVME_CMD_IO_WAF_PER_RUH\n ");
        return NVME_INVALID_OPCODE | NVME_DNR;
        //;
    default:
        if (n->ext_ops.io_cmd) {
            return n->ext_ops.io_cmd(n, ns, cmd, req);
        }

        femu_err("%s, NVME_INVALID_OPCODE\n", __func__);
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

void nvme_post_cqes_io(void *opaque)
{
    NvmeCQueue *cq = opaque;
    NvmeRequest *req, *next;
    int64_t cur_time, ntt = 0;
    int processed = 0;

    QTAILQ_FOREACH_SAFE(req, &cq->req_list, entry, next) {
        if (nvme_cq_full(cq)) {
            break;
        }

        cur_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (cq->cqid != 0 && cur_time < req->expire_time) {
            ntt = req->expire_time;
            break;
        }

        nvme_post_cqe(cq, req);
        processed++;
    }

    if (ntt == 0) {
        ntt = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + CQ_POLLING_PERIOD_NS;
    }

    /* Only interrupt guest when we "do" complete some I/Os */
    if (processed > 0) {
        nvme_isr_notify_io(cq);
    }
}
