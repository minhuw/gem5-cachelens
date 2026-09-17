/* Directed two-frame injection, not a network/NIC model. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <simbricks/network/if.h>

int main(int argc, char **argv)
{
    assert(argc == 4);
    uint8_t frame[1472];
    FILE *input = fopen(argv[3], "rb");
    assert(input && fread(frame, sizeof(frame), 1, input) == 1);
    fclose(input);
    struct SimbricksBaseIfParams params;
    SimbricksNetIfDefaultParams(&params);
    params.sock_path = argv[1];
    params.link_latency = params.sync_interval = 500000; /* ps */
    params.sync_mode = kSimbricksBaseIfSyncRequired;
    struct SimbricksNetIf net;
    struct SimbricksBaseIfSHMPool pool;
    assert(!SimbricksBaseIfSHMPoolCreate(&pool, argv[2], SimbricksBaseIfSHMSize(&params)));
    assert(!SimbricksBaseIfInit(&net.base, &params));
    assert(!SimbricksBaseIfListen(&net.base, &pool));
    struct SimbricksProtoNetIntro tx = {0}, rx;
    struct SimBricksBaseIfEstablishData est = {
        .base_if = &net.base, .tx_intro = &tx, .tx_intro_len = sizeof(tx),
        .rx_intro = &rx, .rx_intro_len = sizeof(rx),
    };
    assert(!SimBricksBaseIfEstablish(&est, 1));
    assert(SimbricksBaseIfSyncEnabled(&net.base));
    /* Same-tick burst: the real EtherLink must reject the second frame. */
    for (unsigned i = 0; i < 2; ++i) {
        volatile union SimbricksProtoNetMsg *msg = SimbricksNetIfOutAlloc(&net, 0);
        assert(msg);
        msg->packet.len = sizeof(frame);
        msg->packet.port = 0;
        memcpy((void *)msg->packet.data, frame, sizeof(frame));
        SimbricksNetIfOutSend(&net, msg, SIMBRICKS_PROTO_NET_MSG_PACKET);
    }
    /* No more injection before 10ms. Publish that bound and consume gem5's
     * messages until the test runner terminates us after gem5 exits. */
    assert(!SimbricksNetIfOutSync(&net, 10000000000ULL));
    unsigned packets = 0;
    for (;;) {
        volatile union SimbricksProtoNetMsg *msg = SimbricksNetIfInPoll(&net, UINT64_MAX);
        if (!msg) continue;
        if (SimbricksNetIfInType(&net, msg) == SIMBRICKS_PROTO_NET_MSG_PACKET) {
            assert(msg->packet.len == sizeof(frame));
            if (++packets == 64) {
                puts("ETHERNET_PEER_OK packets=64 bytes=94208");
                fflush(stdout);
            }
        }
        SimbricksNetIfInDone(&net, msg);
    }
}
