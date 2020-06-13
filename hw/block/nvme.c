/*
 * QEMU NVM Express Controller
 *
 * Copyright (c) 2012, Intel Corporation
 *
 * Written by Keith Busch <keith.busch@intel.com>
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

/**
 * Reference Specs: http://www.nvmexpress.org, 1.4, 1.3, 1.2, 1.1, 1.0e
 *
 *  https://nvmexpress.org/developers/nvme-specification/
 */

/**
 * Usage: add options:
 *      -drive file=<file>,if=none,id=<drive_id>
 *      -device nvme,drive=<drive_id>,serial=<serial>,id=<id[optional]>, \
 *              cmb_size_mb=<cmb_size_mb[optional]>, \
 *              [pmrdev=<mem_backend_file_id>,] \
 *              max_ioqpairs=<N[optional]>, \
 *              aerl=<N[optional]>, aer_max_queued=<N[optional]>, \
 *              mdts=<N[optional]>, zoned=<true|false[optional]>
 *
 * Note cmb_size_mb denotes size of CMB in MB. CMB is assumed to be at
 * offset 0 in BAR2 and supports only WDS, RDS and SQS for now.
 *
 * cmb_size_mb= and pmrdev= options are mutually exclusive due to limitation
 * in available BAR's. cmb_size_mb= will take precedence over pmrdev= when
 * both provided.
 * Enabling pmr emulation can be achieved by pointing to memory-backend-file.
 * For example:
 * -object memory-backend-file,id=<mem_id>,share=on,mem-path=<file_path>, \
 *  size=<size> .... -device nvme,...,pmrdev=<mem_id>
 *
 *
 * nvme device parameters
 * ~~~~~~~~~~~~~~~~~~~~~~
 * - `aerl`
 *   The Asynchronous Event Request Limit (AERL). Indicates the maximum number
 *   of concurrently outstanding Asynchronous Event Request commands suppoert
 *   by the controller. This is a 0's based value.
 *
 * - `aer_max_queued`
 *   This is the maximum number of events that the device will enqueue for
 *   completion when there are no oustanding AERs. When the maximum number of
 *   enqueued events are reached, subsequent events will be dropped.
 *
 * Setting `zoned` to true makes the driver emulate zoned namespaces.
 * In this case, of the following options are available to configure zoned
 * operation:
 *     zone_size=<zone size in MiB, default: 128MiB>
 *
 *     zone_capacity=<zone capacity in MiB, default: zone_size>
 *         The value 0 (default) forces zone capacity to be the same as zone
 *         size. The value of this property may not exceed zone size.
 *
 *     zone_file=<zone metadata file name, default: none>
 *         Zone metadata file, if specified, allows zone information
 *         to be persistent across shutdowns and restarts.
 *
 *     zone_descr_ext_size=<zone descriptor extension size, default 0>
 *         This value needs to be specified in 64B units. If it is zero,
 *         namespace(s) will not support zone descriptor extensions.
 *
 *     max_active=<Maximum Active Resources (zones), default: 0 - no limit>
 *
 *     max_open=<Maximum Open Resources (zones), default: 0 - no limit>
 *
 *     zone_append_size_limit=<zone append size limit, in KiB, default: MDTS>
 *         The maximum I/O size that can be supported by Zone Append
 *         command. Since internally this this value is maintained as
 *         ZASL = log2(<maximum append size> / <page size>), some
 *         values assigned to this property may be rounded down and
 *         result in a lower maximum ZA data size being in effect.
 *         If MDTS property is not assigned, the default value of 128KiB is
 *         used as ZASL.
 *
 *     offline_zones=<the number of offline zones to inject, default: 0>
 *
 *     rdonly_zones=<the number of read-only zones to inject, default: 0>
 *
 *     cross_zone_read=<enables Read Across Zone Boundaries, default: true>
 *
 *     fill_pattern=<data fill pattern, default: 0x00>
 *         The byte pattern to return for any portions of unwritten data
 *         during read.
 *
 *     active_excursions=<enable emulation of zone excursions, default: false>
 *
 *     reset_rcmnd_delay=<Reset Zone Recommended Delay in milliseconds>
 *         The amount of time that passes between the moment when a zone
 *         enters Full state and when Reset Zone Recommended attribute
 *         is set for that zone.
 *
 *     reset_rcmnd_limit=<Reset Zone Recommended Limit in milliseconds>
 *         If this value is zero (default), RZR attribute is not set for
 *          any zones.
 *
 *     finish_rcmnd_delay=<Finish Zone Recommended Delay in milliseconds>
 *         The amount of time that passes between the moment when a zone
 *         enters an Open or Closed state and when Finish Zone Recommended
 *         attribute is set for that zone.
 *
 *     finish_rcmnd_limit=<Finish Zone Recommended Limit in milliseconds>
 *         If this value is zero (default), FZR attribute is not set for
 *         any zones.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "crypto/random.h"
#include "hw/block/block.h"
#include "hw/pci/msix.h"
#include "hw/pci/pci.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "sysemu/sysemu.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "sysemu/hostmem.h"
#include "sysemu/block-backend.h"
#include "exec/memory.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/cutils.h"
#include "trace.h"
#include "nvme.h"

#define NVME_MAX_IOQPAIRS 0xffff
#define NVME_DB_SIZE  4
#define NVME_SPEC_VER 0x00010300
#define NVME_CMB_BIR 2
#define NVME_PMR_BIR 2
#define NVME_TEMPERATURE 0x143
#define NVME_TEMPERATURE_WARNING 0x157
#define NVME_TEMPERATURE_CRITICAL 0x175
#define NVME_NUM_FW_SLOTS 1

#define NVME_GUEST_ERR(trace, fmt, ...) \
    do { \
        (trace_##trace)(__VA_ARGS__); \
        qemu_log_mask(LOG_GUEST_ERROR, #trace \
            " in %s: " fmt "\n", __func__, ## __VA_ARGS__); \
    } while (0)

static const bool nvme_feature_support[NVME_FID_MAX] = {
    [NVME_ARBITRATION]              = true,
    [NVME_POWER_MANAGEMENT]         = true,
    [NVME_TEMPERATURE_THRESHOLD]    = true,
    [NVME_ERROR_RECOVERY]           = true,
    [NVME_VOLATILE_WRITE_CACHE]     = true,
    [NVME_NUMBER_OF_QUEUES]         = true,
    [NVME_INTERRUPT_COALESCING]     = true,
    [NVME_INTERRUPT_VECTOR_CONF]    = true,
    [NVME_WRITE_ATOMICITY]          = true,
    [NVME_ASYNCHRONOUS_EVENT_CONF]  = true,
    [NVME_TIMESTAMP]                = true,
};

static const uint32_t nvme_feature_cap[NVME_FID_MAX] = {
    [NVME_TEMPERATURE_THRESHOLD]    = NVME_FEAT_CAP_CHANGE,
    [NVME_VOLATILE_WRITE_CACHE]     = NVME_FEAT_CAP_CHANGE,
    [NVME_NUMBER_OF_QUEUES]         = NVME_FEAT_CAP_CHANGE,
    [NVME_ASYNCHRONOUS_EVENT_CONF]  = NVME_FEAT_CAP_CHANGE,
    [NVME_TIMESTAMP]                = NVME_FEAT_CAP_CHANGE,
};

static void nvme_process_sq(void *opaque);
static void nvme_sync_zone_file(NvmeCtrl *n, NvmeNamespace *ns,
                                NvmeZone *zone, int len);

static uint16_t nvme_cid(NvmeRequest *req)
{
    if (!req) {
        return 0xffff;
    }

    return le16_to_cpu(req->cqe.cid);
}

static uint16_t nvme_sqid(NvmeRequest *req)
{
    return le16_to_cpu(req->sq->sqid);
}

/*
 * Add a zone to the tail of a zone list.
 */
static void nvme_add_zone_tail(NvmeCtrl *n, NvmeNamespace *ns, NvmeZoneList *zl,
                               NvmeZone *zone)
{
    uint32_t idx = (uint32_t)(zone - ns->zone_array);

    assert(nvme_zone_not_in_list(zone));

    if (!zl->size) {
        zl->head = zl->tail = idx;
        zone->next = zone->prev = NVME_ZONE_LIST_NIL;
    } else {
        ns->zone_array[zl->tail].next = idx;
        zone->prev = zl->tail;
        zone->next = NVME_ZONE_LIST_NIL;
        zl->tail = idx;
    }
    zl->size++;
    nvme_set_zone_meta_dirty(n, ns, true);
}

/*
 * Remove a zone from a zone list. The zone must be linked in the list.
 */
static void nvme_remove_zone(NvmeCtrl *n, NvmeNamespace *ns, NvmeZoneList *zl,
                             NvmeZone *zone)
{
    uint32_t idx = (uint32_t)(zone - ns->zone_array);

    assert(!nvme_zone_not_in_list(zone));

    --zl->size;
    if (zl->size == 0) {
        zl->head = NVME_ZONE_LIST_NIL;
        zl->tail = NVME_ZONE_LIST_NIL;
        nvme_set_zone_meta_dirty(n, ns, true);
    } else if (idx == zl->head) {
        zl->head = zone->next;
        ns->zone_array[zl->head].prev = NVME_ZONE_LIST_NIL;
        nvme_set_zone_meta_dirty(n, ns, true);
    } else if (idx == zl->tail) {
        zl->tail = zone->prev;
        ns->zone_array[zl->tail].next = NVME_ZONE_LIST_NIL;
        nvme_set_zone_meta_dirty(n, ns, true);
    } else {
        ns->zone_array[zone->next].prev = zone->prev;
        ns->zone_array[zone->prev].next = zone->next;
    }

    zone->prev = zone->next = 0;
}

/*
 * Take the first zone out from a list, return NULL if the list is empty.
 */
static NvmeZone *nvme_remove_zone_head(NvmeCtrl *n, NvmeNamespace *ns,
                                       NvmeZoneList *zl)
{
    NvmeZone *zone = nvme_peek_zone_head(ns, zl);

    if (zone) {
        --zl->size;
        if (zl->size == 0) {
            zl->head = NVME_ZONE_LIST_NIL;
            zl->tail = NVME_ZONE_LIST_NIL;
        } else {
            zl->head = zone->next;
            ns->zone_array[zl->head].prev = NVME_ZONE_LIST_NIL;
        }
        zone->prev = zone->next = 0;
        nvme_set_zone_meta_dirty(n, ns, true);
    }

    return zone;
}

/*
 * Check if we can open a zone without exceeding open/active limits.
 * AOR stands for "Active and Open Resources" (see TP 4053 section 2.5).
 */
static int nvme_aor_check(NvmeCtrl *n, NvmeNamespace *ns,
                          uint32_t act, uint32_t opn)
{
    if (n->params.max_active_zones != 0 &&
        ns->nr_active_zones + act > n->params.max_active_zones) {
        trace_pci_nvme_err_insuff_active_res(n->params.max_active_zones);
        return NVME_ZONE_TOO_MANY_ACTIVE | NVME_DNR;
    }
    if (n->params.max_open_zones != 0 &&
        ns->nr_open_zones + opn > n->params.max_open_zones) {
        trace_pci_nvme_err_insuff_open_res(n->params.max_open_zones);
        return NVME_ZONE_TOO_MANY_OPEN | NVME_DNR;
    }

    return NVME_SUCCESS;
}

static inline void nvme_aor_inc_open(NvmeCtrl *n, NvmeNamespace *ns)
{
    assert(ns->nr_open_zones >= 0);
    if (n->params.max_open_zones) {
        ns->nr_open_zones++;
        assert(ns->nr_open_zones <= n->params.max_open_zones);
    }
}

static inline void nvme_aor_dec_open(NvmeCtrl *n, NvmeNamespace *ns)
{
    if (n->params.max_open_zones) {
        assert(ns->nr_open_zones > 0);
        ns->nr_open_zones--;
    }
    assert(ns->nr_open_zones >= 0);
}

static inline void nvme_aor_inc_active(NvmeCtrl *n, NvmeNamespace *ns)
{
    assert(ns->nr_active_zones >= 0);
    if (n->params.max_active_zones) {
        ns->nr_active_zones++;
        assert(ns->nr_active_zones <= n->params.max_active_zones);
    }
}

static inline void nvme_aor_dec_active(NvmeCtrl *n, NvmeNamespace *ns)
{
    if (n->params.max_active_zones) {
        assert(ns->nr_active_zones > 0);
        ns->nr_active_zones--;
        assert(ns->nr_active_zones >= ns->nr_open_zones);
    }
    assert(ns->nr_active_zones >= 0);
}

static void nvme_set_rzr(NvmeCtrl *n, NvmeNamespace *ns, NvmeZone *zone)
{
    assert(zone->flags & NVME_ZFLAGS_SET_RZR);
    zone->tstamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    zone->flags &= ~NVME_ZFLAGS_TS_DELAY;
    zone->d.za |= NVME_ZA_RESET_RECOMMENDED;
    zone->flags &= ~NVME_ZFLAGS_SET_RZR;
    trace_pci_nvme_zone_reset_recommended(zone->d.zslba);
}

static void nvme_clear_rzr(NvmeCtrl *n, NvmeNamespace *ns,
                           NvmeZone *zone, bool notify)
{
    if (n->params.rrl) {
        zone->flags &= ~(NVME_ZFLAGS_SET_RZR | NVME_ZFLAGS_TS_DELAY);
        notify = notify && (zone->d.za & NVME_ZA_RESET_RECOMMENDED);
        zone->d.za &= ~NVME_ZA_RESET_RECOMMENDED;
        zone->tstamp = 0;
    }
}

static void nvme_set_fzr(NvmeCtrl *n, NvmeNamespace *ns, NvmeZone *zone)
{
    assert(zone->flags & NVME_ZFLAGS_SET_FZR);
    zone->tstamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    zone->flags &= ~NVME_ZFLAGS_TS_DELAY;
    zone->d.za |= NVME_ZA_FINISH_RECOMMENDED;
    zone->flags &= ~NVME_ZFLAGS_SET_FZR;
    trace_pci_nvme_zone_finish_recommended(zone->d.zslba);
}

static void nvme_clear_fzr(NvmeCtrl *n, NvmeNamespace *ns,
                           NvmeZone *zone, bool notify)
{
    if (n->params.frl) {
        zone->flags &= ~(NVME_ZFLAGS_SET_FZR | NVME_ZFLAGS_TS_DELAY);
        notify = notify && (zone->d.za & NVME_ZA_FINISH_RECOMMENDED);
        zone->d.za &= ~NVME_ZA_FINISH_RECOMMENDED;
        zone->tstamp = 0;
    }
}

static void nvme_schedule_rzr(NvmeCtrl *n, NvmeNamespace *ns, NvmeZone *zone)
{
    if (n->params.frl) {
        zone->flags &= ~(NVME_ZFLAGS_SET_FZR | NVME_ZFLAGS_TS_DELAY);
        zone->d.za &= ~NVME_ZA_FINISH_RECOMMENDED;
        zone->tstamp = 0;
    }
    if (n->params.rrl) {
        zone->flags |= NVME_ZFLAGS_SET_RZR;
        if (n->params.rzr_delay) {
            zone->tstamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            zone->flags |= NVME_ZFLAGS_TS_DELAY;
        } else {
            nvme_set_rzr(n, ns, zone);
        }
    }
}

static void nvme_schedule_fzr(NvmeCtrl *n, NvmeNamespace *ns, NvmeZone *zone)
{
    if (n->params.rrl) {
        zone->flags &= ~(NVME_ZFLAGS_SET_RZR | NVME_ZFLAGS_TS_DELAY);
        zone->d.za &= ~NVME_ZA_RESET_RECOMMENDED;
        zone->tstamp = 0;
    }
    if (n->params.frl) {
        zone->flags |= NVME_ZFLAGS_SET_FZR;
        if (n->params.fzr_delay) {
            zone->tstamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            zone->flags |= NVME_ZFLAGS_TS_DELAY;
        } else {
            nvme_set_fzr(n, ns, zone);
        }
    }
}

static void nvme_assign_zone_state(NvmeCtrl *n, NvmeNamespace *ns,
                                   NvmeZone *zone, uint8_t state)
{
    if (!nvme_zone_not_in_list(zone)) {
        switch (nvme_get_zone_state(zone)) {
        case NVME_ZONE_STATE_EXPLICITLY_OPEN:
            nvme_remove_zone(n, ns, ns->exp_open_zones, zone);
            nvme_clear_fzr(n, ns, zone, false);
            break;
        case NVME_ZONE_STATE_IMPLICITLY_OPEN:
            nvme_remove_zone(n, ns, ns->imp_open_zones, zone);
            nvme_clear_fzr(n, ns, zone, false);
            break;
        case NVME_ZONE_STATE_CLOSED:
            nvme_remove_zone(n, ns, ns->closed_zones, zone);
            nvme_clear_fzr(n, ns, zone, false);
            break;
        case NVME_ZONE_STATE_FULL:
            nvme_remove_zone(n, ns, ns->full_zones, zone);
            nvme_clear_rzr(n, ns, zone, false);
        }
   }

    nvme_set_zone_state(zone, state);

    switch (state) {
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
        nvme_add_zone_tail(n, ns, ns->exp_open_zones, zone);
        nvme_schedule_fzr(n, ns, zone);
        break;
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        nvme_add_zone_tail(n, ns, ns->imp_open_zones, zone);
        nvme_schedule_fzr(n, ns, zone);
        break;
    case NVME_ZONE_STATE_CLOSED:
        nvme_add_zone_tail(n, ns, ns->closed_zones, zone);
        nvme_schedule_fzr(n, ns, zone);
        break;
    case NVME_ZONE_STATE_FULL:
        nvme_add_zone_tail(n, ns, ns->full_zones, zone);
        nvme_schedule_rzr(n, ns, zone);
        break;
    default:
        zone->d.za = 0;
        /* fall through */
    case NVME_ZONE_STATE_READ_ONLY:
        zone->tstamp = 0;
    }
    nvme_sync_zone_file(n, ns, zone, sizeof(NvmeZone));
}

static bool nvme_addr_is_cmb(NvmeCtrl *n, hwaddr addr)
{
    hwaddr low = n->ctrl_mem.addr;
    hwaddr hi  = n->ctrl_mem.addr + int128_get64(n->ctrl_mem.size);

    return addr >= low && addr < hi;
}

static inline void *nvme_addr_to_cmb(NvmeCtrl *n, hwaddr addr)
{
    assert(nvme_addr_is_cmb(n, addr));

    return &n->cmbuf[addr - n->ctrl_mem.addr];
}

static void nvme_addr_read(NvmeCtrl *n, hwaddr addr, void *buf, int size)
{
    if (n->bar.cmbsz && nvme_addr_is_cmb(n, addr)) {
        memcpy(buf, nvme_addr_to_cmb(n, addr), size);
        return;
    }

    pci_dma_read(&n->parent_obj, addr, buf, size);
}

static int nvme_check_sqid(NvmeCtrl *n, uint16_t sqid)
{
    return sqid < n->params.max_ioqpairs + 1 && n->sq[sqid] != NULL ? 0 : -1;
}

static int nvme_check_cqid(NvmeCtrl *n, uint16_t cqid)
{
    return cqid < n->params.max_ioqpairs + 1 && n->cq[cqid] != NULL ? 0 : -1;
}

static void nvme_inc_cq_tail(NvmeCQueue *cq)
{
    cq->tail++;
    if (cq->tail >= cq->size) {
        cq->tail = 0;
        cq->phase = !cq->phase;
    }
}

static void nvme_inc_sq_head(NvmeSQueue *sq)
{
    sq->head = (sq->head + 1) % sq->size;
}

static uint8_t nvme_cq_full(NvmeCQueue *cq)
{
    return (cq->tail + 1) % cq->size == cq->head;
}

static uint8_t nvme_sq_empty(NvmeSQueue *sq)
{
    return sq->head == sq->tail;
}

static void nvme_irq_check(NvmeCtrl *n)
{
    if (msix_enabled(&(n->parent_obj))) {
        return;
    }
    if (~n->bar.intms & n->irq_status) {
        pci_irq_assert(&n->parent_obj);
    } else {
        pci_irq_deassert(&n->parent_obj);
    }
}

static void nvme_irq_assert(NvmeCtrl *n, NvmeCQueue *cq)
{
    if (cq->irq_enabled) {
        if (msix_enabled(&(n->parent_obj))) {
            trace_pci_nvme_irq_msix(cq->vector);
            msix_notify(&(n->parent_obj), cq->vector);
        } else {
            trace_pci_nvme_irq_pin();
            assert(cq->vector < 32);
            n->irq_status |= 1 << cq->vector;
            nvme_irq_check(n);
        }
    } else {
        trace_pci_nvme_irq_masked();
    }
}

