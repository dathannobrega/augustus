#include "server_rules.h"

#ifdef ENABLE_MULTIPLAYER

#include "mp_debug_log.h"
#include "core/config.h"
#include "network/serialize.h"

#include <string.h>
#include <stdlib.h>

typedef struct {
    const char *name;
    config_key key;
} mp_server_rule_def;

static const mp_server_rule_def RULE_DEFS[] = {
    {"gameplay_fix_immigration", CONFIG_GP_FIX_IMMIGRATION_BUG},
    {"gameplay_fix_100y_ghosts", CONFIG_GP_FIX_100_YEAR_GHOSTS},
    {"gameplay_change_max_grand_temples", CONFIG_GP_CH_MAX_GRAND_TEMPLES},
    {"gameplay_change_jealous_gods", CONFIG_GP_CH_JEALOUS_GODS},
    {"gameplay_change_global_labour", CONFIG_GP_CH_GLOBAL_LABOUR},
    {"gameplay_change_retire_at_60", CONFIG_GP_CH_RETIRE_AT_60},
    {"gameplay_change_fixed_workers", CONFIG_GP_CH_FIXED_WORKERS},
    {"gameplay_wolves_block", CONFIG_GP_CH_WOLVES_BLOCK},
    {"gameplay_buyers_dont_distribute", CONFIG_GP_CH_NO_SUPPLIER_DISTRIBUTION},
    {"gameplay_change_getting_granaries_go_offroad", CONFIG_GP_CH_GETTING_GRANARIES_GO_OFFROAD},
    {"gameplay_change_granaries_get_double", CONFIG_GP_CH_GRANARIES_GET_DOUBLE},
    {"gameplay_change_allow_exporting_from_granaries", CONFIG_GP_CH_ALLOW_EXPORTING_FROM_GRANARIES},
    {"gameplay_change_tower_sentries_go_offroad", CONFIG_GP_CH_TOWER_SENTRIES_GO_OFFROAD},
    {"gameplay_change_farms_deliver_close", CONFIG_GP_CH_FARMS_DELIVER_CLOSE},
    {"gameplay_change_only_deliver_to_accepting_granaries", CONFIG_GP_CH_DELIVER_ONLY_TO_ACCEPTING_GRANARIES},
    {"gameplay_change_all_houses_merge", CONFIG_GP_CH_ALL_HOUSES_MERGE},
    {"gameplay_change_random_mine_or_pit_collapses_take_money", CONFIG_GP_CH_RANDOM_COLLAPSES_TAKE_MONEY},
    {"gameplay_change_multiple_barracks", CONFIG_GP_CH_MULTIPLE_BARRACKS},
    {"gameplay_change_warehouses_dont_accept", CONFIG_GP_CH_WAREHOUSES_DONT_ACCEPT},
    {"gameplay_change_markets_dont_accept", CONFIG_GP_CH_MARKETS_DONT_ACCEPT},
    {"gameplay_change_warehouses_granaries_over_road_placement", CONFIG_GP_CH_WAREHOUSES_GRANARIES_OVER_ROAD_PLACEMENT},
    {"gameplay_change_houses_dont_expand_into_gardens", CONFIG_GP_CH_HOUSES_DONT_EXPAND_INTO_GARDENS},
    {"gameplay_change_monuments_boost_culture_rating", CONFIG_GP_CH_MONUMENTS_BOOST_CULTURE_RATING},
    {"gameplay_change_disable_infinite_wolves_spawning", CONFIG_GP_CH_DISABLE_INFINITE_WOLVES_SPAWNING},
    {"gameplay_change_romers_dont_skip_corners", CONFIG_GP_CH_ROAMERS_DONT_SKIP_CORNERS},
    {"gameplay_change_yearly_autosave", CONFIG_GP_CH_YEARLY_AUTOSAVE},
    {"gameplay_change_auto_kill_animals", CONFIG_GP_CH_AUTO_KILL_ANIMALS},
    {"gameplay_change_nonmilitary_gates_allow_walkers", CONFIG_GP_CH_GATES_DEFAULT_TO_PASS_ALL_WALKERS},
    {"gameplay_change_max_autosave_slots", CONFIG_GP_CH_MAX_AUTOSAVE_SLOTS},
    {"gameplay_change_caravans_move_off_road", CONFIG_GP_CARAVANS_MOVE_OFF_ROAD},
    {"gameplay_change_storage_step_4", CONFIG_GP_STORAGE_INCREMENT_4},
    {"gameplay_change_default_game_speed", CONFIG_GP_CH_DEFAULT_GAME_SPEED},
    {"gameplay_change_stockpiled_getting", CONFIG_GP_CH_ENABLE_GETTING_WHILE_STOCKPILED},
    {"gp_ch_storage_requests_respect_maintain", CONFIG_GP_CH_STORAGE_REQUESTS_RESPECT_MAINTAIN},
    {"gameplay_market_range", CONFIG_GP_CH_MARKET_RANGE},
    {"gp_ch_housing_pre_merge_vacant_lots", CONFIG_GP_CH_HOUSING_PRE_MERGE_VACANT_LOTS},
    {"always_be_able_to_destroy_bridges", CONFIG_GP_CH_ALWAYS_DESTROY_BRIDGES},
    {"gameplay_patrician_devolution_fix", CONFIG_GP_CH_PATRICIAN_DEVOLUTION_FIX}
};

