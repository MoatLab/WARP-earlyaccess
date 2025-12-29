#include "ftl.h"

// #define FEMU_DEBUG_FTL
#define FEMU_FDP_LATENCY_DISABLE 0
#ifdef FEMU_DEBUG_FTL

#endif
static void *ftl_thread(void *arg);
static inline bool _should_gc_fdp_style(struct ssd *ssd, uint16_t rgidx)
{
    return (ssd->rg[rgidx].ru_mgmt->free_ru_cnt <= ssd->rg[rgidx].ru_mgmt->gc_thres_rus);
}
static inline int16_t should_gc_fdp_style(struct ssd *ssd)
{
    //
    if (ssd->nrg > 2)
    {
        for (int i = 0; i < ssd->nrg; i++)
        {
            if (_should_gc_fdp_style(ssd, i))
                return i;
        }
    }
    if (ssd->rg[0].ru_mgmt->free_ru_cnt <= ssd->rg[0].ru_mgmt->gc_thres_rus)
    {
        return 1;
    }

    return -1;
}
static inline bool _should_gc_high_fdp_style(struct ssd *ssd, uint16_t rgidx)
{
    return (ssd->rg[rgidx].ru_mgmt->free_ru_cnt <= ssd->rg[rgidx].ru_mgmt->gc_thres_rus_high);
}
static inline int should_gc_high_fdp_style(struct ssd *ssd)
{
    if (ssd->nrg > 2)
    {
        for (int i = 0; i < ssd->nrg; i++)
        {
            if (_should_gc_high_fdp_style(ssd, i))
                return i;
        }
    }
    if (ssd->rg[0].ru_mgmt->free_ru_cnt <= ssd->rg[0].ru_mgmt->gc_thres_rus_high)
    {
        ftl_debug(" [Do GC] INSIDE should_gc_high_fdp_style viccnt %d free_cnt : %lu thres : %lu {\n", ssd->rg[0].ru_mgmt->victim_ru_cnt, ssd->rg[0].ru_mgmt->free_ru_cnt, ssd->rg[0].ru_mgmt->gc_thres_rus_high);
        return 0;
    }

    return -1; // here, because -1 is true in boolean, crazy happened
}
static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    if(lpn >= ssd->sp.tt_pgs)
        ftl_err("   set_maptbl_ent lpn >= ssd->sp.tt_pg\n");
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch * spp->pgs_per_ch +
            ppa->g.lun * spp->pgs_per_lun +
            ppa->g.pl * spp->pgs_per_pl +
            ppa->g.blk * spp->pgs_per_blk +
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

static inline int victim_ru_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_ru_get_pri(void *a)
{
    ftl_assert(a != NULL);
    return ((FemuReclaimUnit *)a)->vpc;
}

static inline void victim_ru_set_pri(void *a, pqueue_pri_t pri)
{
    ((FemuReclaimUnit *)a)->vpc = pri;
}

/* cost-benefit ? */
static inline int victim_ru_cmp_pri_by_cb(pqueue_pri_t next, pqueue_pri_t curr)
{
    //next > curr //min heap, greedy, cb 
    return (next > curr); //min heap
}

static inline pqueue_pri_t victim_ru_get_pri_by_cb(void *a)
{
    return ((FemuReclaimUnit *)a)->my_cb;
}

static inline void victim_ru_set_pri_by_cb(void *a, pqueue_pri_t pri)
{
    ((FemuReclaimUnit *)a)->my_cb = pri;
}


static inline size_t victim_ru_get_pos(void *a)
{
    return ((FemuReclaimUnit *)a)->pos;
}

static inline void victim_ru_set_pos(void *a, size_t pos)
{
    ((FemuReclaimUnit *)a)->pos = pos;
}

