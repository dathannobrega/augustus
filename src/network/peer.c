#include "peer.h"

#ifdef ENABLE_MULTIPLAYER

#include <string.h>

static uint32_t absolute_delta_u32(uint32_t a, uint32_t b)
{
    return (a > b) ? (a - b) : (b - a);
}

void net_peer_init(net_peer *peer)
{
    memset(peer, 0, sizeof(net_peer));
    peer->socket_fd = -1;
    peer->quality = PEER_QUALITY_UNKNOWN;
    net_codec_init(&peer->codec);
}

void net_peer_reset(net_peer *peer)
{
    int fd = peer->socket_fd;
    net_peer_init(peer);
    /* Socket should be closed by caller before reset */
    (void)fd;
}

void net_peer_set_connected(net_peer *peer, int socket_fd, const char *name)
{
    peer->active = 1;
    peer->socket_fd = socket_fd;
    peer->state = PEER_STATE_CONNECTING;
    if (name) {
        strncpy(peer->name, name, NET_MAX_PLAYER_NAME - 1);
        peer->name[NET_MAX_PLAYER_NAME - 1] = '\0';
    }
}

void net_peer_set_player_id(net_peer *peer, uint8_t player_id)
{
    peer->player_id = player_id;
}

void net_peer_update_heartbeat_sent(net_peer *peer, uint32_t timestamp_ms,
                                    uint32_t sample_id)
{
    peer->last_heartbeat_sent_ms = timestamp_ms;
    peer->last_heartbeat_sample_id = sample_id;
}

void net_peer_note_heartbeat_recv(net_peer *peer, uint32_t timestamp_ms)
{
    peer->last_heartbeat_recv_ms = timestamp_ms;
}

int net_peer_update_heartbeat_response(net_peer *peer, uint32_t timestamp_ms,
                                       uint32_t sample_id)
{
    uint32_t rtt;
    uint32_t jitter_sample;

    peer->last_heartbeat_recv_ms = timestamp_ms;

    if (peer->last_heartbeat_sample_id == 0 ||
        sample_id != peer->last_heartbeat_sample_id ||
        peer->last_heartbeat_sent_ms == 0 ||
        timestamp_ms < peer->last_heartbeat_sent_ms) {
        return 0;
    }

    rtt = timestamp_ms - peer->last_heartbeat_sent_ms;
    jitter_sample = absolute_delta_u32(rtt, peer->rtt_ms);

    if (peer->rtt_smoothed_ms == 0) {
        peer->rtt_smoothed_ms = rtt;
    } else {
        /* Exponential moving average: 7/8 old + 1/8 new */
        peer->rtt_smoothed_ms = (peer->rtt_smoothed_ms * 7 + rtt) / 8;
    }
    if (peer->rtt_jitter_ms == 0) {
        peer->rtt_jitter_ms = jitter_sample;
    } else {
        peer->rtt_jitter_ms = (peer->rtt_jitter_ms * 3 + jitter_sample) / 4;
    }
    peer->rtt_ms = rtt;
    return 1;
}

void net_peer_update_quality(net_peer *peer, uint32_t current_ms)
{
    uint32_t heartbeat_gap = 0;

    if (peer->last_heartbeat_recv_ms > 0 && current_ms >= peer->last_heartbeat_recv_ms) {
        heartbeat_gap = current_ms - peer->last_heartbeat_recv_ms;
    }

    if (heartbeat_gap > NET_TIMEOUT_MS) {
        peer->quality = PEER_QUALITY_CRITICAL;
    } else if (peer->rtt_smoothed_ms == 0) {
        peer->quality = PEER_QUALITY_UNKNOWN;
    } else if (peer->rtt_smoothed_ms < 80 &&
               peer->rtt_jitter_ms < 25 &&
               heartbeat_gap <= NET_HEARTBEAT_INTERVAL_MS * 2) {
        peer->quality = PEER_QUALITY_GOOD;
    } else if (peer->rtt_smoothed_ms < 180 &&
               peer->rtt_jitter_ms < 80 &&
               heartbeat_gap <= NET_HEARTBEAT_INTERVAL_MS * 3) {
        peer->quality = PEER_QUALITY_DEGRADED;
    } else if (peer->rtt_smoothed_ms < 450 &&
               heartbeat_gap <= NET_TIMEOUT_MS) {
        peer->quality = PEER_QUALITY_POOR;
    } else {
        peer->quality = PEER_QUALITY_CRITICAL;
    }
}

int net_peer_is_timed_out(const net_peer *peer, uint32_t current_ms)
{
    if (!peer->active || peer->state == PEER_STATE_DISCONNECTED) {
        return 0;
    }
    if (peer->last_heartbeat_recv_ms == 0) {
        /* Never received a heartbeat - use connection time approximation */
        return 0;
    }
    return (current_ms - peer->last_heartbeat_recv_ms) > NET_TIMEOUT_MS;
}

static const char *PEER_STATE_NAMES[] = {
    "DISCONNECTED",
    "CONNECTING",
    "HELLO_SENT",
    "JOINED",
    "READY",
    "LOADING",
    "IN_GAME",
    "DESYNCED",
    "DISCONNECTING"
};

const char *net_peer_state_name(net_peer_state state)
{
    if (state < 0 || state > PEER_STATE_DISCONNECTING) {
        return "UNKNOWN";
    }
    return PEER_STATE_NAMES[state];
}

static const char *PEER_QUALITY_NAMES[] = {
    "UNKNOWN",
    "GOOD",
    "DEGRADED",
    "POOR",
    "CRITICAL"
};

const char *net_peer_quality_name(net_peer_quality quality)
{
    if (quality < 0 || quality > PEER_QUALITY_CRITICAL) {
        return "UNKNOWN";
    }
    return PEER_QUALITY_NAMES[quality];
}

#endif /* ENABLE_MULTIPLAYER */
