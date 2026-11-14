/* SPDX-License-Identifier: BSD-3-Clause
 * Real PCI transport peer: exact wire tokens, no producer semantic emulation.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <simbricks/pcie/if.h>

struct operation { uint64_t id, addr; unsigned len, read, seed; };
static const struct operation ops[] = {
    {0xfedcba9876543210ULL, 0x38, 160, 0, 0xa5},
    {0x123456789abcdef0ULL, 0x438, 96, 0, 0x5a},
    {0x9988776655443322ULL, 0x838, 80, 1, 0},
    {0xfedcba9876543210ULL, 0x38, 160, 1, 0xa5},
    {0xfedcba9876543210ULL, 0x38, 160, 0, 0x3c},
    {0xfedcba9876543210ULL, 0x38, 160, 1, 0x3c},
};
static void issue(struct SimbricksPcieIf *pci, FILE *log, unsigned op, uint64_t now)
{
    const struct operation *o = &ops[op];
    volatile union SimbricksProtoPcieD2H *m = SimbricksPcieIfD2HOutAlloc(pci, now);
    assert(m);
    if (o->read) {
        m->read.req_id = o->id; m->read.offset = o->addr; m->read.len = o->len;
    } else {
        m->write.req_id = o->id; m->write.offset = o->addr; m->write.len = o->len;
        memset((void *)m->write.data, o->seed, o->len);
    }
    SimbricksPcieIfD2HOutSend(pci, m, o->read ?
        SIMBRICKS_PROTO_PCIE_D2H_MSG_READ : SIMBRICKS_PROTO_PCIE_D2H_MSG_WRITE);
    fprintf(log, "{\"event\":\"issue\",\"test_operation\":%u,\"wire_req_id\":%llu,"
        "\"wire_address\":%llu,\"length_bytes\":%u,\"direction\":\"%s\",\"time_ps\":%llu}\n",
        op, (unsigned long long)o->id, (unsigned long long)o->addr, o->len,
        o->read ? "read" : "write", (unsigned long long)now);
    assert(!fflush(log));
}
int main(int argc, char **argv)
{
    assert(argc == 4 || argc == 5);
    FILE *log = fopen(argv[3], "wx"); assert(log);
    struct SimbricksBaseIfParams params;
    SimbricksPcieIfDefaultParams(&params);
    params.sock_path = argv[1];
    params.link_latency = params.sync_interval = 1000;
    params.sync_mode = kSimbricksBaseIfSyncRequired;
    struct SimbricksPcieIf pci;
    struct SimbricksBaseIfSHMPool pool;
    assert(!SimbricksBaseIfSHMPoolCreate(&pool, argv[2], SimbricksBaseIfSHMSize(&params)));
    assert(!SimbricksBaseIfInit(&pci.base, &params));
    assert(!SimbricksBaseIfListen(&pci.base, &pool));
    struct SimbricksProtoPcieDevIntro tx = {0};
    struct SimbricksProtoPcieHostIntro rx;
    tx.pci_vendor_id = 0x8086; tx.pci_device_id = 0x1572;
    struct SimBricksBaseIfEstablishData est = {
        &pci.base, &tx, sizeof(tx), &rx, sizeof(rx),
    };
    assert(!SimBricksBaseIfEstablish(&est, 1));
    assert(SimbricksBaseIfSyncEnabled(&pci.base));
    unsigned completed = 0, next = 3;
    unsigned char done[sizeof(ops) / sizeof(ops[0])] = {0};
    for (unsigned i = 0; i < 3; ++i) issue(&pci, log, i, 0);
    if (argc == 5) {
        assert(!strcmp(argv[4], "--duplicate-active"));
        issue(&pci, log, 0, 0);
    }
    uint64_t now = 0;
    for (;;) {
        while (SimbricksPcieIfD2HOutSync(&pci, now)) {}
        volatile union SimbricksProtoPcieH2D *m;
        do { m = SimbricksPcieIfH2DInPoll(&pci, UINT64_MAX); } while (!m);
        uint8_t type = SimbricksPcieIfH2DInType(&pci, m);
        if (type == SIMBRICKS_PROTO_MSG_TYPE_TERMINATE) {
            // Transport close uses timestamp zero; it is not a timed DMA event.
            assert(completed == sizeof(ops) / sizeof(ops[0]));
            SimbricksPcieIfH2DInDone(&pci, m);
            assert(!fclose(log));
            return 0;
        }
        assert(m->base.header.timestamp >= now);
        now = m->base.header.timestamp;
        if (type == SIMBRICKS_PROTO_PCIE_H2D_MSG_READCOMP ||
            type == SIMBRICKS_PROTO_PCIE_H2D_MSG_WRITECOMP) {
            uint64_t id = type == SIMBRICKS_PROTO_PCIE_H2D_MSG_READCOMP ?
                m->readcomp.req_id : m->writecomp.req_id;
            unsigned op;
            for (op = 0; op < next; ++op) if (!done[op] && ops[op].id == id) break;
            assert(op < next);
            assert(ops[op].read == (type == SIMBRICKS_PROTO_PCIE_H2D_MSG_READCOMP));
            if (ops[op].read) for (unsigned j = 0; j < ops[op].len; ++j)
                assert(m->readcomp.data[j] == ops[op].seed);
            done[op] = 1; ++completed;
            fprintf(log, "{\"event\":\"complete\",\"test_operation\":%u,"
                "\"wire_req_id\":%llu,\"time_ps\":%llu}\n", op,
                (unsigned long long)id, (unsigned long long)now);
            assert(!fflush(log));
            if (completed >= 3 && next < sizeof(ops) / sizeof(ops[0]))
                issue(&pci, log, next++, now);
            if (completed == sizeof(ops) / sizeof(ops[0])) {
                puts("DMA_WIRE_PEER_OK operations=6 readback_bytes=400"); fflush(stdout);
            }
        } else assert(type == SIMBRICKS_PROTO_MSG_TYPE_SYNC);
        SimbricksPcieIfH2DInDone(&pci, m);
    }
}