static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
                                     victim_line_get_pri, victim_line_set_pri,
                                     victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++)
    {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    fdp_log(" lm->free_line_cnt init %d\n",lm->free_line_cnt);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

// static void ssd_init_write_pointer(struct ssd *ssd)
// {
//     struct write_pointer *wpp = &ssd->wp;
//     struct line_mgmt *lm = &ssd->lm;
//     struct line *curline = NULL;
//     curline = QTAILQ_FIRST(&lm->free_line_list);
//     QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
//     lm->free_line_cnt--;
//     /* wpp->curline is always our next-to-write super-block */
//     wpp->curline = curline;
//     wpp->ch = 0;
//     wpp->lun = 0;
//     wpp->pg = 0;
//     wpp->blk = 0;
//     wpp->pl = 0;
// }
static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline)
    {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

// static void ssd_advance_write_pointer(struct ssd *ssd)
// {
//     struct ssdparams *spp = &ssd->sp;
//     struct write_pointer *wpp = &ssd->wp;
//     struct line_mgmt *lm = &ssd->lm;

//     check_addr(wpp->ch, spp->nchs);
//     wpp->ch++;
//     if (wpp->ch == spp->nchs)
//     {
//         wpp->ch = 0;
//         check_addr(wpp->lun, spp->luns_per_ch);
//         wpp->lun++;
//         /* in this case, we should go to next lun */
//         if (wpp->lun == spp->luns_per_ch)
//         {
//             wpp->lun = 0;
//             /* go to next page in the block */
//             check_addr(wpp->pg, spp->pgs_per_blk);
//             wpp->pg++;
//             if (wpp->pg == spp->pgs_per_blk)
//             {
//                 wpp->pg = 0;
//                 /* move current line to {victim,full} line list */
//                 if (wpp->curline->vpc == spp->pgs_per_line)
//                 {
//                     /* all pgs are still valid, move to full line list */
//                     ftl_assert(wpp->curline->ipc == 0);
//                     QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
//                     lm->full_line_cnt++;
//                 }
//                 else
//                 {
//                     ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
//                     /* there must be some invalid pages in this line */
//                     ftl_assert(wpp->curline->ipc > 0);
//                     pqueue_insert(lm->victim_line_pq, wpp->curline);
//                     lm->victim_line_cnt++;
//                 }
//                 /* current line is used up, pick another empty line */
//                 check_addr(wpp->blk, spp->blks_per_pl);
//                 wpp->curline = NULL;
//                 wpp->curline = get_next_free_line(ssd);
//                 if (!wpp->curline)
//                 {
//                   ftl_err("gc_write_page_fdp_style -> ssd_advance_write_pointer here \n");
//                     /* TODO */
//                     abort();
//                 }
//                 wpp->blk = wpp->curline->id;
//                 check_addr(wpp->blk, spp->blks_per_pl);
//                 /* make sure we are starting from page 0 in the super block */
//                 ftl_assert(wpp->pg == 0);
//                 ftl_assert(wpp->lun == 0);
//                 ftl_assert(wpp->ch == 0);
//                 /* TODO: assume # of pl_per_lun is 1, fix later */
//                 ftl_assert(wpp->pl == 0);
//             }
//         }
//     }
// }
#ifdef SSD_STREAM_WRITE

static void fdp_inc_ru_write_pointer(struct ssd *ssd, FemuReclaimUnit *ru)
{
    struct write_pointer *wptr = ru->ssd_wptr;
    wptr->curline = ru->lines[ru->next_line_index];
    // wptr->curline = get_next_free_line(ssd);
    wptr->ch = 0;
    wptr->lun = 0;
    wptr->pg = 0;
    wptr->blk = wptr->curline->id;
    wptr->pl = 0;
    ru->next_line_index++;
}

static void fdp_set_ru_write_pointer(struct ssd *ssd, FemuReclaimUnit *ru)
{
    struct write_pointer *wptr = ru->ssd_wptr;
    ftl_assert(wptr != NULL);
    ftl_assert(ru->ssd_wptr->curline == ru->lines[0]);
    wptr->curline = ru->lines[0];
    // wptr->curline = get_next_free_line(ssd);
    wptr->ch = 0;
    wptr->lun = 0;
    wptr->pg = 0;
    wptr->blk = wptr->curline->id;
    wptr->pl = 0;
}

static FemuReclaimUnit *get_next_free_ru(struct ssd *ssd, FemuReclaimGroup *rg)
{
    struct ru_mgmt *rm = rg->ru_mgmt;
    // struct line_mgmt *lm = &ssd->lm;
    FemuReclaimUnit *ru = NULL;
    //ftl_log("     INSIDE get_next_free_ru\n");

    ru = QTAILQ_FIRST(&rm->free_ru_list);
    if (!ru)
    {
        ftl_err("No free Reclaim Unit left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
        /*More safe approach?*/
    }

    QTAILQ_REMOVE(&rm->free_ru_list, ru, entry);
    rm->free_ru_cnt--;

    if (ru->lines == NULL)
    {
        //TODO Current assumption : we don't return line when GCed. So this behavior is error
        ftl_err("get_next_free_ru, ru->lines == NULL  !!!!\n");
        abort();
        for (int i = 0; i < ru->n_lines; ++i)
        {
            ru->lines[i] = get_next_free_line(ssd);
        }
    }
    ru->last_init_time = qemu_clock_get_us(QEMU_CLOCK_REALTIME); 
    ru->chance_token = 3;

    return ru;
}
// IH; Do we need namespace param for future support? - multi namespace, direct ru indexing, fdp ns fields, and so on.
static FemuReclaimUnit *fdp_get_new_ru(struct ssd *ssd, uint16_t rgidx, uint16_t ruhid)
{
    FemuRuHandle *eruh = &ssd->ruhs[ruhid];
    FemuReclaimGroup *rg = &ssd->rg[rgidx];
    FemuReclaimUnit *new_ru = NULL;
    //ftl_log("     INSIDE fdp_get_new_ru\n");
    if ((new_ru = get_next_free_ru(ssd, rg)) == NULL)
    {
        ftl_err("NO reclaim Unit. Device is full error\n");

        return NULL;
    }
    new_ru->rgidx = rgidx;
    new_ru->ruh = eruh;
    // Step 3. Wrap new free superblock to reclaim unit.
    fdp_set_ru_write_pointer(ssd, new_ru);

    //eruh->rus[rgidx] = new_ru;                  //Feb12 ASSERTION ERROR after GC because of this ftl.c:671:`ruh->curr_ru==ruh->rus[rgid]' failed.
    eruh->ru_in_use_cnt++;
    //eruh->ruh->rus[rgidx] = new_ru->ru;     // Feb12 ASSERTION ERROR mark_page_valid & Assertion `ru == ruh->curr_ru' failed. & Assertion `(ru == femu_ru->ru)' failed.
    //eruh->curr_ru = new_ru;                 // Feb12 ASSERTION ERROR mark_page_valid & Assertion `ru == ruh->curr_ru' failed.
    //ftl_log("     eruh->rus[%d] = new_ru fin(new_ru.ruamw:%lu,   ruh->rus[rg].ruamw : %lu)\n", rgidx, new_ru->ru->ruamw, eruh->ruh->rus[rgidx]->ruamw);
    // ftl_assert(eruh->curr_ru == new_ru);
    ftl_assert(new_ru->ruh == eruh);

    new_ru->last_init_time = qemu_clock_get_us(QEMU_CLOCK_REALTIME);  

    return new_ru;
}
// /* Let's ensure that victim ru is in both rg->rm and ruh->rm */
static int check_ruh_gc_entry(struct ssd *ssd, FemuReclaimUnit *ru)
{
    //struct ssdparams *spp = &ssd->sp;
    FemuReclaimGroup *rg = &ssd->rg[ru->rgidx];
    //struct ru_mgmt *rm = rg->ru_mgmt; //FIXEME when multi rg
    //FemuRuHandle *ruh = ru->ruh; //&ssd->ruhs[]

    ftl_assert(ru !=NULL);
    ftl_assert(ru->rgidx >= 0); //stupid assert. rgidx is uint16
    //ftl_assert(ru->ruh != NULL);

    switch (rg->ru_mgmt->mgmt_type)
    {
    case GC_GLOBAL_CB:
    case GC_GLOBAL_GREEDY:
    case GC_GLOBAL_RAND:
        /* No need to handle in such cases */
        return 0;
        break;
    default:
        //if ru
        return 1;
        break;
    }
}

static FemuReclaimUnit *fdp_advance_ru_pointer(struct ssd *ssd, FemuReclaimGroup *rg, FemuRuHandle *ruh, FemuReclaimUnit *ru)
{
    struct ssdparams *spp = &ssd->sp;
    struct ru_mgmt *rm = rg->ru_mgmt;
    struct write_pointer *wpp = ru->ssd_wptr;
    FemuReclaimUnit *curr_ru = ru; // rg idx 0
    FemuReclaimUnit *new_ru = NULL;
    bool isFull = true;
    //ftl_debug("             check_addr(wpp->ch, spp->nchs); \n");
    check_addr(wpp->ch, spp->nchs);
    //ftl_debug("             INSIDE fdp_advance_ru_pointer \n");
    wpp->ch++;
    if (wpp->ch == spp->nchs)
    {
        //ftl_debug("             if (wpp->ch == spp->nchs) \n");
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        //ftl_debug("             wpp->lun++; \n");
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch)
        {
            //ftl_debug("             if (wpp->lun == spp->luns_per_ch) \n");
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            //ftl_debug("             wpp->pg++; \n");
            if (wpp->pg == spp->pgs_per_blk)
            {
                //ftl_debug("             if (wpp->pg == spp->pgs_per_blk) \n");
                if (ru->next_line_index == ru->n_lines)
                {
                    wpp->pg = 0;
                    /*Now calling new should be Reclaim Unit, not a line */
                    //ftl_debug("             if( ru->next_line_index == ru->n_lines) \n");
                    for (int i = 0; i < ru->n_lines; ++i)
                    {
                        /* move current line to {victim,full} line list */
                        struct line *line = ru->lines[i];
                        if (line->vpc != spp->pgs_per_line)
                        {
                            ftl_assert(line->vpc >= 0 && line->vpc < spp->pgs_per_line);
                            /* update corresponding ru status */
                            //ru->vpc += line->vpc;
                            if(ru->vpc != line->vpc && spp->lines_per_ru == 1){
                                ru->vpc = line->vpc ;
                            }
                            isFull = false;
                        }else {
                            ru->vpc = line->vpc ;
                        }
                    }
                    
                    if (isFull)
                    {
                        /* all pgs are still valid, move to full line list */
                        ftl_assert(wpp->curline->ipc == 0);
                        QTAILQ_INSERT_TAIL(&rm->full_ru_list, curr_ru, entry);
                        rm->full_ru_cnt++;
                        ftl_debug(" curr_ru %d %p  isFull rm->full_ru_cnt %d \n",curr_ru->ruidx, curr_ru ,rm->full_ru_cnt);
                    }
                    else
                    {

                        for (int i = 0; i < ru->n_lines; ++i)
                        {
                            struct line *line = ru->lines[i];
                            //fdp_log("       victim ru insert curr_ru->line[%d] id %d vpc %d ipc %d free pg %d\n", i, line->id, line->vpc, line->ipc, (spp->pgs_per_line - (line->vpc + line->ipc)));
                            ftl_assert((spp->pgs_per_line - (line->vpc + line->ipc)) == 0); // Victim ru have either valid or invalid pages only. Not free pages.
                        }
                        ftl_debug("   fdp_advance_ru_pointer - RUH %d victim ru id %d , %p \n",curr_ru->ruh->ruhid , curr_ru->ruidx, curr_ru);
                        
                        if (rm->mgmt_type == GC_GLOBAL_CB){
                            ru->utilization = (float) ru->vpc / ru->npages;
                            ru->my_cb = CACL_COST_BENEFIT_APPROX(ru->utilization, ru->last_invalidated_time) ;
                            pqueue_insert(rm->victim_ru_cb, curr_ru);

                        }else{
                            ru->utilization = (float) ru->vpc / ru->npages;
                            pqueue_insert(rm->victim_ru_pq, curr_ru);
                            if ( check_ruh_gc_entry(ssd, curr_ru) ){
                                rm = curr_ru->ruh->ru_mgmt;
                                ftl_assert(rm!=NULL);
                                pqueue_insert(rm->victim_ru_pq, curr_ru);
                            }
                            //ftl_debug("Err if rm->mgmt_type == GC_GLOBAL_CB");
                        }
                        rm->victim_ru_cnt++;

                    }

                    if (ruh != NULL)
                    {
                        /* current line is used up, pick another empty line */
                        check_addr(wpp->blk, spp->blks_per_pl);

                        if((new_ru = fdp_get_new_ru(ssd, ru->rgidx, ruh->ruhid)) == NULL){
                            ftl_err("No free space left in device. \n");
                            ftl_assert(false && __LINE__ );

                        }

                        //ruh->curr_ru = new_ru; 
                        //ruh->rus[rg->rgidx] = new_ru;
                        //ruh->ruh->rus[rg->rgidx] = new_ru->nvme_ru;
                        //ftl_assert(ruh->curr_ru == new_ru);
                        //ftl_assert(new_ru->ruh == ruh);
                        //ftl_assert(ruh->ruh->rus[rg->rgidx] == new_ru->nvme_ru);    
                        wpp = new_ru->ssd_wptr;
                        ftl_debug("ruh %d - call new ru ( new_ru %d line id %d %p curr_ru %d ru_in_use_cnt %u )  \n", ruh->ruhid, new_ru->ruidx, new_ru->lines[0]->id, new_ru, curr_ru->ruidx, ruh->ru_in_use_cnt);
                        // Assume fdp get new ru make ru->lines have free lines

                        if (wpp == NULL)
                        {
                            ftl_err(" wpp==NULL How can wpp be NULL?\n");
                            abort();
                        }
                        wpp->blk = wpp->curline->id;
                        check_addr(wpp->blk, spp->blks_per_pl);
                        /* make sure we are starting from page 0 in the super block */
                        ftl_assert(wpp->pg == 0);
                        ftl_assert(wpp->lun == 0);
                        ftl_assert(wpp->ch == 0);
                        /* TODO: assume # of pl_per_lun is 1, fix later */
                        ftl_assert(wpp->pl == 0);
                        ftl_assert(ru->ruh == ruh);

                    }
                }
                else
                {
                    // ftl_err("ru->lines %p  \n", ru->lines);
                    ftl_assert(false); //fallout
                    fdp_inc_ru_write_pointer(ssd, ru);
                }
            }
        }
    }

    if (new_ru != NULL)
    {
        //ftl_debug("           RET fdp_advance_ru_pointer new_ru %p (rg%d ruh%d : ruamw : %lu) ruh->curr_ru %p \n", new_ru, new_ru->rgidx, new_ru->ruh->ruhid, new_ru->ru->ruamw, ruh->curr_ru);
        return new_ru;
    }
    //ftl_debug("         RET fdp_advance_ru_pointer curr_ru(ruamw : %lu) \n",curr_ru->ru->ruamw);
    return curr_ru;
}

// static FemuReclaimUnit *fdp_advance_ru_pointer_for_gc(struct ssd *ssd, FemuReclaimGroup *rg, FemuRuHandle *ruh, FemuReclaimUnit *ru)
// {
//     struct ssdparams *spp = &ssd->sp;
//     struct ru_mgmt *rm = rg->ru_mgmt;
//     struct write_pointer *wpp = ru->ssd_wptr;
//     FemuReclaimUnit *curr_ru = ru; // rg idx 0
//     FemuReclaimUnit *new_ru = NULL;
//     bool isFull = true;
//     check_addr(wpp->ch, spp->nchs);
//     wpp->ch++;
//     if (wpp->ch == spp->nchs)
//     {
//         wpp->ch = 0;
//         check_addr(wpp->lun, spp->luns_per_ch);
//         wpp->lun++;
//         /* in this case, we should go to next lun */
//         if (wpp->lun == spp->luns_per_ch)
//         {
//             wpp->lun = 0;
//             /* go to next page in the block */
//             check_addr(wpp->pg, spp->pgs_per_blk);
//             wpp->pg++;
//             if (wpp->pg == spp->pgs_per_blk)
//             {
//                 if (ru->next_line_index == ru->n_lines)
//                 {
//                     wpp->pg = 0;
//                     /*Now calling new should be Reclaim Unit, not a line */
//                     for (int i = 0; i < ru->n_lines; ++i)
//                     {
//                         /* move current line to {victim,full} line list */
//                         struct line *line = ru->lines[i];
//                         if (line->vpc != spp->pgs_per_line)
//                         {
//                             ftl_assert(line->vpc >= 0 && line->vpc < spp->pgs_per_line);
//                             /* update corresponding ru status */
//                             if(ru->vpc != line->vpc && spp->lines_per_ru == 1){
//                                 ru->vpc = line->vpc ;
//                             }
//                             isFull = false;
//                         }else {
//                             ru->vpc = line->vpc ;
//                         }
//                     }          
//                     if (isFull)
//                     {
//                         /* all pgs are still valid, move to full line list */
//                         ftl_assert(wpp->curline->ipc == 0);
//                         QTAILQ_INSERT_TAIL(&rm->full_ru_list, curr_ru, entry);
//                         rm->full_ru_cnt++;
//                         ftl_debug(" curr_ru %d %p  isFull rm->full_ru_cnt %d \n",curr_ru->ruidx, curr_ru ,rm->full_ru_cnt);
//                     }
//                     else
//                     {
//                         for (int i = 0; i < ru->n_lines; ++i)
//                         {
//                             struct line *line = ru->lines[i];
//                             //fdp_log("       victim ru insert curr_ru->line[%d] id %d vpc %d ipc %d free pg %d\n", i, line->id, line->vpc, line->ipc, (spp->pgs_per_line - (line->vpc + line->ipc)));
//                             ftl_assert((spp->pgs_per_line - (line->vpc + line->ipc)) == 0); // Victim ru have either valid or invalid pages only. Not free pages.
//                         }
//                         ftl_debug("   fdp_advance_ru_pointer - RUH %d victim ru id %d , %p \n",curr_ru->ruh->ruhid , curr_ru->ruidx, curr_ru);
//                         pqueue_insert(rm->victim_ru_pq, curr_ru);
//                         rm->victim_ru_cnt++;
//                     }
//                     if (ruh != NULL)
//                     {
//                         /* current line is used up, pick another empty line */
//                         check_addr(wpp->blk, spp->blks_per_pl);
//                         new_ru = fdp_get_new_ru(ssd, ru->rgidx, ruh->ruhid);
//                         ruh->gc_ru = new_ru;
//                         //ruh->curr_ru = new_ru; 
//                         //ruh->rus[rg->rgidx] = new_ru;
//                         //ruh->ruh->rus[rg->rgidx] = new_ru->nvme_ru;
//                         //ftl_assert(ruh->curr_ru == new_ru);
//                         //ftl_assert(new_ru->ruh == ruh);
//                         //ftl_assert(ruh->ruh->rus[rg->rgidx] == new_ru->nvme_ru);    
//                         wpp = new_ru->ssd_wptr;
//                         ftl_debug("ruh %d - call new ru ( new_ru %d %p curr_ru %d at %p )\n", ruh->ruhid, new_ru->ruidx, new_ru, curr_ru->ruidx, curr_ru);
//                         // Assume fdp get new ru make ru->lines have free lines

//                         if (wpp == NULL)
//                         {
//                             ftl_err(" wpp==NULL How can wpp be NULL?\n");
//                             abort();
//                         }
//                         wpp->blk = wpp->curline->id;
//                         check_addr(wpp->blk, spp->blks_per_pl);
//                         /* make sure we are starting from page 0 in the super block */
//                         ftl_assert(wpp->pg == 0);
//                         ftl_assert(wpp->lun == 0);
//                         ftl_assert(wpp->ch == 0);
//                         /* TODO: assume # of pl_per_lun is 1, fix later */
//                         ftl_assert(wpp->pl == 0);
//                         ftl_assert(ru->ruh == ruh);

//                     }
//                 }
//                 else
//                 {
//                     // ftl_err("ru->lines %p  \n", ru->lines);
//                     fdp_inc_ru_write_pointer(ssd, ru);
//                 }
//             }
//         }
//     }
//     if (new_ru != NULL)
//     {
//         //ftl_debug("           RET fdp_advance_ru_pointer new_ru %p (rg%d ruh%d : ruamw : %lu) ruh->curr_ru %p \n", new_ru, new_ru->rgidx, new_ru->ruh->ruhid, new_ru->ru->ruamw, ruh->curr_ru);
//         return new_ru;
//     }
//     //ftl_debug("         RET fdp_advance_ru_pointer curr_ru(ruamw : %lu) \n",curr_ru->ru->ruamw);
//     return curr_ru;
// }

#endif

static FemuReclaimUnit *femu_fdp_get_ru(struct ssd *ssd, uint16_t rgid, uint16_t ruhid)
{
    FemuRuHandle *ruh = &ssd->ruhs[ruhid];
    // if(ruh->curr_rg == rgid)
    // return ruh->curr_ru;
    // else{
    // ftl_debug("         INSIDE femu_fdp_get_ru \n");
    if (ruh == NULL)
    {
        ftl_err("            ruh == NULL\n");
    }
    // ruh->curr_rg = rgid;
    if (ruh->rus == NULL)
    {
        ftl_err("            ruh->rus[rgid] == NULL\n");
    }
    if (ruh->rus[rgid] == NULL)
    {
        ftl_err("            ruh->rus[rgid] == NULL\n");
    }
    // ftl_debug("         ruh->curr_rg = rgid; fin \n");
    if(ssd->nrg == 1){
        //ftl_err("ruh->curr_ru idx %d at %p != ruh->rus[rgid] idx %d at %p  \n", ruh->curr_ru->ruidx , ruh->curr_ru, ruh->rus[rgid]->ruidx, ruh->rus[rgid] );
        if(ruh->curr_ru!=ruh->rus[rgid]){
            ftl_err("ruh->curr_ru idx %d at %p != ruh->rus[rgid] idx %d at %p  \n", ruh->curr_ru->ruidx , ruh->curr_ru, ruh->rus[rgid]->ruidx, ruh->rus[rgid] );
        }
        ftl_assert (ruh->curr_ru==ruh->rus[rgid]);
    }
    return ruh->rus[rgid]; // NULL
}
#ifdef SSD_STREAM_WRITE

// static FemuReclaimUnit* femu_fdp_get_new_ru(struct ssd *ssd , FemuRuHandle *ruh, uint16_t rgid, uint16_t ruhid){
//     FemuReclaimUnit *ru = NULL;
// //Step 1.
//     //1. static ru
//     ru = femu_fdp_get_ru(ssd, rgid, ruhid);
//     //2. dynamic malloc ru
//     //ru = (FemuReclaimUnit *)g_malloc0(sizeof(FemuReclaimUnit));
//     //With 1 or 2, get RU (by femu_fdp_init_ssd_reclaim_unit(ssd))

//     ru->wptr = get_next_free_super(ssd);
//     ru->ruh = ruh;
//     ruh->curr_ru = ru;
//     ruh->rus[rgid] = &ru; //ruh->rus[rgid] = &ru;
//     return ru;
// }

#endif
static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    // ftl_assert(is_power_of_2(spp->luns_per_ch));
    // ftl_assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams *spp, FemuCtrl *n)
{
    spp->secsz = n->bb_params.secsz;             // 512
    spp->secs_per_pg = n->bb_params.secs_per_pg; // 8
    spp->pgs_per_blk = n->bb_params.pgs_per_blk; // 256 4K * 256 = 1M
    spp->blks_per_pl = n->bb_params.blks_per_pl; /* 256 16GB */
    spp->pls_per_lun = n->bb_params.pls_per_lun; // 1
    spp->luns_per_ch = n->bb_params.luns_per_ch; // 8
    spp->nchs = n->bb_params.nchs;               // 8

    spp->pg_rd_lat = n->bb_params.pg_rd_lat;
    spp->pg_wr_lat = n->bb_params.pg_wr_lat;
    spp->blk_er_lat = n->bb_params.blk_er_lat;
    spp->ch_xfer_lat = n->bb_params.ch_xfer_lat;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun; // = 2048 * 1
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch = spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */ // 32chnl * 4way = 128
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;              // 128 * 512 = 65536
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */ // = 2048 * 1
    
    spp->lazy_gc_pcent = n->bb_params.lazy_gc_pcent / 100.0;
    spp->gc_thres_pcent = n->bb_params.gc_thres_pcent / 100.0;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = n->bb_params.gc_thres_pcent_high / 100.0;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;
    //spp->enable_gc_delay=false;

    spp->lines_per_ru = 1;
    spp->total_ru_cnt = spp->tt_lines / spp->lines_per_ru;
    ftl_log(" spp->tt_lines %d spp->lines_per_ru %d spp->total_ru_cnt %d \n", spp->tt_lines, spp->lines_per_ru, spp->total_ru_cnt);
    check_params(spp);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++)
    {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++)
    {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++)
    {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++)
    {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++)
    {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}
#ifdef CYLON_FDP_TRIM_ERASE_ALL

static void ssd_reset_maptbl(struct ssd *ssd){
    struct ssdparams *spp = &ssd->sp;
    for (int i = 0; i < spp->tt_pgs; i++)
    {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
        ssd->rmap[i] = INVALID_LPN;    
    }
}
#endif

static void ssd_init_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++)
    {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++)
    {
        ssd->rmap[i] = INVALID_LPN;
    }
}
static void femu_fdp_init_ru_mgmt(struct ssd *ssd, FemuReclaimGroup *rg)
{
    struct ru_mgmt *rm = rg->ru_mgmt;
    rm->tt_rus = rg->tt_nru;
    rm->free_ru_cnt = rg->tt_nru;
    rm->victim_ru_cnt_type_init = 0;
    rm->victim_ru_cnt_type_permnt = 0;
    rm->victim_ru_cnt = 0;
    rm->full_ru_cnt = 0;
    //Declare global gc strategy
    //rm->mgmt_type = GC_GLOBAL_CB;
    //rm->mgmt_type = GC_NOISY_RUH_CUSTOM;
    rm->mgmt_type = GC_GLOBAL_GREEDY;

    QTAILQ_INIT(&rm->free_ru_list);
    QTAILQ_INIT(&rm->full_ru_list);

    rm->victim_ru_cb = pqueue_init(rm->tt_rus, victim_ru_cmp_pri_by_cb,
                                victim_ru_get_pri_by_cb, victim_ru_set_pri_by_cb,
                                victim_ru_get_pos, victim_ru_set_pos);
        
    rm->victim_ru_pq = pqueue_init(rm->tt_rus, victim_ru_cmp_pri,
                                victim_ru_get_pri, victim_ru_set_pri,
                                victim_ru_get_pos, victim_ru_set_pos);
}
static void _femu_fdp_init_ru_mgmt(struct ssd *ssd, struct ru_mgmt *init)
{
    struct ru_mgmt *rm = init;
    // rm->tt_rus = rg->tt_nru;
    // rm->free_ru_cnt = rg->tt_nru;
    rm->victim_ru_cnt_type_init = 0;
    rm->victim_ru_cnt_type_permnt = 0;
    rm->victim_ru_cnt = 0;
    rm->full_ru_cnt = 0;
    rm->custom_gc_threshold = 0;

    //Declare per RUH gc strategy
    rm->mgmt_type = GC_GLOBAL_GREEDY;   //default
    
    QTAILQ_INIT(&rm->free_ru_list);
    QTAILQ_INIT(&rm->full_ru_list);

    rm->victim_ru_cb = pqueue_init(rm->tt_rus, victim_ru_cmp_pri_by_cb,
                                victim_ru_get_pri_by_cb, victim_ru_set_pri_by_cb,
                                victim_ru_get_pos, victim_ru_set_pos);

    rm->victim_ru_pq = pqueue_init(rm->tt_rus, victim_ru_cmp_pri,
                                victim_ru_get_pri, victim_ru_set_pri,
                                victim_ru_get_pos, victim_ru_set_pos);

}
static void femu_fdp_init_ssd_reclaim_unit(struct ssd *ssd, FemuReclaimUnit *femu_ru, int rgidx, int index)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = NULL;
    femu_ru->n_lines = spp->lines_per_ru;
    femu_ru->next_line_index = 1; // wpp for line in ru
    femu_ru->vpc = 0;
    femu_ru->ipc = 0;
    femu_ru->ssd_wptr = (struct write_pointer *)g_malloc0(sizeof(struct write_pointer));
    femu_ru->npages = spp->lines_per_ru * spp->pgs_per_line;
    femu_ru->last_init_time = qemu_clock_get_us(QEMU_CLOCK_REALTIME);    //init when full & gc. 
    //femu_ru->last_invalidated_time = 0; //update when invalidated. init to 0 when gced

    wpp = femu_ru->ssd_wptr;
    femu_ru->lines = (struct line **)g_malloc0(femu_ru->n_lines * sizeof(struct line *));
    // femu_ru->ssd_wptr->curline = &lm->lines[(rgidx * lines_per_rg) + (index * lines_per_ru)];
    for (int i = 0; i < femu_ru->n_lines; i++)
    {
        femu_ru->lines[i] = get_next_free_line(ssd);
        femu_ru->lines[i]->my_ru = femu_ru;
    }
    wpp->curline = femu_ru->lines[0];
    // RU == superblock
    wpp->ch = 0;
    wpp->lun = 0; // wpp->lun == spp->luns_per_ch
    wpp->pl = 0;
    wpp->blk = wpp->curline->id;
    wpp->pg = 0; // wpp->pg == spp->pgs_per_blk
    ftl_assert(wpp->curline->id == femu_ru->lines[0]->id);
    ftl_assert(wpp->blk == femu_ru->lines[0]->id);

    // ftl_log("   femu_ru wptr set up wpp->blk  %d\n", wpp->blk);
}
static void femu_fdp_ssd_init_reclaim_group(FemuCtrl *n, struct ssd *ssd)
{
    NvmeSubsystem *subsys = n->subsys;
    // NvmeEnduranceGroup *endgrp = &n->subsys->endgrp;
    uint64_t rgs = subsys->params.fdp.nrg;
    NvmeReclaimUnit **russ = NULL;
    FemuReclaimGroup *rg = NULL;
    uint64_t tt_nru = ssd->sp.total_ru_cnt;
    ftl_assert(ssd->sp.total_ru_cnt > 0);

    ftl_debug("   femu_fdp_ssd_init_reclaim_group start\n");
    ssd->rg = (FemuReclaimGroup *)g_malloc0(rgs * sizeof(FemuReclaimGroup));
    ftl_debug("       ssd->rg init\n");

    /* FIXME 1 reclaim group */
    rgs = 1; // test only 1
    ssd->nrg = rgs;
    ssd->rus = (FemuReclaimUnit **)g_malloc0(rgs * sizeof(FemuReclaimUnit *));

    for (int i = 0; i < rgs; i++)
    {
        fdp_log("       ssd->rg[i].rus init  tt_nru : %lu\n", tt_nru);

        rg = &ssd->rg[i];
        rg->rgidx = i;
        rg->tt_nru = tt_nru / ssd->nrg;
        ssd->rus[i] = (FemuReclaimUnit *)g_malloc0(tt_nru * sizeof(FemuReclaimUnit));
        rg->rus = ssd->rus[i];
        rg->ru_mgmt = (struct ru_mgmt *)g_malloc0(sizeof(struct ru_mgmt));
        femu_fdp_init_ru_mgmt(ssd, &ssd->rg[i]);
        fdp_log(" %lu reclaim unit(s) allocated to rg[%d]\n", tt_nru, i);
    }

    // ftl_assert((endgrp->fdp.rus != NULL));
    russ = subsys->endgrp.fdp.rus;
    if (russ != NULL)
    {
        int i = 0;
        int j = 0;
        for (i = 0; i < rgs; i++)
        {
            rg = &ssd->rg[i];
            rg->ru_mgmt->free_ru_cnt = 0;
            for (j = 0; j < rg->tt_nru; j++)
            {
                if (&russ[i][j] != NULL)
                {
                    // direct map with nvme reclaim unit
                    ssd->rg[i].rus[j].rgidx = i;
                    ssd->rg[i].rus[j].nvme_ru = &subsys->endgrp.fdp.rus[i][j];
                    ssd->rg[i].rus[j].ruidx = j;
                    femu_fdp_init_ssd_reclaim_unit(ssd, &ssd->rg[i].rus[j], i, j);

                    QTAILQ_INSERT_TAIL(&ssd->rg[i].ru_mgmt->free_ru_list, &ssd->rg[i].rus[j], entry);
                    ssd->rg[i].ru_mgmt->free_ru_cnt++;
                }
                else
                {
                    ftl_err("Warning : Not initialized FemuReclaimUnit - ru is NULL");
                    // abort();
                }
            }
            /* rg shares the gc threshold % for reclaiming */
            rg->ru_mgmt->gc_thres_pcent = n->bb_params.gc_thres_pcent / 100.0;
            rg->ru_mgmt->gc_thres_pcent_high = n->bb_params.gc_thres_pcent_high / 100.0;
            rg->ru_mgmt->gc_thres_rus = (int)((1 - rg->ru_mgmt->gc_thres_pcent) * tt_nru);
            rg->ru_mgmt->gc_thres_rus_high = (int)((1 - rg->ru_mgmt->gc_thres_pcent_high) * tt_nru);
            ftl_log("rg[%d] gc thresshold(set : %d %%) (%ld/%ld ru) \n", i, n->bb_params.gc_thres_pcent, rg->ru_mgmt->gc_thres_rus, tt_nru);
            ftl_log("rg[%d] gc thresshigh(set : %d %%) (%ld/%ld ru) \n", i, n->bb_params.gc_thres_pcent_high, rg->ru_mgmt->gc_thres_rus_high, tt_nru);
        }
        ftl_log("       subsys->endgrp.fdp.rus[%d][%d] (ruamw : %lu)\n", i - 1, j - 1, ssd->rg[i - 1].rus[j - 1].nvme_ru->ruamw);
    }
}