typedef struct {
    int valid;
    int32_t values[sizeof(RULE_DEFS) / sizeof(RULE_DEFS[0])];
} mp_server_rule_state;

static mp_server_rule_state rule_state;

static int rule_def_count(void)
{
    return (int)(sizeof(RULE_DEFS) / sizeof(RULE_DEFS[0]));
}

static int find_rule_index_by_key(config_key key)
{
    for (int i = 0; i < rule_def_count(); i++) {
        if (RULE_DEFS[i].key == key) {
            return i;
        }
    }
    return -1;
}

static int find_rule_index_by_name(const char *name)
{
    if (!name || !name[0]) {
        return -1;
    }
    for (int i = 0; i < rule_def_count(); i++) {
        if (strcmp(RULE_DEFS[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void capture_values(int32_t *values)
{
    if (!values) {
        return;
    }
    for (int i = 0; i < rule_def_count(); i++) {
        values[i] = config_get(RULE_DEFS[i].key);
    }
}

void mp_server_rules_init(void)
{
    mp_server_rules_clear();
}

void mp_server_rules_capture_from_config(void)
{
    capture_values(rule_state.values);
    rule_state.valid = 1;
}

void mp_server_rules_apply_to_config(void)
{
    if (!rule_state.valid) {
        return;
    }
    for (int i = 0; i < rule_def_count(); i++) {
        config_set(RULE_DEFS[i].key, rule_state.values[i]);
    }
}

void mp_server_rules_clear(void)
{
    memset(&rule_state, 0, sizeof(rule_state));
}

int mp_server_rules_apply_named_rule(const char *name, const char *value)
{
    int index;
    int parsed_value;

    index = find_rule_index_by_name(name);
    if (index < 0 || !value) {
        if (name && name[0]) {
            MP_LOG_WARN("RULES", "Ignoring unknown authoritative rule '%s'", name);
        }
        return 0;
    }

    parsed_value = atoi(value);
    config_set(RULE_DEFS[index].key, parsed_value);
    rule_state.values[index] = parsed_value;
    rule_state.valid = 1;
    return 1;
}

void mp_server_rules_serialize(uint8_t *buffer, uint32_t buffer_size, uint32_t *out_size)
{
    net_serializer s;

    if (out_size) {
        *out_size = 0;
    }
    if (!buffer || !out_size) {
        return;
    }
    if (!rule_state.valid) {
        mp_server_rules_capture_from_config();
    }

    net_serializer_init(&s, buffer, buffer_size);
    net_write_u8(&s, (uint8_t)rule_def_count());
    for (int i = 0; i < rule_def_count(); i++) {
        net_write_u16(&s, (uint16_t)RULE_DEFS[i].key);
        net_write_i32(&s, rule_state.values[i]);
    }

    if (net_serializer_has_overflow(&s)) {
        MP_LOG_ERROR("RULES", "Server rule payload overflow (%u bytes buffer)", buffer_size);
        return;
    }

    *out_size = (uint32_t)net_serializer_position(&s);
}

int mp_server_rules_deserialize(const uint8_t *buffer, uint32_t size)
{
    net_serializer s;
    int32_t parsed_values[sizeof(RULE_DEFS) / sizeof(RULE_DEFS[0])];
    uint8_t rule_count;

    if (!buffer || size == 0) {
        mp_server_rules_clear();
        return 0;
    }

    capture_values(parsed_values);

    net_serializer_init(&s, (uint8_t *)buffer, size);
    rule_count = net_read_u8(&s);

    for (uint8_t i = 0; i < rule_count; i++) {
        config_key key = (config_key)net_read_u16(&s);
        int32_t value = net_read_i32(&s);
        int index = find_rule_index_by_key(key);
        if (index >= 0) {
            parsed_values[index] = value;
        }
    }

    if (net_serializer_has_overflow(&s)) {
        MP_LOG_ERROR("RULES", "Malformed authoritative rules payload (%u bytes)", size);
        mp_server_rules_clear();
        return 0;
    }

    memcpy(rule_state.values, parsed_values, sizeof(parsed_values));
    rule_state.valid = 1;
    mp_server_rules_apply_to_config();
    MP_LOG_INFO("RULES", "Applied %u authoritative gameplay rules", (unsigned int)rule_count);
    return 1;
}

#endif /* ENABLE_MULTIPLAYER */
