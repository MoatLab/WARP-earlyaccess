#ifndef __FEMU_FTL_H
#define __FEMU_FTL_H

#include "../nvme.h"

#define INVALID_PPA     (~(0ULL))
#define INVALID_LPN     (~(0ULL))
#define UNMAPPED_PPA    (~(0ULL))

#define SSD_STREAM_WRITE 

typedef struct FemuReclaimGroup FemuReclaimGroup;
typedef struct FemuRuHandle FemuRuHandle;
typedef struct FemuReclaimUnit FemuReclaimUnit; 

enum {
    NAND_READ =  0,
    NAND_WRITE = 1,
    NAND_ERASE = 2,

    NAND_READ_LATENCY = 40000,
    NAND_PROG_LATENCY = 200000,
    NAND_ERASE_LATENCY = 2000000,
};

enum {
    USER_IO = 0,
    GC_IO = 1,
};

enum {
    SEC_FREE = 0,
    SEC_INVALID = 1,
    SEC_VALID = 2,

    PG_FREE = 0,
    PG_INVALID = 1,
    PG_VALID = 2
};

enum {
    FEMU_ENABLE_GC_DELAY = 1,
    FEMU_DISABLE_GC_DELAY = 2,

    FEMU_ENABLE_DELAY_EMU = 3,
    FEMU_DISABLE_DELAY_EMU = 4,

    FEMU_RESET_ACCT = 5,
    FEMU_ENABLE_LOG = 6,
    FEMU_DISABLE_LOG = 7,
};


#define BLK_BITS    (16)
#define PG_BITS     (16)
#define SEC_BITS    (8)
#define PL_BITS     (8)
#define LUN_BITS    (8)
#define CH_BITS     (7)

/* describe a physical page addr */
struct ppa {
    union {
        struct {
            //uint64_t spblk : ? 
            uint64_t blk : BLK_BITS;
            uint64_t pg  : PG_BITS;
            uint64_t sec : SEC_BITS;
            uint64_t pl  : PL_BITS;
            uint64_t lun : LUN_BITS;
            uint64_t ch  : CH_BITS;
            uint64_t rsv : 1;
        } g;

        uint64_t ppa;
    };
};

typedef int nand_sec_status_t;

struct nand_page {
    nand_sec_status_t *sec;
    int nsecs;
    int status;
};

struct nand_block {
    struct nand_page *pg;
    int npgs;
    int ipc; /* invalid page count */
    int vpc; /* valid page count */
    int erase_cnt;
    int wp; /* current write pointer */
};

struct nand_plane {
    struct nand_block *blk;
    int nblks;
};