// static void femu_fdp_ssd_init_ru_handles(FemuCtrl *n, struct ssd *ssd, FemuRuHandle * fruh, uint16_t nruh){
//     //1 to 1 mapping
//     NvmeEnduranceGroup *endgrp = &n->subsys->endgrp;
//     NvmeRuHandle *ruh = endgrp->fdp.ruhs;
// }
static void femu_fdp_ssd_init_ru_handles(FemuCtrl *n, struct ssd *ssd)
{
    NvmeNamespace *ns = &n->namespaces[0];
    NvmeSubsystem *subsys = n->subsys;
    NvmeEnduranceGroup *endgrp = &n->subsys->endgrp;
    NvmeRuHandle *nvme_ruh = endgrp->fdp.ruhs; 
    uint16_t nruh = subsys->params.fdp.nruh;
    uint16_t ph, *ruhid;
    // uint16_t rgif = endgrp->fdp.rgif;

    ssd->ruhs = (FemuRuHandle *)g_malloc0(nruh * sizeof(FemuRuHandle));
    ssd->nruhs = nruh;
    ruhid = ns->fdp.phs;
    femu_debug(" femu_fdp_ssd_init_ru_handles \n");
    for (ph = 0; ph < ns->fdp.nphs; ph++, ruhid++)
    {
        // femu_fdp_ssd_init_ru_handles(n,ssd,&ssd->ruhs[i]);
        nvme_ruh = &endgrp->fdp.ruhs[*ruhid];
        uint16_t i = *ruhid;
        ssd->ruhs[i].ruh = &endgrp->fdp.ruhs[i]; // here ??
        ssd->ruhs[i].ruh_type = endgrp->fdp.ruhs[i].ruht;
        ssd->ruhs[i].ruhid = i;
        // ssd->ruhs[i].n_ru = 0;
        ssd->ruhs[i].ruh_live_pages_cnt =0;
        ssd->ruhs[i].ru_in_use_cnt = 0;
        ssd->ruhs[i].curr_rg = 0;
        ssd->ruhs[i].hbmw = 0;
        ssd->ruhs[i].mbmw = 0;
        ssd->ruhs[i].mbe = 0;

        // ssd->ruhs[i].rus = ssd->rus;
        femu_log("        ph : %d, *ruhid : %d , (ruh = &endgrp->fdp.ruhs[%d] at %p and ssd->ruhs[i].ruh at %p) \n", ph, i, i, &endgrp->fdp.ruhs[*ruhid], ssd->ruhs[i].ruh);
        ssd->ruhs[i].rus = (FemuReclaimUnit **)g_malloc0(sizeof(FemuReclaimUnit *) * endgrp->fdp.nrg);
        for (int j = 0; j < endgrp->fdp.nrg; j++)
        {
            ssd->ruhs[i].rus[j] = fdp_get_new_ru(ssd, j, i);
            ssd->ruhs[i].rus[j]->ruh = &ssd->ruhs[i];
            ssd->ruhs[i].ruh->rus[j] = ssd->ruhs[i].rus[j]->nvme_ru;
            ssd->ruhs[i].curr_ru = ssd->ruhs[i].rus[j];
            // ftl_debug("ssd->ruhs[i].rus[j]->ru %p , &ruh->rus[j] %p , endgrp->fdp.rus[j] :%p \n",  (void *) ssd->ruhs[i].rus[j]->ru,  ruh->rus[j], (void *)endgrp->fdp.rus[j]);
            ftl_assert((ssd->ruhs[i].rus[j]->nvme_ru == nvme_ruh->rus[j])); // qemu-system-x86_64: ../hw/femu/bbssd/ftl.c:976: femu_fdp_ssd_init_ru_handles: Assertion `(ssd->ruhs[i].rus[j]->ru == ruh->rus[j])' failed.
            ftl_assert((ssd->ruhs[i].rus[j]->ruh == &ssd->ruhs[i]));
        }
        ftl_assert((ssd->ruhs[i].ruh == &endgrp->fdp.ruhs[i]));
        if (nvme_ruh->ruht == NVME_RUHT_PERSISTENTLY_ISOLATED)  //FIXME 
        {
            ssd->ruhs[i].ru_mgmt = (struct ru_mgmt *)g_malloc0(sizeof(struct ru_mgmt));
            // ssd->ruhs[i].ru_mgmt->tt_rus = rg->tt_nru;
            // ssd->ruhs[i].ru_mgmt->free_ru_cnt = rg->tt_nru;
            _femu_fdp_init_ru_mgmt(ssd, ssd->ruhs[i].ru_mgmt);
        }
        femu_debug(" init ssd->ruhs[i].rus %p ssd->ruhs[i].curr_ru %p line id %d \n", ssd->ruhs[i].rus, ssd->ruhs[i].curr_ru , ssd->ruhs[i].curr_ru->lines[0]->id );
    }
    //ftl_assert that femu_fdp_ssd_init_reclaim_group( ) has called before. 
    struct ru_mgmt *rm = ssd->rg->ru_mgmt; //FIXME mutli rg
    if(rm->mgmt_type == GC_NOISY_RUH_CUSTOM ) {
        //0/819/51960331, 1/96/4170425, 2/55/2452209 5.0 rHMW 
        uint64_t avail_ru_cnt;
        ftl_assert(rm->tt_rus != 0);
        ftl_assert(rm->gc_thres_rus_high != 0);

        avail_ru_cnt = rm->tt_rus - rm->gc_thres_rus_high ; //overflow?
        avail_ru_cnt = rm->tt_rus - 1 ; //overflow?

        // ssd->ruhs[1].ru_mgmt->custom_gc_threshold = 96;      //avail_ru_cnt/()   //1.272
        // ssd->ruhs[2].ru_mgmt->custom_gc_threshold = 55;                          //1.272

        ssd->ruhs[1].ru_mgmt->custom_gc_threshold = 96+5; 
        ssd->ruhs[2].ru_mgmt->custom_gc_threshold = 55+5;
        ssd->ruhs[0].ru_mgmt->custom_gc_threshold = avail_ru_cnt - 96 - 55 -10;

        //ftl_assert( (ssd->ruhs[0].ru_mgmt->custom_gc_threshold+ ssd->ruhs[1].ru_mgmt->custom_gc_threshold+ ssd->ruhs[2].ru_mgmt->custom_gc_threshold) <= (rm->tt_rus - rm->gc_thres_rus_high));
    }
}
void ssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;
    ssd->n = n;
    ftl_assert(ssd);

    ssd_init_params(spp, n);

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++)
    {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    // ssd_init_write_pointer(ssd);

    femu_log("\t femu_fdp_ssd_init_reclaim_group start \n");
    /* initalize FemuReclaimUnit pool. for now. */
    femu_fdp_ssd_init_reclaim_group(n, ssd);
    femu_log("\t femu_fdp_ssd_init_reclaim_group fin \n");

    femu_log("\t femu_fdp_ssd_init_ru_handles start \n");
    /* initalize FemuRuHandle. for now. */
    femu_fdp_ssd_init_ru_handles(n, ssd);
    femu_log("\t femu_fdp_ssd_init_ru_handles fin \n");

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >= 0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c)
    {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : lun->next_lun_avail_time;
        if (ncmd->type == USER_IO)
        {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        else
        {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}
static struct ppa fdp_get_new_page(struct ssd *ssd, FemuReclaimUnit *ru)
{
    struct write_pointer *wpp = ru->ssd_wptr;
    struct ppa ppa;
    if (ru == NULL )
        ftl_err("Assertion Failed in fdp_get_new_page (ru == NULL)\n");
    ftl_assert((ru != NULL));
    ftl_assert((wpp != NULL));

    ppa.ppa = 0;          // pg start
    ppa.g.ch = wpp->ch;   // ch wpp
    ppa.g.lun = wpp->lun; //
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;

    if (ppa.g.pl != 0 )
        ftl_err("Assertion Failed in fdp_get_new_page (ppa.g.pl == 0)\n");
    ftl_assert(ppa.g.pl == 0);

    if (ru->lines[ru->next_line_index - 1]->id != wpp->blk)
        ftl_err("Assertion Failed in fdp_get_new_page (ppa.g.pl == 0)\n");
    ftl_assert(ru->lines[ru->next_line_index - 1]->id == wpp->blk);
    return ppa;
}

/* update SSD status about one page from PG_VALID -> PG_INVALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    bool was_full_line = false;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg != NULL);
    if (pg->status != PG_VALID ){
        ftl_err("        ftl_assert(pg->status == PG_VALID)! ppa.g.blk %d pg->status  %d (VALID %d)\n", ppa->g.blk ,pg->status , PG_VALID);
        ftl_err("        ftl_assert(pg->status == PG_VALID)! ppa.g.blk %d pg->status  %d (VALID %d)\n", ppa->g.blk ,pg->status , PG_VALID);
        
        //ftl_assert(pg->status == PG_VALID);
        if (pg->status == PG_INVALID )
        {
            //page already invalidated.
            ftl_err("        page already invalidated. ppa.g.blk %d pg->status  %d (VALID %d)\n", ppa->g.blk ,pg->status , PG_VALID);
            return;
        }
        else {
            ftl_err("        ftl_assert(pg->status == PG_VALID)! ppa.g.blk %d pg->status  %d (VALID %d)\n", ppa->g.blk ,pg->status , PG_VALID);
            ftl_err("        ftl_assert(pg->status == PG_VALID)! ppa.g.blk %d pg->status  %d (VALID %d)\n", ppa->g.blk ,pg->status , PG_VALID);

            ftl_assert(pg->status == PG_VALID);            //Can free page hit this?
        }
    }

    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk != NULL);

    if( !(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk ) || !(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk)){
        ftl_err("        ftl_assert!(blk->ipc %d >= 0 && blk->ipc < spp->pgs_per_blk %d);   \n", blk->ipc ,spp->pgs_per_blk);
        ftl_err("        ftl_assert!(blk->vpc %d > 0 && blk->vpc <= spp->pgs_per_blk %d);   \n", blk->vpc ,spp->pgs_per_blk);

    }

    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);//
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    
    if (line->vpc == spp->pgs_per_line)
    {
        ftl_assert(line->ipc == 0); //qemu-system-x86_64: ../hw/femu/bbssd/ftl.c:1394: mark_page_invalid: Assertion `line->ipc == 0' failed. WHEN RUN FIO twice
        was_full_line = true;
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    /* Adjust the position of the victime line in the pq under over-writes */
    #ifndef SSD_STREAM_WRITE
    struct line_mgmt *lm = &ssd->lm;
    if (line->pos)
    {
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    }
    else
    {
        line->vpc--;
    }

    if (was_full_line)
    {
        /* move line: "full" -> "victim" */
        ftl_log( " Overwrite -- : lm->free_line_cnt %d lm->full_line_cnt  %d \n", ssd->lm.free_line_cnt, lm->full_line_cnt );

        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;

    }
    #endif
    #ifdef SSD_STREAM_WRITE
    struct ru_mgmt *rm = ssd->rg[0].ru_mgmt;    //FIXME : if multi-rg support
    FemuReclaimUnit *ru = line->my_ru;

    /* update corresponding ru status */
    ru->ipc = line->ipc;
    ftl_assert(ru != NULL);
    
    //ftl_debug(" .. Mark page invalid\n");
    switch(rm->mgmt_type){
    case GC_GLOBAL_GREEDY:
    case GC_NOISY_RUH_CUSTOM:   //FORNOW
    case GC_GLOBAL_RAND:

        /* GREEDY : ru is queued */
        if(ru->pos){
            //fdp_log( " Overwrite ruhid %d ru->pos %d \n", ru->ruh->ruhid ,ru->pos);
            pqueue_change_priority(rm->victim_ru_pq, ru->vpc - 1, ru);
            ru->utilization = ((float)ru->vpc / (float)(ru->vpc+ru->ipc)); 
            if ( check_ruh_gc_entry(ssd, ru) ){
                rm = ru->ruh->ru_mgmt;
                ftl_assert(rm!=NULL);
                //pqueue_insert(rm->victim_ru_pq, curr_ru);
                pqueue_change_priority(rm->victim_ru_pq, ru->vpc, ru);
            }
        }
        else{
            ru->vpc--;
            ru->utilization = ((float)ru->vpc / (float)(ru->vpc+ru->ipc)); 
        }


        line->vpc--;
        ftl_assert(ru->vpc==line->vpc);
        
        if ( was_full_line && ((ru->vpc+1)==spp->pgs_per_line) ){ //FIXME : when ru has multiple lines
            
            QTAILQ_REMOVE(&rm->full_ru_list, ru, entry);
            rm->full_ru_cnt--;
            
            ftl_log( "      ... victim ru %p ruhid %d (age %lu util %.2f score %.2f)inserted  \n", ru, ru->ruh->ruhid, ru->last_invalidated_time, ru->utilization, ru->my_cb);
            pqueue_insert(rm->victim_ru_pq, ru);
            rm->victim_ru_cnt++;
            if ( check_ruh_gc_entry(ssd, ru) ){
                rm = ru->ruh->ru_mgmt;
                ftl_assert(rm!=NULL);
                pqueue_insert(rm->victim_ru_pq, ru);
                //pqueue_change_priority(rm->victim_ru_pq, ru->vpc, ru);
                rm->victim_ru_cnt++;
            }            
            // if (ru->ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED ){
            //     rm = ru->ruh->ru_mgmt; 
            //     pqueue_insert(rm->victim_ru_pq, ru);
            //     rm->victim_ru_cnt++;
            // }
        }

        break;
    case GC_GLOBAL_CB:
        
        /* Cost-benefit 

            i) Cost = amount of data read and write. therefore, 1+u = (u)
            ii) Benefit = amount of data that can be restore * age. therefore, (1-u)*age
        
            From i) and ii) Therefore, 
            ... CB = u / {(1-u)*age} ...(1)

            iii) age = Now - creation time, 
                // ru's creation_time |<----------------------------------------------->|  get_curr_time  => age
                //      ru's creation_time |<------------------------------------------>|  get_curr_time  => age

                Therefore, age has proportional relation to below:
                    age = Now - creation_time ,
                    So, age ∝ -creation_time
                    ... age ∝ 1/creation_time ...(2)


                iii-1) when it's invalidated, decrease u. 
                iii-2) when it's invalidated,
                    1) creation time reseted = last_invalidated_time. // Default
                    2) creation time remains //OK?
                
                From (2), iii-1) and iii-2.1), 
                age ∝ 1/creation_time = 1/last_invalidated_time
                Therefore, 
                    1/age ∝ last_invalidated_time
                    last_invalidated_time ∝ 1/age ...(3)

                Therefore, 
                CB  = u / {(1-u)*age} ...(1)
                    = (u/(1-u)) * (1/age)
                    ∝ (u/(1-u)) * last_invalidated_time ...(3)
                
                Therefore,
                    CB  ∝ (u/(1-u)) * last_invalidated_time
                        := (u/(1-u)) * last_invalidated_time ...[fin]
        */

        /*set inv cnt and utilization of RU*/
        ru->vpc--;
        line->vpc--;
        ftl_assert(ru->vpc==line->vpc);
        ru->utilization = (float)((float)ru->vpc / (float)(ru->npages)); 

        /* set age */
        ru->last_invalidated_time = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

        /* Calculate CB */
        //ru->my_cb = (ru->utilization/(1-ru->utilization)) * ru->last_invalidated_time ;
        if (ru->vpc == 0){
            ru->utilization = 0;
            ru->my_cb = 0;

        }else{
            ru->my_cb = CACL_COST_BENEFIT_APPROX(ru->utilization,ru->last_invalidated_time);
        }
        /* Put in to victim pqueue*/
        if(ru->pos){
            pqueue_change_priority(rm->victim_ru_cb, ru->my_cb, ru);
            // if (ru->ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED ){
            //     rm = ru->ruh->ru_mgmt; 
            //     pqueue_change_priority(rm->victim_ru_cb, ru->my_cb, ru);
            // }
        }

        if ( was_full_line && ((ru->vpc+1)==spp->pgs_per_line) ){ //FIXME : when ru has multiple lines
            QTAILQ_REMOVE(&rm->full_ru_list, ru, entry);
            rm->full_ru_cnt--;
            ftl_log( "      ... victim ru %p ruhid %d (age %lu util %.2f score %.2f)inserted \n", ru, ru->ruh->ruhid, ru->last_invalidated_time, ru->utilization, ru->my_cb);
            pqueue_insert(rm->victim_ru_cb, ru);
            rm->victim_ru_cnt++;
            // if (ru->ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED ){
            //     rm = ru->ruh->ru_mgmt; 
            //     pqueue_insert(rm->victim_ru_cb, ru);
            //     rm->victim_ru_cnt++;
            // }
        }

        break;
        // case GC_GLOBAL_CB_version2:
        //     /*set inv cnt and utilization of RU*/
        //     line->vpc--;
        //     ftl_assert(ru->vpc==line->vpc);
        //     ru->utilization = (float)((float)ru->vpc / (float)(ru->npages)); 
        //     ru->ruh->ruh_live_pages_cnt -=1 ;
        //     /* set age */
        //     // ru->last_invalidated_time = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        //     /* Calculate CB */
        //     //ru->my_cb = (ru->utilization/(1-ru->utilization)) * ru->last_invalidated_time ;
        //     ru->my_cb = CACL_COST_BENEFIT_APPROX(ru->utilization,ru->last_init_time);
        //     /* Put in to victim pqueue*/
        //     if(ru->pos){
        //         pqueue_change_priority(rm->victim_ru_pq, ru->my_cb, ru);
        //         if (ru->ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED ){
        //             rm = ru->ruh->ru_mgmt; 
        //             pqueue_change_priority(rm->victim_ru_pq, ru->my_cb, ru);
        //         }
        //     }else if ( was_full_line && ((ru->vpc+1)==spp->pgs_per_line) ){ //FIXME : when ru has multiple lines
        //         QTAILQ_REMOVE(&rm->full_ru_list, ru, entry);
        //         rm->full_ru_cnt--;
        //         pqueue_insert(rm->victim_ru_pq, ru);
        //         rm->victim_ru_cnt++;        
        //         if (ru->ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED ){
        //             rm = ru->ruh->ru_mgmt; 
        //             pqueue_insert(rm->victim_ru_pq, ru);
        //             rm->victim_ru_cnt++;
        //         }
        //     }
        //     break;

    case GC_GLOBAL_WARM:

        //WARM style hot/cold cost benefit
        // else{
        //     ru->last_invalidated_time = (qemu_clock_get_ns(QEMU_CLOCK_REALTIME)/1000)/1000;     //us
        //     if(ru->pos){
        //         //ru is queued
        //         if (ru is in cold queue and within the watermark){
        //             pqueue_remove(rm->victim_ru_pq , ru);
        //         }else if (ru is in cold queue and over the watermark){
        //             pqueue_change_priority(rm->victim_ru_pq , ru->last_invalidated_time, ru);    // insert to min heap
        //         }else{
        //             //ru is not in cold queue
        //             pqueue_change_priority(rm->victim_ru_pq_hot, ru->last_invalidated_time, ru); // insert to min heap
        //         }
        //     }
        //     if(was_full_line){
        //         QTAILQ_REMOVE(&rm->full_ru_list, ru, entry);
        //         rm->full_ru_cnt--;
        //         pqueue_insert(rm->victim_ru_pq_hot, ru);
        //         rm->victim_ru_cnt++;
        //     }
        // }
        default:
        break;
    };
    if (ru->ruh->ruh_live_pages_cnt > 0)
        ru->ruh->ruh_live_pages_cnt -=1 ;
    
    #endif

}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa, FemuReclaimUnit *ru)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    if ( pg->status != PG_FREE){
        ftl_err("mark_page_valid at ppa %lu g.ch %d g.way %d g.blk %d g.pg %d (ruh %d pg->status != PG_FREE status : %d ruidx %d %p )\n", ppa->ppa, ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg,ru->ruh->ruhid ,pg->status, ru->ruidx ,ru);
    }
    ftl_assert(pg->status == PG_FREE);  //qemu-system-x86_64: 
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
    
    /* update corresponding ru status */
    //ru->vpc++;
    ftl_assert( line->my_ru == ru);
    if( (ru->n_lines == 1) && (ru->vpc+1 == line->vpc) )
        ru->vpc++;
    else{
        ftl_debug("mark_page_valid multiple line or vpc mismatch here \n");
        ru->vpc=0;
        for(int i = 0; i < ru->n_lines ; i++){
            ru->vpc += ru->lines[i]->vpc;
        }
    }

    //logging 
    ru->ruh->ruh_live_pages_cnt += 1; 

}