static void nvme_irq_deassert(NvmeCtrl *n, NvmeCQueue *cq)
{
    if (cq->irq_enabled) {
        if (msix_enabled(&(n->parent_obj))) {
            return;
        } else {
            assert(cq->vector < 32);
            n->irq_status &= ~(1 << cq->vector);
            nvme_irq_check(n);
        }
    }
}

static void nvme_req_clear(NvmeRequest *req)
{
    req->ns = NULL;
    memset(&req->cqe, 0x0, sizeof(req->cqe));
}

static void nvme_req_exit(NvmeRequest *req)
{
    if (req->qsg.sg) {
        qemu_sglist_destroy(&req->qsg);
    }

    if (req->iov.iov) {
        qemu_iovec_destroy(&req->iov);
    }
}

static uint16_t nvme_map_addr_cmb(NvmeCtrl *n, QEMUIOVector *iov, hwaddr addr,
                                  size_t len)
{
    if (!len) {
        return NVME_SUCCESS;
    }

    trace_pci_nvme_map_addr_cmb(addr, len);

    if (!nvme_addr_is_cmb(n, addr) || !nvme_addr_is_cmb(n, addr + len - 1)) {
        return NVME_DATA_TRAS_ERROR;
    }

    qemu_iovec_add(iov, nvme_addr_to_cmb(n, addr), len);

    return NVME_SUCCESS;
}

static uint16_t nvme_map_addr(NvmeCtrl *n, QEMUSGList *qsg, QEMUIOVector *iov,
                              hwaddr addr, size_t len)
{
    if (!len) {
        return NVME_SUCCESS;
    }

    trace_pci_nvme_map_addr(addr, len);

    if (nvme_addr_is_cmb(n, addr)) {
        if (qsg && qsg->sg) {
            return NVME_INVALID_USE_OF_CMB | NVME_DNR;
        }

        assert(iov);

        if (!iov->iov) {
            qemu_iovec_init(iov, 1);
        }

        return nvme_map_addr_cmb(n, iov, addr, len);
    }

    if (iov && iov->iov) {
        return NVME_INVALID_USE_OF_CMB | NVME_DNR;
    }

    assert(qsg);

    if (!qsg->sg) {
        pci_dma_sglist_init(qsg, &n->parent_obj, 1);
    }

    qemu_sglist_add(qsg, addr, len);

    return NVME_SUCCESS;
}

static uint16_t nvme_map_prp(NvmeCtrl *n, uint64_t prp1, uint64_t prp2,
                             uint32_t len, NvmeRequest *req)
{
    hwaddr trans_len = n->page_size - (prp1 % n->page_size);
    trans_len = MIN(len, trans_len);
    int num_prps = (len >> n->page_bits) + 1;
    uint16_t status;
    bool prp_list_in_cmb = false;

    QEMUSGList *qsg = &req->qsg;
    QEMUIOVector *iov = &req->iov;

    trace_pci_nvme_map_prp(trans_len, len, prp1, prp2, num_prps);

    if (unlikely(!prp1)) {
        trace_pci_nvme_err_invalid_prp();
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (nvme_addr_is_cmb(n, prp1)) {
        qemu_iovec_init(iov, num_prps);
    } else {
        pci_dma_sglist_init(qsg, &n->parent_obj, num_prps);
    }

    status = nvme_map_addr(n, qsg, iov, prp1, trans_len);
    if (status) {
        return status;
    }

    len -= trans_len;
    if (len) {
        if (unlikely(!prp2)) {
            trace_pci_nvme_err_invalid_prp2_missing();
            return NVME_INVALID_FIELD | NVME_DNR;
        }

        if (len > n->page_size) {
            uint64_t prp_list[n->max_prp_ents];
            uint32_t nents, prp_trans;
            int i = 0;

            if (nvme_addr_is_cmb(n, prp2)) {
                prp_list_in_cmb = true;
            }

            nents = (len + n->page_size - 1) >> n->page_bits;
            prp_trans = MIN(n->max_prp_ents, nents) * sizeof(uint64_t);
            nvme_addr_read(n, prp2, (void *)prp_list, prp_trans);
            while (len != 0) {
                uint64_t prp_ent = le64_to_cpu(prp_list[i]);

                if (i == n->max_prp_ents - 1 && len > n->page_size) {
                    if (unlikely(!prp_ent || prp_ent & (n->page_size - 1))) {
                        trace_pci_nvme_err_invalid_prplist_ent(prp_ent);
                        return NVME_INVALID_FIELD | NVME_DNR;
                    }

                    if (prp_list_in_cmb != nvme_addr_is_cmb(n, prp_ent)) {
                        return NVME_INVALID_USE_OF_CMB | NVME_DNR;
                    }

                    i = 0;
                    nents = (len + n->page_size - 1) >> n->page_bits;
                    prp_trans = MIN(n->max_prp_ents, nents) * sizeof(uint64_t);
                    nvme_addr_read(n, prp_ent, (void *)prp_list,
                        prp_trans);
                    prp_ent = le64_to_cpu(prp_list[i]);
                }

                if (unlikely(!prp_ent || prp_ent & (n->page_size - 1))) {
                    trace_pci_nvme_err_invalid_prplist_ent(prp_ent);
                    return NVME_INVALID_FIELD | NVME_DNR;
                }

                trans_len = MIN(len, n->page_size);
                status = nvme_map_addr(n, qsg, iov, prp_ent, trans_len);
                if (status) {
                    return status;
                }

                len -= trans_len;
                i++;
            }
        } else {
            if (unlikely(prp2 & (n->page_size - 1))) {
                trace_pci_nvme_err_invalid_prp2_align(prp2);
                return NVME_INVALID_FIELD | NVME_DNR;
            }
            status = nvme_map_addr(n, qsg, iov, prp2, len);
            if (status) {
                return status;
            }
        }
    }

    return NVME_SUCCESS;
}

static uint16_t nvme_dma_prp(NvmeCtrl *n, uint8_t *ptr, uint32_t len,
                             uint64_t prp1, uint64_t prp2, DMADirection dir,
                             NvmeRequest *req)
{
    uint16_t status = NVME_SUCCESS;

    status = nvme_map_prp(n, prp1, prp2, len, req);
    if (status) {
        return status;
    }

    /* assert that only one of qsg and iov carries data */
    assert((req->qsg.nsg > 0) != (req->iov.niov > 0));

    if (req->qsg.nsg > 0) {
        uint64_t residual;

        if (dir == DMA_DIRECTION_TO_DEVICE) {
            residual = dma_buf_write(ptr, len, &req->qsg);
        } else {
            residual = dma_buf_read(ptr, len, &req->qsg);
        }

        if (unlikely(residual)) {
            trace_pci_nvme_err_invalid_dma();
            status = NVME_INVALID_FIELD | NVME_DNR;
        }
    } else {
        size_t bytes;

        if (dir == DMA_DIRECTION_TO_DEVICE) {
            bytes = qemu_iovec_to_buf(&req->iov, 0, ptr, len);
        } else {
            bytes = qemu_iovec_from_buf(&req->iov, 0, ptr, len);
        }

        if (unlikely(bytes != len)) {
            trace_pci_nvme_err_invalid_dma();
            status = NVME_INVALID_FIELD | NVME_DNR;
        }
    }

    return status;
}

static uint16_t nvme_map_dptr(NvmeCtrl *n, size_t len, NvmeRequest *req)
{
    NvmeCmd *cmd = &req->cmd;
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);

    return nvme_map_prp(n, prp1, prp2, len, req);
}

static void nvme_post_cqes(void *opaque)
{
    NvmeCQueue *cq = opaque;
    NvmeCtrl *n = cq->ctrl;
    NvmeRequest *req, *next;

    QTAILQ_FOREACH_SAFE(req, &cq->req_list, entry, next) {
        NvmeSQueue *sq;
        hwaddr addr;

        if (nvme_cq_full(cq)) {
            break;
        }

        QTAILQ_REMOVE(&cq->req_list, req, entry);
        sq = req->sq;
        req->cqe.status = cpu_to_le16((req->status << 1) | cq->phase);
        req->cqe.sq_id = cpu_to_le16(sq->sqid);
        req->cqe.sq_head = cpu_to_le16(sq->head);
        addr = cq->dma_addr + cq->tail * n->cqe_size;
        nvme_inc_cq_tail(cq);
        pci_dma_write(&n->parent_obj, addr, (void *)&req->cqe,
            sizeof(req->cqe));
        nvme_req_exit(req);
        QTAILQ_INSERT_TAIL(&sq->req_list, req, entry);
    }
    if (cq->tail != cq->head) {
        nvme_irq_assert(n, cq);
    }
}

static void nvme_fill_data(QEMUSGList *qsg, QEMUIOVector *iov,
                           uint64_t offset, uint8_t pattern)
{
    ScatterGatherEntry *entry;
    uint32_t len, ent_len;

    if (qsg->nsg > 0) {
        entry = qsg->sg;
        for (len = qsg->size; len > 0; len -= ent_len) {
            ent_len = MIN(len, entry->len);
            if (offset > ent_len) {
                offset -= ent_len;
            } else if (offset != 0) {
                dma_memory_set(qsg->as, entry->base + offset,
                               pattern, ent_len - offset);
                offset = 0;
            } else {
                dma_memory_set(qsg->as, entry->base, pattern, ent_len);
            }
            entry++;
        }
    }
}

static void nvme_enqueue_req_completion(NvmeCQueue *cq, NvmeRequest *req)
{
    assert(cq->cqid == req->sq->cqid);
    trace_pci_nvme_enqueue_req_completion(nvme_cid(req), cq->cqid,
                                          req->status);
    QTAILQ_REMOVE(&req->sq->out_req_list, req, entry);
    QTAILQ_INSERT_TAIL(&cq->req_list, req, entry);
    timer_mod(cq->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500);
}

static void nvme_process_aers(void *opaque)
{
    NvmeCtrl *n = opaque;
    NvmeAsyncEvent *event, *next;

    trace_pci_nvme_process_aers(n->aer_queued);

    QTAILQ_FOREACH_SAFE(event, &n->aer_queue, entry, next) {
        NvmeRequest *req;
        NvmeAerResult *result;

        /* can't post cqe if there is nothing to complete */
        if (!n->outstanding_aers) {
            trace_pci_nvme_no_outstanding_aers();
            break;
        }

        /* ignore if masked (cqe posted, but event not cleared) */
        if (n->aer_mask & (1 << event->result.event_type)) {
            trace_pci_nvme_aer_masked(event->result.event_type, n->aer_mask);
            continue;
        }

        QTAILQ_REMOVE(&n->aer_queue, event, entry);
        n->aer_queued--;

        n->aer_mask |= 1 << event->result.event_type;
        n->outstanding_aers--;

        req = n->aer_reqs[n->outstanding_aers];

        result = (NvmeAerResult *) &req->cqe.result32;
        result->event_type = event->result.event_type;
        result->event_info = event->result.event_info;
        result->log_page = event->result.log_page;
        g_free(event);

        req->status = NVME_SUCCESS;

        trace_pci_nvme_aer_post_cqe(result->event_type, result->event_info,
                                    result->log_page);

        nvme_enqueue_req_completion(&n->admin_cq, req);
    }
}

static void nvme_enqueue_event(NvmeCtrl *n, uint8_t event_type,
                               uint8_t event_info, uint8_t log_page)
{
    NvmeAsyncEvent *event;

    trace_pci_nvme_enqueue_event(event_type, event_info, log_page);

    if (n->aer_queued == n->params.aer_max_queued) {
        trace_pci_nvme_enqueue_event_noqueue(n->aer_queued);
        return;
    }

    event = g_new(NvmeAsyncEvent, 1);
    event->result = (NvmeAerResult) {
        .event_type = event_type,
        .event_info = event_info,
        .log_page   = log_page,
    };

    QTAILQ_INSERT_TAIL(&n->aer_queue, event, entry);
    n->aer_queued++;

    nvme_process_aers(n);
}

static void nvme_clear_events(NvmeCtrl *n, uint8_t event_type)
{
    n->aer_mask &= ~(1 << event_type);
    if (!QTAILQ_EMPTY(&n->aer_queue)) {
        nvme_process_aers(n);
    }
}

static inline uint16_t nvme_check_mdts(NvmeCtrl *n, size_t len)
{
    uint8_t mdts = n->params.mdts;

    if (mdts && len > n->page_size << mdts) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    return NVME_SUCCESS;
}

static inline uint16_t nvme_check_bounds(NvmeCtrl *n, NvmeNamespace *ns,
                                         uint64_t slba, uint32_t nlb)
{
    uint64_t nsze = le64_to_cpu(ns->id_ns.nsze);

    if (unlikely(UINT64_MAX - slba < nlb || slba + nlb > nsze)) {
        return NVME_LBA_RANGE | NVME_DNR;
    }

    return NVME_SUCCESS;
}

static void nvme_auto_transition_zone(NvmeCtrl *n, NvmeNamespace *ns,
                                      bool implicit, bool adding_active)
{
    NvmeZone *zone;

    if (n->params.active_excursions && adding_active &&
        n->params.max_active_zones &&
        ns->nr_active_zones == n->params.max_active_zones) {
        zone = nvme_peek_zone_head(ns, ns->closed_zones);
        if (zone) {
            /*
             * The namespace is at the limit of active zones.
             * Try to finish one of the currently active zones
             * to make the needed active zone resource available.
             */
            nvme_aor_dec_active(n, ns);
            nvme_assign_zone_state(n, ns, zone, NVME_ZONE_STATE_FULL);
            zone->d.za &= ~(NVME_ZA_FINISH_RECOMMENDED |
                            NVME_ZA_RESET_RECOMMENDED);
            zone->d.za |= NVME_ZA_FINISHED_BY_CTLR;
            zone->flags = 0;
            zone->tstamp = 0;
            trace_pci_nvme_zone_finished_by_controller(zone->d.zslba);
        }
    }

    if (implicit && n->params.max_open_zones &&
        ns->nr_open_zones == n->params.max_open_zones) {
        zone = nvme_remove_zone_head(n, ns, ns->imp_open_zones);
        if (zone) {
            /*
             * Automatically close this implicitly open zone.
             */
            nvme_aor_dec_open(n, ns);
            nvme_assign_zone_state(n, ns, zone, NVME_ZONE_STATE_CLOSED);
        }
    }
}

static uint16_t nvme_check_zone_write(NvmeZone *zone, uint64_t slba,
                                      uint32_t nlb)
{
    uint16_t status;

    if (unlikely((slba + nlb) > nvme_zone_wr_boundary(zone))) {
        return NVME_ZONE_BOUNDARY_ERROR;
    }

    switch (nvme_get_zone_state(zone)) {
    case NVME_ZONE_STATE_EMPTY:
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
    case NVME_ZONE_STATE_CLOSED:
        status = NVME_SUCCESS;
        break;
    case NVME_ZONE_STATE_FULL:
        status = NVME_ZONE_FULL;
        break;
    case NVME_ZONE_STATE_OFFLINE:
        status = NVME_ZONE_OFFLINE;
        break;
    case NVME_ZONE_STATE_READ_ONLY:
        status = NVME_ZONE_READ_ONLY;
        break;
    default:
        assert(false);
    }
    return status;
}

static uint16_t nvme_check_zone_read(NvmeCtrl *n, NvmeZone *zone, uint64_t slba,
                                     uint32_t nlb, bool zone_x_ok)
{
    uint64_t lba = slba, count;
    uint16_t status;
    uint8_t zs;

    do {
        if (!zone_x_ok && (lba + nlb > nvme_zone_rd_boundary(n, zone))) {
            return NVME_ZONE_BOUNDARY_ERROR | NVME_DNR;
        }

        zs = nvme_get_zone_state(zone);
        switch (zs) {
        case NVME_ZONE_STATE_EMPTY:
        case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        case NVME_ZONE_STATE_EXPLICITLY_OPEN:
        case NVME_ZONE_STATE_FULL:
        case NVME_ZONE_STATE_CLOSED:
        case NVME_ZONE_STATE_READ_ONLY:
            status = NVME_SUCCESS;
            break;
        case NVME_ZONE_STATE_OFFLINE:
            status = NVME_ZONE_OFFLINE | NVME_DNR;
            break;
        default:
            assert(false);
        }
        if (status != NVME_SUCCESS) {
            break;
        }

        if (lba + nlb > nvme_zone_rd_boundary(n, zone)) {
            count = nvme_zone_rd_boundary(n, zone) - lba;
        } else {
            count = nlb;
        }

        lba += count;
        nlb -= count;
        zone++;
    } while (nlb);

    return status;
}

static uint16_t nvme_auto_open_zone(NvmeCtrl *n, NvmeNamespace *ns,
                                    NvmeZone *zone)
{
    uint16_t status = NVME_SUCCESS;
    uint8_t zs = nvme_get_zone_state(zone);

    if (zs == NVME_ZONE_STATE_EMPTY) {
        nvme_auto_transition_zone(n, ns, true, true);
        status = nvme_aor_check(n, ns, 1, 1);
    } else if (zs == NVME_ZONE_STATE_CLOSED) {
        nvme_auto_transition_zone(n, ns, true, false);
        status = nvme_aor_check(n, ns, 0, 1);
    }

    return status;
}

static inline uint32_t nvme_zone_idx(NvmeCtrl *n, uint64_t slba)
{
    return n->zone_size_log2 > 0 ? slba >> n->zone_size_log2 :
                                   slba / n->zone_size;
}

static void nvme_finalize_zone_write(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    NvmeNamespace *ns;
    NvmeZone *zone;
    uint64_t slba;
    uint32_t nlb, zone_idx;
    uint8_t zs;

    if (rw->opcode != NVME_CMD_WRITE &&
        rw->opcode != NVME_CMD_ZONE_APND &&
        rw->opcode != NVME_CMD_WRITE_ZEROES) {
         return;
    }

    slba = le64_to_cpu(rw->slba);
    nlb  = le16_to_cpu(rw->nlb) + 1;
    zone_idx = nvme_zone_idx(n, slba);
    assert(zone_idx < n->num_zones);
    ns = req->ns;
    zone = &ns->zone_array[zone_idx];

    zone->d.wp += nlb;

    zs = nvme_get_zone_state(zone);
    if (zone->d.wp == nvme_zone_wr_boundary(zone)) {
        switch (zs) {
        case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        case NVME_ZONE_STATE_EXPLICITLY_OPEN:
            nvme_aor_dec_open(n, ns);
            /* fall through */
        case NVME_ZONE_STATE_CLOSED:
            nvme_aor_dec_active(n, ns);
            /* fall through */
        case NVME_ZONE_STATE_EMPTY:
            break;
        default:
            assert(false);
        }
        nvme_assign_zone_state(n, ns, zone, NVME_ZONE_STATE_FULL);
    } else {
        switch (zs) {
        case NVME_ZONE_STATE_EMPTY:
            nvme_aor_inc_active(n, ns);
            /* fall through */
        case NVME_ZONE_STATE_CLOSED:
            nvme_aor_inc_open(n, ns);
            nvme_assign_zone_state(n, ns, zone,
                                   NVME_ZONE_STATE_IMPLICITLY_OPEN);
        }
    }

    req->cqe.result64 = zone->d.wp;
    return;
}

static void nvme_rw_cb(void *opaque, int ret)
{
    NvmeRequest *req = opaque;
    NvmeSQueue *sq = req->sq;
    NvmeCtrl *n = sq->ctrl;
    NvmeCQueue *cq = n->cq[sq->cqid];

    trace_pci_nvme_rw_cb(nvme_cid(req));

    if (!ret) {
        if (n->params.zoned) {
            if (req->fill_ofs >= 0) {
                nvme_fill_data(&req->qsg, &req->iov, req->fill_ofs,
                               n->params.fill_pattern);
            }
            nvme_finalize_zone_write(n, req);
        }
        block_acct_done(blk_get_stats(n->conf.blk), &req->acct);
        req->status = NVME_SUCCESS;
    } else {
        block_acct_failed(blk_get_stats(n->conf.blk), &req->acct);
        req->status = NVME_INTERNAL_DEV_ERROR;
    }

    nvme_enqueue_req_completion(cq, req);
}

static uint16_t nvme_flush(NvmeCtrl *n, NvmeRequest *req)
{
    block_acct_start(blk_get_stats(n->conf.blk), &req->acct, 0,
         BLOCK_ACCT_FLUSH);
    req->aiocb = blk_aio_flush(n->conf.blk, nvme_rw_cb, req);

    return NVME_NO_COMPLETE;
}

