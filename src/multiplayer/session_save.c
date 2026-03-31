#include "session_save.h"

#ifdef ENABLE_MULTIPLAYER

#include "player_registry.h"
#include "ownership.h"
#include "empire_sync.h"
#include "trade_sync.h"
#include "mp_trade_route.h"
#include "time_sync.h"
#include "checksum.h"
#include "command_bus.h"
#include "worldgen.h"
#include "game_manifest.h"
#include "server_rules.h"
#include "dedicated_server.h"
#include "mp_debug_log.h"
#include "empire/city.h"
#include "network/session.h"
#include "network/serialize.h"
#include "network/protocol.h"
#include "core/log.h"

#include <string.h>
#include <stdlib.h>

#define DOMAIN_BUFFER_SIZE MP_SAVE_MAX_DOMAIN_SIZE
#define MP_SAVE_MAX_WIRE_DOMAINS 32

/* v2 header wire size = 58 bytes, v3 = 62 bytes, v4 = 73 bytes, v5 = 93 bytes,
 * v6/v7 = 97 bytes, v8 = 101 bytes */
#define HEADER_V2_SIZE 58
#define HEADER_V3_SIZE 62
#define HEADER_V4_SIZE 73
#define HEADER_V5_SIZE 93
#define HEADER_V6_SIZE 97
#define HEADER_V8_SIZE 101

/* FNV-1a 32-bit checksum */
#define FNV_OFFSET 2166136261u
#define FNV_PRIME  16777619u