static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++)
    {
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);

        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay)
    {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
// static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
// {
//     struct ppa new_ppa;
//     struct nand_lun *new_lun;
//     uint64_t lpn = get_rmap_ent(ssd, old_ppa);

//     ftl_assert(valid_lpn(ssd, lpn));
//     new_ppa = get_new_page(ssd);
//     /* update maptbl and rmap */
//     set_maptbl_ent(ssd, lpn, &new_ppa);
//     /* update rmap */
//     set_rmap_ent(ssd, lpn, &new_ppa);

//     mark_page_valid(ssd, &new_ppa);

//     /* need to advance the write pointer here */
//     ssd_advance_write_pointer(ssd);

//     if (ssd->sp.enable_gc_delay) {
//         struct nand_cmd gcw;
//         gcw.type = GC_IO;
//         gcw.cmd = NAND_WRITE;
//         gcw.stime = 0;
//         ssd_advance_status(ssd, &new_ppa, &gcw);
//     }

//     /* advance per-ch gc_endtime as well */
// #if 0
//     new_ch = get_ch(ssd, &new_ppa);
//     new_ch->gc_endtime = new_ch->next_ch_avail_time;
// #endif

//     new_lun = get_lun(ssd, &new_ppa);
//     new_lun->gc_endtime = new_lun->next_lun_avail_time;

//     return 0;
// }

/* move valid page data (already in DRAM) from victim line to a new page */
static void gc_write_page_fdp_style(struct ssd *ssd, struct ppa *old_ppa, FemuReclaimUnit *new_ru)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    //FemuReclaimUnit *curr_ru = NULL;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);
    FemuReclaimUnit * ret_ru = NULL;
    ftl_assert(valid_lpn(ssd, lpn));
    //ftl_debug("gc_write_page_fdp_style here  ");
    // new_ppa = get_new_page(ssd);  //fix this
    new_ppa = fdp_get_new_page(ssd, new_ru);
    //fdp_log("fdp_get_new_page at ppa %lu g.ch %d g.way %d g.blk %d g.pg %d \n", new_ppa.ppa, new_ppa.g.ch, new_ppa.g.lun, new_ppa.g.blk, new_ppa.g.pg );

    /* update maptbl and rmap */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);
    mark_page_valid(ssd, &new_ppa, new_ru);

    //new_ru->ru->ruamw -= ssd->spp->page_size; atomic 

    /* need to advance the write pointer here */
    if(new_ru->ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED ){
        //ftl_debug("         NVME_RUHT_PERSISTENTLY_ISOLATED \n");
        //curr_ru = fdp_advance_ru_pointer(ssd, &ssd->rg[new_ru->rgidx], new_ru->ruh, new_ru);
        if( (ret_ru = fdp_advance_ru_pointer(ssd, &ssd->rg[new_ru->rgidx], new_ru->ruh, new_ru)) != new_ru->ruh->gc_ru){
            new_ru->ruh->gc_ru = ret_ru;
        }
        //new_ru->ruh->curr_ru = curr_ru;
    }
    else{
        //# iniitially isolated types
        int gcruh_id = ssd->nruhs-1;
        if (new_ru->ruh == &ssd->ruhs[gcruh_id]){
            //ftl_debug("         type : GC -> GC  id : %p \n", new_ru);
        }else{
            //ftl_debug("         type : Host -> GC id : %p \n", new_ru);
            new_ru->ruh = &ssd->ruhs[gcruh_id];
        }
        //curr_ru = fdp_advance_ru_pointer(ssd, &ssd->rg[new_ru->rgidx], new_ru->ruh, new_ru);
        if( (ret_ru = fdp_advance_ru_pointer(ssd, &ssd->rg[new_ru->rgidx], new_ru->ruh, new_ru)) != ssd->ruhs[gcruh_id].curr_ru ) {
            ssd->ruhs[gcruh_id].rus[new_ru->rgidx] = ret_ru;
            ssd->ruhs[gcruh_id].curr_ru = ret_ru;
            ssd->ruhs[gcruh_id].ruh->rus[new_ru->rgidx] = ret_ru->nvme_ru;
        } 
        //new_ru->ruh->curr_ru = curr_ru;
        //new_ru->ruh->rus[new_ru->rgidx] = curr_ru;

    }


    if (ssd->sp.enable_gc_delay)
    {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;
}