static uint16_t nvme_write_zeroes(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    NvmeNamespace *ns = req->ns;
    NvmeZone *zone = NULL;
    const uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    const uint8_t data_shift = ns->id_ns.lbaf[lba_index].ds;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb  = le16_to_cpu(rw->nlb) + 1;
    uint64_t offset = slba << data_shift;
    uint32_t count = nlb << data_shift;
    uint32_t zone_idx;
    uint16_t status;

    trace_pci_nvme_write_zeroes(nvme_cid(req), slba, nlb);

    status = nvme_check_bounds(n, ns, slba, nlb);
    if (status) {
        trace_pci_nvme_err_invalid_lba_range(slba, nlb, ns->id_ns.nsze);
        return status;
    }

    if (n->params.zoned) {
        zone_idx = nvme_zone_idx(n, slba);
        assert(zone_idx < n->num_zones);
        zone = &ns->zone_array[zone_idx];

        status = nvme_check_zone_write(zone, slba, nlb);
        if (status != NVME_SUCCESS) {
            trace_pci_nvme_err_zone_write_not_ok(slba, nlb, status);
            return status | NVME_DNR;
        }

        assert(nvme_wp_is_valid(zone));
        if (unlikely(slba != zone->d.wp)) {
            trace_pci_nvme_err_write_not_at_wp(slba, zone->d.zslba,
                                               zone->d.wp);
            return NVME_ZONE_INVALID_WRITE | NVME_DNR;
        }

        status = nvme_auto_open_zone(n, ns, zone);
        if (status != NVME_SUCCESS) {
            return status;
        }
    }

    block_acct_start(blk_get_stats(n->conf.blk), &req->acct, 0,
                     BLOCK_ACCT_WRITE);
    req->aiocb = blk_aio_pwrite_zeroes(n->conf.blk, offset, count,
                                        BDRV_REQ_MAY_UNMAP, nvme_rw_cb, req);

    return NVME_NO_COMPLETE;
}

static uint16_t nvme_rw(NvmeCtrl *n, NvmeRequest *req, bool append)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    NvmeNamespace *ns = req->ns;
    NvmeZone *zone = NULL;
    uint32_t nlb  = le32_to_cpu(rw->nlb) + 1;
    uint64_t slba = le64_to_cpu(rw->slba);

    uint8_t lba_index  = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    uint8_t data_shift = ns->id_ns.lbaf[lba_index].ds;
    uint64_t data_size = (uint64_t)nlb << data_shift;
    uint64_t data_offset;
    uint32_t zone_idx = 0;
    bool is_write = rw->opcode == NVME_CMD_WRITE || append;
    enum BlockAcctType acct = is_write ? BLOCK_ACCT_WRITE : BLOCK_ACCT_READ;
    uint16_t status;

    trace_pci_nvme_rw(is_write ? "write" : "read", nlb, data_size, slba);

    status = nvme_check_mdts(n, data_size);
    if (status) {
        trace_pci_nvme_err_mdts(nvme_cid(req), data_size);
        block_acct_invalid(blk_get_stats(n->conf.blk), acct);
        return status;
    }

    status = nvme_check_bounds(n, ns, slba, nlb);
    if (status) {
        trace_pci_nvme_err_invalid_lba_range(slba, nlb, ns->id_ns.nsze);
        block_acct_invalid(blk_get_stats(n->conf.blk), acct);
        return status;
    }

    if (n->params.zoned) {
        zone_idx = nvme_zone_idx(n, slba);
        assert(zone_idx < n->num_zones);
        zone = &ns->zone_array[zone_idx];

        if (is_write) {
            status = nvme_check_zone_write(zone, slba, nlb);
            if (status != NVME_SUCCESS) {
                trace_pci_nvme_err_zone_write_not_ok(slba, nlb, status);
                return status | NVME_DNR;
            }

            assert(nvme_wp_is_valid(zone));
            if (append) {
                if (unlikely(slba != zone->d.zslba)) {
                    trace_pci_nvme_err_append_not_at_start(slba, zone->d.zslba);
                    return NVME_ZONE_INVALID_WRITE | NVME_DNR;
                }
                if (data_size > (n->page_size << n->zasl)) {
                    trace_pci_nvme_err_append_too_large(slba, nlb, n->zasl);
                    return NVME_INVALID_FIELD | NVME_DNR;
                }
                slba = zone->d.wp;
            } else if (unlikely(slba != zone->d.wp)) {
                trace_pci_nvme_err_write_not_at_wp(slba, zone->d.zslba,
                                                   zone->d.wp);
                return NVME_ZONE_INVALID_WRITE | NVME_DNR;
            }

            status = nvme_auto_open_zone(n, ns, zone);
            if (status != NVME_SUCCESS) {
                return status;
            }

            req->fill_ofs = -1LL;
        } else {
            status = nvme_check_zone_read(n, zone, slba, nlb,
                                          n->params.cross_zone_read);
            if (status != NVME_SUCCESS) {
                trace_pci_nvme_err_zone_read_not_ok(slba, nlb, status);
                return status | NVME_DNR;
            }

            if (slba + nlb > zone->d.wp) {
                /*
                 * All or some data is read above the WP. Need to
                 * fill out the buffer area that has no backing data
                 * with a predefined data pattern (zeros by default)
                 */
                if (slba >= zone->d.wp) {
                    req->fill_ofs = 0;
                } else {
                    req->fill_ofs = ((zone->d.wp - slba) << data_shift);
                }
            } else {
                req->fill_ofs = -1LL;
            }
        }
    } else if (append) {
        trace_pci_nvme_err_invalid_opc(rw->opcode);
        return NVME_INVALID_OPCODE | NVME_DNR;
    }

    if (nvme_map_dptr(n, data_size, req)) {
        block_acct_invalid(blk_get_stats(n->conf.blk), acct);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (unlikely(n->params.zoned && req->fill_ofs == 0)) {
        /* No backend I/O necessary, only need to fill the buffer */
        nvme_fill_data(&req->qsg, &req->iov, 0, n->params.fill_pattern);
        req->status = NVME_SUCCESS;
        return NVME_SUCCESS;
    }

    data_offset = slba << data_shift;

    if (req->qsg.nsg > 0) {
        block_acct_start(blk_get_stats(n->conf.blk), &req->acct, req->qsg.size,
                         acct);
        req->aiocb = is_write ?
            dma_blk_write(n->conf.blk, &req->qsg, data_offset, BDRV_SECTOR_SIZE,
                          nvme_rw_cb, req) :
            dma_blk_read(n->conf.blk, &req->qsg, data_offset, BDRV_SECTOR_SIZE,
                         nvme_rw_cb, req);
    } else {
        block_acct_start(blk_get_stats(n->conf.blk), &req->acct, req->iov.size,
                         acct);
        req->aiocb = is_write ?
            blk_aio_pwritev(n->conf.blk, data_offset, &req->iov, 0, nvme_rw_cb,
                            req) :
            blk_aio_preadv(n->conf.blk, data_offset, &req->iov, 0, nvme_rw_cb,
                           req);
    }

    return NVME_NO_COMPLETE;
}

static uint16_t nvme_get_mgmt_zone_slba_idx(NvmeCtrl *n, NvmeNamespace *ns,
                                            NvmeCmd *c, uint64_t *slba,
                                            uint32_t *zone_idx)
{
    uint32_t dw10 = le32_to_cpu(c->cdw10);
    uint32_t dw11 = le32_to_cpu(c->cdw11);

    if (!n->params.zoned) {
        trace_pci_nvme_err_invalid_opc(c->opcode);
        return NVME_INVALID_OPCODE | NVME_DNR;
    }

    *slba = ((uint64_t)dw11) << 32 | dw10;
    if (unlikely(*slba >= ns->id_ns.nsze)) {
        trace_pci_nvme_err_invalid_lba_range(*slba, 0, ns->id_ns.nsze);
        *slba = 0;
        return NVME_LBA_RANGE | NVME_DNR;
    }

    *zone_idx = nvme_zone_idx(n, *slba);
    assert(*zone_idx < n->num_zones);

    return NVME_SUCCESS;
}

static uint16_t nvme_open_zone(NvmeCtrl *n, NvmeNamespace *ns,
                               NvmeZone *zone, uint8_t state)
{
    uint16_t status;

    switch (state) {
    case NVME_ZONE_STATE_EMPTY:
        nvme_auto_transition_zone(n, ns, false, true);
        status = nvme_aor_check(n, ns, 1, 0);
        if (status != NVME_SUCCESS) {
            return status;
        }
        nvme_aor_inc_active(n, ns);
        /* fall through */
    case NVME_ZONE_STATE_CLOSED:
        status = nvme_aor_check(n, ns, 0, 1);
        if (status != NVME_SUCCESS) {
            if (state == NVME_ZONE_STATE_EMPTY) {
                nvme_aor_dec_active(n, ns);
            }
            return status;
        }
        nvme_aor_inc_open(n, ns);
        /* fall through */
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        nvme_assign_zone_state(n, ns, zone, NVME_ZONE_STATE_EXPLICITLY_OPEN);
        /* fall through */
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
        return NVME_SUCCESS;
    }

    return NVME_ZONE_INVAL_TRANSITION;
}

static bool nvme_cond_open_all(uint8_t state)
{
    return state == NVME_ZONE_STATE_CLOSED;
}

static uint16_t nvme_close_zone(NvmeCtrl *n,  NvmeNamespace *ns,
                                NvmeZone *zone, uint8_t state)
{
    switch (state) {
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        nvme_aor_dec_open(n, ns);
        nvme_assign_zone_state(n, ns, zone, NVME_ZONE_STATE_CLOSED);
        /* fall through */
    case NVME_ZONE_STATE_CLOSED:
        return NVME_SUCCESS;
    }

    return NVME_ZONE_INVAL_TRANSITION;
}

static bool nvme_cond_close_all(uint8_t state)
{
    return state == NVME_ZONE_STATE_IMPLICITLY_OPEN ||
           state == NVME_ZONE_STATE_EXPLICITLY_OPEN;
}

static uint16_t nvme_finish_zone(NvmeCtrl *n, NvmeNamespace *ns,
                                 NvmeZone *zone, uint8_t state)
{
    switch (state) {
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        nvme_aor_dec_open(n, ns);
        /* fall through */
    case NVME_ZONE_STATE_CLOSED:
        nvme_aor_dec_active(n, ns);
        /* fall through */
    case NVME_ZONE_STATE_EMPTY:
        zone->d.wp = nvme_zone_wr_boundary(zone);
        nvme_assign_zone_state(n, ns, zone, NVME_ZONE_STATE_FULL);
        /* fall through */
    case NVME_ZONE_STATE_FULL:
        return NVME_SUCCESS;
    }

    return NVME_ZONE_INVAL_TRANSITION;
}

static bool nvme_cond_finish_all(uint8_t state)
{
    return state == NVME_ZONE_STATE_IMPLICITLY_OPEN ||
           state == NVME_ZONE_STATE_EXPLICITLY_OPEN ||
           state == NVME_ZONE_STATE_CLOSED;
}

static uint16_t nvme_reset_zone(NvmeCtrl *n, NvmeNamespace *ns,
                                NvmeZone *zone, uint8_t state)
{
    switch (state) {
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        nvme_aor_dec_open(n, ns);
        /* fall through */
    case NVME_ZONE_STATE_CLOSED:
        nvme_aor_dec_active(n, ns);
        /* fall through */
    case NVME_ZONE_STATE_FULL:
        zone->d.wp = zone->d.zslba;
        nvme_assign_zone_state(n, ns, zone, NVME_ZONE_STATE_EMPTY);
        /* fall through */
    case NVME_ZONE_STATE_EMPTY:
        return NVME_SUCCESS;
    }

    return NVME_ZONE_INVAL_TRANSITION;
}

static bool nvme_cond_reset_all(uint8_t state)
{
    return state == NVME_ZONE_STATE_IMPLICITLY_OPEN ||
           state == NVME_ZONE_STATE_EXPLICITLY_OPEN ||
           state == NVME_ZONE_STATE_CLOSED ||
           state == NVME_ZONE_STATE_FULL;
}

static uint16_t nvme_offline_zone(NvmeCtrl *n, NvmeNamespace *ns,
                                  NvmeZone *zone, uint8_t state)
{
    switch (state) {
    case NVME_ZONE_STATE_READ_ONLY:
        nvme_assign_zone_state(n, ns, zone, NVME_ZONE_STATE_OFFLINE);
        /* fall through */
    case NVME_ZONE_STATE_OFFLINE:
        return NVME_SUCCESS;
    }

    return NVME_ZONE_INVAL_TRANSITION;
}

static bool nvme_cond_offline_all(uint8_t state)
{
    return state == NVME_ZONE_STATE_READ_ONLY;
}

static uint16_t nvme_set_zd_ext(NvmeCtrl *n, NvmeNamespace *ns,
    NvmeZone *zone, uint8_t state)
{
    uint16_t status;

    if (state == NVME_ZONE_STATE_EMPTY) {
        nvme_auto_transition_zone(n, ns, false, true);
        status = nvme_aor_check(n, ns, 1, 0);
        if (status != NVME_SUCCESS) {
            return status;
        }
        nvme_aor_inc_active(n, ns);
        zone->d.za |= NVME_ZA_ZD_EXT_VALID;
        nvme_assign_zone_state(n, ns, zone, NVME_ZONE_STATE_CLOSED);
        return NVME_SUCCESS;
    }

    return NVME_ZONE_INVAL_TRANSITION;
}

typedef uint16_t (*op_handler_t)(NvmeCtrl *, NvmeNamespace *, NvmeZone *,
                                 uint8_t);
typedef bool (*need_to_proc_zone_t)(uint8_t);

static uint16_t name_do_zone_op(NvmeCtrl *n, NvmeNamespace *ns,
                                NvmeZone *zone, uint8_t state, bool all,
                                op_handler_t op_hndlr,
                                need_to_proc_zone_t proc_zone)
{
    int i;
    uint16_t status = 0;

    if (!all) {
        status = op_hndlr(n, ns, zone, state);
    } else {
        for (i = 0; i < n->num_zones; i++, zone++) {
            state = nvme_get_zone_state(zone);
            if (proc_zone(state)) {
                status = op_hndlr(n, ns, zone, state);
                if (status != NVME_SUCCESS) {
                    break;
                }
            }
        }
    }

    return status;
}

static uint16_t nvme_zone_mgmt_send(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeCmd *cmd = (NvmeCmd *)&req->cmd;
    NvmeNamespace *ns = req->ns;
    uint32_t dw13 = le32_to_cpu(cmd->cdw13);
    uint64_t prp1, prp2;
    uint64_t slba = 0;
    uint32_t zone_idx = 0;
    uint16_t status;
    uint8_t action, state;
    bool all;
    NvmeZone *zone;
    uint8_t *zd_ext;

    action = dw13 & 0xff;
    all = dw13 & 0x100;

    req->status = NVME_SUCCESS;

    if (!all) {
        status = nvme_get_mgmt_zone_slba_idx(n, ns, cmd, &slba, &zone_idx);
        if (status) {
            return status;
        }
    }

    zone = &ns->zone_array[zone_idx];
    if (slba != zone->d.zslba) {
        trace_pci_nvme_err_unaligned_zone_cmd(action, slba, zone->d.zslba);
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    state = nvme_get_zone_state(zone);

    switch (action) {

    case NVME_ZONE_ACTION_OPEN:
        trace_pci_nvme_open_zone(slba, zone_idx, all);
        status = name_do_zone_op(n, ns, zone, state, all,
                                 nvme_open_zone, nvme_cond_open_all);
        break;

    case NVME_ZONE_ACTION_CLOSE:
        trace_pci_nvme_close_zone(slba, zone_idx, all);
        status = name_do_zone_op(n, ns, zone, state, all,
                                 nvme_close_zone, nvme_cond_close_all);
        break;

    case NVME_ZONE_ACTION_FINISH:
        trace_pci_nvme_finish_zone(slba, zone_idx, all);
        status = name_do_zone_op(n, ns, zone, state, all,
                                 nvme_finish_zone, nvme_cond_finish_all);
        break;

    case NVME_ZONE_ACTION_RESET:
        trace_pci_nvme_reset_zone(slba, zone_idx, all);
        status = name_do_zone_op(n, ns, zone, state, all,
                                 nvme_reset_zone, nvme_cond_reset_all);
        break;

    case NVME_ZONE_ACTION_OFFLINE:
        trace_pci_nvme_offline_zone(slba, zone_idx, all);
        status = name_do_zone_op(n, ns, zone, state, all,
                                 nvme_offline_zone, nvme_cond_offline_all);
        break;

    case NVME_ZONE_ACTION_SET_ZD_EXT:
        trace_pci_nvme_set_descriptor_extension(slba, zone_idx);
        if (all || !n->params.zd_extension_size) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }
        zd_ext = nvme_get_zd_extension(n, ns, zone_idx);
        prp1 = le64_to_cpu(cmd->dptr.prp1);
        prp2 = le64_to_cpu(cmd->dptr.prp2);
        status = nvme_dma_prp(n, zd_ext, n->params.zd_extension_size,
                              prp1, prp2, DMA_DIRECTION_TO_DEVICE, req);
        if (status) {
            trace_pci_nvme_err_zd_extension_map_error(zone_idx);
            return status;
        }

        status = nvme_set_zd_ext(n, ns, zone, state);
        if (status == NVME_SUCCESS) {
            trace_pci_nvme_zd_extension_set(zone_idx);
            return status;
        }
        break;

    default:
        trace_pci_nvme_err_invalid_mgmt_action(action);
        status = NVME_INVALID_FIELD;
    }

    if (status == NVME_ZONE_INVAL_TRANSITION) {
        trace_pci_nvme_err_invalid_zone_state_transition(state, action, slba,
                                                         zone->d.za);
    }
    if (status) {
        status |= NVME_DNR;
    }

    return status;
}

static bool nvme_zone_matches_filter(uint32_t zafs, NvmeZone *zl)
{
    int zs = nvme_get_zone_state(zl);

    switch (zafs) {
    case NVME_ZONE_REPORT_ALL:
        return true;
    case NVME_ZONE_REPORT_EMPTY:
        return (zs == NVME_ZONE_STATE_EMPTY);
    case NVME_ZONE_REPORT_IMPLICITLY_OPEN:
        return (zs == NVME_ZONE_STATE_IMPLICITLY_OPEN);
    case NVME_ZONE_REPORT_EXPLICITLY_OPEN:
        return (zs == NVME_ZONE_STATE_EXPLICITLY_OPEN);
    case NVME_ZONE_REPORT_CLOSED:
        return (zs == NVME_ZONE_STATE_CLOSED);
    case NVME_ZONE_REPORT_FULL:
        return (zs == NVME_ZONE_STATE_FULL);
    case NVME_ZONE_REPORT_READ_ONLY:
        return (zs == NVME_ZONE_STATE_READ_ONLY);
    case NVME_ZONE_REPORT_OFFLINE:
        return (zs == NVME_ZONE_STATE_OFFLINE);
    default:
        return false;
    }
}

