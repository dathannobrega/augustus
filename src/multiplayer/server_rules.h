#ifndef MULTIPLAYER_SERVER_RULES_H
#define MULTIPLAYER_SERVER_RULES_H

#ifdef ENABLE_MULTIPLAYER

#include <stdint.h>

void mp_server_rules_init(void);
void mp_server_rules_capture_from_config(void);
void mp_server_rules_apply_to_config(void);
void mp_server_rules_clear(void);
int mp_server_rules_apply_named_rule(const char *name, const char *value);
void mp_server_rules_serialize(uint8_t *buffer, uint32_t buffer_size, uint32_t *out_size);
int mp_server_rules_deserialize(const uint8_t *buffer, uint32_t size);

#endif /* ENABLE_MULTIPLAYER */

#endif /* MULTIPLAYER_SERVER_RULES_H */