/* here ppa identifies the block we want to clen */
// static void clean_one_block(struct ssd *ssd, struct ppa *ppa)
// {
//     struct ssdparams *spp = &ssd->sp;
//     struct nand_page *pg_iter = NULL;
//     int cnt = 0;

//     for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
//         ppa->g.pg = pg;
//         pg_iter = get_pg(ssd, ppa);
//         /* there shouldn't be any free page in victim blocks */
//         ftl_assert(pg_iter->status != PG_FREE);
//         if (pg_iter->status == PG_VALID) {
//             gc_read_page(ssd, ppa);
//             /* delay the maptbl update until "write" happens */
//             gc_write_page(ssd, ppa);
//             cnt++;
//         }
//     }
//     ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
// }
/* here ppa identifies the block we want to clean */
static int clean_one_block_fdp_style(struct ssd *ssd, struct ppa *ppa, FemuReclaimUnit *new_ru)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    FemuRuHandle * ruh = new_ru->ruh;
    int cnt = 0;
    //ftl_log(" \tINSIDE clean_one_block_fdp_style\n");
    for (int pg = 0; pg < spp->pgs_per_blk; pg++)
    {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        if (pg_iter->status == PG_FREE){
            ftl_err(" pg g.blk %d new_ru %d %p violate pg_iter->status != PG_FREE \n", ppa->g.blk, new_ru->ruidx, new_ru);
            ftl_assert(pg_iter->status != PG_FREE);
        }
        if (pg_iter->status == PG_VALID)
        {
            gc_read_page(ssd, ppa);
            /* delay the maptbl update until "write" happens */
            if( new_ru->ruh->ruh_type == NVME_RUHT_INITIALLY_ISOLATED && new_ru->ruh->curr_ru != new_ru ){
                new_ru = ruh->curr_ru;

            }else if (new_ru->ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED && new_ru->ruh->gc_ru != new_ru )
            {
                /* code */
                //ftl_debug("clean_one_block_fdp_style  new_ru->ruh->gc_ru != new_ru TOUCHING HERE \n");    //OK
                new_ru = ruh->gc_ru;
            }
            
            gc_write_page_fdp_style(ssd, ppa, new_ru);
            cnt++;
        }
    }
    if(get_blk(ssd, ppa)->vpc != cnt){
        struct nand_block *blk = get_blk(ssd, ppa);
        ftl_err("blk npgs %d ipc %d vpc %d  \n",blk->npgs, blk->ipc, blk->vpc);
        ftl_err("\n");
    }
    //qemu-system-x86_64: ../hw/femu/bbssd/ftl.c:1698: clean_one_block_fdp_style: Assertion `get_blk(ssd, ppa)->vpc == cnt' failed.

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
    //ftl_log(" \tRET clean_one_block_fdp_style\n");
    return cnt;
}
// static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
// {
//     //struct line_mgmt *lm = &ssd->lm;
//     struct line *line = get_line(ssd, ppa);
//     line->ipc = 0;
//     line->vpc = 0;
//     /* move this line to free line list */
//     //QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
//     //lm->free_line_cnt++;
// }
static void mark_ru_free(struct ssd *ssd, uint16_t rgid, FemuReclaimUnit *ru)
{
    struct ssdparams *spp = &ssd->sp;
    //ftl_debug(" INSIDE mark_ru_free \n");
    struct ru_mgmt *rm = ssd->rg[rgid].ru_mgmt;
    // Assume we dont use 'mark_line_free' (TODO check this wether correct or not)
    ftl_assert(ru != NULL);

    for (int i = 0; i < ru->n_lines; ++i)
    {
        ru->lines[i]->ipc = 0;
        ru->lines[i]->vpc = 0;
        ru->lines[i]->pos = 0;
        bool assert_flag = false;
        struct ppa ppa;
        struct nand_block *blk=NULL;
        struct nand_page *pg = NULL;
        ppa.g.blk = ru->lines[i]->id;
        for (int ch = 0; ch < spp->nchs; ++ch)
        {
            for (int lun = 0; lun < spp->luns_per_ch; ++lun)
            {
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pl = 0;
                // lunp = get_lun(ssd, &ppa);
                //blk_p = get_blk(ssd,&ppa);
                // vpc_cnt += clean_one_block_fdp_style(ssd, &ppa, new_ru);
                // cnt+=1;
                //#fdp_log("100p GC causing high waf \n");
                //#clean_one_block_fdp_style(ssd, &ppa, new_ru);
                blk = get_blk(ssd, &ppa);
                for (int i = 0; i< spp->pgs_per_blk; i++ ){
                    pg = &blk->pg[i];
                    if ( pg->status != PG_FREE){
                        assert_flag = true;
                        ftl_err("  mark_ru_free :: pg->status %d != PG_FREE \n", pg->status);
                        break;
                        //mark_block_free(ssd, &ppa);
                    }
                }
                if(assert_flag){
                    assert_flag = false;
                    mark_block_free(ssd, &ppa);
                    ftl_assert(false);
                }
            }
        }
    }
    ru->vpc = 0;
    ru->pri = 0;
    ru->pos = 0;
    ru->next_line_index = 1;


    //ftl_debug(" INSIDE fdp_set_ru_write_pointer \n");
    fdp_set_ru_write_pointer(ssd, ru);

    //ftl_debug(" before : mark_ru_free ru->ru->ruamw %lu = ru->ruh->ruh->ruamw %lu;", ru->nvme_ru->ruamw, ru->ruh->ruh->ruamw);
    ru->nvme_ru->ruamw = ru->ruh->ruh->ruamw;
    //ftl_debug(" after  : mark_ru_free ru->ru->ruamw %lu = ru->ruh->ruh->ruamw %lu;", ru->nvme_ru->ruamw, ru->ruh->ruh->ruamw);

    QTAILQ_INSERT_TAIL(&rm->free_ru_list, ru, entry);
    rm->free_ru_cnt++;

    //I'm not sure about these codes at this moment ; inho
    //ru->last_init_time =  qemu_clock_get_ns(QEMU_CLOCK_REALTIME)/1000/1000;
    //ru->last_invalidated_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    //ftl_debug("  mark_ru_free FIN\n");

}   
// static void mark_ru_free_by_ruh(struct ssd *ssd, FemuRuHandle *ruh, FemuReclaimUnit *ru){
//     struct ssdparams *spp = &ssd->sp;
//     struct ru_mgmt *rm = ruh->ru_mgmt;
//     /*
//     Assume we call 'mark_line_free' before this
//     for(int i=0; i<ru->n_lines; ++i){   }
//     */
//    ru->lines = NULL;
//    ru->vpc = 0;
//    ru->pri = 0;
//    ru->pos = 0;
//    ru->next_line_index = 0;
//     QTAILQ_INSERT_TAIL(rm->free_ru_list, ru, entry);
//}

// static inline void print_entry(FILE *out, void *a) {
//     //QueueElement *elem = (QueueElement *)a;
//     FemuReclaimUnit * ru = ( FemuReclaimUnit *)a;
//     //fprintf(out, "Priority: %llu, Value: %d\n", elem->priority, elem->value);
//     printf("ruidx %d ru->vpc %d : line->vpc %d ru->ipc %d ru->age %lu ru->my_cb %.2f\n", ru->ruidx, ru->vpc, ru->lines[0]->vpc , ru->ipc, ru->last_invalidated_time, ru->my_cb);
// }


//check ru->gc_write_ptr to check or align. 
static int check_gc_ruh_available(struct ssd *ssd, FemuRuHandle * ruh){
    if(ruh->ruh_type == NVME_RUHT_INITIALLY_ISOLATED){
        if(ssd->ruhs[ssd->nruhs - 1].curr_ru == NULL){
            ssd->ruhs[ssd->nruhs - 1].curr_ru = fdp_get_new_ru(ssd, ruh->curr_ru->rgidx, ruh->ruhid);
            ssd->ruhs[ssd->nruhs - 1].rus[ssd->ruhs[ssd->nruhs - 1].curr_ru->rgidx] = ssd->ruhs[ssd->nruhs - 1].curr_ru;
            //Not neccesary I think for now
            ssd->ruhs[ssd->nruhs - 1].ruh->rus[ssd->ruhs[ssd->nruhs - 1].curr_ru->rgidx] = ssd->ruhs[ssd->nruhs - 1].curr_ru->nvme_ru;  //qemu-system-x86_64: ../hw/femu/bbssd/ftl.c:2106: select_victim_ru: Assertion `victim_ru != ((void *)0)' failed.
        }
        if (ssd->ruhs[ssd->nruhs - 1].curr_ru == NULL){
                //        assert(ssd->ruhs[ssd->nruhs-1].curr_ru != NULL); //This means no space left
                return -1;
        }

    }
    else if(ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED){
        if(ruh->gc_ru == NULL){
            ruh->gc_ru = fdp_get_new_ru(ssd, ruh->curr_ru->rgidx, ruh->ruhid);
            ftl_debug("check_gc_ruh_available ruh %d gc_ru idx %d , %p call new ru  \n", ruh->ruhid, ruh->gc_ru->ruidx, ruh->gc_ru );

            if (ruh->gc_ru == NULL){
                //assert(ruh->gc_ru != NULL);//This means no space left
                return -1;
            }
        }
    }else{
        ftl_err("Unsupported RUH type : %d\n",ruh->ruh_type);
        ftl_assert(false); //        ftl_err("Unsupported RUH type : %d\n",ruh->ruh_type);

    }
    return 0;
}

static FemuReclaimUnit *select_victim_ru_from_ruh(struct ssd *ssd, uint16_t rgid, uint16_t ruhid){
    FemuReclaimUnit *victim_ru = NULL;
    struct ru_mgmt *ru_mgmt = ssd->ruhs[ruhid].ru_mgmt;

    victim_ru = pqueue_pop(ru_mgmt->victim_ru_pq);
    if(victim_ru != NULL){
        ru_mgmt->victim_ru_cnt--;
    }else{
        ftl_err("select_victim_ru_from_ruh : victim_ru is null\n" );
    }
    return victim_ru;
}

static FemuReclaimUnit *select_victim_ru(struct ssd *ssd, uint16_t rgid, uint16_t ruhid, uint16_t type)
{

    FemuReclaimUnit *victim_ru = NULL;
    //QTAILQ_HEAD(ru_list_temp, FemuReclaimUnit) ru_list_temp;
    struct ru_mgmt *ru_mgmt = ssd->rg[rgid].ru_mgmt;
    ru_mgmt->mgmt_type = GC_GLOBAL_GREEDY;
    //ru_mgmt->mgmt_type = GC_GLOBAL_CB;
    //ru_mgmt->mgmt_type = GC_GLOBAL_RAND;

    switch (ru_mgmt->mgmt_type)
    {
    case GC_GLOBAL_CB:
        /**
         * @brief 
         * for each victim ru in queue, update the cost-benefit in min heap order 
         * now = qemu_clock_get_us(now)
         * for each victim in queue
         *      victim->my_cb = victim->utilization / ((1-victim->utilization) *  ) ;
         */

        ru_mgmt = ssd->rg[rgid].ru_mgmt;
        ftl_assert(ru_mgmt != NULL);
        victim_ru = pqueue_pop(ru_mgmt->victim_ru_cb);
        ftl_debug("victim_ru %p rm->victim_cnt %d \n",victim_ru, ru_mgmt->victim_ru_cnt);
        break;
    case GC_GLOBAL_GREEDY:
        ru_mgmt = ssd->rg[rgid].ru_mgmt;
        //ftl_err("RUH%d is NVME_RUHT_PERSISTENTLY_ISOLATED gc type %d vic ru cnt %d\n", ruhid, ru_mgmt->mgmt_type,ru_mgmt->victim_ru_cnt );
        ftl_assert(ru_mgmt != NULL);
        victim_ru = pqueue_pop(ru_mgmt->victim_ru_pq);
        //ftl_assert(victim_ru != NULL);
        break;

    case GC_GLOBAL_RAND:
        ru_mgmt = ssd->rg[rgid].ru_mgmt;
        //ftl_err("RUH%d is NVME_RUHT_PERSISTENTLY_ISOLATED gc type %d vic ru cnt %d\n", ruhid, ru_mgmt->mgmt_type,ru_mgmt->victim_ru_cnt );
        ftl_assert(ru_mgmt != NULL);
        victim_ru = pqueue_randpop(ru_mgmt->victim_ru_pq);
        ftl_assert(victim_ru != NULL);
        break;
    
    case GC_NOISY_RUH_CUSTOM :
        /** 
         * gc_candidate
         * for i,active_ruhs :
         *      
         *      if gc_candidate != NULL:
         *          ru = pqueue_peek(ruh[i]->victim_ru_pq);
         *          if ru->util < gc_candidate->util :
         *              remember i
         *              gc_candidate = ru
         *      else :
         *          gc_candidate = pqueue_peek(ruh[i]->victim_ru_pq);
         *      if gc_candidate
         *
         * 
         * return gc_candidate
         * **/
        
        FemuReclaimUnit * ru=NULL;
        int remember = -1;
        for (int i =0 ; i < ssd->nruhs; i++){
            ftl_assert( ssd->ruhs[i].ru_mgmt != NULL);
            if ( ssd->ruhs[i].ru_in_use_cnt > ssd->ruhs[i].ru_mgmt->custom_gc_threshold ){
                if (victim_ru == NULL ){
                    remember = i ;
                    victim_ru = pqueue_peek(ssd->ruhs[i].ru_mgmt->victim_ru_pq);
                }else{
                    ru = pqueue_peek(ssd->ruhs[i].ru_mgmt->victim_ru_pq);
                    if (ru==NULL){
                        continue;
                    }
                    else if ( ru->vpc < victim_ru->vpc ){
                        remember = i;
                        victim_ru = ru;
                    }
                }
            }
        }
        ru_mgmt = ssd->rg[rgid].ru_mgmt;
        if(remember < 0){
            ftl_err("No reclaim unit to GC. Select in Global greedy : ");
            victim_ru = pqueue_pop(ru_mgmt->victim_ru_pq);
            ru_mgmt->victim_ru_cnt-=1;
            pqueue_remove(ssd->ruhs[victim_ru->ruh->ruhid].ru_mgmt->victim_ru_pq, victim_ru);
            ssd->ruhs[victim_ru->ruh->ruhid].ru_mgmt->victim_ru_cnt-=1;
            return victim_ru;
        }

        victim_ru = pqueue_pop(ssd->ruhs[remember].ru_mgmt->victim_ru_pq);
        ftl_assert(victim_ru != NULL);
        pqueue_remove(ru_mgmt->victim_ru_pq, victim_ru);

        break; 

    case GC_SELECTIVE_RUH : 
    case GC_EXPLOIT_SEQUENTIAL : 
        //RUH select logic here
        /**
         * @brief 
         * If there is a sequential stream among the workload, it will naturally generates low utilization block, leading low write amplification.
         * The theory looks the same with greedy algorithm. 
         * The difference of this alogrithm is not selecting sequential stream at all. 
         * 
         * Three information for RUH
         *      1. RUH global wa    //ruh_mgmt->waf_score_global
         *      2. RUH recent wa    //ruh_mgmt->waf_score_transitory
         *      3. RUH global util  //ruh_mgmt->utilization (ruh_mgmt->vpc_total / (ru_in_use_cnt * pgs_per_ru ))
         * 
         * 1) If it keeps invalidated sequentially then let this RU to be keep invalidated. (using zone like ptr?)
         * 2) If recent wa == 1.0 then likely this stream is sequential. Wait for this to invalidate itself.
         * 
         */
        ru_mgmt = ssd->rg[rgid].ru_mgmt;
        ftl_assert(ru_mgmt != NULL);
        //victim_ru = select_victim_ru_from_ruh(ssd,rgid,ruhid);
        victim_ru = pqueue_pop(ru_mgmt->victim_ru_pq);
        int vic_ruh = victim_ru->ruh->ruhid;

        struct ru_mgmt *ruh_mgmt = ssd->ruhs[vic_ruh].ru_mgmt;
        //The high utilization + wa ~= 1.1  = define sequential 
        //The higher utilization + lower wa >= 1.1 = likely sequential 
        // wa >= 1.1 & ruh_mgmt->utilization lower than ~ 0.8  = likely the high waf ruh.
        if (ru_mgmt->is_gc_triggered == false){
            ru_mgmt->is_gc_triggered = true;
            //goto return
            //return victim_ru;

        }else if(ru_mgmt->is_force_gc_triggered == false){
            ru_mgmt->is_force_gc_triggered = true;
            //goto return
            //return victim_ru;

        }else if(victim_ru->utilization <= 0.0 ){
            //goto return
            //return victim_ru;
            ftl_assert(victim_ru->utilization <= 0.0);
        }
        else{
            if ( ruh_mgmt->waf_score_global == 1.0 && ruh_mgmt->waf_score_transitory == 1.0){
                //preserve_victim(victim_ru);
                if( victim_ru->chance_token < 0 ){
                    victim_ru->chance_token--;
                    pqueue_insert( ru_mgmt->victim_ru_pq , victim_ru );
                }else{
                    //state changes to likely sequential
                    //goto return
                    //return victim_ru;
                }


            }else if(ruh_mgmt->waf_score_global < 1.1 ){
            }

            // if(victim_ru != NULL){
            //     ru_mgmt->victim_ru_cnt--;
            // }
            // ftl_assert(victim_ru != NULL);

        }
        break ;

    case GC_SELECTIVE_RUH_SOCIAL_WELFARE : 
        //RUH select logic here
        /**
         * @brief 
         * A theory that a social walfare algorithm of distributing and selecting garbage collection .
         * A limit can be 
         *  1. Capacity limit of GC. When the SSD needs GC, pick RUH based on the limit that we set previously. (This set when there is first background GC) 
         *  2. Sched based GC. If the certain RUH is too victimized, relax that. Simple algorithm such as two-chance; an algorithm that avoids consequent
         *  3. Plus, this architecture allows to select victim ru within the RUH by it's own policy. Thus, based on the utilization that each RUH has, RUH can either perform CB-based or greedy based victim ru selection.  
         */
        //int ruh_cnt_in_used ;
        //for ruh in ruhs_cnt_in_use:
        //  expected_ru_in_use[ruhid] = ( ssd->ruhs[ruhid].ru_cnt_in_use * ssd->ruhs[ruhid]. )


        victim_ru = select_victim_ru_from_ruh(ssd,rgid,ruhid);
        break ;

    case GC_BIT_POPULATION :
        /**
         * @brief 
         * This schemes allows multi-ru selection that based on the BIT population among multiple RUHs, which is a posterior information.
         * Let some algorithm such as a cost-benefit selects the victim ru. 
         * Among other victim ru, select another victim that can be merged with the one picked at the first stage.
         * Look at the average BIT time that the victim RU has. Lookup age queue. find similar one with less error rate. If there is no such vic ru, skip this. 
         * This can be apporximate by the age detection.
         */

        
        break;

    case GC_SELECTIVE_MIDAS_OP :
        ru_mgmt = ssd->ruhs[ruhid].ru_mgmt;
        if (ru_mgmt == NULL)
        {
            ftl_err("RUH%d is NVME_RUHT_PERSISTENTLY_ISOLATED but ru_mgmt is NULL\n", ruhid);
            abort();
        }
        break;
    default:
        ftl_err("GC policy not set \n");
        break;
    }

    //pqueue_dump(ru_mgmt->victim_ru_pq,(FILE * ) STDOUT_FILENO, print_entry); // It's min heap
    // ftl_assert(victim_ru != NULL);  //SIGSEV here. Dec9 Inho
    // ftl_assert(victim_ru->ruh != NULL);

    //WARM style
    //while(cold queue is empty) 
    //  ru = pop from cold 
    //  ru->my_cb calculate with current time 
    //  pqueue_insert ( rm->victim_ru_pq_cb , ru->my_cb, ru)
    //
    //victim_ru = pop from cb
    //while(cb queue is empty)
    //  ru = pop from cb 
    //  pqeueu_insert( rm->victim_ru_pq  , ru->last_invalid_time , ru)
    if(victim_ru != NULL){
        ru_mgmt->victim_ru_cnt--;
    }
    return victim_ru;
}