static uint16_t nvme_zone_mgmt_recv(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeCmd *cmd = (NvmeCmd *)&req->cmd;
    NvmeNamespace *ns = req->ns;
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    /* cdw12 is zero-based number of dwords to return. Convert to bytes */
    uint32_t len = (le32_to_cpu(cmd->cdw12) + 1) << 2;
    uint32_t dw13 = le32_to_cpu(cmd->cdw13);
    uint32_t zone_idx, zra, zrasf, partial;
    uint64_t max_zones, nr_zones = 0;
    uint16_t ret;
    uint64_t slba;
    NvmeZoneDescr *z;
    NvmeZone *zs;
    NvmeZoneReportHeader *header;
    void *buf, *buf_p;
    size_t zone_entry_sz;

    req->status = NVME_SUCCESS;

    ret = nvme_get_mgmt_zone_slba_idx(n, ns, cmd, &slba, &zone_idx);
    if (ret) {
        return ret;
    }

    if (len < sizeof(NvmeZoneReportHeader)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    zra = dw13 & 0xff;
    if (!(zra == NVME_ZONE_REPORT || zra == NVME_ZONE_REPORT_EXTENDED)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (zra == NVME_ZONE_REPORT_EXTENDED && !n->params.zd_extension_size) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    zrasf = (dw13 >> 8) & 0xff;
    if (zrasf > NVME_ZONE_REPORT_OFFLINE) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    partial = (dw13 >> 16) & 0x01;

    zone_entry_sz = sizeof(NvmeZoneDescr);
    if (zra == NVME_ZONE_REPORT_EXTENDED) {
        zone_entry_sz += n->params.zd_extension_size;
    }

    max_zones = (len - sizeof(NvmeZoneReportHeader)) / zone_entry_sz;
    buf = g_malloc0(len);

    header = (NvmeZoneReportHeader *)buf;
    buf_p = buf + sizeof(NvmeZoneReportHeader);

    while (zone_idx < n->num_zones && nr_zones < max_zones) {
        zs = &ns->zone_array[zone_idx];

        if (!nvme_zone_matches_filter(zrasf, zs)) {
            zone_idx++;
            continue;
        }

        z = (NvmeZoneDescr *)buf_p;
        buf_p += sizeof(NvmeZoneDescr);
        nr_zones++;

        z->zt = zs->d.zt;
        z->zs = zs->d.zs;
        z->zcap = cpu_to_le64(zs->d.zcap);
        z->zslba = cpu_to_le64(zs->d.zslba);
        z->za = zs->d.za;

        if (nvme_wp_is_valid(zs)) {
            z->wp = cpu_to_le64(zs->d.wp);
        } else {
            z->wp = cpu_to_le64(~0ULL);
        }

        if (zra == NVME_ZONE_REPORT_EXTENDED) {
            if (zs->d.za & NVME_ZA_ZD_EXT_VALID) {
                memcpy(buf_p, nvme_get_zd_extension(n, ns, zone_idx),
                       n->params.zd_extension_size);
            }
            buf_p += n->params.zd_extension_size;
        }

        zone_idx++;
    }

    if (!partial) {
        for (; zone_idx < n->num_zones; zone_idx++) {
            zs = &ns->zone_array[zone_idx];
            if (nvme_zone_matches_filter(zrasf, zs)) {
                nr_zones++;
            }
        }
    }
    header->nr_zones = cpu_to_le64(nr_zones);

    ret = nvme_dma_prp(n, (uint8_t *)buf, len, prp1, prp2,
                       DMA_DIRECTION_FROM_DEVICE, req);
    g_free(buf);

    return ret;
}

static uint16_t nvme_io_cmd(NvmeCtrl *n, NvmeRequest *req)
{
    uint32_t nsid = le32_to_cpu(req->cmd.nsid);

    trace_pci_nvme_io_cmd(nvme_cid(req), nsid, nvme_sqid(req),
                          req->cmd.opcode);

    if (unlikely(nsid == 0 || nsid > n->num_namespaces)) {
        trace_pci_nvme_err_invalid_ns(nsid, n->num_namespaces);
        return NVME_INVALID_NSID | NVME_DNR;
    }

    req->ns = &n->namespaces[nsid - 1];
    switch (req->cmd.opcode) {
    case NVME_CMD_FLUSH:
        return nvme_flush(n, req);
    case NVME_CMD_WRITE_ZEROES:
        return nvme_write_zeroes(n, req);
    case NVME_CMD_ZONE_APND:
        return nvme_rw(n, req, true);
    case NVME_CMD_WRITE:
    case NVME_CMD_READ:
        return nvme_rw(n, req, false);
    case NVME_CMD_ZONE_MGMT_SEND:
        return nvme_zone_mgmt_send(n, req);
    case NVME_CMD_ZONE_MGMT_RECV:
        return nvme_zone_mgmt_recv(n, req);
    default:
        trace_pci_nvme_err_invalid_opc(req->cmd.opcode);
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static void nvme_free_sq(NvmeSQueue *sq, NvmeCtrl *n)
{
    n->sq[sq->sqid] = NULL;
    timer_del(sq->timer);
    timer_free(sq->timer);
    g_free(sq->io_req);
    if (sq->sqid) {
        g_free(sq);
    }
}

static uint16_t nvme_del_sq(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeDeleteQ *c = (NvmeDeleteQ *)&req->cmd;
    NvmeRequest *r, *next;
    NvmeSQueue *sq;
    NvmeCQueue *cq;
    uint16_t qid = le16_to_cpu(c->qid);

    if (unlikely(!qid || nvme_check_sqid(n, qid))) {
        trace_pci_nvme_err_invalid_del_sq(qid);
        return NVME_INVALID_QID | NVME_DNR;
    }

    trace_pci_nvme_del_sq(qid);

    sq = n->sq[qid];
    while (!QTAILQ_EMPTY(&sq->out_req_list)) {
        r = QTAILQ_FIRST(&sq->out_req_list);
        assert(r->aiocb);
        blk_aio_cancel(r->aiocb);
    }
    if (!nvme_check_cqid(n, sq->cqid)) {
        cq = n->cq[sq->cqid];
        QTAILQ_REMOVE(&cq->sq_list, sq, entry);

        nvme_post_cqes(cq);
        QTAILQ_FOREACH_SAFE(r, &cq->req_list, entry, next) {
            if (r->sq == sq) {
                QTAILQ_REMOVE(&cq->req_list, r, entry);
                QTAILQ_INSERT_TAIL(&sq->req_list, r, entry);
            }
        }
    }

    nvme_free_sq(sq, n);
    return NVME_SUCCESS;
}

static void nvme_init_sq(NvmeSQueue *sq, NvmeCtrl *n, uint64_t dma_addr,
    uint16_t sqid, uint16_t cqid, uint16_t size)
{
    int i;
    NvmeCQueue *cq;

    sq->ctrl = n;
    sq->dma_addr = dma_addr;
    sq->sqid = sqid;
    sq->size = size;
    sq->cqid = cqid;
    sq->head = sq->tail = 0;
    sq->io_req = g_new0(NvmeRequest, sq->size);

    QTAILQ_INIT(&sq->req_list);
    QTAILQ_INIT(&sq->out_req_list);
    for (i = 0; i < sq->size; i++) {
        sq->io_req[i].sq = sq;
        QTAILQ_INSERT_TAIL(&(sq->req_list), &sq->io_req[i], entry);
    }
    sq->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, nvme_process_sq, sq);

    assert(n->cq[cqid]);
    cq = n->cq[cqid];
    QTAILQ_INSERT_TAIL(&(cq->sq_list), sq, entry);
    n->sq[sqid] = sq;
}

static uint16_t nvme_create_sq(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeSQueue *sq;
    NvmeCreateSq *c = (NvmeCreateSq *)&req->cmd;

    uint16_t cqid = le16_to_cpu(c->cqid);
    uint16_t sqid = le16_to_cpu(c->sqid);
    uint16_t qsize = le16_to_cpu(c->qsize);
    uint16_t qflags = le16_to_cpu(c->sq_flags);
    uint64_t prp1 = le64_to_cpu(c->prp1);

    trace_pci_nvme_create_sq(prp1, sqid, cqid, qsize, qflags);

    if (unlikely(!cqid || nvme_check_cqid(n, cqid))) {
        trace_pci_nvme_err_invalid_create_sq_cqid(cqid);
        return NVME_INVALID_CQID | NVME_DNR;
    }
    if (unlikely(!sqid || !nvme_check_sqid(n, sqid))) {
        trace_pci_nvme_err_invalid_create_sq_sqid(sqid);
        return NVME_INVALID_QID | NVME_DNR;
    }
    if (unlikely(!qsize || qsize > NVME_CAP_MQES(n->bar.cap))) {
        trace_pci_nvme_err_invalid_create_sq_size(qsize);
        return NVME_MAX_QSIZE_EXCEEDED | NVME_DNR;
    }
    if (unlikely(!prp1 || prp1 & (n->page_size - 1))) {
        trace_pci_nvme_err_invalid_create_sq_addr(prp1);
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (unlikely(!(NVME_SQ_FLAGS_PC(qflags)))) {
        trace_pci_nvme_err_invalid_create_sq_qflags(NVME_SQ_FLAGS_PC(qflags));
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    sq = g_malloc0(sizeof(*sq));
    nvme_init_sq(sq, n, prp1, sqid, cqid, qsize + 1);
    return NVME_SUCCESS;
}

static uint16_t nvme_smart_info(NvmeCtrl *n, uint8_t rae, uint32_t buf_len,
                                uint64_t off, NvmeRequest *req)
{
    NvmeCmd *cmd = &req->cmd;
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    uint32_t nsid = le32_to_cpu(cmd->nsid);

    uint32_t trans_len;
    time_t current_ms;
    uint64_t units_read = 0, units_written = 0;
    uint64_t read_commands = 0, write_commands = 0;
    NvmeSmartLog smart;
    BlockAcctStats *s;

    if (nsid && nsid != 0xffffffff) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    s = blk_get_stats(n->conf.blk);

    units_read = s->nr_bytes[BLOCK_ACCT_READ] >> BDRV_SECTOR_BITS;
    units_written = s->nr_bytes[BLOCK_ACCT_WRITE] >> BDRV_SECTOR_BITS;
    read_commands = s->nr_ops[BLOCK_ACCT_READ];
    write_commands = s->nr_ops[BLOCK_ACCT_WRITE];

    if (off > sizeof(smart)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    trans_len = MIN(sizeof(smart) - off, buf_len);

    memset(&smart, 0x0, sizeof(smart));

    smart.data_units_read[0] = cpu_to_le64(DIV_ROUND_UP(units_read, 1000));
    smart.data_units_written[0] = cpu_to_le64(DIV_ROUND_UP(units_written,
                                                           1000));
    smart.host_read_commands[0] = cpu_to_le64(read_commands);
    smart.host_write_commands[0] = cpu_to_le64(write_commands);

    smart.temperature = cpu_to_le16(n->temperature);

    if ((n->temperature >= n->features.temp_thresh_hi) ||
        (n->temperature <= n->features.temp_thresh_low)) {
        smart.critical_warning |= NVME_SMART_TEMPERATURE;
    }

    current_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    smart.power_on_hours[0] =
        cpu_to_le64((((current_ms - n->starttime_ms) / 1000) / 60) / 60);

    if (!rae) {
        nvme_clear_events(n, NVME_AER_TYPE_SMART);
    }

    return nvme_dma_prp(n, (uint8_t *) &smart + off, trans_len, prp1, prp2,
                        DMA_DIRECTION_FROM_DEVICE, req);
}

static uint16_t nvme_fw_log_info(NvmeCtrl *n, uint32_t buf_len, uint64_t off,
                                 NvmeRequest *req)
{
    uint32_t trans_len;
    NvmeCmd *cmd = &req->cmd;
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    NvmeFwSlotInfoLog fw_log = {
        .afi = 0x1,
    };

    strpadcpy((char *)&fw_log.frs1, sizeof(fw_log.frs1), "1.0", ' ');

    if (off > sizeof(fw_log)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    trans_len = MIN(sizeof(fw_log) - off, buf_len);

    return nvme_dma_prp(n, (uint8_t *) &fw_log + off, trans_len, prp1, prp2,
                        DMA_DIRECTION_FROM_DEVICE, req);
}

static uint16_t nvme_error_info(NvmeCtrl *n, uint8_t rae, uint32_t buf_len,
                                uint64_t off, NvmeRequest *req)
{
    uint32_t trans_len;
    NvmeCmd *cmd = &req->cmd;
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    NvmeErrorLog errlog;

    if (!rae) {
        nvme_clear_events(n, NVME_AER_TYPE_ERROR);
    }

    if (off > sizeof(errlog)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    memset(&errlog, 0x0, sizeof(errlog));

    trans_len = MIN(sizeof(errlog) - off, buf_len);

    return nvme_dma_prp(n, (uint8_t *)&errlog, trans_len, prp1, prp2,
                        DMA_DIRECTION_FROM_DEVICE, req);
}

static uint16_t nvme_cmd_effects(NvmeCtrl *n, uint8_t csi, uint32_t buf_len,
                                 uint64_t off, NvmeRequest *req)
{
    NvmeCmd *cmd = &req->cmd;
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    NvmeEffectsLog cmd_eff_log = {};
    uint32_t *iocs = cmd_eff_log.iocs;
    uint32_t trans_len;

    trace_pci_nvme_cmd_supp_and_effects_log_read();

    if (off >= sizeof(cmd_eff_log)) {
        trace_pci_nvme_err_invalid_effects_log_offset(off);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    iocs[NVME_ADM_CMD_DELETE_SQ] = NVME_CMD_EFFECTS_CSUPP;
    iocs[NVME_ADM_CMD_CREATE_SQ] = NVME_CMD_EFFECTS_CSUPP;
    iocs[NVME_ADM_CMD_DELETE_CQ] = NVME_CMD_EFFECTS_CSUPP;
    iocs[NVME_ADM_CMD_CREATE_CQ] = NVME_CMD_EFFECTS_CSUPP;
    iocs[NVME_ADM_CMD_IDENTIFY] = NVME_CMD_EFFECTS_CSUPP;
    iocs[NVME_ADM_CMD_SET_FEATURES] = NVME_CMD_EFFECTS_CSUPP;
    iocs[NVME_ADM_CMD_GET_FEATURES] = NVME_CMD_EFFECTS_CSUPP;
    iocs[NVME_ADM_CMD_GET_LOG_PAGE] = NVME_CMD_EFFECTS_CSUPP;
    iocs[NVME_ADM_CMD_ASYNC_EV_REQ] = NVME_CMD_EFFECTS_CSUPP;

    if (NVME_CC_CSS(n->bar.cc) != CSS_ADMIN_ONLY) {
        iocs[NVME_CMD_FLUSH] = NVME_CMD_EFFECTS_CSUPP | NVME_CMD_EFFECTS_LBCC;
        iocs[NVME_CMD_WRITE_ZEROES] = NVME_CMD_EFFECTS_CSUPP |
                                      NVME_CMD_EFFECTS_LBCC;
        iocs[NVME_CMD_WRITE] = NVME_CMD_EFFECTS_CSUPP | NVME_CMD_EFFECTS_LBCC;
        iocs[NVME_CMD_READ] = NVME_CMD_EFFECTS_CSUPP;
    }
    if (csi == NVME_CSI_ZONED && NVME_CC_CSS(n->bar.cc) == CSS_CSI) {
        iocs[NVME_CMD_ZONE_APND] = NVME_CMD_EFFECTS_CSUPP |
                                   NVME_CMD_EFFECTS_LBCC;
        iocs[NVME_CMD_ZONE_MGMT_SEND] = NVME_CMD_EFFECTS_CSUPP;
        iocs[NVME_CMD_ZONE_MGMT_RECV] = NVME_CMD_EFFECTS_CSUPP;
    }

    trans_len = MIN(sizeof(cmd_eff_log) - off, buf_len);

    return nvme_dma_prp(n, ((uint8_t *)&cmd_eff_log) + off, trans_len,
                        prp1, prp2, DMA_DIRECTION_FROM_DEVICE, req);
}

static uint16_t nvme_get_log(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeCmd *cmd = &req->cmd;

    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t dw11 = le32_to_cpu(cmd->cdw11);
    uint32_t dw12 = le32_to_cpu(cmd->cdw12);
    uint32_t dw13 = le32_to_cpu(cmd->cdw13);
    uint8_t  lid = dw10 & 0xff;
    uint8_t  lsp = (dw10 >> 8) & 0xf;
    uint8_t  rae = (dw10 >> 15) & 0x1;
    uint8_t csi = le32_to_cpu(cmd->cdw14) >> 24;
    uint32_t numdl, numdu;
    uint64_t off, lpol, lpou;
    size_t   len;
    uint16_t status;

    numdl = (dw10 >> 16);
    numdu = (dw11 & 0xffff);
    lpol = dw12;
    lpou = dw13;

    len = (((numdu << 16) | numdl) + 1) << 2;
    off = (lpou << 32ULL) | lpol;

    if (off & 0x3) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    trace_pci_nvme_get_log(nvme_cid(req), lid, lsp, rae, len, off);

    status = nvme_check_mdts(n, len);
    if (status) {
        trace_pci_nvme_err_mdts(nvme_cid(req), len);
        return status;
    }

    switch (lid) {
    case NVME_LOG_ERROR_INFO:
        return nvme_error_info(n, rae, len, off, req);
    case NVME_LOG_SMART_INFO:
        return nvme_smart_info(n, rae, len, off, req);
    case NVME_LOG_FW_SLOT_INFO:
        return nvme_fw_log_info(n, len, off, req);
    case NVME_LOG_CMD_EFFECTS:
        return nvme_cmd_effects(n, csi, len, off, req);
    default:
        trace_pci_nvme_err_invalid_log_page(nvme_cid(req), lid);
        return NVME_INVALID_FIELD | NVME_DNR;
    }
}

static void nvme_free_cq(NvmeCQueue *cq, NvmeCtrl *n)
{
    n->cq[cq->cqid] = NULL;
    timer_del(cq->timer);
    timer_free(cq->timer);
    msix_vector_unuse(&n->parent_obj, cq->vector);
    if (cq->cqid) {
        g_free(cq);
    }
}

static uint16_t nvme_del_cq(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeDeleteQ *c = (NvmeDeleteQ *)&req->cmd;
    NvmeCQueue *cq;
    uint16_t qid = le16_to_cpu(c->qid);

    if (unlikely(!qid || nvme_check_cqid(n, qid))) {
        trace_pci_nvme_err_invalid_del_cq_cqid(qid);
        return NVME_INVALID_CQID | NVME_DNR;
    }

    cq = n->cq[qid];
    if (unlikely(!QTAILQ_EMPTY(&cq->sq_list))) {
        trace_pci_nvme_err_invalid_del_cq_notempty(qid);
        return NVME_INVALID_QUEUE_DEL;
    }
    nvme_irq_deassert(n, cq);
    trace_pci_nvme_del_cq(qid);
    nvme_free_cq(cq, n);
    return NVME_SUCCESS;
}

static void nvme_init_cq(NvmeCQueue *cq, NvmeCtrl *n, uint64_t dma_addr,
    uint16_t cqid, uint16_t vector, uint16_t size, uint16_t irq_enabled)
{
    int ret;

    ret = msix_vector_use(&n->parent_obj, vector);
    assert(ret == 0);
    cq->ctrl = n;
    cq->cqid = cqid;
    cq->size = size;
    cq->dma_addr = dma_addr;
    cq->phase = 1;
    cq->irq_enabled = irq_enabled;
    cq->vector = vector;
    cq->head = cq->tail = 0;
    QTAILQ_INIT(&cq->req_list);
    QTAILQ_INIT(&cq->sq_list);
    n->cq[cqid] = cq;
    cq->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, nvme_post_cqes, cq);
}

static uint16_t nvme_create_cq(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeCQueue *cq;
    NvmeCreateCq *c = (NvmeCreateCq *)&req->cmd;
    uint16_t cqid = le16_to_cpu(c->cqid);
    uint16_t vector = le16_to_cpu(c->irq_vector);
    uint16_t qsize = le16_to_cpu(c->qsize);
    uint16_t qflags = le16_to_cpu(c->cq_flags);
    uint64_t prp1 = le64_to_cpu(c->prp1);

    trace_pci_nvme_create_cq(prp1, cqid, vector, qsize, qflags,
                             NVME_CQ_FLAGS_IEN(qflags) != 0);

    if (unlikely(!cqid || !nvme_check_cqid(n, cqid))) {
        trace_pci_nvme_err_invalid_create_cq_cqid(cqid);
        return NVME_INVALID_CQID | NVME_DNR;
    }
    if (unlikely(!qsize || qsize > NVME_CAP_MQES(n->bar.cap))) {
        trace_pci_nvme_err_invalid_create_cq_size(qsize);
        return NVME_MAX_QSIZE_EXCEEDED | NVME_DNR;
    }
    if (unlikely(!prp1)) {
        trace_pci_nvme_err_invalid_create_cq_addr(prp1);
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (unlikely(!msix_enabled(&n->parent_obj) && vector)) {
        trace_pci_nvme_err_invalid_create_cq_vector(vector);
        return NVME_INVALID_IRQ_VECTOR | NVME_DNR;
    }
    if (unlikely(vector >= n->params.msix_qsize)) {
        trace_pci_nvme_err_invalid_create_cq_vector(vector);
        return NVME_INVALID_IRQ_VECTOR | NVME_DNR;
    }
    if (unlikely(!(NVME_CQ_FLAGS_PC(qflags)))) {
        trace_pci_nvme_err_invalid_create_cq_qflags(NVME_CQ_FLAGS_PC(qflags));
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    cq = g_malloc0(sizeof(*cq));
    nvme_init_cq(cq, n, prp1, cqid, vector, qsize + 1,
        NVME_CQ_FLAGS_IEN(qflags));

    /*
     * It is only required to set qs_created when creating a completion queue;
     * creating a submission queue without a matching completion queue will
     * fail.
     */
    n->qs_created = true;
    return NVME_SUCCESS;
}

static uint16_t nvme_rpt_empty_id_struct(NvmeCtrl *n, uint64_t prp1,
                                         uint64_t prp2, NvmeRequest *req)
{
    static const int data_len = NVME_IDENTIFY_DATA_SIZE;
    uint32_t *list;
    uint16_t ret;

    list = g_malloc0(data_len);
    ret = nvme_dma_prp(n, (uint8_t *)list, data_len, prp1, prp2,
                       DMA_DIRECTION_FROM_DEVICE, req);
    g_free(list);
    return ret;
}

static inline bool nvme_csi_has_nvm_support(NvmeNamespace *ns)
{
    switch (ns->csi) {
    case NVME_CSI_NVM:
    case NVME_CSI_ZONED:
        return true;
    }
    return false;
}

static uint16_t nvme_identify_ctrl(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    uint64_t prp1 = le64_to_cpu(c->prp1);
    uint64_t prp2 = le64_to_cpu(c->prp2);

    trace_pci_nvme_identify_ctrl();

    return nvme_dma_prp(n, (uint8_t *)&n->id_ctrl, sizeof(n->id_ctrl), prp1,
                        prp2, DMA_DIRECTION_FROM_DEVICE, req);
}

static uint16_t nvme_identify_ctrl_csi(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    NvmeIdCtrlZoned *id;
    uint64_t prp1 = le64_to_cpu(c->prp1);
    uint64_t prp2 = le64_to_cpu(c->prp2);
    uint16_t ret;

    trace_pci_nvme_identify_ctrl_csi(c->csi);

    if (c->csi == NVME_CSI_NVM) {
        return nvme_rpt_empty_id_struct(n, prp1, prp2, req);
    } else if (c->csi == NVME_CSI_ZONED && n->params.zoned) {
        id = g_malloc0(sizeof(*id));
        id->zasl = n->zasl;
        ret = nvme_dma_prp(n, (uint8_t *)id, sizeof(*id), prp1, prp2,
                           DMA_DIRECTION_FROM_DEVICE, req);
        g_free(id);
        return ret;
    }

    return NVME_INVALID_FIELD | NVME_DNR;
}

static uint16_t nvme_identify_ns(NvmeCtrl *n, NvmeRequest *req,
                                 bool only_active)
{
    NvmeNamespace *ns;
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    uint32_t nsid = le32_to_cpu(c->nsid);
    uint64_t prp1 = le64_to_cpu(c->prp1);
    uint64_t prp2 = le64_to_cpu(c->prp2);

    trace_pci_nvme_identify_ns(nsid);

    if (unlikely(nsid == 0 || nsid > n->num_namespaces)) {
        trace_pci_nvme_err_invalid_ns(nsid, n->num_namespaces);
        return NVME_INVALID_NSID | NVME_DNR;
    }

    ns = &n->namespaces[nsid - 1];
    assert(nsid == ns->nsid);

    if (only_active && !ns->attached) {
        return nvme_rpt_empty_id_struct(n, prp1, prp2, req);
    }

    if (c->csi == NVME_CSI_NVM && nvme_csi_has_nvm_support(ns)) {
        return nvme_dma_prp(n, (uint8_t *)&ns->id_ns, sizeof(ns->id_ns), prp1,
                            prp2, DMA_DIRECTION_FROM_DEVICE, req);
    }

    return NVME_INVALID_CMD_SET | NVME_DNR;
}

static uint16_t nvme_identify_ns_csi(NvmeCtrl *n, NvmeRequest *req,
                                     bool only_active)
{
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    NvmeNamespace *ns;
    uint32_t nsid = le32_to_cpu(c->nsid);
    uint64_t prp1 = le64_to_cpu(c->prp1);
    uint64_t prp2 = le64_to_cpu(c->prp2);

    trace_pci_nvme_identify_ns_csi(nsid, c->csi);

    if (unlikely(nsid == 0 || nsid > n->num_namespaces)) {
        trace_pci_nvme_err_invalid_ns(nsid, n->num_namespaces);
        return NVME_INVALID_NSID | NVME_DNR;
    }

    ns = &n->namespaces[nsid - 1];
    assert(nsid == ns->nsid);

    if (only_active && !ns->attached) {
        return nvme_rpt_empty_id_struct(n, prp1, prp2, req);
    }

    if (c->csi == NVME_CSI_NVM && nvme_csi_has_nvm_support(ns)) {
        return nvme_rpt_empty_id_struct(n, prp1, prp2, req);
    } else if (c->csi == NVME_CSI_ZONED && ns->csi == NVME_CSI_ZONED) {
        return nvme_dma_prp(n, (uint8_t *)ns->id_ns_zoned,
                            sizeof(*ns->id_ns_zoned), prp1, prp2,
                            DMA_DIRECTION_FROM_DEVICE, req);
    }

    return NVME_INVALID_FIELD | NVME_DNR;
}

static uint16_t nvme_identify_nslist(NvmeCtrl *n, NvmeRequest *req,
                                     bool only_active)
{
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    static const int data_len = NVME_IDENTIFY_DATA_SIZE;
    uint32_t min_nsid = le32_to_cpu(c->nsid);
    uint64_t prp1 = le64_to_cpu(c->prp1);
    uint64_t prp2 = le64_to_cpu(c->prp2);
    uint32_t *list;
    uint16_t ret;
    int i, j = 0;

    trace_pci_nvme_identify_nslist(min_nsid);

    /*
     * Both 0xffffffff (NVME_NSID_BROADCAST) and 0xfffffffe are invalid values
     * since the Active Namespace ID List should return namespaces with ids
     * *higher* than the NSID specified in the command. This is also specified
     * in the spec (NVM Express v1.3d, Section 5.15.4).
     */
    if (min_nsid >= NVME_NSID_BROADCAST - 1) {
        return NVME_INVALID_NSID | NVME_DNR;
    }

    list = g_malloc0(data_len);
    for (i = 0; i < n->num_namespaces; i++) {
        if (i < min_nsid || (only_active && !n->namespaces[i].attached)) {
            continue;
        }
        list[j++] = cpu_to_le32(i + 1);
        if (j == data_len / sizeof(uint32_t)) {
            break;
        }
    }
    ret = nvme_dma_prp(n, (uint8_t *)list, data_len, prp1, prp2,
                       DMA_DIRECTION_FROM_DEVICE, req);
    g_free(list);
    return ret;
}

static uint16_t nvme_identify_nslist_csi(NvmeCtrl *n, NvmeRequest *req,
                                         bool only_active)
{
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    static const int data_len = NVME_IDENTIFY_DATA_SIZE;
    uint32_t min_nsid = le32_to_cpu(c->nsid);
    uint64_t prp1 = le64_to_cpu(c->prp1);
    uint64_t prp2 = le64_to_cpu(c->prp2);
    uint32_t *list;
    uint16_t ret;
    int i, j = 0;

    trace_pci_nvme_identify_nslist_csi(min_nsid, c->csi);

    if (c->csi != NVME_CSI_NVM && c->csi != NVME_CSI_ZONED) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    list = g_malloc0(data_len);
    for (i = 0; i < n->num_namespaces; i++) {
        if (i < min_nsid || c->csi != n->namespaces[i].csi ||
            (only_active && !n->namespaces[i].attached)) {
            continue;
        }
        list[j++] = cpu_to_le32(i + 1);
        if (j == data_len / sizeof(uint32_t)) {
            break;
        }
    }
    ret = nvme_dma_prp(n, (uint8_t *)list, data_len, prp1, prp2,
                       DMA_DIRECTION_FROM_DEVICE, req);
    g_free(list);
    return ret;
}

static uint16_t nvme_identify_ns_descr_list(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    NvmeNamespace *ns;
    uint32_t nsid = le32_to_cpu(c->nsid);
    uint64_t prp1 = le64_to_cpu(c->prp1);
    uint64_t prp2 = le64_to_cpu(c->prp2);
    void *buf_ptr;
    NvmeIdNsDescr *desc;
    static const int data_len = NVME_IDENTIFY_DATA_SIZE;
    uint8_t *buf;
    uint16_t status;

    trace_pci_nvme_identify_ns_descr_list(nsid);

    if (unlikely(nsid == 0 || nsid > n->num_namespaces)) {
        trace_pci_nvme_err_invalid_ns(nsid, n->num_namespaces);
        return NVME_INVALID_NSID | NVME_DNR;
    }

    ns = &n->namespaces[nsid - 1];
    assert(nsid == ns->nsid);

    buf = g_malloc0(data_len);
    buf_ptr = buf;

    /*
     * Because the NGUID and EUI64 fields are 0 in the Identify Namespace data
     * structure, a Namespace UUID (nidt = 0x3) must be reported in the
     * Namespace Identification Descriptor. Add a very basic Namespace UUID
     * here.
     */
    desc = buf_ptr;
    desc->nidt = NVME_NIDT_UUID;
    desc->nidl = NVME_NIDL_UUID;
    buf_ptr += sizeof(*desc);
    memcpy(buf_ptr, ns->uuid.data, NVME_NIDL_UUID);
    buf_ptr += NVME_NIDL_UUID;

    desc = buf_ptr;
    desc->nidt = NVME_NIDT_CSI;
    desc->nidl = NVME_NIDL_CSI;
    buf_ptr += sizeof(*desc);
    *(uint8_t *)buf_ptr = ns->csi;

    status = nvme_dma_prp(n, buf, data_len, prp1, prp2,
                          DMA_DIRECTION_FROM_DEVICE, req);
    g_free(buf);
    return status;
}

static uint16_t nvme_identify_cmd_set(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    uint64_t prp1 = le64_to_cpu(c->prp1);
    uint64_t prp2 = le64_to_cpu(c->prp2);
    static const int data_len = NVME_IDENTIFY_DATA_SIZE;
    uint32_t *list;
    uint8_t *ptr;
    uint16_t status;

    trace_pci_nvme_identify_cmd_set();

    list = g_malloc0(data_len);
    ptr = (uint8_t *)list;
    NVME_SET_CSI(*ptr, NVME_CSI_NVM);
    if (n->params.zoned) {
        NVME_SET_CSI(*ptr, NVME_CSI_ZONED);
    }
    status = nvme_dma_prp(n, (uint8_t *)list, data_len, prp1, prp2,
                          DMA_DIRECTION_FROM_DEVICE, req);
    g_free(list);
    return status;
}

static uint16_t nvme_identify(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;

    switch (le32_to_cpu(c->cns)) {
    case NVME_ID_CNS_NS:
        return nvme_identify_ns(n, req, true);
    case NVME_ID_CNS_CS_NS:
        return nvme_identify_ns_csi(n, req, true);
    case NVME_ID_CNS_NS_PRESENT:
        return nvme_identify_ns(n, req, false);
    case NVME_ID_CNS_CS_NS_PRESENT:
        return nvme_identify_ns_csi(n, req, false);
    case NVME_ID_CNS_CTRL:
        return nvme_identify_ctrl(n, req);
    case NVME_ID_CNS_CS_CTRL:
        return nvme_identify_ctrl_csi(n, req);
    case NVME_ID_CNS_NS_ACTIVE_LIST:
        return nvme_identify_nslist(n, req, true);
    case NVME_ID_CNS_CS_NS_ACTIVE_LIST:
        return nvme_identify_nslist_csi(n, req, true);
    case NVME_ID_CNS_NS_PRESENT_LIST:
        return nvme_identify_nslist(n, req, false);
    case NVME_ID_CNS_CS_NS_PRESENT_LIST:
        return nvme_identify_nslist_csi(n, req, false);
    case NVME_ID_CNS_NS_DESCR_LIST:
        return nvme_identify_ns_descr_list(n, req);
    case NVME_ID_CNS_IO_COMMAND_SET:
        return nvme_identify_cmd_set(n, req);
    default:
        trace_pci_nvme_err_invalid_identify_cns(le32_to_cpu(c->cns));
        return NVME_INVALID_FIELD | NVME_DNR;
    }
}

static uint16_t nvme_abort(NvmeCtrl *n, NvmeRequest *req)
{
    uint16_t sqid = le32_to_cpu(req->cmd.cdw10) & 0xffff;

    req->cqe.result32 = 1;
    if (nvme_check_sqid(n, sqid)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    return NVME_SUCCESS;
}

static inline void nvme_set_timestamp(NvmeCtrl *n, uint64_t ts)
{
    trace_pci_nvme_setfeat_timestamp(ts);

    n->host_timestamp = le64_to_cpu(ts);
    n->timestamp_set_qemu_clock_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
}

static inline uint64_t nvme_get_timestamp(const NvmeCtrl *n)
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

    /*
     * If the sum of the Timestamp value set by the host and the elapsed
     * time exceeds 2^48, the value returned should be reduced modulo 2^48.
     */
    ts.timestamp = (n->host_timestamp + elapsed_time) & 0xffffffffffff;

    /* If the host timestamp is non-zero, set the timestamp origin */
    ts.origin = n->host_timestamp ? 0x01 : 0x00;

    trace_pci_nvme_getfeat_timestamp(ts.all);

    return cpu_to_le64(ts.all);
}

static uint16_t nvme_get_feature_timestamp(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeCmd *cmd = &req->cmd;
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);

    uint64_t timestamp = nvme_get_timestamp(n);

    return nvme_dma_prp(n, (uint8_t *)&timestamp, sizeof(timestamp), prp1,
                        prp2, DMA_DIRECTION_FROM_DEVICE, req);
}

static uint16_t nvme_get_feature(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeCmd *cmd = &req->cmd;
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t dw11 = le32_to_cpu(cmd->cdw11);
    uint32_t nsid = le32_to_cpu(cmd->nsid);
    uint32_t result;
    uint8_t fid = NVME_GETSETFEAT_FID(dw10);
    NvmeGetFeatureSelect sel = NVME_GETFEAT_SELECT(dw10);
    uint16_t iv;

    static const uint32_t nvme_feature_default[NVME_FID_MAX] = {
        [NVME_ARBITRATION] = NVME_ARB_AB_NOLIMIT,
    };

    trace_pci_nvme_getfeat(nvme_cid(req), fid, sel, dw11);

    if (!nvme_feature_support[fid]) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (nvme_feature_cap[fid] & NVME_FEAT_CAP_NS) {
        if (!nsid || nsid > n->num_namespaces) {
            /*
             * The Reservation Notification Mask and Reservation Persistence
             * features require a status code of Invalid Field in Command when
             * NSID is 0xFFFFFFFF. Since the device does not support those
             * features we can always return Invalid Namespace or Format as we
             * should do for all other features.
             */
            return NVME_INVALID_NSID | NVME_DNR;
        }
    }

    switch (sel) {
    case NVME_GETFEAT_SELECT_CURRENT:
        break;
    case NVME_GETFEAT_SELECT_SAVED:
        /* no features are saveable by the controller; fallthrough */
    case NVME_GETFEAT_SELECT_DEFAULT:
        goto defaults;
    case NVME_GETFEAT_SELECT_CAP:
        result = nvme_feature_cap[fid];
        goto out;
    }

    switch (fid) {
    case NVME_TEMPERATURE_THRESHOLD:
        result = 0;

        /*
         * The controller only implements the Composite Temperature sensor, so
         * return 0 for all other sensors.
         */
        if (NVME_TEMP_TMPSEL(dw11) != NVME_TEMP_TMPSEL_COMPOSITE) {
            goto out;
        }

        switch (NVME_TEMP_THSEL(dw11)) {
        case NVME_TEMP_THSEL_OVER:
            result = n->features.temp_thresh_hi;
            goto out;
        case NVME_TEMP_THSEL_UNDER:
            result = n->features.temp_thresh_low;
            goto out;
        }

        return NVME_INVALID_FIELD | NVME_DNR;
    case NVME_VOLATILE_WRITE_CACHE:
        result = blk_enable_write_cache(n->conf.blk);
        trace_pci_nvme_getfeat_vwcache(result ? "enabled" : "disabled");
        goto out;
    case NVME_ASYNCHRONOUS_EVENT_CONF:
        result = n->features.async_config;
        goto out;
    case NVME_TIMESTAMP:
        return nvme_get_feature_timestamp(n, req);
    default:
        break;
    }

defaults:
    switch (fid) {
    case NVME_TEMPERATURE_THRESHOLD:
        result = 0;

        if (NVME_TEMP_TMPSEL(dw11) != NVME_TEMP_TMPSEL_COMPOSITE) {
            break;
        }

        if (NVME_TEMP_THSEL(dw11) == NVME_TEMP_THSEL_OVER) {
            result = NVME_TEMPERATURE_WARNING;
        }

        break;
    case NVME_NUMBER_OF_QUEUES:
        result = (n->params.max_ioqpairs - 1) |
            ((n->params.max_ioqpairs - 1) << 16);
        trace_pci_nvme_getfeat_numq(result);
        break;
    case NVME_INTERRUPT_VECTOR_CONF:
        iv = dw11 & 0xffff;
        if (iv >= n->params.max_ioqpairs + 1) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }

        result = iv;
        if (iv == n->admin_cq.vector) {
            result |= NVME_INTVC_NOCOALESCING;
        }

        break;
    case NVME_COMMAND_SET_PROFILE:
        result = 0;
        break;
    default:
        result = nvme_feature_default[fid];
        break;
    }

out:
    req->cqe.result32 = cpu_to_le32(result);
    return NVME_SUCCESS;
}

static uint16_t nvme_set_feature_timestamp(NvmeCtrl *n, NvmeRequest *req)
{
    uint16_t ret;
    uint64_t timestamp;
    NvmeCmd *cmd = &req->cmd;
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);

    ret = nvme_dma_prp(n, (uint8_t *)&timestamp, sizeof(timestamp), prp1,
                       prp2, DMA_DIRECTION_TO_DEVICE, req);
    if (ret != NVME_SUCCESS) {
        return ret;
    }

    nvme_set_timestamp(n, timestamp);

    return NVME_SUCCESS;
}

static uint16_t nvme_set_feature(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeCmd *cmd = &req->cmd;
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t dw11 = le32_to_cpu(cmd->cdw11);
    uint32_t nsid = le32_to_cpu(cmd->nsid);
    uint8_t fid = NVME_GETSETFEAT_FID(dw10);
    uint8_t save = NVME_SETFEAT_SAVE(dw10);

    trace_pci_nvme_setfeat(nvme_cid(req), fid, save, dw11);

    if (save) {
        return NVME_FID_NOT_SAVEABLE | NVME_DNR;
    }

    if (!nvme_feature_support[fid]) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (nvme_feature_cap[fid] & NVME_FEAT_CAP_NS) {
        if (!nsid || (nsid != NVME_NSID_BROADCAST &&
                      nsid > n->num_namespaces)) {
            return NVME_INVALID_NSID | NVME_DNR;
        }
    } else if (nsid && nsid != NVME_NSID_BROADCAST) {
        if (nsid > n->num_namespaces) {
            return NVME_INVALID_NSID | NVME_DNR;
        }

        return NVME_FEAT_NOT_NS_SPEC | NVME_DNR;
    }

    if (!(nvme_feature_cap[fid] & NVME_FEAT_CAP_CHANGE)) {
        return NVME_FEAT_NOT_CHANGEABLE | NVME_DNR;
    }

    switch (fid) {
    case NVME_TEMPERATURE_THRESHOLD:
        if (NVME_TEMP_TMPSEL(dw11) != NVME_TEMP_TMPSEL_COMPOSITE) {
            break;
        }

        switch (NVME_TEMP_THSEL(dw11)) {
        case NVME_TEMP_THSEL_OVER:
            n->features.temp_thresh_hi = NVME_TEMP_TMPTH(dw11);
            break;
        case NVME_TEMP_THSEL_UNDER:
            n->features.temp_thresh_low = NVME_TEMP_TMPTH(dw11);
            break;
        default:
            return NVME_INVALID_FIELD | NVME_DNR;
        }

        if (((n->temperature >= n->features.temp_thresh_hi) ||
            (n->temperature <= n->features.temp_thresh_low)) &&
            NVME_AEC_SMART(n->features.async_config) & NVME_SMART_TEMPERATURE) {
            nvme_enqueue_event(n, NVME_AER_TYPE_SMART,
                               NVME_AER_INFO_SMART_TEMP_THRESH,
                               NVME_LOG_SMART_INFO);
        }

        break;
    case NVME_VOLATILE_WRITE_CACHE:
        if (!(dw11 & 0x1) && blk_enable_write_cache(n->conf.blk)) {
            blk_flush(n->conf.blk);
        }

        blk_set_enable_write_cache(n->conf.blk, dw11 & 1);
        break;
    case NVME_NUMBER_OF_QUEUES:
        if (n->qs_created) {
            return NVME_CMD_SEQ_ERROR | NVME_DNR;
        }

        /*
         * NVMe v1.3, Section 5.21.1.7: 0xffff is not an allowed value for NCQR
         * and NSQR.
         */
        if ((dw11 & 0xffff) == 0xffff || ((dw11 >> 16) & 0xffff) == 0xffff) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }

        trace_pci_nvme_setfeat_numq((dw11 & 0xFFFF) + 1,
                                    ((dw11 >> 16) & 0xFFFF) + 1,
                                    n->params.max_ioqpairs,
                                    n->params.max_ioqpairs);
        req->cqe.result32 = cpu_to_le32((n->params.max_ioqpairs - 1) |
                                        ((n->params.max_ioqpairs - 1) << 16));
        break;
    case NVME_ASYNCHRONOUS_EVENT_CONF:
        n->features.async_config = dw11;
        break;
    case NVME_TIMESTAMP:
        return nvme_set_feature_timestamp(n, req);
    case NVME_COMMAND_SET_PROFILE:
        if (dw11 & 0x1ff) {
            trace_pci_nvme_err_invalid_iocsci(dw11 & 0x1ff);
            return NVME_CMD_SET_CMB_REJECTED | NVME_DNR;
        }
        break;
    default:
        return NVME_FEAT_NOT_CHANGEABLE | NVME_DNR;
    }
    return NVME_SUCCESS;
}

static uint16_t nvme_aer(NvmeCtrl *n, NvmeRequest *req)
{
    trace_pci_nvme_aer(nvme_cid(req));

    if (n->outstanding_aers > n->params.aerl) {
        trace_pci_nvme_aer_aerl_exceeded();
        return NVME_AER_LIMIT_EXCEEDED;
    }

    n->aer_reqs[n->outstanding_aers] = req;
    n->outstanding_aers++;
    if (!QTAILQ_EMPTY(&n->aer_queue)) {
        nvme_process_aers(n);
    }

    return NVME_NO_COMPLETE;
}

static uint16_t nvme_admin_cmd(NvmeCtrl *n, NvmeRequest *req)
{
    trace_pci_nvme_admin_cmd(nvme_cid(req), nvme_sqid(req), req->cmd.opcode);

    switch (req->cmd.opcode) {
    case NVME_ADM_CMD_DELETE_SQ:
        return nvme_del_sq(n, req);
    case NVME_ADM_CMD_CREATE_SQ:
        return nvme_create_sq(n, req);
    case NVME_ADM_CMD_GET_LOG_PAGE:
        return nvme_get_log(n, req);
    case NVME_ADM_CMD_DELETE_CQ:
        return nvme_del_cq(n, req);
    case NVME_ADM_CMD_CREATE_CQ:
        return nvme_create_cq(n, req);
    case NVME_ADM_CMD_IDENTIFY:
        return nvme_identify(n, req);
    case NVME_ADM_CMD_ABORT:
        return nvme_abort(n, req);
    case NVME_ADM_CMD_SET_FEATURES:
        return nvme_set_feature(n, req);
    case NVME_ADM_CMD_GET_FEATURES:
        return nvme_get_feature(n, req);
    case NVME_ADM_CMD_ASYNC_EV_REQ:
        return nvme_aer(n, req);
    default:
        trace_pci_nvme_err_invalid_admin_opc(req->cmd.opcode);
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static void nvme_process_sq(void *opaque)
{
    NvmeSQueue *sq = opaque;
    NvmeCtrl *n = sq->ctrl;
    NvmeCQueue *cq = n->cq[sq->cqid];

    uint16_t status;
    hwaddr addr;
    NvmeCmd cmd;
    NvmeRequest *req;

    while (!(nvme_sq_empty(sq) || QTAILQ_EMPTY(&sq->req_list))) {
        addr = sq->dma_addr + sq->head * n->sqe_size;
        nvme_addr_read(n, addr, (void *)&cmd, sizeof(cmd));
        nvme_inc_sq_head(sq);

        req = QTAILQ_FIRST(&sq->req_list);
        QTAILQ_REMOVE(&sq->req_list, req, entry);
        QTAILQ_INSERT_TAIL(&sq->out_req_list, req, entry);
        nvme_req_clear(req);
        req->cqe.cid = cmd.cid;
        memcpy(&req->cmd, &cmd, sizeof(NvmeCmd));

        status = sq->sqid ? nvme_io_cmd(n, req) :
            nvme_admin_cmd(n, req);
        if (status != NVME_NO_COMPLETE) {
            req->status = status;
            nvme_enqueue_req_completion(cq, req);
        }
    }
}

static void nvme_clear_ctrl(NvmeCtrl *n)
{
    int i;

    blk_drain(n->conf.blk);

    for (i = 0; i < n->params.max_ioqpairs + 1; i++) {
        if (n->sq[i] != NULL) {
            nvme_free_sq(n->sq[i], n);
        }
    }
    for (i = 0; i < n->params.max_ioqpairs + 1; i++) {
        if (n->cq[i] != NULL) {
            nvme_free_cq(n->cq[i], n);
        }
    }

    while (!QTAILQ_EMPTY(&n->aer_queue)) {
        NvmeAsyncEvent *event = QTAILQ_FIRST(&n->aer_queue);
        QTAILQ_REMOVE(&n->aer_queue, event, entry);
        g_free(event);
    }

    n->aer_queued = 0;
    n->outstanding_aers = 0;
    n->qs_created = false;

    blk_flush(n->conf.blk);
    n->bar.cc = 0;
}

static int nvme_start_ctrl(NvmeCtrl *n)
{
    uint32_t page_bits = NVME_CC_MPS(n->bar.cc) + 12;
    uint32_t page_size = 1 << page_bits;
    int i;

    if (unlikely(n->cq[0])) {
        trace_pci_nvme_err_startfail_cq();
        return -1;
    }
    if (unlikely(n->sq[0])) {
        trace_pci_nvme_err_startfail_sq();
        return -1;
    }
    if (unlikely(!n->bar.asq)) {
        trace_pci_nvme_err_startfail_nbarasq();
        return -1;
    }
    if (unlikely(!n->bar.acq)) {
        trace_pci_nvme_err_startfail_nbaracq();
        return -1;
    }
    if (unlikely(n->bar.asq & (page_size - 1))) {
        trace_pci_nvme_err_startfail_asq_misaligned(n->bar.asq);
        return -1;
    }
    if (unlikely(n->bar.acq & (page_size - 1))) {
        trace_pci_nvme_err_startfail_acq_misaligned(n->bar.acq);
        return -1;
    }
    if (unlikely(NVME_CC_MPS(n->bar.cc) <
                 NVME_CAP_MPSMIN(n->bar.cap))) {
        trace_pci_nvme_err_startfail_page_too_small(
                    NVME_CC_MPS(n->bar.cc),
                    NVME_CAP_MPSMIN(n->bar.cap));
        return -1;
    }
    if (unlikely(NVME_CC_MPS(n->bar.cc) >
                 NVME_CAP_MPSMAX(n->bar.cap))) {
        trace_pci_nvme_err_startfail_page_too_large(
                    NVME_CC_MPS(n->bar.cc),
                    NVME_CAP_MPSMAX(n->bar.cap));
        return -1;
    }
    if (unlikely(NVME_CC_IOCQES(n->bar.cc) <
                 NVME_CTRL_CQES_MIN(n->id_ctrl.cqes))) {
        trace_pci_nvme_err_startfail_cqent_too_small(
                    NVME_CC_IOCQES(n->bar.cc),
                    NVME_CTRL_CQES_MIN(n->bar.cap));
        return -1;
    }
    if (unlikely(NVME_CC_IOCQES(n->bar.cc) >
                 NVME_CTRL_CQES_MAX(n->id_ctrl.cqes))) {
        trace_pci_nvme_err_startfail_cqent_too_large(
                    NVME_CC_IOCQES(n->bar.cc),
                    NVME_CTRL_CQES_MAX(n->bar.cap));
        return -1;
    }
    if (unlikely(NVME_CC_IOSQES(n->bar.cc) <
                 NVME_CTRL_SQES_MIN(n->id_ctrl.sqes))) {
        trace_pci_nvme_err_startfail_sqent_too_small(
                    NVME_CC_IOSQES(n->bar.cc),
                    NVME_CTRL_SQES_MIN(n->bar.cap));
        return -1;
    }
    if (unlikely(NVME_CC_IOSQES(n->bar.cc) >
                 NVME_CTRL_SQES_MAX(n->id_ctrl.sqes))) {
        trace_pci_nvme_err_startfail_sqent_too_large(
                    NVME_CC_IOSQES(n->bar.cc),
                    NVME_CTRL_SQES_MAX(n->bar.cap));
        return -1;
    }
    if (unlikely(!NVME_AQA_ASQS(n->bar.aqa))) {
        trace_pci_nvme_err_startfail_asqent_sz_zero();
        return -1;
    }
    if (unlikely(!NVME_AQA_ACQS(n->bar.aqa))) {
        trace_pci_nvme_err_startfail_acqent_sz_zero();
        return -1;
    }

    n->page_bits = page_bits;
    n->page_size = page_size;
    n->max_prp_ents = n->page_size / sizeof(uint64_t);
    n->cqe_size = 1 << NVME_CC_IOCQES(n->bar.cc);
    n->sqe_size = 1 << NVME_CC_IOSQES(n->bar.cc);
    nvme_init_cq(&n->admin_cq, n, n->bar.acq, 0, 0,
        NVME_AQA_ACQS(n->bar.aqa) + 1, 1);
    nvme_init_sq(&n->admin_sq, n, n->bar.asq, 0, 0,
        NVME_AQA_ASQS(n->bar.aqa) + 1);

    for (i = 0; i < n->num_namespaces; i++) {
        n->namespaces[i].attached = false;
        switch (n->namespaces[i].csi) {
        case NVME_CSI_NVM:
            if (NVME_CC_CSS(n->bar.cc) == CSS_NVM_ONLY ||
                NVME_CC_CSS(n->bar.cc) == CSS_CSI) {
                n->namespaces[i].attached = true;
            }
            break;
        case NVME_CSI_ZONED:
            if (NVME_CC_CSS(n->bar.cc) == CSS_CSI) {
                n->namespaces[i].attached = true;
            }
            break;
        }
    }

    if (n->params.zoned) {
        if (!n->zasl_bs) {
            assert(n->params.mdts);
            n->zasl = n->params.mdts;
        } else {
            n->zasl = nvme_ilog2(n->zasl_bs / n->page_size);
        }
    }

    nvme_set_timestamp(n, 0ULL);

    QTAILQ_INIT(&n->aer_queue);

    return 0;
}

static void nvme_write_bar(NvmeCtrl *n, hwaddr offset, uint64_t data,
    unsigned size)
{
    if (unlikely(offset & (sizeof(uint32_t) - 1))) {
        NVME_GUEST_ERR(pci_nvme_ub_mmiowr_misaligned32,
                       "MMIO write not 32-bit aligned,"
                       " offset=0x%"PRIx64"", offset);
        /* should be ignored, fall through for now */
    }

    if (unlikely(size < sizeof(uint32_t))) {
        NVME_GUEST_ERR(pci_nvme_ub_mmiowr_toosmall,
                       "MMIO write smaller than 32-bits,"
                       " offset=0x%"PRIx64", size=%u",
                       offset, size);
        /* should be ignored, fall through for now */
    }

    switch (offset) {
    case 0xc:   /* INTMS */
        if (unlikely(msix_enabled(&(n->parent_obj)))) {
            NVME_GUEST_ERR(pci_nvme_ub_mmiowr_intmask_with_msix,
                           "undefined access to interrupt mask set"
                           " when MSI-X is enabled");
            /* should be ignored, fall through for now */
        }
        n->bar.intms |= data & 0xffffffff;
        n->bar.intmc = n->bar.intms;
        trace_pci_nvme_mmio_intm_set(data & 0xffffffff, n->bar.intmc);
        nvme_irq_check(n);
        break;
    case 0x10:  /* INTMC */
        if (unlikely(msix_enabled(&(n->parent_obj)))) {
            NVME_GUEST_ERR(pci_nvme_ub_mmiowr_intmask_with_msix,
                           "undefined access to interrupt mask clr"
                           " when MSI-X is enabled");
            /* should be ignored, fall through for now */
        }
        n->bar.intms &= ~(data & 0xffffffff);
        n->bar.intmc = n->bar.intms;
        trace_pci_nvme_mmio_intm_clr(data & 0xffffffff, n->bar.intmc);
        nvme_irq_check(n);
        break;
    case 0x14:  /* CC */
        trace_pci_nvme_mmio_cfg(data & 0xffffffff);

        if (NVME_CC_CSS(data) != NVME_CC_CSS(n->bar.cc)) {
            if (NVME_CC_EN(n->bar.cc)) {
                NVME_GUEST_ERR(pci_nvme_err_change_css_when_enabled,
                               "changing selected command set when enabled");
            } else {
                switch (NVME_CC_CSS(data)) {
                case CSS_NVM_ONLY:
                    if (n->params.zoned) {
                        NVME_GUEST_ERR(pci_nvme_err_only_zoned_cmd_set_avail,
                               "only NVM+ZONED command set can be selected");
                    break;
                }
                trace_pci_nvme_css_nvm_cset_selected_by_host(data &
                                                             0xffffffff);
                break;
                case CSS_CSI:
                    NVME_SET_CC_CSS(n->bar.cc, CSS_CSI);
                    trace_pci_nvme_css_all_csets_sel_by_host(data &
                                                             0xffffffff);
                    break;
                case CSS_ADMIN_ONLY:
                    break;
                default:
                    NVME_GUEST_ERR(pci_nvme_ub_unknown_css_value,
                                   "unknown value in CC.CSS field");
                }
            }
        }

        /* Windows first sends data, then sends enable bit */
        if (!NVME_CC_EN(data) && !NVME_CC_EN(n->bar.cc) &&
            !NVME_CC_SHN(data) && !NVME_CC_SHN(n->bar.cc))
        {
            n->bar.cc = data;
        }

        if (NVME_CC_EN(data) && !NVME_CC_EN(n->bar.cc)) {
            n->bar.cc = data;
            if (unlikely(nvme_start_ctrl(n))) {
                trace_pci_nvme_err_startfail();
                n->bar.csts = NVME_CSTS_FAILED;
            } else {
                trace_pci_nvme_mmio_start_success();
                n->bar.csts = NVME_CSTS_READY;
            }
        } else if (!NVME_CC_EN(data) && NVME_CC_EN(n->bar.cc)) {
            trace_pci_nvme_mmio_stopped();
            nvme_clear_ctrl(n);
            n->bar.csts &= ~NVME_CSTS_READY;
        }
        if (NVME_CC_SHN(data) && !(NVME_CC_SHN(n->bar.cc))) {
            trace_pci_nvme_mmio_shutdown_set();
            nvme_clear_ctrl(n);
            n->bar.cc = data;
            n->bar.csts |= NVME_CSTS_SHST_COMPLETE;
        } else if (!NVME_CC_SHN(data) && NVME_CC_SHN(n->bar.cc)) {
            trace_pci_nvme_mmio_shutdown_cleared();
            n->bar.csts &= ~NVME_CSTS_SHST_COMPLETE;
            n->bar.cc = data;
        }
        break;
    case 0x1C:  /* CSTS */
        if (data & (1 << 4)) {
            NVME_GUEST_ERR(pci_nvme_ub_mmiowr_ssreset_w1c_unsupported,
                           "attempted to W1C CSTS.NSSRO"
                           " but CAP.NSSRS is zero (not supported)");
        } else if (data != 0) {
            NVME_GUEST_ERR(pci_nvme_ub_mmiowr_ro_csts,
                           "attempted to set a read only bit"
                           " of controller status");
        }
        break;
    case 0x20:  /* NSSR */
        if (data == 0x4E564D65) {
            trace_pci_nvme_ub_mmiowr_ssreset_unsupported();
        } else {
            /* The spec says that writes of other values have no effect */
            return;
        }
        break;
    case 0x24:  /* AQA */
        n->bar.aqa = data & 0xffffffff;
        trace_pci_nvme_mmio_aqattr(data & 0xffffffff);
        break;
    case 0x28:  /* ASQ */
        n->bar.asq = data;
        trace_pci_nvme_mmio_asqaddr(data);
        break;
    case 0x2c:  /* ASQ hi */
        n->bar.asq |= data << 32;
        trace_pci_nvme_mmio_asqaddr_hi(data, n->bar.asq);
        break;
    case 0x30:  /* ACQ */
        trace_pci_nvme_mmio_acqaddr(data);
        n->bar.acq = data;
        break;
    case 0x34:  /* ACQ hi */
        n->bar.acq |= data << 32;
        trace_pci_nvme_mmio_acqaddr_hi(data, n->bar.acq);
        break;
    case 0x38:  /* CMBLOC */
        NVME_GUEST_ERR(pci_nvme_ub_mmiowr_cmbloc_reserved,
                       "invalid write to reserved CMBLOC"
                       " when CMBSZ is zero, ignored");
        return;
    case 0x3C:  /* CMBSZ */
        NVME_GUEST_ERR(pci_nvme_ub_mmiowr_cmbsz_readonly,
                       "invalid write to read only CMBSZ, ignored");
        return;
    case 0xE00: /* PMRCAP */
        NVME_GUEST_ERR(pci_nvme_ub_mmiowr_pmrcap_readonly,
                       "invalid write to PMRCAP register, ignored");
        return;
    case 0xE04: /* TODO PMRCTL */
        break;
    case 0xE08: /* PMRSTS */
        NVME_GUEST_ERR(pci_nvme_ub_mmiowr_pmrsts_readonly,
                       "invalid write to PMRSTS register, ignored");
        return;
    case 0xE0C: /* PMREBS */
        NVME_GUEST_ERR(pci_nvme_ub_mmiowr_pmrebs_readonly,
                       "invalid write to PMREBS register, ignored");
        return;
    case 0xE10: /* PMRSWTP */
        NVME_GUEST_ERR(pci_nvme_ub_mmiowr_pmrswtp_readonly,
                       "invalid write to PMRSWTP register, ignored");
        return;
    case 0xE14: /* TODO PMRMSC */
         break;
    default:
        NVME_GUEST_ERR(pci_nvme_ub_mmiowr_invalid,
                       "invalid MMIO write,"
                       " offset=0x%"PRIx64", data=%"PRIx64"",
                       offset, data);
        break;
    }
}

static uint64_t nvme_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    NvmeCtrl *n = (NvmeCtrl *)opaque;
    uint8_t *ptr = (uint8_t *)&n->bar;
    uint64_t val = 0;

    trace_pci_nvme_mmio_read(addr);

    if (unlikely(addr & (sizeof(uint32_t) - 1))) {
        NVME_GUEST_ERR(pci_nvme_ub_mmiord_misaligned32,
                       "MMIO read not 32-bit aligned,"
                       " offset=0x%"PRIx64"", addr);
        /* should RAZ, fall through for now */
    } else if (unlikely(size < sizeof(uint32_t))) {
        NVME_GUEST_ERR(pci_nvme_ub_mmiord_toosmall,
                       "MMIO read smaller than 32-bits,"
                       " offset=0x%"PRIx64"", addr);
        /* should RAZ, fall through for now */
    }

    if (addr < sizeof(n->bar)) {
        /*
         * When PMRWBM bit 1 is set then read from
         * from PMRSTS should ensure prior writes
         * made it to persistent media
         */
        if (addr == 0xE08 &&
            (NVME_PMRCAP_PMRWBM(n->bar.pmrcap) & 0x02)) {
            memory_region_msync(&n->pmrdev->mr, 0, n->pmrdev->size);
        }
        memcpy(&val, ptr + addr, size);
    } else {
        NVME_GUEST_ERR(pci_nvme_ub_mmiord_invalid_ofs,
                       "MMIO read beyond last register,"
                       " offset=0x%"PRIx64", returning 0", addr);
    }

    return val;
}

static void nvme_process_db(NvmeCtrl *n, hwaddr addr, int val)
{
    uint32_t qid;

    if (unlikely(addr & ((1 << 2) - 1))) {
        NVME_GUEST_ERR(pci_nvme_ub_db_wr_misaligned,
                       "doorbell write not 32-bit aligned,"
                       " offset=0x%"PRIx64", ignoring", addr);
        return;
    }

    if (((addr - 0x1000) >> 2) & 1) {
        /* Completion queue doorbell write */

        uint16_t new_head = val & 0xffff;
        int start_sqs;
        NvmeCQueue *cq;

        qid = (addr - (0x1000 + (1 << 2))) >> 3;
        if (unlikely(nvme_check_cqid(n, qid))) {
            NVME_GUEST_ERR(pci_nvme_ub_db_wr_invalid_cq,
                           "completion queue doorbell write"
                           " for nonexistent queue,"
                           " sqid=%"PRIu32", ignoring", qid);

            /*
             * NVM Express v1.3d, Section 4.1 state: "If host software writes
             * an invalid value to the Submission Queue Tail Doorbell or
             * Completion Queue Head Doorbell regiter and an Asynchronous Event
             * Request command is outstanding, then an asynchronous event is
             * posted to the Admin Completion Queue with a status code of
             * Invalid Doorbell Write Value."
             *
             * Also note that the spec includes the "Invalid Doorbell Register"
             * status code, but nowhere does it specify when to use it.
             * However, it seems reasonable to use it here in a similar
             * fashion.
             */
            if (n->outstanding_aers) {
                nvme_enqueue_event(n, NVME_AER_TYPE_ERROR,
                                   NVME_AER_INFO_ERR_INVALID_DB_REGISTER,
                                   NVME_LOG_ERROR_INFO);
            }

            return;
        }

        cq = n->cq[qid];
        if (unlikely(new_head >= cq->size)) {
            NVME_GUEST_ERR(pci_nvme_ub_db_wr_invalid_cqhead,
                           "completion queue doorbell write value"
                           " beyond queue size, sqid=%"PRIu32","
                           " new_head=%"PRIu16", ignoring",
                           qid, new_head);

            if (n->outstanding_aers) {
                nvme_enqueue_event(n, NVME_AER_TYPE_ERROR,
                                   NVME_AER_INFO_ERR_INVALID_DB_VALUE,
                                   NVME_LOG_ERROR_INFO);
            }

            return;
        }

        trace_pci_nvme_mmio_doorbell_cq(cq->cqid, new_head);

        start_sqs = nvme_cq_full(cq) ? 1 : 0;
        cq->head = new_head;
        if (start_sqs) {
            NvmeSQueue *sq;
            QTAILQ_FOREACH(sq, &cq->sq_list, entry) {
                timer_mod(sq->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500);
            }
            timer_mod(cq->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500);
        }

        if (cq->tail == cq->head) {
            nvme_irq_deassert(n, cq);
        }
    } else {
        /* Submission queue doorbell write */

        uint16_t new_tail = val & 0xffff;
        NvmeSQueue *sq;

        qid = (addr - 0x1000) >> 3;
        if (unlikely(nvme_check_sqid(n, qid))) {
            NVME_GUEST_ERR(pci_nvme_ub_db_wr_invalid_sq,
                           "submission queue doorbell write"
                           " for nonexistent queue,"
                           " sqid=%"PRIu32", ignoring", qid);

            if (n->outstanding_aers) {
                nvme_enqueue_event(n, NVME_AER_TYPE_ERROR,
                                   NVME_AER_INFO_ERR_INVALID_DB_REGISTER,
                                   NVME_LOG_ERROR_INFO);
            }

            return;
        }

        sq = n->sq[qid];
        if (unlikely(new_tail >= sq->size)) {
            NVME_GUEST_ERR(pci_nvme_ub_db_wr_invalid_sqtail,
                           "submission queue doorbell write value"
                           " beyond queue size, sqid=%"PRIu32","
                           " new_tail=%"PRIu16", ignoring",
                           qid, new_tail);

            if (n->outstanding_aers) {
                nvme_enqueue_event(n, NVME_AER_TYPE_ERROR,
                                   NVME_AER_INFO_ERR_INVALID_DB_VALUE,
                                   NVME_LOG_ERROR_INFO);
            }

            return;
        }

        trace_pci_nvme_mmio_doorbell_sq(sq->sqid, new_tail);

        sq->tail = new_tail;
        timer_mod(sq->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500);
    }
}

static void nvme_mmio_write(void *opaque, hwaddr addr, uint64_t data,
    unsigned size)
{
    NvmeCtrl *n = (NvmeCtrl *)opaque;

    trace_pci_nvme_mmio_write(addr, data);

    if (addr < sizeof(n->bar)) {
        nvme_write_bar(n, addr, data, size);
    } else {
        nvme_process_db(n, addr, data);
    }
}

static const MemoryRegionOps nvme_mmio_ops = {
    .read = nvme_mmio_read,
    .write = nvme_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 2,
        .max_access_size = 8,
    },
};

static void nvme_cmb_write(void *opaque, hwaddr addr, uint64_t data,
    unsigned size)
{
    NvmeCtrl *n = (NvmeCtrl *)opaque;
    stn_le_p(&n->cmbuf[addr], size, data);
}

static uint64_t nvme_cmb_read(void *opaque, hwaddr addr, unsigned size)
{
    NvmeCtrl *n = (NvmeCtrl *)opaque;
    return ldn_le_p(&n->cmbuf[addr], size);
}

static const MemoryRegionOps nvme_cmb_ops = {
    .read = nvme_cmb_read,
    .write = nvme_cmb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static int nvme_validate_zone_file(NvmeCtrl *n, NvmeNamespace *ns,
                                   uint64_t capacity)
{
    NvmeZoneMeta *meta = ns->zone_meta;
    NvmeZone *zone = ns->zone_array;
    uint64_t start = 0, zone_size = n->zone_size;
    int i, n_imp_open = 0, n_exp_open = 0, n_closed = 0, n_full = 0;

    if (meta->magic != NVME_ZONE_META_MAGIC) {
        return 1;
    }
    if (meta->version != NVME_ZONE_META_VER) {
        return 2;
    }
    if (meta->zone_size != zone_size) {
        return 3;
    }
    if (meta->zone_capacity != n->zone_capacity) {
        return 4;
    }
    if (meta->nr_offline_zones != n->params.nr_offline_zones) {
        return 5;
    }
    if (meta->nr_rdonly_zones != n->params.nr_rdonly_zones) {
        return 6;
    }
    if (meta->lba_size != n->conf.logical_block_size) {
        return 7;
    }
    if (meta->zd_extension_size != n->params.zd_extension_size) {
        return 8;
    }

    for (i = 0; i < n->num_zones; i++, zone++) {
        if (start + zone_size > capacity) {
            zone_size = capacity - start;
        }
        if (zone->d.zt != NVME_ZONE_TYPE_SEQ_WRITE) {
            return 9;
        }
        if (zone->d.zcap != n->zone_capacity) {
            return 10;
        }
        if (zone->d.zslba != start) {
            return 11;
        }
        switch (nvme_get_zone_state(zone)) {
        case NVME_ZONE_STATE_EMPTY:
        case NVME_ZONE_STATE_OFFLINE:
        case NVME_ZONE_STATE_READ_ONLY:
            if (zone->d.wp != start) {
                return 12;
            }
            break;
        case NVME_ZONE_STATE_IMPLICITLY_OPEN:
            if (zone->d.wp < start ||
                zone->d.wp >= zone->d.zslba + zone->d.zcap) {
                return 13;
            }
            n_imp_open++;
            break;
        case NVME_ZONE_STATE_EXPLICITLY_OPEN:
            if (zone->d.wp < start ||
                zone->d.wp >= zone->d.zslba + zone->d.zcap) {
                return 13;
            }
            n_exp_open++;
            break;
        case NVME_ZONE_STATE_CLOSED:
            if (zone->d.wp < start ||
                zone->d.wp >= zone->d.zslba + zone->d.zcap) {
                return 13;
            }
            n_closed++;
            break;
        case NVME_ZONE_STATE_FULL:
            if (zone->d.wp != zone->d.zslba + zone->d.zcap) {
                return 14;
            }
            n_full++;
            break;
        default:
            return 15;
        }

        start += zone_size;
    }

    if (n_imp_open != nvme_zone_list_size(ns->exp_open_zones)) {
        return 16;
    }
    if (n_exp_open != nvme_zone_list_size(ns->imp_open_zones)) {
        return 17;
    }
    if (n_closed != nvme_zone_list_size(ns->closed_zones)) {
        return 18;
    }
    if (n_full != nvme_zone_list_size(ns->full_zones)) {
        return 19;
    }

    return 0;
}

static int nvme_init_zone_file(NvmeCtrl *n, NvmeNamespace *ns,
                               uint64_t capacity)
{
    NvmeZoneMeta *meta = ns->zone_meta;
    NvmeZone *zone;
    Error *err;
    uint64_t start = 0, zone_size = n->zone_size;
    uint32_t rnd;
    int i;
    uint16_t zs;

    if (n->params.zone_file) {
        meta->magic = NVME_ZONE_META_MAGIC;
        meta->version = NVME_ZONE_META_VER;
        meta->zone_size = zone_size;
        meta->zone_capacity = n->zone_capacity;
        meta->lba_size = n->conf.logical_block_size;
        meta->nr_offline_zones = n->params.nr_offline_zones;
        meta->nr_rdonly_zones = n->params.nr_rdonly_zones;
        meta->zd_extension_size = n->params.zd_extension_size;
    } else {
        ns->zone_array = g_malloc0(n->zone_array_size);
        ns->exp_open_zones = g_malloc0(sizeof(NvmeZoneList));
        ns->imp_open_zones = g_malloc0(sizeof(NvmeZoneList));
        ns->closed_zones = g_malloc0(sizeof(NvmeZoneList));
        ns->full_zones = g_malloc0(sizeof(NvmeZoneList));
        ns->zd_extensions =
            g_malloc0(n->params.zd_extension_size * n->num_zones);
    }
    zone = ns->zone_array;

    nvme_init_zone_list(ns->exp_open_zones);
    nvme_init_zone_list(ns->imp_open_zones);
    nvme_init_zone_list(ns->closed_zones);
    nvme_init_zone_list(ns->full_zones);
    if (n->params.zone_file) {
        nvme_set_zone_meta_dirty(n, ns, true);
    }

    for (i = 0; i < n->num_zones; i++, zone++) {
        if (start + zone_size > capacity) {
            zone_size = capacity - start;
        }
        zone->d.zt = NVME_ZONE_TYPE_SEQ_WRITE;
        nvme_set_zone_state(zone, NVME_ZONE_STATE_EMPTY);
        zone->d.za = 0;
        zone->d.zcap = n->zone_capacity;
        zone->d.zslba = start;
        zone->d.wp = start;
        zone->prev = 0;
        zone->next = 0;
        start += zone_size;
    }

    /* If required, make some zones Offline or Read Only */

    for (i = 0; i < n->params.nr_offline_zones; i++) {
        do {
            qcrypto_random_bytes(&rnd, sizeof(rnd), &err);
            rnd %= n->num_zones;
        } while (rnd < n->params.max_open_zones);
        zone = &ns->zone_array[rnd];
        zs = nvme_get_zone_state(zone);
        if (zs != NVME_ZONE_STATE_OFFLINE) {
            nvme_set_zone_state(zone, NVME_ZONE_STATE_OFFLINE);
        } else {
            i--;
        }
    }

    for (i = 0; i < n->params.nr_rdonly_zones; i++) {
        do {
            qcrypto_random_bytes(&rnd, sizeof(rnd), &err);
            rnd %= n->num_zones;
        } while (rnd < n->params.max_open_zones);
        zone = &ns->zone_array[rnd];
        zs = nvme_get_zone_state(zone);
        if (zs != NVME_ZONE_STATE_OFFLINE &&
            zs != NVME_ZONE_STATE_READ_ONLY) {
            nvme_set_zone_state(zone, NVME_ZONE_STATE_READ_ONLY);
        } else {
            i--;
        }
    }

    return 0;
}

static int nvme_open_zone_file(NvmeCtrl *n, bool *init_meta)
{
    struct stat statbuf;
    size_t fsize;
    int ret;

    ret = stat(n->params.zone_file, &statbuf);
    if (ret && errno == ENOENT) {
        *init_meta = true;
    } else if (!S_ISREG(statbuf.st_mode)) {
        fprintf(stderr, "%s is not a regular file\n", strerror(errno));
        return -1;
    }

    n->zone_file_fd = open(n->params.zone_file,
                           O_RDWR | O_LARGEFILE | O_BINARY | O_CREAT, 644);
    if (n->zone_file_fd < 0) {
            fprintf(stderr, "failed to create zone file %s, err %s\n",
                    n->params.zone_file, strerror(errno));
            return -1;
    }

    fsize = n->meta_size * n->num_namespaces;

    if (stat(n->params.zone_file, &statbuf)) {
        fprintf(stderr, "can't stat zone file %s, err %s\n",
                n->params.zone_file, strerror(errno));
        return -1;
    }
    if (statbuf.st_size != fsize) {
        ret = ftruncate(n->zone_file_fd, fsize);
        if (ret < 0) {
            fprintf(stderr, "can't truncate zone file %s, err %s\n",
                    n->params.zone_file, strerror(errno));
            return -1;
        }
        *init_meta = true;
    }

    return 0;
}

static int nvme_map_zone_file(NvmeCtrl *n, NvmeNamespace *ns, bool *init_meta)
{
    off_t meta_ofs = n->meta_size * (ns->nsid - 1);

    ns->zone_meta = mmap(0, n->meta_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, n->zone_file_fd, meta_ofs);
    if (ns->zone_meta == MAP_FAILED) {
        fprintf(stderr, "failed to map zone file %s, ofs %lu, err %s\n",
                n->params.zone_file, meta_ofs, strerror(errno));
        return -1;
    }

    ns->zone_array = (NvmeZone *)(ns->zone_meta + 1);
    ns->exp_open_zones = &ns->zone_meta->exp_open_zones;
    ns->imp_open_zones = &ns->zone_meta->imp_open_zones;
    ns->closed_zones = &ns->zone_meta->closed_zones;
    ns->full_zones = &ns->zone_meta->full_zones;

    if (n->params.zd_extension_size) {
        ns->zd_extensions = (uint8_t *)(ns->zone_meta + 1);
        ns->zd_extensions += n->zone_array_size;
    }

    return 0;
}

static void nvme_sync_zone_file(NvmeCtrl *n, NvmeNamespace *ns,
                                NvmeZone *zone, int len)
{
    uintptr_t addr, zd = (uintptr_t)zone;

    addr = zd & qemu_real_host_page_mask;
    len += zd - addr;
    if (msync((void *)addr, len, MS_ASYNC) < 0)
        fprintf(stderr, "msync: failed to sync zone descriptors, file %s\n",
                strerror(errno));

    if (nvme_zone_meta_dirty(n, ns)) {
        nvme_set_zone_meta_dirty(n, ns, false);
        if (msync(ns->zone_meta, sizeof(NvmeZoneMeta), MS_ASYNC) < 0)
            fprintf(stderr, "msync: failed to sync zone meta, file %s\n",
                    strerror(errno));
    }
}

/*
 * Close or finish all the zones that might be still open after power-down.
 */
static void nvme_prepare_zones(NvmeCtrl *n, NvmeNamespace *ns)
{
    NvmeZone *zone;
    uint32_t set_state;
    int i;

    assert(!ns->nr_active_zones);
    assert(!ns->nr_open_zones);

    zone = ns->zone_array;
    for (i = 0; i < n->num_zones; i++, zone++) {
        zone->tstamp = 0;

        switch (nvme_get_zone_state(zone)) {
        case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        case NVME_ZONE_STATE_EXPLICITLY_OPEN:
            break;
        case NVME_ZONE_STATE_CLOSED:
            nvme_aor_inc_active(n, ns);
            /* pass through */
        default:
            continue;
        }

        if (zone->d.za & NVME_ZA_ZD_EXT_VALID) {
            set_state = NVME_ZONE_STATE_CLOSED;
        } else if (zone->d.wp == zone->d.zslba) {
            set_state = NVME_ZONE_STATE_EMPTY;
        } else if (n->params.max_active_zones == 0 ||
                   ns->nr_active_zones < n->params.max_active_zones) {
            set_state = NVME_ZONE_STATE_CLOSED;
        } else {
            set_state = NVME_ZONE_STATE_FULL;
        }

        switch (set_state) {
        case NVME_ZONE_STATE_CLOSED:
            trace_pci_nvme_power_on_close(nvme_get_zone_state(zone),
                                          zone->d.zslba);
            nvme_aor_inc_active(n, ns);
            nvme_add_zone_tail(n, ns, ns->closed_zones, zone);
        break;
        case NVME_ZONE_STATE_EMPTY:
            trace_pci_nvme_power_on_reset(nvme_get_zone_state(zone),
                                          zone->d.zslba);
        break;
        case NVME_ZONE_STATE_FULL:
            trace_pci_nvme_power_on_full(nvme_get_zone_state(zone),
                                         zone->d.zslba);
            zone->d.wp = nvme_zone_wr_boundary(zone);
        }

        nvme_set_zone_state(zone, set_state);
    }
}

static int nvme_load_zone_meta(NvmeCtrl *n, NvmeNamespace *ns,
                               uint64_t capacity, bool init_meta)
{
    int ret = 0;

    if (n->params.zone_file) {
        ret = nvme_map_zone_file(n, ns, &init_meta);
        trace_pci_nvme_mapped_zone_file(n->params.zone_file, ret);
        if (ret < 0) {
            return ret;
        }

        if (!init_meta) {
            ret = nvme_validate_zone_file(n, ns, capacity);
            if (ret) {
                trace_pci_nvme_err_zone_file_invalid(ret);
                init_meta = true;
            }
        }
    } else {
        init_meta = true;
    }

    if (init_meta) {
        ret = nvme_init_zone_file(n, ns, capacity);
    } else {
        nvme_prepare_zones(n, ns);
    }
    if (!ret && n->params.zone_file) {
        nvme_sync_zone_file(n, ns, ns->zone_array, n->zone_array_size);
    }

    return ret;
}

static void nvme_zoned_init_ctrl(NvmeCtrl *n, bool *init_meta, Error **errp)
{
    uint64_t zone_size, zone_cap;
    uint32_t nz;

    if (n->params.zone_size_mb) {
        zone_size = n->params.zone_size_mb;
    } else {
        zone_size = NVME_DEFAULT_ZONE_SIZE;
    }
    if (n->params.zone_capacity_mb) {
        zone_cap = n->params.zone_capacity_mb;
    } else {
        zone_cap = zone_size;
    }
    n->zone_size = zone_size * MiB / n->conf.logical_block_size;
    n->zone_capacity = zone_cap * MiB / n->conf.logical_block_size;
    if (n->zone_capacity > n->zone_size) {
        error_setg(errp, "zone capacity exceeds zone size");
        return;
    }

    nz = DIV_ROUND_UP(n->ns_size / n->conf.logical_block_size, n->zone_size);
    n->num_zones = nz;
    n->zone_array_size = sizeof(NvmeZone) * nz;
    n->zone_size_log2 = is_power_of_2(n->zone_size) ? nvme_ilog2(n->zone_size) :
                                                      0;
    n->meta_size = sizeof(NvmeZoneMeta) + n->zone_array_size +
                          nz * n->params.zd_extension_size;
    n->meta_size = ROUND_UP(n->meta_size, qemu_real_host_page_size);

    if (!n->params.zasl_kb) {
        n->zasl_bs = n->params.mdts ? 0 : NVME_DEFAULT_MAX_ZA_SIZE * KiB;
    } else {
        n->zasl_bs = n->params.zasl_kb * KiB;
    }

    /* Make sure that the values of all Zoned Command Set properties are sane */
    if (n->params.max_open_zones > nz) {
        warn_report("max_open_zones value %u exceeds the number of zones %u,"
                    " adjusting", n->params.max_open_zones, nz);
        n->params.max_open_zones = nz;
    }
    if (n->params.max_active_zones > nz) {
        warn_report("max_active_zones value %u exceeds the number of zones %u,"
                    " adjusting", n->params.max_active_zones, nz);
        n->params.max_active_zones = nz;
    }
    if (n->params.max_open_zones < nz) {
        if (n->params.nr_offline_zones > nz - n->params.max_open_zones) {
            n->params.nr_offline_zones = nz - n->params.max_open_zones;
        }
        if (n->params.nr_rdonly_zones >
            nz - n->params.max_open_zones - n->params.nr_offline_zones) {
            n->params.nr_rdonly_zones =
                nz - n->params.max_open_zones - n->params.nr_offline_zones;
        }
    }
    if (n->params.zd_extension_size) {
        if (n->params.zd_extension_size & 0x3f) {
            error_setg(errp,
                "zone descriptor extension size must be a multiple of 64B");
            return;
        }
        if ((n->params.zd_extension_size >> 6) > 0xff) {
            error_setg(errp, "zone descriptor extension size is too large");
            return;
        }
    }

    if (n->params.zone_file) {
        if (nvme_open_zone_file(n, init_meta) < 0) {
            error_setg(errp, "cannot open zone metadata file");
            return;
        }
    }

    return;
}

static int nvme_zoned_init_ns(NvmeCtrl *n, NvmeNamespace *ns, int lba_index,
                              bool init_meta, Error **errp)
{
    int ret;

    ret = nvme_load_zone_meta(n, ns, n->num_zones * n->zone_size,
                              init_meta);
    if (ret) {
        error_setg(errp, "could not load/init zone metadata");
        return -1;
    }

    ns->id_ns_zoned = g_malloc0(sizeof(*ns->id_ns_zoned));

    /* MAR/MOR are zeroes-based, 0xffffffff means no limit */
    ns->id_ns_zoned->mar = cpu_to_le32(n->params.max_active_zones - 1);
    ns->id_ns_zoned->mor = cpu_to_le32(n->params.max_open_zones - 1);

    ns->id_ns_zoned->rrl = cpu_to_le32(n->params.rrl);
    ns->id_ns_zoned->frl = cpu_to_le32(n->params.frl);
    if (n->params.rrl || n->params.frl) {
        n->rzr_delay_ns = n->params.rzr_delay * NANOSECONDS_PER_SECOND;
        n->rrl_ns = n->params.rrl * NANOSECONDS_PER_SECOND;
        n->fzr_delay_ns = n->params.fzr_delay * NANOSECONDS_PER_SECOND;
        n->frl_ns = n->params.frl * NANOSECONDS_PER_SECOND;
    }

    ns->id_ns_zoned->zoc = cpu_to_le16(n->params.active_excursions ? 0x2 : 0);
    ns->id_ns_zoned->ozcs = n->params.cross_zone_read ? 0x01 : 0x00;

    ns->id_ns_zoned->lbafe[lba_index].zsze = cpu_to_le64(n->zone_size);
    ns->id_ns_zoned->lbafe[lba_index].zdes =
        n->params.zd_extension_size >> 6; /* Units of 64B */

    if (n->params.fill_pattern == 0) {
        ns->id_ns.dlfeat = 0x01;
    } else if (n->params.fill_pattern == 0xff) {
        ns->id_ns.dlfeat = 0x02;
    }

    return 0;
}

static void nvme_zoned_clear(NvmeCtrl *n)
{
    int i;

    if (n->params.zone_file)  {
        close(n->zone_file_fd);
    }
    for (i = 0; i < n->num_namespaces; i++) {
        NvmeNamespace *ns = &n->namespaces[i];
        g_free(ns->id_ns_zoned);
        if (!n->params.zone_file) {
            g_free(ns->zone_array);
            g_free(ns->exp_open_zones);
            g_free(ns->imp_open_zones);
            g_free(ns->closed_zones);
            g_free(ns->full_zones);
            g_free(ns->zd_extensions);
        }
    }
}

static void nvme_check_constraints(NvmeCtrl *n, Error **errp)
{
    NvmeParams *params = &n->params;

    if (params->num_queues) {
        warn_report("num_queues is deprecated; please use max_ioqpairs "
                    "instead");

        params->max_ioqpairs = params->num_queues - 1;
    }

    if (params->max_ioqpairs < 1 ||
        params->max_ioqpairs > NVME_MAX_IOQPAIRS) {
        error_setg(errp, "max_ioqpairs must be between 1 and %d",
                   NVME_MAX_IOQPAIRS);
        return;
    }

    if (params->msix_qsize < 1 ||
        params->msix_qsize > PCI_MSIX_FLAGS_QSIZE + 1) {
        error_setg(errp, "msix_qsize must be between 1 and %d",
                   PCI_MSIX_FLAGS_QSIZE + 1);
        return;
    }

    if (!n->conf.blk) {
        error_setg(errp, "drive property not set");
        return;
    }

    if (!params->serial) {
        error_setg(errp, "serial property not set");
        return;
    }

    if (!n->params.cmb_size_mb && n->pmrdev) {
        if (host_memory_backend_is_mapped(n->pmrdev)) {
            error_setg(errp, "can't use already busy memdev: %s",
                       object_get_canonical_path_component(OBJECT(n->pmrdev)));
            return;
        }

        if (!is_power_of_2(n->pmrdev->size)) {
            error_setg(errp, "pmr backend size needs to be power of 2 in size");
            return;
        }

        host_memory_backend_set_mapped(n->pmrdev, true);
    }
}

static void nvme_init_state(NvmeCtrl *n)
{
    n->num_namespaces = 1;
    /* add one to max_ioqpairs to account for the admin queue pair */
    n->reg_size = pow2ceil(sizeof(NvmeBar) +
                           2 * (n->params.max_ioqpairs + 1) * NVME_DB_SIZE);
    n->namespaces = g_new0(NvmeNamespace, n->num_namespaces);
    n->sq = g_new0(NvmeSQueue *, n->params.max_ioqpairs + 1);
    n->cq = g_new0(NvmeCQueue *, n->params.max_ioqpairs + 1);
    n->temperature = NVME_TEMPERATURE;
    n->features.temp_thresh_hi = NVME_TEMPERATURE_WARNING;
    n->starttime_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    n->aer_reqs = g_new0(NvmeRequest *, n->params.aerl + 1);
}

static void nvme_init_blk(NvmeCtrl *n, Error **errp)
{
    int64_t bs_size;

    if (!blkconf_blocksizes(&n->conf, errp)) {
        return;
    }
    blkconf_apply_backend_options(&n->conf, blk_is_read_only(n->conf.blk),
                                  false, errp);

    bs_size = blk_getlength(n->conf.blk);
    if (bs_size < 0) {
        error_setg_errno(errp, -bs_size, "could not get backing file size");
        return;
    }

    n->ns_size = bs_size;
}

static void nvme_init_namespace(NvmeCtrl *n, NvmeNamespace *ns, bool init_meta,
                                Error **errp)
{
    NvmeIdNs *id_ns = &ns->id_ns;
    int lba_index;

    ns->csi = NVME_CSI_NVM;
    qemu_uuid_generate(&ns->uuid); /* TODO make UUIDs persistent */
    lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    id_ns->lbaf[lba_index].ds = nvme_ilog2(n->conf.logical_block_size);
    id_ns->nsze = cpu_to_le64(nvme_ns_nlbas(n, ns));

    if (n->params.zoned) {
        ns->csi = NVME_CSI_ZONED;
        id_ns->ncap = cpu_to_le64(n->zone_capacity * n->num_zones);
        if (nvme_zoned_init_ns(n, ns, lba_index, init_meta, errp) != 0) {
            return;
        }
    } else {
        ns->csi = NVME_CSI_NVM;
        id_ns->ncap = id_ns->nsze;
    }

    /* no thin provisioning */
    id_ns->nuse = id_ns->ncap;
}

static void nvme_init_cmb(NvmeCtrl *n, PCIDevice *pci_dev)
{
    NVME_CMBLOC_SET_BIR(n->bar.cmbloc, NVME_CMB_BIR);
    NVME_CMBLOC_SET_OFST(n->bar.cmbloc, 0);

    NVME_CMBSZ_SET_SQS(n->bar.cmbsz, 1);
    NVME_CMBSZ_SET_CQS(n->bar.cmbsz, 0);
    NVME_CMBSZ_SET_LISTS(n->bar.cmbsz, 1);
    NVME_CMBSZ_SET_RDS(n->bar.cmbsz, 1);
    NVME_CMBSZ_SET_WDS(n->bar.cmbsz, 1);
    NVME_CMBSZ_SET_SZU(n->bar.cmbsz, 2); /* MBs */
    NVME_CMBSZ_SET_SZ(n->bar.cmbsz, n->params.cmb_size_mb);

    n->cmbuf = g_malloc0(NVME_CMBSZ_GETSIZE(n->bar.cmbsz));
    memory_region_init_io(&n->ctrl_mem, OBJECT(n), &nvme_cmb_ops, n,
                          "nvme-cmb", NVME_CMBSZ_GETSIZE(n->bar.cmbsz));
    pci_register_bar(pci_dev, NVME_CMBLOC_BIR(n->bar.cmbloc),
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &n->ctrl_mem);
}

static void nvme_init_pmr(NvmeCtrl *n, PCIDevice *pci_dev)
{
    /* Controller Capabilities register */
    NVME_CAP_SET_PMRS(n->bar.cap, 1);

    /* PMR Capabities register */
    n->bar.pmrcap = 0;
    NVME_PMRCAP_SET_RDS(n->bar.pmrcap, 0);
    NVME_PMRCAP_SET_WDS(n->bar.pmrcap, 0);
    NVME_PMRCAP_SET_BIR(n->bar.pmrcap, NVME_PMR_BIR);
    NVME_PMRCAP_SET_PMRTU(n->bar.pmrcap, 0);
    /* Turn on bit 1 support */
    NVME_PMRCAP_SET_PMRWBM(n->bar.pmrcap, 0x02);
    NVME_PMRCAP_SET_PMRTO(n->bar.pmrcap, 0);
    NVME_PMRCAP_SET_CMSS(n->bar.pmrcap, 0);

    /* PMR Control register */
    n->bar.pmrctl = 0;
    NVME_PMRCTL_SET_EN(n->bar.pmrctl, 0);

    /* PMR Status register */
    n->bar.pmrsts = 0;
    NVME_PMRSTS_SET_ERR(n->bar.pmrsts, 0);
    NVME_PMRSTS_SET_NRDY(n->bar.pmrsts, 0);
    NVME_PMRSTS_SET_HSTS(n->bar.pmrsts, 0);
    NVME_PMRSTS_SET_CBAI(n->bar.pmrsts, 0);

    /* PMR Elasticity Buffer Size register */
    n->bar.pmrebs = 0;
    NVME_PMREBS_SET_PMRSZU(n->bar.pmrebs, 0);
    NVME_PMREBS_SET_RBB(n->bar.pmrebs, 0);
    NVME_PMREBS_SET_PMRWBZ(n->bar.pmrebs, 0);

    /* PMR Sustained Write Throughput register */
    n->bar.pmrswtp = 0;
    NVME_PMRSWTP_SET_PMRSWTU(n->bar.pmrswtp, 0);
    NVME_PMRSWTP_SET_PMRSWTV(n->bar.pmrswtp, 0);

    /* PMR Memory Space Control register */
    n->bar.pmrmsc = 0;
    NVME_PMRMSC_SET_CMSE(n->bar.pmrmsc, 0);
    NVME_PMRMSC_SET_CBA(n->bar.pmrmsc, 0);

    pci_register_bar(pci_dev, NVME_PMRCAP_BIR(n->bar.pmrcap),
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &n->pmrdev->mr);
}

static void nvme_init_pci(NvmeCtrl *n, PCIDevice *pci_dev, Error **errp)
{
    uint8_t *pci_conf = pci_dev->config;

    pci_conf[PCI_INTERRUPT_PIN] = 1;
    pci_config_set_prog_interface(pci_conf, 0x2);
    pci_config_set_class(pci_conf, PCI_CLASS_STORAGE_EXPRESS);
    pcie_endpoint_cap_init(pci_dev, 0x80);

    memory_region_init_io(&n->iomem, OBJECT(n), &nvme_mmio_ops, n, "nvme",
                          n->reg_size);
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &n->iomem);
    if (msix_init_exclusive_bar(pci_dev, n->params.msix_qsize, 4, errp)) {
        return;
    }

    if (n->params.cmb_size_mb) {
        nvme_init_cmb(n, pci_dev);
    } else if (n->pmrdev) {
        nvme_init_pmr(n, pci_dev);
    }
}

static void nvme_init_ctrl(NvmeCtrl *n, PCIDevice *pci_dev)
{
    NvmeIdCtrl *id = &n->id_ctrl;
    uint8_t *pci_conf = pci_dev->config;
    char *subnqn;

    id->vid = cpu_to_le16(pci_get_word(pci_conf + PCI_VENDOR_ID));
    id->ssvid = cpu_to_le16(pci_get_word(pci_conf + PCI_SUBSYSTEM_VENDOR_ID));
    strpadcpy((char *)id->mn, sizeof(id->mn), "QEMU NVMe Ctrl", ' ');
    strpadcpy((char *)id->fr, sizeof(id->fr), "1.0", ' ');
    strpadcpy((char *)id->sn, sizeof(id->sn), n->params.serial, ' ');
    id->rab = 6;
    id->ieee[0] = 0x00;
    id->ieee[1] = 0x02;
    id->ieee[2] = 0xb3;
    id->mdts = n->params.mdts;
    id->ver = cpu_to_le32(NVME_SPEC_VER);
    id->oacs = cpu_to_le16(0);

    /*
     * Because the controller always completes the Abort command immediately,
     * there can never be more than one concurrently executing Abort command,
     * so this value is never used for anything. Note that there can easily be
     * many Abort commands in the queues, but they are not considered
     * "executing" until processed by nvme_abort.
     *
     * The specification recommends a value of 3 for Abort Command Limit (four
     * concurrently outstanding Abort commands), so lets use that though it is
     * inconsequential.
     */
    id->acl = 3;
    id->aerl = n->params.aerl;
    id->frmw = (NVME_NUM_FW_SLOTS << 1) | NVME_FRMW_SLOT1_RO;
    id->lpa = NVME_LPA_CSE | NVME_LPA_EXTENDED;

    /* recommended default value (~70 C) */
    id->wctemp = cpu_to_le16(NVME_TEMPERATURE_WARNING);
    id->cctemp = cpu_to_le16(NVME_TEMPERATURE_CRITICAL);

    id->sqes = (0x6 << 4) | 0x6;
    id->cqes = (0x4 << 4) | 0x4;
    id->nn = cpu_to_le32(n->num_namespaces);
    id->oncs = cpu_to_le16(NVME_ONCS_WRITE_ZEROES | NVME_ONCS_TIMESTAMP |
                           NVME_ONCS_FEATURES);

    subnqn = g_strdup_printf("nqn.2019-08.org.qemu:%s", n->params.serial);
    strpadcpy((char *)id->subnqn, sizeof(id->subnqn), subnqn, '\0');
    g_free(subnqn);

    id->psd[0].mp = cpu_to_le16(0x9c4);
    id->psd[0].enlat = cpu_to_le32(0x10);
    id->psd[0].exlat = cpu_to_le32(0x4);
    if (blk_enable_write_cache(n->conf.blk)) {
        id->vwc = 1;
    }

    n->bar.cap = 0;
    NVME_CAP_SET_MQES(n->bar.cap, 0x7ff);
    NVME_CAP_SET_CQR(n->bar.cap, 1);
    NVME_CAP_SET_TO(n->bar.cap, 0xf);
    /*
     * The driver now always supports NS Types, even when "zoned" property
     * is set to zero. If this is the case, all commands that support CSI field
     * only handle NVM Command Set.
     */
    NVME_CAP_SET_CSS(n->bar.cap, (CAP_CSS_NVM | CAP_CSS_CSI_SUPP));
    NVME_CAP_SET_MPSMAX(n->bar.cap, 4);

    n->bar.vs = NVME_SPEC_VER;
    n->bar.intmc = n->bar.intms = 0;
}

static void nvme_realize(PCIDevice *pci_dev, Error **errp)
{
    NvmeCtrl *n = NVME(pci_dev);
    NvmeNamespace *ns;
    Error *local_err = NULL;
    bool init_meta = false;

    int i;

    nvme_check_constraints(n, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    nvme_init_state(n);
    nvme_init_blk(n, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    nvme_init_pci(n, pci_dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    if (n->params.zoned) {
        nvme_zoned_init_ctrl(n, &init_meta, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }
    }
    nvme_init_ctrl(n, pci_dev);

    ns = n->namespaces;
    for (i = 0; i < n->num_namespaces; i++, ns++) {
        ns->nsid = i + 1;
        nvme_init_namespace(n, ns, init_meta, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }
    }
}

static void nvme_exit(PCIDevice *pci_dev)
{
    NvmeCtrl *n = NVME(pci_dev);

    nvme_clear_ctrl(n);
    if (n->params.zoned) {
        nvme_zoned_clear(n);
    }
    g_free(n->namespaces);
    g_free(n->cq);
    g_free(n->sq);
    g_free(n->aer_reqs);

    if (n->params.cmb_size_mb) {
        g_free(n->cmbuf);
    }

    if (n->pmrdev) {
        host_memory_backend_set_mapped(n->pmrdev, false);
    }
    msix_uninit_exclusive_bar(pci_dev);
}

static Property nvme_props[] = {
    DEFINE_BLOCK_PROPERTIES(NvmeCtrl, conf),
    DEFINE_PROP_LINK("pmrdev", NvmeCtrl, pmrdev, TYPE_MEMORY_BACKEND,
                     HostMemoryBackend *),
    DEFINE_PROP_STRING("serial", NvmeCtrl, params.serial),
    DEFINE_PROP_UINT32("cmb_size_mb", NvmeCtrl, params.cmb_size_mb, 0),
    DEFINE_PROP_UINT32("num_queues", NvmeCtrl, params.num_queues, 0),
    DEFINE_PROP_UINT32("max_ioqpairs", NvmeCtrl, params.max_ioqpairs, 64),
    DEFINE_PROP_UINT16("msix_qsize", NvmeCtrl, params.msix_qsize, 65),
    DEFINE_PROP_UINT8("aerl", NvmeCtrl, params.aerl, 3),
    DEFINE_PROP_UINT32("aer_max_queued", NvmeCtrl, params.aer_max_queued, 64),
    DEFINE_PROP_UINT8("mdts", NvmeCtrl, params.mdts, 7),
    DEFINE_PROP_BOOL("zoned", NvmeCtrl, params.zoned, false),
    DEFINE_PROP_UINT64("zone_size", NvmeCtrl, params.zone_size_mb,
                       NVME_DEFAULT_ZONE_SIZE),
    DEFINE_PROP_UINT64("zone_capacity", NvmeCtrl, params.zone_capacity_mb, 0),
    DEFINE_PROP_UINT32("zone_append_size_limit", NvmeCtrl, params.zasl_kb, 0),
    DEFINE_PROP_STRING("zone_file", NvmeCtrl, params.zone_file),
    DEFINE_PROP_UINT32("zone_descr_ext_size", NvmeCtrl,
                       params.zd_extension_size, 0),
    DEFINE_PROP_UINT32("max_active", NvmeCtrl, params.max_active_zones, 0),
    DEFINE_PROP_UINT32("max_open", NvmeCtrl, params.max_open_zones, 0),
    DEFINE_PROP_UINT32("offline_zones", NvmeCtrl, params.nr_offline_zones, 0),
    DEFINE_PROP_UINT32("rdonly_zones", NvmeCtrl, params.nr_rdonly_zones, 0),
    DEFINE_PROP_UINT32("reset_rcmnd_delay", NvmeCtrl, params.rzr_delay, 0),
    DEFINE_PROP_UINT32("reset_rcmnd_limit", NvmeCtrl, params.rrl, 0),
    DEFINE_PROP_UINT32("finish_rcmnd_delay", NvmeCtrl, params.fzr_delay, 0),
    DEFINE_PROP_UINT32("finish_rcmnd_limit", NvmeCtrl, params.frl, 0),
    DEFINE_PROP_BOOL("cross_zone_read", NvmeCtrl, params.cross_zone_read, true),
    DEFINE_PROP_BOOL("active_excursions", NvmeCtrl, params.active_excursions,
                     false),
    DEFINE_PROP_UINT8("fill_pattern", NvmeCtrl, params.fill_pattern, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription nvme_vmstate = {
    .name = "nvme",
    .unmigratable = 1,
};

static void nvme_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->realize = nvme_realize;
    pc->exit = nvme_exit;
    pc->class_id = PCI_CLASS_STORAGE_EXPRESS;
    pc->vendor_id = PCI_VENDOR_ID_INTEL;
    pc->device_id = 0x5845;
    pc->revision = 2;

    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    dc->desc = "Non-Volatile Memory Express";
    device_class_set_props(dc, nvme_props);
    dc->vmsd = &nvme_vmstate;
}

static void nvme_instance_init(Object *obj)
{
    NvmeCtrl *s = NVME(obj);

    device_add_bootindex_property(obj, &s->conf.bootindex,
                                  "bootindex", "/namespace@1,0",
                                  DEVICE(obj));
}

static const TypeInfo nvme_info = {
    .name          = TYPE_NVME,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(NvmeCtrl),
    .class_init    = nvme_class_init,
    .instance_init = nvme_instance_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { }
    },
};

static void nvme_register_types(void)
{
    type_register_static(&nvme_info);
}

type_init(nvme_register_types)