struct nand_lun {
    struct nand_plane *pl;
    int npls;
    uint64_t next_lun_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct ssd_channel {
    struct nand_lun *lun;
    int nluns;
    uint64_t next_ch_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct ssdparams {
    int secsz;        /* sector size in bytes */
    int secs_per_pg;  /* # of sectors per page */
    int pgs_per_blk;  /* # of NAND pages per block */
    int blks_per_pl;  /* # of blocks per plane */
    int pls_per_lun;  /* # of planes per LUN (Die) */
    int luns_per_ch;  /* # of LUNs per channel */
    int nchs;         /* # of channels in the SSD */

    int pg_rd_lat;    /* NAND page read latency in nanoseconds */
    int pg_wr_lat;    /* NAND page program latency in nanoseconds */
    int blk_er_lat;   /* NAND block erase latency in nanoseconds */
    int ch_xfer_lat;  /* channel transfer latency for one page in nanoseconds
                       * this defines the channel bandwith
                       */

    double gc_thres_pcent;
    int gc_thres_lines;
    double gc_thres_pcent_high;
    int gc_thres_lines_high;
    bool enable_gc_delay;

    /* below are all calculated values */
    int secs_per_blk; /* # of sectors per block */
    int secs_per_pl;  /* # of sectors per plane */
    int secs_per_lun; /* # of sectors per LUN */
    int secs_per_ch;  /* # of sectors per channel */
    int tt_secs;      /* # of sectors in the SSD */

    int pgs_per_pl;   /* # of pages per plane */
    int pgs_per_lun;  /* # of pages per LUN (Die) */
    int pgs_per_ch;   /* # of pages per channel */
    int tt_pgs;       /* total # of pages in the SSD */

    int blks_per_lun; /* # of blocks per LUN */
    int blks_per_ch;  /* # of blocks per channel */
    int tt_blks;      /* total # of blocks in the SSD */

    int secs_per_line;
    int pgs_per_line;
    int blks_per_line;
    int tt_lines;

    int pls_per_ch;   /* # of planes per channel */
    int tt_pls;       /* total # of planes in the SSD */

    int tt_luns;      /* total # of LUNs in the SSD */


    int lines_per_ru;
};

typedef struct line {
    int id;  /* line id, the same as corresponding block id */
    int ipc; /* invalid page count in this line */
    int vpc; /* valid page count in this line */
    QTAILQ_ENTRY(line) entry; /* in either {free,victim,full} list */
    /* position in the priority queue for victim lines */
    size_t                  pos;
} line;

/* wp: record next write addr */
struct write_pointer {
    struct line *curline;
    int ch;
    int lun;
    int pg;
    int blk;
    int pl;
};

struct line_mgmt {
    //Superblock management
    struct line *lines;
    /* free line list, we only need to maintain a list of blk numbers */
    QTAILQ_HEAD(free_line_list, line) free_line_list;
    pqueue_t *victim_line_pq;
    //QTAILQ_HEAD(victim_line_list, line) victim_line_list;
    QTAILQ_HEAD(full_line_list, line) full_line_list;
    int tt_lines;
    int free_line_cnt;
    int victim_line_cnt;
    int full_line_cnt;
};

struct nand_cmd {
    int type;
    int cmd;
    int64_t stime; /* Coperd: request arrival time */
};

    #ifdef SUPERBLOCK
    typedef struct Superblock {
        // Inho: Here, Reclaim Unit can have multiple superblocks, not vice versa. This should be fixed.
        // Since NvmeReclaimUnit < is resides in nvme.h and superblock is in ftl.h
        // How can I solve this? --> define FemuReclaimUnit in here and wrap NvmeReclaimUnit
        NvmeReclaimUnit *ru;                
        NvmeRuHandle *ruh;    
        struct line *line;
        uint32_t ruhid;
        struct write_pointer *wptr;
        QTAILQ_ENTRY(Superblock) entry; /* in either {free,victim,full} list */
    }Superblock;

    typedef struct super_mgmt{
        Superblock *superblocks;

        QTAILQ_HEAD(free_super_list, Superblock) free_super_list;
        pqueue_t *victim_superblock_pq;
        //QTAILQ_HEAD(victim_line_list, line) victim_line_list;
        QTAILQ_HEAD(full_super_list, Superblock) full_super_list;

        int tt_supers;          //tt:total
        int free_super_cnt;
        int victim_super_cnt;
        int full_super_cnt;
    }super_mgmt;
    #endif

typedef struct rg_mgmt{
    FemuReclaimGroup * rgs;
}rg_mgmt;

typedef struct ru_mgmt{
    QTAILQ_HEAD(free_ru_list, FemuReclaimUnit) free_ru_list;
    //pqueue_t *victim_ru_pq_type_init;    
    //pqueue_t *victim_ru_pq_type_permnt;
    pqueue_t *victim_ru_pq;

    //QTAILQ_HEAD(victim_line_list, line) victim_line_list;
    QTAILQ_HEAD(full_ru_list, FemuReclaimUnit) full_ru_list;
    int tt_rus;
    int free_ru_cnt;
    int victim_ru_cnt_type_init;
    int victim_ru_cnt_type_permnt;
    int victim_ru_cnt;
    int full_ru_cnt;
}ru_mgmt;

typedef struct FemuReclaimGroup{

    //FemuRuHandle *ruhs;
    FemuReclaimUnit *rus;
    uint64_t tt_nru;
    struct ru_mgmt *ru_mgmt;


}FemuReclaimGroup;

typedef struct FemuReclaimUnit{
    uint16_t rgidx;
    NvmeReclaimUnit *ru;                
    FemuRuHandle *ruh;    
    //struct write_pointer *wptr;
    struct write_pointer *ssd_wptr;
    struct line **lines;
    QTAILQ_ENTRY(FemuReclaimUnit) entry; 
    int vpc;
    int pri;
    int pos;
    int n_lines;
    int next_line_index;

}FemuReclaimUnit;

typedef struct FemuRuHandle{
    uint16_t ruh_type;
    uint16_t ruhid;               //how can ftl know this?
    //int n_ru;
    int ru_in_use_cnt;
    uint16_t curr_rg;               //init 0
    NvmeRuHandle *ruh;              //1. pointer to original reclaim unit handle
    FemuReclaimUnit **rus;           //2. List that this ruh have. I don't think this is necessary. 
    FemuReclaimUnit *curr_ru;       //3. Current wptr (RU).

}FemuRuHandle;

struct ssd {
    char *ssdname;
    struct ssdparams sp;
    struct ssd_channel *ch;
    struct ppa *maptbl; /* page level mapping table */
    uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
    struct write_pointer wp;
    struct line_mgmt lm;

    /* lockless ring for communication with NVMe IO thread */
    struct rte_ring **to_ftl;
    struct rte_ring **to_poller;
    bool *dataplane_started_ptr;
    QemuThread ftl_thread;

    /* FEMU backend internal units for the FDP and Stream SSD*/
    FemuReclaimGroup *rg;
    FemuReclaimUnit **rus;
    FemuRuHandle *ruhs;
    
};

void ssd_init(FemuCtrl *n);
//#define FEMU_DEBUG_FTL
#ifdef FEMU_DEBUG_FTL
#define ftl_debug(fmt, ...) \
    do { fprintf(stderr, "[FEMU] FTL-Dbg: " fmt, ## __VA_ARGS__); } while (0)
#else
#define ftl_debug(fmt, ...) \
    do { } while (0)
#endif

#define ftl_err(fmt, ...) \
    do { fprintf(stderr, "[FEMU] FTL-Err: " fmt, ## __VA_ARGS__); } while (0)

#define ftl_log(fmt, ...) \
    do { printf("[FEMU] FTL-Log: " fmt, ## __VA_ARGS__); } while (0)

#define fdp_log(fmt, ...) \
    do { printf("[FEMU] FTL-Log: " fmt, ## __VA_ARGS__); } while (0)

/* FEMU assert() */
#ifdef FEMU_DEBUG_FTL
#define ftl_assert(expression) assert(expression)
#else
#define ftl_assert(expression)
#endif

#endif