static int do_gc_fdp_style(struct ssd *ssd, uint16_t rgid, uint16_t ruhid, bool force)
{
    struct ssdparams *spp = &ssd->sp;
    FemuRuHandle *ruh = &ssd->ruhs[ruhid];
    FemuReclaimUnit *victim_ru = NULL;
    FemuReclaimUnit *new_ru = NULL;
    struct ru_mgmt * rm = ssd->rg[rgid].ru_mgmt;
    struct line *victim_line = NULL;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;
    int vpc_cnt=0;
    int cnt=0;
    int ret = 0;
    
    // Resolved Issues :
    //  issue 1. We lose pointer to old RU after gc. We lose ruh->curr_ru and ruh->rus[rgid] because the pointer is not changed to valid copy ru
    //  issue 2. About new ru, after gc, new ru that have valid copy doesn't know whether it has valid data or not.
    
    if ((victim_ru = select_victim_ru(ssd, rgid, ruhid, ruh->ruh_type)) == NULL)
    {
        ftl_err(" unable to find victim RU, gc skip\n");
        return -1;
    }

    ruh = victim_ru->ruh; //Thanks for reporting...

    ftl_assert(victim_ru != NULL);
    ftl_assert(victim_ru->ruh != NULL);
    ftl_assert(victim_ru->ruh->rus != NULL);
    ftl_assert(victim_ru->ruh->rus[rgid] != NULL);
    ftl_assert(ruh != NULL);
    //ftl_assert(ruh->rus[rgid] != NULL);   //Handles later


    if ( ((ruh->rus[rgid] != NULL) && (victim_ru == ruh->rus[rgid])) || (victim_ru == victim_ru->ruh->rus[rgid]) || victim_ru == victim_ru->ruh->curr_ru )
    {
        ftl_err(" Victim RU was in active state(currently being written) \n"); //Really happens?
        abort();        //This needs work
    }

    /**
     * @brief Rules for FDP GC
     * 
     * GC allocates valid pages into new reclaim unit.
     * Base on the RUH type, this new RU(new_ru) could be 
     * 
     * 1) GC_RUH's active RU (II)
     * 2) victim RU's RUH (PI)
     * 
     */
    if (ruh->ruh_type == NVME_RUHT_INITIALLY_ISOLATED){
        if(ssd->ruhs[ssd->nruhs-1].curr_ru == NULL || ssd->ruhs[ssd->nruhs-1].rus[rgid] != NULL ){
            if ((ret = check_gc_ruh_available(ssd, ruh)) < 0 ){
                ftl_err("No free space left in device. \n");
                ftl_assert(false && __LINE__ );
            }
        }
        new_ru = ssd->ruhs[ssd->nruhs-1].curr_ru;
        //ftl_err("GC victim ru id %d at %p (vpc %d:%d ipc %d) new ru at %p new_ru->ruh->ruhid %d (vpc %d:%d ipc %d) \n", victim_ru->ruidx, victim_ru,victim_ru->vpc, victim_ru->lines[0]->vpc, victim_ru->lines[0]->ipc , new_ru, new_ru->ruh->ruhid, new_ru->vpc, new_ru->lines[0]->vpc, new_ru->lines[0]->ipc );
        
        ftl_assert(new_ru->ruh->ruhid == ssd->ruhs[ssd->nruhs-1].ruhid); // II -> new_ru should be one of GC RUH; Assertion for future weird bug.
        ftl_assert(new_ru->ruh == &ssd->ruhs[ssd->nruhs-1]);     // II -> new_ru should be one of GC RUH; Assertion for future weird bug.

    }else if(ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED){
        if (ruh->gc_ru == NULL){
            if ((ret = check_gc_ruh_available(ssd, ruh)) < 0 ){
                ftl_err("No free space left in device. \n");
                ftl_assert(false && __LINE__ );
            }
        }
        new_ru = ruh->gc_ru;

        ftl_assert(new_ru->ruh->ruhid == ruh->ruhid);// PI -> new_ru should be one of GC RUH;
        ftl_assert(new_ru->ruh == ruh);// PI -> new_ru should be one of GC RUH;

    }
    else
    {
        ftl_err("Undefined RUH type. If you want new Ruht, then welcome ! \n");
        ftl_assert(false && __LINE__ );
        abort();        //This needs work
        return 0;
    }

    if(!force){
        //background & ru->util high then skip this ru
        //float org = victim_ru->utilization ; 
        victim_ru->utilization = (float) victim_ru->vpc / (victim_ru->vpc+victim_ru->ipc);  //Preventing crazy things
        //ftl_assert( org >= victim_ru->utilization -0.01 );
        //ftl_assert( org <= victim_ru->utilization +0.01 );
        
        if(victim_ru->utilization > spp->lazy_gc_pcent){
            //ftl_err("victim RU util is high(%f), gc skip (victim %d free %lu full %d total %lu victim_ru->util %2.f)\n",victim_ru->utilization , rm->victim_ru_cnt, rm->free_ru_cnt, rm->full_ru_cnt, ( rm->victim_ru_cnt+rm->free_ru_cnt+rm->full_ru_cnt) ,  victim_ru->utilization);
            pqueue_insert(rm->victim_ru_pq, victim_ru);
            rm->victim_ru_cnt++;

            
            if( check_ruh_gc_entry(ssd, victim_ru) ){
                rm = victim_ru->ruh->ru_mgmt;
                pqueue_insert(rm->victim_ru_pq, victim_ru);
                rm->victim_ru_cnt++;
            }
            return -1;
        }
    }else{
        ftl_debug("[Forground GC]");
    }

    ftl_debug( " FDP GC do_gc_fdp_style BEGIN : victim ru idx %d line id %d, %p rg->free_ru_cnt %lu lm->free_line_cnt %d \n",victim_ru->ruidx, victim_ru->lines[0]->id, victim_ru ,ssd->rg[rgid].ru_mgmt->free_ru_cnt, ssd->lm.free_line_cnt);


    //ftl_debug( " [BEFORE RU status] : victim ru idx %d , %p  ruh %d rus[%d] %d at %p curr_ru %d at %p (nvme_ru %p ), gc_ru %d at %p (nvme_ru %p)\n",victim_ru->ruidx , victim_ru ,ruhid, rgid, ruh->rus[rgid]->ruidx, ruh->rus[rgid], ruh->curr_ru->ruidx, ruh->curr_ru, ruh->curr_ru->nvme_ru, ruh->gc_ru->ruidx, ruh->gc_ru, ruh->gc_ru->nvme_ru); //OK
    //mapping changing 
    for (int i = 0; i < spp->lines_per_ru; ++i)
    {
        // OPT: need loop unrolling?
        victim_line = victim_ru->lines[i];
        ppa.g.blk = victim_line->id;
        //ftl_log("GC-ing ch[%d]w[%d]line:%d, vpc=%d,ipc=%d,victim=%d,full=%d,free=%d\n", spp->nchs, spp->luns_per_ch, ppa.g.blk,
        //          victim_line->vpc, victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
        //          ssd->lm.free_line_cnt);

        // copy back valid data
        for (ch = 0; ch < spp->nchs; ++ch)
        {
            for (lun = 0; lun < spp->luns_per_ch; ++lun)
            {
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pl = 0;
                lunp = get_lun(ssd, &ppa);
                vpc_cnt += clean_one_block_fdp_style(ssd, &ppa, new_ru);
                cnt+=1;
                mark_block_free(ssd, &ppa);
                if (spp->enable_gc_delay)
                {
                    struct nand_cmd gce;
                    gce.type = GC_IO;
                    gce.cmd = NAND_ERASE;
                    gce.stime = 0;
                    ssd_advance_status(ssd, &ppa, &gce);
                }
                lunp->gc_endtime = lunp->next_lun_avail_time;

            }
        }
        // update line status
    }
    nvme_fdp_stat_inc(&ssd->n->subsys->endgrp.fdp.mbmw, (uint64_t) ( vpc_cnt * spp->secsz * spp->secs_per_pg ));
    nvme_fdp_stat_inc(&victim_ru->ruh->mbmw, (uint64_t) ( vpc_cnt * spp->secsz * spp->secs_per_pg ));   //(2)
    nvme_fdp_stat_inc(&victim_ru->ruh->ruh->mbmw, (uint64_t) ( vpc_cnt * spp->secsz * spp->secs_per_pg ));   //(2)

    nvme_fdp_stat_inc(&ssd->n->subsys->endgrp.fdp.mbe, (uint64_t) (cnt * ((spp->secsz * spp->secs_per_pg)) * spp->pgs_per_blk));
    nvme_fdp_stat_inc(&victim_ru->ruh->mbe, (uint64_t) (cnt * ((spp->secsz * spp->secs_per_pg)) * spp->pgs_per_blk)); //(2)
    nvme_fdp_stat_inc(&victim_ru->ruh->ruh->mbe, (uint64_t) (cnt * ((spp->secsz * spp->secs_per_pg)) * spp->pgs_per_blk)); //(2)

    if(ruh->ruh_type == NVME_RUHT_INITIALLY_ISOLATED && victim_ru->ruh->ruhid != (ssd->nruhs-1)){
        nvme_fdp_stat_inc(&ssd->ruhs[ssd->nruhs-1].hbmw, (uint64_t) ( vpc_cnt * spp->secsz * spp->secs_per_pg ));   //(3)
        nvme_fdp_stat_inc(&ssd->ruhs[ssd->nruhs-1].mbmw, (uint64_t) ( vpc_cnt * spp->secsz * spp->secs_per_pg ));   //(3)
        nvme_fdp_stat_inc(&ssd->ruhs[ssd->nruhs-1].ruh->hbmw, (uint64_t) ( vpc_cnt * spp->secsz * spp->secs_per_pg ));   //(3)
        nvme_fdp_stat_inc(&ssd->ruhs[ssd->nruhs-1].ruh->mbmw, (uint64_t) ( vpc_cnt * spp->secsz * spp->secs_per_pg ));   //(3)
    }


    if (ssd->ruhs[ victim_ru->ruh->ruhid].ru_in_use_cnt > 0)
        ssd->ruhs[ victim_ru->ruh->ruhid].ru_in_use_cnt -= 1;

    ssd->ruhs[ victim_ru->ruh->ruhid ].ruh_live_pages_cnt -= vpc_cnt;
    // update_ruh_waf(ssd, victim_ru)
    // ftl_debug( "\n Background ++ : is_force %d rg->free_ru_cnt %lu ruhid %d lm->free_line_cnt %d vpc_cnt %d ru->pgs %d total_ru_use_in_cnt %d/%u/%lu, %d/%u/%lu, %d/%u/%lu  (ruh/cnt/live_pgs) total_hbmw_in_pgs(ruh/hb/mb) %d/%lu/%lu %d/%lu/%lu %d/%lu/%lu\n", 
    //                                 force, ssd->rg[rgid].ru_mgmt->free_ru_cnt, 
    //                                 victim_ru->ruh->ruhid, ssd->lm.free_line_cnt, vpc_cnt, spp->pgs_per_blk * spp->nchs * spp->luns_per_ch, 
    //                                 ssd->ruhs[0].ruhid, ssd->ruhs[0].ru_in_use_cnt,ssd->ruhs[0].ruh_live_pages_cnt,
    //                                 ssd->ruhs[1].ruhid, ssd->ruhs[1].ru_in_use_cnt,ssd->ruhs[1].ruh_live_pages_cnt,
    //                                 ssd->ruhs[2].ruhid, ssd->ruhs[2].ru_in_use_cnt,ssd->ruhs[2].ruh_live_pages_cnt,
    //                                 ssd->ruhs[0].ruhid, ssd->ruhs[0].hbmw, ssd->ruhs[0].mbmw,
    //                                 ssd->ruhs[1].ruhid, ssd->ruhs[1].hbmw, ssd->ruhs[1].mbmw,
    //                                 ssd->ruhs[2].ruhid, ssd->ruhs[2].hbmw, ssd->ruhs[2].mbmw
    //                                 );
    ftl_debug( "\n Background ++ : is_force %d rg->free_ru_cnt %lu ruhid %d lm->free_line_cnt %d vpc_cnt %d ru->pgs %d total_ru_use_in_cnt %d/%d/%d, %d/%d/%d, %d/%d/%d  (ruh/cnt/live_pgs) total_hbmw_in_pgs(ruh/hb/mb) %d/%lu/%lu %d/%lu/%lu %d/%lu/%lu\n", 
                                    force, ssd->rg[rgid].ru_mgmt->free_ru_cnt, 
                                    victim_ru->ruh->ruhid, ssd->lm.free_line_cnt, vpc_cnt, spp->pgs_per_blk * spp->nchs * spp->luns_per_ch, 
                                    ssd->ruhs[0].ruhid, ssd->ruhs[0].ru_in_use_cnt,ssd->ruhs[0].ruh_live_pages_cnt,
                                    ssd->ruhs[1].ruhid, ssd->ruhs[1].ru_in_use_cnt,ssd->ruhs[1].ruh_live_pages_cnt,
                                    ssd->ruhs[2].ruhid, ssd->ruhs[2].ru_in_use_cnt,ssd->ruhs[2].ruh_live_pages_cnt,
                                    ssd->ruhs[0].ruhid, ssd->ruhs[0].hbmw, ssd->ruhs[0].mbmw,
                                    ssd->ruhs[1].ruhid, ssd->ruhs[1].hbmw, ssd->ruhs[1].mbmw,
                                    ssd->ruhs[2].ruhid, ssd->ruhs[2].hbmw, ssd->ruhs[2].mbmw
                                    );
    //ftl_debug( " [AFTER RU status] : victim ru idx %d , %p  ruh %d rus[%d] %d at %p curr_ru %d at %p (nvme_ru %p ), gc_ru %d at %p (nvme_ru %p)\n",victim_ru->ruidx , victim_ru ,ruhid, rgid, ruh->rus[rgid]->ruidx, ruh->rus[rgid], ruh->curr_ru->ruidx, ruh->curr_ru, ruh->curr_ru->nvme_ru, ruh->gc_ru->ruidx, ruh->gc_ru, ruh->gc_ru->nvme_ru); //OK
    
    //mark_line_free(ssd, &ppa); //free line in mark ru free
    mark_ru_free(ssd, rgid, victim_ru);
    
    return 0;
}
#ifndef SSD_STREAM_WRITE
static int do_gc(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;
    // 1. victim selection
    victim_line = select_victim_line(ssd, force);
    if (!victim_line)
    {
        ftl_err("GC - Unable to find victim line! \n");
        return -1;
    }

    ppa.g.blk = victim_line->id;
    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

    /* copy back valid data */
    for (ch = 0; ch < spp->nchs; ch++)
    {
        for (lun = 0; lun < spp->luns_per_ch; lun++)
        {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            lunp = get_lun(ssd, &ppa);
            clean_one_block(ssd, &ppa);
            mark_block_free(ssd, &ppa);

            if (spp->enable_gc_delay)
            {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }

    /* update line status */
    mark_line_free(ssd, &ppa);

    return 0;
}
#endif
static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;

    if (end_lpn >= spp->tt_pgs)
    {
        ftl_err("start_lpn=%" PRIu64 ",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }
    //ftl_debug("start_lpn=%" PRIu64 ", end_lpn=%" PRIu64 ",tt_pgs=%d\n", start_lpn, end_lpn, ssd->sp.tt_pgs);
    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++)
    {
        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa))
        {
            // printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            // printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            // ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}
#ifndef SSD_STREAM_WRITE
static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;

    if (end_lpn >= spp->tt_pgs)
    {
        ftl_err("start_lpn=%" PRIu64 ",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }
    ftl_log(" ftl_thread enter ssd_write\n");

    while (should_gc_high(ssd))
    {
        /* perform GC here until !should_gc(ssd) */
        r = do_gc(ssd, true);
        if (r == -1)
            break;
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++)
    {
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa))
        {
            /* update old page information first */
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        /* new write */
        ppa = get_new_page(ssd);
        /* update maptbl and rmap table at the same time*/
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(ssd);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}
#endif
/**
 * [Inho] FDP SSD write implementation
 * Step1. find stream based on req->dspec. This precedure should identify
 * 1) endgrp idx,
 * 2) Placement id (PID),
 * 3) FDP-Handle id(ruhid)
 * 4) Based on the handle id, select RU(reclaim unit)
 * 5) Based on the selected RU, find superblock (implemented as a 'line')
 *
 *
 * Step2. Find line and write the data. Then perform write latency model.
 * Note that line write should performed as same as superblock distributed to the channels and planes, etc,.
 *
 *
 * Step3. Update the mapping address info to the mapping table. This should update
 *
 *
 * Step4. Fin
 * 1) LBA->PPA map
 * 2) PPA->LBA map (reverse map)
 */
#ifdef SSD_STREAM_WRITE
static uint64_t ssd_stream_write(FemuCtrl *n, struct ssd *ssd, NvmeRequest *req)
{
    NvmeNamespace *ns = req->ns;

    // FDP data struct
    FemuReclaimGroup *rg = NULL; //&ssd->rg[rgid];
    FemuRuHandle *ruh = NULL;    //&ssd->ruhs[ruhid];
    FemuReclaimUnit *ru = NULL;  // ruh->rus[rgid];
    // SSD data struct
    struct ssdparams *spp = &ssd->sp;
    // Superblock *sb = ru->wptr;
    // struct write_pointer *wp = ru->ssd_wptr;
    // SLBA, LBA
    NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    uint64_t lba = req->slba;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    // FDP vars
    uint16_t pid = le16_to_cpu(rw->dspec);
    uint32_t dw12 = le32_to_cpu(req->cmd.cdw12);
    uint8_t dtype = (dw12 >> 20) & 0xf;
    uint16_t ph, rgid, ruhid;
    int r;
    if (dtype != NVME_DIRECTIVE_DATA_PLACEMENT ||
        !nvme_parse_pid(ns, pid, &ph, &rgid))
    {
        ph = 0;
        rgid = 0;
    }
#ifdef FORCE_NOFDP
    ph = 0;
#endif
    //FIXME change it to noFDP 
    ruhid = ns->fdp.phs[ph]; // ns -> handler index
    // ru = &ns->endgrp->fdp.ruhs[ruhid].rus[rg];

    rg = &ssd->rg[rgid];
    ruh = &ssd->ruhs[ruhid];
    if ( unlikely(ruh->curr_ru == NULL) ) {
        ftl_err("Really hitting this area ? Only hit when ruh->ru is trimmed or first used\n");
        ruh->rus[rgid] = fdp_get_new_ru(ssd, rgid, ruhid);
        ruh->ruh->rus[rgid] = ruh->rus[rgid]->nvme_ru;
        ruh->curr_ru = ruh->rus[rgid];
    }else {
        ru = ruh->rus[rgid];
        if(ssd->nrg == 1){
            ftl_assert(ruh->curr_ru == ru);
        }
    }

    if (end_lpn >= spp->tt_pgs)
    {
        ftl_err("start_lpn=%" PRIu64 ",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    while (! (should_gc_high_fdp_style(ssd) < 0) )
    {
        /* perform GC here until !should_gc(ssd) */
        ftl_log("fdp write invokes forground garbage collection\n"); // Here, Jan12
        r = do_gc_fdp_style(ssd, rgid, ruhid, true);                 // foreground gc fdp style.
        // r = do_gc(ssd, true); //foreground gc
        if (r == -1)
            break;
    }
    // fdp_log("   STEP 1 fin\n");
    // Step2. Find line and write the data. Then perform write latency model.
    // Note that line write should performed as same as superblock distributed to the channels and planes, etc,.
    //  1) Based on the selected RU, find superblock (implemented as a 'line')
    //  2) Perform latency model related to line(superblock)
    //  3) Update latency
    //  +1) Based on the selected RU, find superblock (implemented as a 'line')
    // fdp_log("   Step2. Find line and write the data. Then perform write latency model.\n");
    
    for (lpn = start_lpn; lpn <= end_lpn; lpn++)
    {
        if (ru != ruh->curr_ru){
            ftl_err("ru idx %d , %p != ruh->curr_ru idx %d , %p \n",ru->ruidx, ru, ruh->curr_ru->ruidx, ruh->curr_ru); // Here, Jan12
            ftl_assert(ru == ruh->curr_ru); //something crazy happened :)
            ftl_assert(ru == ruh->rus[rgid]);
        }

       
        ppa = get_maptbl_ent(ssd, lpn);

        if (mapped_ppa(&ppa))
        {
            //fdp_log( " Forground -- : ruh %d rg->free_ru_cnt %lu  rg->full_ru_cnt %d  rg->victim_ru_cnt %d lm->free_line_cnt %d \n", ruhid, ssd->rg[rgid].ru_mgmt->free_ru_cnt,ssd->rg[rgid].ru_mgmt->full_ru_cnt, ssd->rg[rgid].ru_mgmt->victim_ru_cnt, ssd->lm.free_line_cnt);
            /* update old page information first */
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        /* new write */
        // ppa = get_new_page(ssd);
        ppa = fdp_get_new_page(ssd, ru);

        /* update maptbl and rmap table at the same time*/ // Step3
        set_maptbl_ent(ssd, lpn, &ppa);

        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa, ru);
        // fdp_log("       set_maptbl_ent, set_rmap_ent, mark_page_valid fin\n");

        /* need to advance the write pointer here */
        if((ru = fdp_advance_ru_pointer(ssd, rg, ruh, ru)) != ruh->curr_ru ){
            //fdp_log( " fdp_advance_ru_pointer \n");
            ftl_debug( "ruh %d wptr update prev_ru %d at %p (nvme_ru %p) curr_ru %d at %p (nvme_ru %p) \n", ruhid, ruh->curr_ru->ruidx, ruh->curr_ru, ruh->curr_ru->nvme_ru, ru->ruidx, ru, ru->nvme_ru);
            ruh->rus[rgid] = ru;
            ruh->curr_ru = ru;
            ruh->ruh->rus[rgid] = ru->nvme_ru;
        }
        
        ftl_assert(ruh->curr_ru == ru);
        ftl_assert(ru->ruh == ruh);
        ftl_assert(ruh->ruh->rus[rg->rgidx] == ru->nvme_ru);   

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        // get latency statistics

        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}
#endif

/**
 * [Inho] FDP SSD write implementation
 * nvme_do_write_fdp
 * Step1. find stream based on req->dspec. This precedure should identify
 * 1) endgrp idx,
 * 2) Placement id (PID),
 * 3) FDP-Handle id(ruhid)
 * 4) Based on the handle id, select RU(reclaim unit)
 *
 *
 * Step2. Find line and write the data. Then perform write latency model.
 * Note that line write should performed as same as superblock distributed to the channels and planes, etc,.
 *  1) Based on the selected RU, find superblock (implemented as a 'line')
 *  2) Perform ssd write(write latency)
 *
 *
 * Step3. Update the mapping address info to the mapping table. This should update
 * 1) LBA->PPA map
 * 2) PPA->LBA map (reverse map)
 *
 * nvme_do_write_fdp
 * Step4. Update RU info to the endgrp. (Fin)
 */

uint64_t nvme_do_write_fdp(FemuCtrl *n, NvmeRequest *req, uint64_t slba,
                           uint32_t nlb)
{
    NvmeNamespace *ns = req->ns;
    struct ssd *ssd = n->ssd;
    NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    uint64_t data_size = nvme_l2b(ns, nlb);
    uint32_t dw12 = le32_to_cpu(req->cmd.cdw12);
    uint8_t dtype = (dw12 >> 20) & 0xf;
    uint64_t lat = -1;
    uint16_t pid = le16_to_cpu(rw->dspec);
    uint16_t ruhid;
    uint16_t ph, rg;
    // NvmeRuHandle *ruh;

    FemuReclaimUnit *femu_ru;
    NvmeReclaimUnit *ru;

    // Step1.  find stream based on req->dspec. This precedure should identify
    //  1) endgrp idx,
    //  2) Placement id (PID),
    if (dtype != NVME_DIRECTIVE_DATA_PLACEMENT ||
        !nvme_parse_pid(ns, pid, &ph, &rg))
    {
        ph = 0;
        rg = 0;
    }
    // get reclaim unit handle id by placement handle id
    ruhid = ns->fdp.phs[ph]; // ns -> handler index

    /************************/
    //enforcing noFDP
#ifdef FORCE_NOFDP
    ph=0;
    ruhid = ns->fdp.phs[ph];
#endif
    /************************/

    // ru = &ns->endgrp->fdp.ruhs[ruhid].rus[rg];
    ru = ns->endgrp->fdp.ruhs[ruhid].rus[rg]; // TODO : After new ru, this doesn't points to the new one
    // #if FEMU_FDP_LATENCY_DISABLE
    femu_ru = femu_fdp_get_ru(ssd, rg, ruhid);
    if ( unlikely(femu_ru == NULL) ) {
        FemuRuHandle *fruh = &ssd->ruhs[ruhid];
        fruh->rus[rg] = fdp_get_new_ru(ssd, rg, ruhid);
        fruh->curr_ru = fruh->rus[rg];
        fruh->ruh->rus[rg] = fruh->rus[rg]->nvme_ru;
    }

    if ((ru != femu_ru->nvme_ru))
    {
        ftl_err(" ru %p != femu_ru->ru %p in ruh %d( ruh %d : nvme_ruh->rus[rg] at %p and ru at %p) \n", ru, femu_ru->nvme_ru, ruhid, ruhid, ns->endgrp->fdp.ruhs[ruhid].rus[rg], ru);
        ftl_err(" ns->endgrp : %p &n->subsys->endgrp %p \n ", ns->endgrp, &n->subsys->endgrp);
        ftl_err("(ns->endgrp->fdp.ruhs[%d] at %p and (n->ssd->ruhs->ruh[%d] at %p)  \n", ruhid, &ns->endgrp->fdp.ruhs[ruhid], ruhid, n->ssd->ruhs[ruhid].ruh);
        ftl_err(" ssd->ruhs[ruhid %d].curr_ru [idx %d] at %p (nvme_ru %p) ssd->ruhs[ruhid %d].gc_ru [idx %d] at %p (nvme_ru %p) \n", ruhid, ssd->ruhs[ruhid].curr_ru->ruidx, ssd->ruhs[ruhid].curr_ru, ssd->ruhs[ruhid].curr_ru->nvme_ru,  ruhid, ssd->ruhs[ruhid].gc_ru->ruidx, ssd->ruhs[ruhid].gc_ru, ssd->ruhs[ruhid].gc_ru->nvme_ru);

        ftl_assert((ru == femu_ru->nvme_ru)); // TODO
    }
    femu_ru->rgidx = rg;

    // ftl_assert((rg == femu_ru->rg));

    // Step2. Find line and write the data. Then perform write latency model.
    // Note that line write should performed as same as superblock distributed to the channels and planes, etc,.
    //  1) Based on the selected RU, find superblock (implemented as a 'line')
    //  2) Perform latency model related to line(superblock)
    //  3) Update latency
    // Step3. Update the mapping address info to the mapping table. This should update
    //      LBA->PPA map & PPA->LBA map (reverse map)
    // Step 2 and 3 go to ssd__stream_write
    // Step4. Update RU info to the endgrp. (Fin)

    nvme_fdp_stat_inc(&ns->endgrp->fdp.hbmw, data_size * ssd->sp.secsz );
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].hbmw, data_size * ssd->sp.secsz );  //(1)
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].ruh->hbmw, data_size * ssd->sp.secsz );  //(1)

    nvme_fdp_stat_inc(&ns->endgrp->fdp.mbmw, data_size * ssd->sp.secsz );
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].mbmw, data_size * ssd->sp.secsz );  //(1)
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].ruh->mbmw, data_size * ssd->sp.secsz );  //(1)

    if (ru->ruamw == 0)
    {
        ftl_log("ru->ruamw is 0. IGNORE is current behavior of Cylon \n");
    }
    // Inho : I think entire loop should go into ssd_stream_write. including nvme_update_ruh
    while (nlb)
    {
        if (nlb < ru->ruamw)
        {
            ru->ruamw -= nlb;
#ifdef SSD_STREAM_WRITE
            // data has written. latency model here
            if ((lat = ssd_stream_write(n, ssd, req)) <= 0)
            {
                // write failed
                ftl_err("ssd_stream_wrte write failed\n");
            }
#endif
            break;
        }
        nlb -= ru->ruamw;
        nvme_update_ruh(n, ns, pid);
        break;
    }
    // fclose(fp);
    return lat;
}