static uint32_t compute_fnv1a(const uint8_t *data, uint32_t size)
{
    uint32_t hash = FNV_OFFSET;
    for (uint32_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

static uint32_t compute_payload_checksum(const uint8_t *data, uint32_t size)
{
    uint32_t hash = 0x12345678;
    for (uint32_t i = 0; i < size; i++) {
        hash = ((hash << 5) | (hash >> 27)) ^ data[i];
    }
    return hash;
}

static uint32_t header_wire_size_for_version(uint32_t version)
{
    if (version >= 8) {
        return HEADER_V8_SIZE;
    }
    if (version >= 6) {
        return HEADER_V6_SIZE;
    }
    if (version >= 5) {
        return HEADER_V5_SIZE;
    }
    if (version >= 4) {
        return HEADER_V4_SIZE;
    }
    if (version >= 3) {
        return HEADER_V3_SIZE;
    }
    return HEADER_V2_SIZE;
}

static uint32_t *header_domain_size_slot(mp_save_header *header, uint8_t tag)
{
    if (!header) {
        return 0;
    }

    switch (tag) {
        case MP_DOMAIN_TAG_PLAYER_REGISTRY:
            return &header->player_registry_size;
        case MP_DOMAIN_TAG_OWNERSHIP:
            return &header->ownership_size;
        case MP_DOMAIN_TAG_WORLDGEN:
            return &header->worldgen_size;
        case MP_DOMAIN_TAG_EMPIRE_SYNC:
            return &header->empire_sync_size;
        case MP_DOMAIN_TAG_TRADE_SYNC_ROUTES:
            return &header->trade_sync_routes_size;
        case MP_DOMAIN_TAG_TRADE_SYNC_TRADERS:
            return &header->trade_sync_traders_size;
        case MP_DOMAIN_TAG_P2P_ROUTES:
            return &header->p2p_routes_size;
        case MP_DOMAIN_TAG_TIME_SYNC:
            return &header->time_sync_size;
        case MP_DOMAIN_TAG_SERVER_RULES:
            return &header->server_rules_size;
        default:
            return 0;
    }
}

static int deserialize_domain_by_tag(uint8_t tag, const uint8_t *data, uint32_t size)
{
    switch (tag) {
        case MP_DOMAIN_TAG_PLAYER_REGISTRY:
            mp_player_registry_deserialize(data, size);
            return 1;
        case MP_DOMAIN_TAG_OWNERSHIP:
            mp_ownership_deserialize(data, size);
            return 1;
        case MP_DOMAIN_TAG_WORLDGEN:
            mp_worldgen_deserialize(data, size);
            return 1;
        case MP_DOMAIN_TAG_EMPIRE_SYNC:
            mp_empire_sync_deserialize(data, size);
            return 1;
        case MP_DOMAIN_TAG_TRADE_SYNC_ROUTES:
            mp_trade_sync_deserialize_routes(data, size);
            return 1;
        case MP_DOMAIN_TAG_TRADE_SYNC_TRADERS:
            mp_trade_sync_deserialize_traders(data, size);
            return 1;
        case MP_DOMAIN_TAG_P2P_ROUTES:
            mp_trade_route_deserialize(data, size);
            return 1;
        case MP_DOMAIN_TAG_TIME_SYNC:
            mp_time_sync_deserialize(data, size);
            return 1;
        case MP_DOMAIN_TAG_SERVER_RULES:
            return mp_server_rules_deserialize(data, size);
        default:
            MP_LOG_WARN("SESSION_SAVE", "Skipping unknown save domain tag=0x%02x size=%u",
                        (unsigned int)tag, (unsigned int)size);
            return 1;
    }
}

static uint32_t required_domain_mask_for_version(uint32_t version)
{
    if (version >= 8) {
        return (1u << MP_DOMAIN_TAG_PLAYER_REGISTRY) |
               (1u << MP_DOMAIN_TAG_OWNERSHIP) |
               (1u << MP_DOMAIN_TAG_WORLDGEN) |
               (1u << MP_DOMAIN_TAG_EMPIRE_SYNC) |
               (1u << MP_DOMAIN_TAG_TRADE_SYNC_ROUTES) |
               (1u << MP_DOMAIN_TAG_TRADE_SYNC_TRADERS) |
               (1u << MP_DOMAIN_TAG_P2P_ROUTES) |
               (1u << MP_DOMAIN_TAG_TIME_SYNC) |
               (1u << MP_DOMAIN_TAG_SERVER_RULES);
    }

    return (1u << MP_DOMAIN_TAG_PLAYER_REGISTRY) |
           (1u << MP_DOMAIN_TAG_OWNERSHIP) |
           (1u << MP_DOMAIN_TAG_WORLDGEN) |
           (1u << MP_DOMAIN_TAG_EMPIRE_SYNC) |
           (1u << MP_DOMAIN_TAG_TRADE_SYNC_ROUTES) |
           (1u << MP_DOMAIN_TAG_TRADE_SYNC_TRADERS) |
           (1u << MP_DOMAIN_TAG_P2P_ROUTES) |
           (1u << MP_DOMAIN_TAG_TIME_SYNC);
}

int mp_session_save_to_buffer(uint8_t *buffer, uint32_t buffer_size, uint32_t *out_size)
{
    mp_save_header header;
    uint8_t *temp;
    uint8_t *domain_bufs[MP_SAVE_DOMAIN_COUNT];
    uint32_t domain_sizes[MP_SAVE_DOMAIN_COUNT] = {0};
    mp_domain_entry domain_table[MP_SAVE_DOMAIN_COUNT];
    const uint8_t domain_tags[MP_SAVE_DOMAIN_COUNT] = {
        MP_DOMAIN_TAG_PLAYER_REGISTRY,
        MP_DOMAIN_TAG_OWNERSHIP,
        MP_DOMAIN_TAG_WORLDGEN,
        MP_DOMAIN_TAG_EMPIRE_SYNC,
        MP_DOMAIN_TAG_TRADE_SYNC_ROUTES,
        MP_DOMAIN_TAG_TRADE_SYNC_TRADERS,
        MP_DOMAIN_TAG_P2P_ROUTES,
        MP_DOMAIN_TAG_TIME_SYNC,
        MP_DOMAIN_TAG_SERVER_RULES
    };
    net_serializer s;
    uint32_t domain_table_wire_size;
    uint32_t total;

    if (!net_session_is_host()) {
        log_error("Only host can save multiplayer session", 0, 0);
        return 0;
    }

    memset(&header, 0, sizeof(header));
    header.magic = MP_SAVE_MAGIC;
    header.version = MP_SAVE_VERSION;
    header.protocol_version = NET_PROTOCOL_VERSION;
    header.session_id = net_session_get()->session_id;
    header.session_seed = mp_worldgen_get_spawn_table()->session_seed;
    header.host_player_id = net_session_get_local_player_id();
    header.player_count = (uint8_t)mp_player_registry_get_count();
    header.snapshot_tick = net_session_get_authoritative_tick();
    header.checksum = mp_checksum_compute();
    header.domain_count = MP_SAVE_DOMAIN_COUNT;
    header.compat_flags = MP_SAVE_FLAG_HAS_P2P_ROUTES | MP_SAVE_FLAG_HAS_TRADE_SYNC;

    temp = (uint8_t *)malloc(DOMAIN_BUFFER_SIZE * MP_SAVE_DOMAIN_COUNT);
    if (!temp) {
        log_error("Failed to allocate save buffer", 0, 0);
        return 0;
    }

    for (int i = 0; i < MP_SAVE_DOMAIN_COUNT; i++) {
        domain_bufs[i] = temp + (DOMAIN_BUFFER_SIZE * i);
    }

    mp_player_registry_serialize(domain_bufs[0], &domain_sizes[0]);
    mp_ownership_serialize(domain_bufs[1], &domain_sizes[1]);
    mp_worldgen_serialize(domain_bufs[2], &domain_sizes[2]);
    mp_empire_sync_serialize(domain_bufs[3], &domain_sizes[3]);
    mp_trade_sync_serialize_routes(domain_bufs[4], &domain_sizes[4]);
    mp_trade_sync_serialize_traders(domain_bufs[5], &domain_sizes[5]);
    mp_trade_route_serialize(domain_bufs[6], &domain_sizes[6], DOMAIN_BUFFER_SIZE);
    mp_time_sync_serialize(domain_bufs[7], &domain_sizes[7]);
    mp_server_rules_serialize(domain_bufs[8], DOMAIN_BUFFER_SIZE, &domain_sizes[8]);

    for (int i = 0; i < MP_SAVE_DOMAIN_COUNT; i++) {
        if (domain_sizes[i] > DOMAIN_BUFFER_SIZE) {
            log_error("Save domain exceeds max size", 0, (int)domain_sizes[i]);
            free(temp);
            return 0;
        }
        domain_table[i].tag = domain_tags[i];
        domain_table[i].size = domain_sizes[i];
        domain_table[i].checksum = compute_fnv1a(domain_bufs[i], domain_sizes[i]);
        header.total_payload_size += domain_sizes[i];
    }

    header.player_registry_size = domain_sizes[0];
    header.ownership_size = domain_sizes[1];
    header.worldgen_size = domain_sizes[2];
    header.empire_sync_size = domain_sizes[3];
    header.trade_sync_routes_size = domain_sizes[4];
    header.trade_sync_traders_size = domain_sizes[5];
    header.p2p_routes_size = domain_sizes[6];
    header.time_sync_size = domain_sizes[7];
    header.server_rules_size = domain_sizes[8];
    header.next_command_sequence_id = mp_command_bus_get_next_sequence_id();

    domain_table_wire_size = (uint32_t)(header.domain_count * 9u);
    total = HEADER_V8_SIZE + domain_table_wire_size + header.total_payload_size;

    if (total > buffer_size) {
        log_error("Save buffer too small", 0, (int)total);
        free(temp);
        return 0;
    }

    {
        uint32_t running = 0x12345678;
        for (int d = 0; d < MP_SAVE_DOMAIN_COUNT; d++) {
            for (uint32_t i = 0; i < domain_sizes[d]; i++) {
                running = ((running << 5) | (running >> 27)) ^ domain_bufs[d][i];
            }
        }
        header.payload_checksum = running;
    }

    {
        const mp_game_manifest *manifest = mp_game_manifest_get();
        if (manifest && manifest->valid) {
            memcpy(header.world_instance_uuid, manifest->world_instance_uuid,
                   MP_WORLD_UUID_SIZE);
        }
    }

    net_serializer_init(&s, buffer, buffer_size);
    net_write_u32(&s, header.magic);
    net_write_u32(&s, header.version);
    net_write_u32(&s, header.protocol_version);
    net_write_u32(&s, header.session_id);
    net_write_u32(&s, header.session_seed);
    net_write_u8(&s, header.host_player_id);
    net_write_u8(&s, header.player_count);
    net_write_u32(&s, header.snapshot_tick);
    net_write_u32(&s, header.checksum);
    net_write_u32(&s, header.player_registry_size);
    net_write_u32(&s, header.ownership_size);
    net_write_u32(&s, header.worldgen_size);
    net_write_u32(&s, header.empire_sync_size);
    net_write_u32(&s, header.trade_sync_routes_size);
    net_write_u32(&s, header.trade_sync_traders_size);
    net_write_u32(&s, header.p2p_routes_size);
    net_write_u32(&s, header.time_sync_size);
    net_write_u32(&s, header.server_rules_size);
    net_write_u32(&s, header.next_command_sequence_id);
    net_write_u32(&s, header.total_payload_size);
    net_write_u8(&s, header.domain_count);
    net_write_u16(&s, header.compat_flags);
    net_write_u32(&s, header.payload_checksum);
    net_write_raw(&s, header.world_instance_uuid, MP_WORLD_UUID_SIZE);
    header.header_checksum = compute_fnv1a(buffer, (uint32_t)net_serializer_position(&s));
    net_write_u32(&s, header.header_checksum);

    for (int d = 0; d < MP_SAVE_DOMAIN_COUNT; d++) {
        net_write_u8(&s, domain_table[d].tag);
        net_write_u32(&s, domain_table[d].size);
        net_write_u32(&s, domain_table[d].checksum);
    }

    for (int d = 0; d < MP_SAVE_DOMAIN_COUNT; d++) {
        net_write_raw(&s, domain_bufs[d], domain_sizes[d]);
    }

    if (net_serializer_has_overflow(&s)) {
        log_error("Save serialization overflow", 0, (int)net_serializer_position(&s));
        free(temp);
        return 0;
    }

    *out_size = (uint32_t)net_serializer_position(&s);
    free(temp);

    MP_LOG_INFO("SESSION_SAVE", "Multiplayer session saved: %u bytes, tick=%u, seq=%u",
                *out_size, header.snapshot_tick, header.next_command_sequence_id);
    return 1;
}

int mp_session_load_from_buffer(const uint8_t *buffer, uint32_t size)
{
    mp_save_header header;
    uint32_t header_wire_size;
    uint32_t domain_table_size = 0;
    uint32_t expected_total = 0;
    uint32_t required_mask;
    uint32_t seen_mask = 0;
    mp_domain_entry *domain_table = 0;

    if (!mp_session_save_read_header(buffer, size, &header)) {
        log_error("Failed to read multiplayer save header", 0, 0);
        return 0;
    }

    if (header.version < MP_SAVE_VERSION) {
        log_error("Multiplayer save version too old for this build", 0, (int)header.version);
        return 0;
    }

    header_wire_size = header_wire_size_for_version(header.version);

    if (header.version >= 5) {
        net_serializer dt;

        if (header.domain_count == 0 || header.domain_count > MP_SAVE_MAX_WIRE_DOMAINS) {
            log_error("Invalid multiplayer domain count", 0, (int)header.domain_count);
            return 0;
        }

        if (header.version >= 5 && header.header_checksum != 0) {
            uint32_t actual_hdr_cksum = compute_fnv1a(buffer, header_wire_size - 4);
            if (actual_hdr_cksum != header.header_checksum) {
                log_error("Multiplayer save: header checksum mismatch", 0,
                          (int)actual_hdr_cksum);
                return 0;
            }
        }

        domain_table_size = (uint32_t)(header.domain_count * 9u);
        if (header_wire_size + domain_table_size > size) {
            log_error("Multiplayer save: buffer too small for domain table", 0,
                      (int)(header_wire_size + domain_table_size));
            return 0;
        }

        domain_table = (mp_domain_entry *)calloc(header.domain_count, sizeof(mp_domain_entry));
        if (!domain_table) {
            log_error("Failed to allocate domain table", 0, (int)header.domain_count);
            return 0;
        }

        net_serializer_init(&dt, (uint8_t *)buffer + header_wire_size, domain_table_size);
        for (uint32_t d = 0; d < header.domain_count; d++) {
            uint32_t *slot;

            domain_table[d].tag = net_read_u8(&dt);
            domain_table[d].size = net_read_u32(&dt);
            domain_table[d].checksum = net_read_u32(&dt);

            if (domain_table[d].size > MP_SAVE_MAX_DOMAIN_SIZE) {
                log_error("Multiplayer save: domain too large", 0, (int)domain_table[d].size);
                free(domain_table);
                return 0;
            }

            expected_total += domain_table[d].size;
            slot = header_domain_size_slot(&header, domain_table[d].tag);
            if (slot) {
                *slot = domain_table[d].size;
                seen_mask |= (1u << domain_table[d].tag);
            }
        }

        if (net_serializer_has_overflow(&dt)) {
            log_error("Multiplayer save: malformed domain table", 0, 0);
            free(domain_table);
            return 0;
        }

        if (expected_total > MP_SAVE_MAX_FILE_SIZE) {
            log_error("Multiplayer save: total payload exceeds max file size", 0,
                      (int)expected_total);
            free(domain_table);
            return 0;
        }

        if (header.total_payload_size > 0 && header.total_payload_size != expected_total) {
            log_error("Multiplayer save: payload size mismatch", 0,
                      (int)header.total_payload_size);
            free(domain_table);
            return 0;
        }

        if (header_wire_size + domain_table_size + expected_total > size) {
            log_error("Multiplayer save: buffer too small for declared payload", 0,
                      (int)(header_wire_size + domain_table_size + expected_total));
            free(domain_table);
            return 0;
        }

        required_mask = required_domain_mask_for_version(header.version);
        if ((seen_mask & required_mask) != required_mask) {
            log_error("Multiplayer save: missing required domains", 0, (int)seen_mask);
            free(domain_table);
            return 0;
        }
    } else {
        expected_total = header.player_registry_size +
                         header.ownership_size +
                         header.worldgen_size +
                         header.empire_sync_size +
                         header.trade_sync_routes_size +
                         header.trade_sync_traders_size +
                         header.p2p_routes_size +
                         header.time_sync_size;

        if (header_wire_size + expected_total > size) {
            log_error("Multiplayer save: buffer too small for declared domains", 0,
                      (int)(header_wire_size + expected_total));
            return 0;
        }
    }

    if (header.version >= 4 && header.version < 5 && header.payload_checksum != 0) {
        const uint8_t *payload_start = buffer + header_wire_size;
        uint32_t actual_checksum = compute_payload_checksum(payload_start, expected_total);
        if (actual_checksum != header.payload_checksum) {
            log_error("Multiplayer save: payload checksum mismatch", 0,
                      (int)actual_checksum);
            free(domain_table);
            return 0;
        }
    }

    if (header.version >= 5) {
        uint32_t offset = header_wire_size + domain_table_size;

        for (uint32_t d = 0; d < header.domain_count; d++) {
            const uint8_t *domain_data = buffer + offset;

            if (offset + domain_table[d].size > size) {
                log_error("Multiplayer save: truncated domain payload", 0,
                          (int)domain_table[d].tag);
                free(domain_table);
                return 0;
            }

            if (domain_table[d].checksum != compute_fnv1a(domain_data, domain_table[d].size)) {
                log_error("Multiplayer save: domain checksum mismatch", 0,
                          (int)domain_table[d].tag);
                free(domain_table);
                return 0;
            }

            if (!deserialize_domain_by_tag(domain_table[d].tag, domain_data, domain_table[d].size)) {
                log_error("Multiplayer save: failed to deserialize domain", 0,
                          (int)domain_table[d].tag);
                free(domain_table);
                return 0;
            }

            offset += domain_table[d].size;
        }
    } else {
        net_serializer s;
        net_serializer_init(&s, (uint8_t *)buffer, size);
        s.position = header_wire_size;

        if (header.player_registry_size > 0) {
            mp_player_registry_deserialize(buffer + s.position, header.player_registry_size);
            s.position += header.player_registry_size;
        }
        if (header.ownership_size > 0) {
            mp_ownership_deserialize(buffer + s.position, header.ownership_size);
            s.position += header.ownership_size;
        }
        if (header.worldgen_size > 0) {
            mp_worldgen_deserialize(buffer + s.position, header.worldgen_size);
            s.position += header.worldgen_size;
        }
        if (header.empire_sync_size > 0) {
            mp_empire_sync_deserialize(buffer + s.position, header.empire_sync_size);
            s.position += header.empire_sync_size;
        }
        if (header.trade_sync_routes_size > 0) {
            mp_trade_sync_deserialize_routes(buffer + s.position, header.trade_sync_routes_size);
            s.position += header.trade_sync_routes_size;
        }
        if (header.trade_sync_traders_size > 0) {
            mp_trade_sync_deserialize_traders(buffer + s.position, header.trade_sync_traders_size);
            s.position += header.trade_sync_traders_size;
        }
        if (header.p2p_routes_size > 0) {
            mp_trade_route_deserialize(buffer + s.position, header.p2p_routes_size);
            s.position += header.p2p_routes_size;
        }
        if (header.time_sync_size > 0) {
            mp_time_sync_deserialize(buffer + s.position, header.time_sync_size);
            s.position += header.time_sync_size;
        }
    }

    if (header.next_command_sequence_id > 0) {
        mp_command_bus_init_from_save(header.next_command_sequence_id);
    }

    {
        mp_game_manifest *manifest = mp_game_manifest_get_mutable();
        if (manifest) {
            const mp_spawn_table *spawn_table = mp_worldgen_get_spawn_table();
            uint8_t restored_max_players = header.player_count;

            if (mp_dedicated_server_is_enabled()) {
                const mp_dedicated_server_options *options = mp_dedicated_server_get_options();
                if (options && options->max_players > 0) {
                    restored_max_players = options->max_players;
                }
            }

            if (restored_max_players == 0 && spawn_table) {
                uint32_t total_capacity = (uint32_t)spawn_table->spawn_count +
                                          (uint32_t)spawn_table->reserved_count;
                if (total_capacity > 0 && total_capacity <= NET_MAX_PLAYERS) {
                    restored_max_players = (uint8_t)total_capacity;
                } else if (spawn_table->player_count > 0 &&
                           spawn_table->player_count <= NET_MAX_PLAYERS) {
                    restored_max_players = spawn_table->player_count;
                }
            }

            manifest->mode = MP_GAME_MODE_SAVED_GAME;
            manifest->save_version = header.version;
            manifest->session_seed = header.session_seed;
            manifest->player_count = header.player_count;
            manifest->max_players = restored_max_players > 0
                ? restored_max_players
                : header.player_count;
            manifest->feature_flags = header.compat_flags;
            memcpy(manifest->world_instance_uuid, header.world_instance_uuid,
                   MP_WORLD_UUID_SIZE);
            manifest->valid = 1;
        }
    }

    for (int i = 0; i < MP_MAX_PLAYERS; i++) {
        mp_player *p = mp_player_registry_get((uint8_t)i);
        if (p && p->active && !p->is_host) {
            p->status = MP_PLAYER_AWAITING_RECONNECT;
            p->connection_state = MP_CONNECTION_DISCONNECTED;
        }
    }

    empire_city_refresh_all_trade_route_bindings();

    MP_LOG_INFO("SESSION_SAVE", "Multiplayer session loaded: tick=%u, players=%d, seq=%u",
                header.snapshot_tick, (int)header.player_count,
                header.next_command_sequence_id);

    free(domain_table);
    return 1;
}

int mp_session_save_is_multiplayer(const uint8_t *buffer, uint32_t size)
{
    if (size < 4) {
        return 0;
    }
    {
        uint32_t magic = (uint32_t)buffer[0]
                       | ((uint32_t)buffer[1] << 8)
                       | ((uint32_t)buffer[2] << 16)
                       | ((uint32_t)buffer[3] << 24);
        return magic == MP_SAVE_MAGIC;
    }
}

int mp_session_save_read_header(const uint8_t *buffer, uint32_t size, mp_save_header *header)
{
    net_serializer s;

    if (size < HEADER_V2_SIZE) {
        log_error("Save too small for header", 0, (int)size);
        return 0;
    }

    memset(header, 0, sizeof(*header));
    net_serializer_init(&s, (uint8_t *)buffer, size);

    header->magic = net_read_u32(&s);
    header->version = net_read_u32(&s);
    header->protocol_version = net_read_u32(&s);
    header->session_id = net_read_u32(&s);
    header->session_seed = net_read_u32(&s);
    header->host_player_id = net_read_u8(&s);
    header->player_count = net_read_u8(&s);
    header->snapshot_tick = net_read_u32(&s);
    header->checksum = net_read_u32(&s);
    header->player_registry_size = net_read_u32(&s);
    header->ownership_size = net_read_u32(&s);
    header->worldgen_size = net_read_u32(&s);
    header->empire_sync_size = net_read_u32(&s);
    header->trade_sync_routes_size = net_read_u32(&s);
    header->trade_sync_traders_size = net_read_u32(&s);
    if (header->version >= 6) {
        header->p2p_routes_size = net_read_u32(&s);
    }
    header->time_sync_size = net_read_u32(&s);
    if (header->version >= 8) {
        header->server_rules_size = net_read_u32(&s);
    }

    if (header->magic != MP_SAVE_MAGIC) {
        log_error("Invalid multiplayer save magic", 0, (int)header->magic);
        return 0;
    }
    if (header->version > MP_SAVE_VERSION) {
        log_error("Unsupported multiplayer save version", 0, (int)header->version);
        return 0;
    }
    if (header->version >= 8 && size < HEADER_V8_SIZE) {
        log_error("Save too small for v8 header", 0, (int)size);
        return 0;
    }
    if (header->version >= 6 && header->version < 8 && size < HEADER_V6_SIZE) {
        log_error("Save too small for v6/v7 header", 0, (int)size);
        return 0;
    }

    if (header->version < 2) {
        header->worldgen_size = 0;
        header->session_seed = 0;
    }

    if (header->version >= 3 && size >= HEADER_V3_SIZE) {
        header->next_command_sequence_id = net_read_u32(&s);
    } else {
        header->next_command_sequence_id = 1;
    }

    if (header->version >= 4 && size >= HEADER_V4_SIZE) {
        header->total_payload_size = net_read_u32(&s);
        header->domain_count = net_read_u8(&s);
        header->compat_flags = net_read_u16(&s);
        header->payload_checksum = net_read_u32(&s);
    } else {
        header->total_payload_size = 0;
        header->domain_count = (header->version >= 6) ? 8 : 0;
        header->compat_flags = 0;
        header->payload_checksum = 0;
    }

    if (header->version >= 5 && size >= header_wire_size_for_version(header->version)) {
        net_read_raw(&s, header->world_instance_uuid, MP_WORLD_UUID_SIZE);
        header->header_checksum = net_read_u32(&s);
    } else {
        memset(header->world_instance_uuid, 0, MP_WORLD_UUID_SIZE);
        header->header_checksum = 0;
    }

    if (header->player_count > NET_MAX_PLAYERS) {
        log_error("Invalid player count in save", 0, (int)header->player_count);
        return 0;
    }

    if (net_serializer_has_overflow(&s)) {
        log_error("Multiplayer save header overflow", 0, (int)header->version);
        return 0;
    }

    return 1;
}

#endif /* ENABLE_MULTIPLAYER */