static uint64_t ssd_trim(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    NvmeDsmRange *ranges = req->dsm_ranges;
    int nr_ranges = req->dsm_nr_ranges;
    //uint32_t attributes = req->dsm_attributes;
    int total_trimmed_pages = 0;
    int total_already_invalid = 0;
    int total_out_of_bounds = 0;
    // Removed unused counters: total_trimmed_pages, total_already_invalid, total_out_of_bounds
    if (!ranges || nr_ranges <= 0) {
        ftl_err("TRIM: Invalid ranges or count\n");
        return 0;
    }
    // printf("TRIM: Processing %d ranges (attributes=0x%x)\n", nr_ranges, attributes);

    for (int range_idx = 0; range_idx < nr_ranges; range_idx++) {
        uint64_t slba = le64_to_cpu(ranges[range_idx].slba);
        uint32_t nlb = le32_to_cpu(ranges[range_idx].nlb);
        // uint32_t cattr = le32_to_cpu(ranges[range_idx].cattr);

        uint64_t start_lpn = slba / spp->secs_per_pg;
        uint64_t end_lpn = (slba + nlb - 1) / spp->secs_per_pg;
        uint64_t lpn;
        struct ppa ppa;
        int trimmed_pages = 0;
        int already_invalid = 0;

        // ftl_debug("TRIM Range %d: LBA %lu + %u sectors, LPN range %lu-%lu (%lu pages), cattr=0x%x\n", 
        //        range_idx, slba, nlb, start_lpn, end_lpn, end_lpn - start_lpn + 1, cattr);

        // Boundary check
        if (end_lpn >= spp->tt_pgs) {
            ftl_err("TRIM: Range %d exceeds FTL capacity - end_lpn=%lu, tt_pgs=%d\n", 
                   range_idx, end_lpn, spp->tt_pgs);
            total_out_of_bounds++;
            continue;  // Skip this range, continue with others
        }

        // Process each LPN in this range
        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);

            // Skip already unmapped/invalid pages
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                already_invalid++;
                continue;
            }

            // Invalidate the existing mapped page
            mark_page_invalid(ssd, &ppa);

            // Clear reverse mapping
            set_rmap_ent(ssd, INVALID_LPN, &ppa);

            // Set mapping table entry as unmapped
            ppa.ppa = UNMAPPED_PPA;
            set_maptbl_ent(ssd, lpn, &ppa);

            trimmed_pages++;
        }

        total_trimmed_pages += trimmed_pages;
        total_already_invalid += already_invalid;

        // ftl_debug("TRIM Range %d: %d pages trimmed, %d already invalid\n", 
        //        range_idx, trimmed_pages, already_invalid);
    }

    ftl_debug("TRIM: Completed - %d pages trimmed, %d already invalid, %d out of bounds across %d ranges\n", 
           total_trimmed_pages, total_already_invalid, total_out_of_bounds, nr_ranges);

    // Free the ranges array
    g_free(ranges);
    req->dsm_ranges = NULL;
    req->dsm_nr_ranges = 0;
    req->dsm_attributes = 0;

    return 0;  // Assume TRIM operations have no NAND latency

}

// void ssd_trim_fdp_style(FemuCtrl *n, NvmeRequest *req, uint64_t slba, uint32_t nlb){

//     struct ssd *ssd = n->ssd;
//     struct ssdparams *spp = &ssd->sp;
//     struct ppa ppa;
//     // erase only relevent blocks 
//     //struct nand_block *blk;
//     //struct nand_page *pg;

//     //Prototype erase all blocks 
//     //TODO FIXME : fix with lba range
// #ifdef CYLON_FDP_TRIM_ERASE_ALL
//     //#RESET ALL 
//     int ch, lun, bbk; 
//     struct nand_lun *lunp;
//     NvmeEnduranceGroup *endgrp = &n->subsys->endgrp;
//     FemuReclaimUnit *v_ru=NULL;
//     FemuReclaimGroup *v_rg=ssd->rg;
//     NvmeRuHandle *ruh;

//     for (ch = 0; ch < spp->nchs; ++ch)
//     {
//         for (lun = 0; lun < spp->luns_per_ch; ++lun)
//         {
//             for (bbk = 0; bbk < spp->blks_per_pl; ++bbk){
//                 ppa.g.ch = ch;
//                 ppa.g.lun = lun;
//                 ppa.g.pl = 0;
//                 ppa.g.blk = bbk;
//                 lunp = get_lun(ssd, &ppa);
//                 //ftl_err("FEMU lpn %ld \n", lpn);

//                 mark_block_free(ssd, &ppa);        
                

//                 if (spp->enable_gc_delay)
//                 {
//                     struct nand_cmd gce;
//                     gce.type = GC_IO;
//                     gce.cmd = NAND_ERASE;
//                     gce.stime = 0;
//                     ssd_advance_status(ssd, &ppa, &gce);
//                 }

//                 lunp->gc_endtime = lunp->next_lun_avail_time;
//             }
//         }
//     }
//     //nvme_fdp_stat_inc(&ssd->n->subsys->endgrp.fdp.mbe, (uint64_t)(cnt * ((spp->secsz * spp->secs_per_pg)/1024) * spp->pgs_per_blk)/1024 );
//     //mark_line_free(ssd, &ppa); //free line in mark ru free
//     //loop till victim queue empty 
//     //      get first 
//     //      remove item
    
//     //FIXME : fix when multi rg supoprt
//     //1. victim ru
//     while( (v_ru = pqueue_peek(v_rg->ru_mgmt->victim_ru_pq)) != NULL ){
//         pqueue_remove(v_rg->ru_mgmt->victim_ru_pq, v_ru);     //      remove item
//         v_rg->ru_mgmt->victim_ru_cnt--;
//         mark_ru_free(ssd, v_ru->rgidx, v_ru);                //      mark it free
//     }
//     //2. full ru
//     struct ru_mgmt *rm = v_rg->ru_mgmt;
//     while( (v_ru = (FemuReclaimUnit *) QTAILQ_FIRST(&rm->full_ru_list) )!= NULL ){
//         QTAILQ_REMOVE(&rm->full_ru_list, v_ru, entry);
//         //pqueue_remove(v_rg->ru_mgmt->free_ru_list, v_ru);     //      remove item
//         v_rg->ru_mgmt->full_ru_cnt--;
//         mark_ru_free(ssd, v_ru->rgidx, v_ru);                //      mark it free
//     }
//     //3. working (open) ru
//     ruh = endgrp->fdp.ruhs;
//     for (int i = 0; i < endgrp->fdp.nruh; i++, ruh++) {
//         ruh->hbmw = 0;
//         ruh->mbmw = 0;
//         ruh->mbe = 0;
//         ssd->ruhs[i].hbmw = 0;
//         ssd->ruhs[i].mbmw = 0;
//         ssd->ruhs[i].mbe = 0;
//         if ( !( ssd->ruhs[i].curr_ru == NULL) ){
//             //reclaim 
//             mark_ru_free(ssd , ssd->ruhs[i].curr_ru->rgidx, ssd->ruhs[i].curr_ru);
//         }
//         if ( (ssd->ruhs[i].ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED ) && ( ssd->ruhs[i].gc_ru != NULL )){
//             mark_ru_free(ssd , ssd->ruhs[i].gc_ru->rgidx, ssd->ruhs[i].gc_ru);
//             ssd->ruhs[i].gc_ru=NULL;
//         }
//         ssd->ruhs[i].curr_ru = NULL;
//         //ssd->ruhs[i].rus[]
//         ssd->ruhs[i].rus[v_rg->rgidx] = fdp_get_new_ru(ssd , v_rg->rgidx, ssd->ruhs[i].ruhid);
//         ssd->ruhs[i].ruh->rus[v_rg->rgidx] = ssd->ruhs[i].rus[v_rg->rgidx]->nvme_ru;
//         ssd->ruhs[i].curr_ru = ssd->ruhs[i].rus[v_rg->rgidx];
//         ssd->ruhs[i].ruh->hbmw = 0 ;
//         ssd->ruhs[i].ruh->mbmw = 0 ;
//         ssd->ruhs[i].ruh->mbe = 0 ;
//     }
//     ssd_reset_maptbl(ssd);
//     femu_log("TRIM victim ru cnt : %d \n", v_rg->ru_mgmt->victim_ru_cnt );
//     ssd->n->subsys->endgrp.fdp.hbmw = 0 ;
//     ssd->n->subsys->endgrp.fdp.mbmw = 0 ;
//     ssd->n->subsys->endgrp.fdp.mbe = 0 ;
// #else
//     //#DEFAULT
//     //for given LBA space, TRIM func should
//         //1. invalidate the ppa in blk struct
//         //2. Update ppa "invalid"
//         //3. Possibly have quick erase but now can leave it to GC 
//         //4. For FDP, make sure the corresponding RU should be enqued to vicim queue
//         //5. May decrease or increase FDP counter such as "available space" but leave it for now.
//     uint64_t lba = slba;
//     uint64_t start_lpn = lba / spp->secs_per_pg;
//     uint64_t end_lpn = (lba + nlb - 1) / spp->secs_per_pg;
//     uint64_t lpn;
//     uint64_t start_blk_index ;
//     uint64_t end_blk_index ; 
//     #ifdef FEMU_DEBUG_FTL
//     ppa = get_maptbl_ent(ssd, start_lpn);
//     start_blk_index = ppa.g.blk;
//     ppa = get_maptbl_ent(ssd, end_lpn);
//     end_blk_index = ppa.g.blk;
//     ftl_debug("req TRIM sbla %ld start lpn %ld nlb %d end lpn %ld start_blk id %ld end_blk id %ld flags %d %x cdw10 %d %x cd11 %d %x\n", slba, start_lpn, nlb, end_lpn, start_blk_index, end_blk_index,req->cmd.flags, req->cmd.flags,req->cmd.cdw10,req->cmd.cdw10,req->cmd.cdw11,req->cmd.cdw11 );
//     #endif
//     if (end_lpn >= spp->tt_pgs)
//     {
//         ftl_err("start_lpn=%ld end_lpn=%ld" PRIu64 ",tt_pgs=%d\n",start_lpn, end_lpn, ssd->sp.tt_pgs);
//         // end_lpn = spp->tt_pgs; 
//     }else {
//         for (lpn = start_lpn; lpn <= end_lpn; lpn++)
//         {
//             ppa = get_maptbl_ent(ssd, lpn);
//             ftl_assert(&ppa != NULL);
//             if (mapped_ppa(&ppa))
//             {
//                 mark_page_invalid(ssd, &ppa);   //This does 1. block vpc-=1 and line vpc-=1 2.the RU victimize as well. 
//                 set_rmap_ent(ssd, INVALID_LPN, &ppa);   //map invalid set
//             }
//         }
//     }
// #endif
// }

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int16_t rgidx = 0;
    int rc;
    int i;

    while (!*(ssd->dataplane_started_ptr))
    {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1)
    {
        for (i = 1; i <= n->nr_pollers; i++)
        {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1)
            {
                ftl_err("FEMU: FTL to_ftl dequeue failed\n");
            }

            // ftl_assert(req);
            switch (req->cmd.opcode)
            {
            case NVME_CMD_FLUSH:
                ftl_err("FTL received NVME_CMD_FLUSH type %d(0x%x), ERROR\n", req->cmd.opcode, req->cmd.opcode);    
                ftl_assert(req != NULL);
                ftl_assert(req->sq != NULL);
                break;
            case NVME_CMD_WRITE:
                lat = nvme_do_write_fdp(n, req, req->slba, req->nlb);
                // if(n->endgrp->fdp.enabled){
                //     ftl_debug("ftl_thread fdp_write ready \n");
                //     lat = nvme_do_write_fdp(n, req, req->slba, req->nlb);
                // }
                // else{
                //     //lat = ssd_write(ssd, req);
                // }
                break;
            case NVME_CMD_READ:
                lat = ssd_read(ssd, req);
                break;
            case NVME_CMD_DSM:
                lat = 25000;
                ftl_debug(" req->cmd cdw10 %d (%x) cdq 11 %d (%x) \n", req->cmd.cdw10,req->cmd.cdw10,req->cmd.cdw11,req->cmd.cdw11);
                if (req->dsm_ranges && req->dsm_nr_ranges > 0) {
                    lat = ssd_trim(ssd, req);
                }
                // if (req->cmd.cdw11 & 0x4 ){
                //     //DEALLOCATE
                //     //ssd_trim_fdp_style(n, req, req->slba, req->nlb);

                // }
                break;
            default:
                if (req->cmd.opcode == NVME_CMD_IO_MGMT_RECV)
                {
                    ftl_log("FTL thread received NVME_CMD_IO_MGMT_RECV. Nothing to handle\n");
                }
                else
                {
                    //BUG HERE [FEMU] FTL-Err: FTL received unkown request type 0(0x0), ERROR
                    ftl_err("FTL received unkown request type %d(0x%x), ERROR\n", req->cmd.opcode, req->cmd.opcode);    
                }
                lat = 25000;
                break;
            }

            req->reqlat = lat;
            req->expire_time += lat;
            
            //BUG HERE Thread 9 "qemu-system-x86" received signal SIGSEGV, Segmentation fault.
            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1); 
            
            if (rc != 1)
            {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            /* clean one line if needed (in the background) */
            if (!((rgidx = should_gc_fdp_style(ssd)) < 0))
            {
                //ftl_log("   FTL thread decide to do gc in FDP style. (should_gc_fdp_style(ssd) >= 0) \n");
                //do_gc_fdp_style(ssd, rgidx, 0, false);
                if (ssd->nrg == 1)
                    do_gc_fdp_style(ssd, 0, 0, false);
                else
                {
                    do_gc_fdp_style(ssd, rgidx, 0, false);
                }
            }
        }
    }

    return NULL;
}
