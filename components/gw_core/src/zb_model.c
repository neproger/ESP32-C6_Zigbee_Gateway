#include "gw_core/zb_model.h"

#include <stdbool.h>
#include <string.h>

static bool s_inited;
static gw_zb_endpoint_t s_eps[GW_ZB_MAX_ENDPOINTS];
static size_t s_ep_count;

static bool uid_equals(const gw_device_uid_t *a, const gw_device_uid_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return strncmp(a->uid, b->uid, sizeof(a->uid)) == 0;
}

esp_err_t gw_zb_model_init(void)
{
    s_inited = true;
    s_ep_count = 0;
    memset(s_eps, 0, sizeof(s_eps));
    return ESP_OK;
}

esp_err_t gw_zb_model_upsert_endpoint(const gw_zb_endpoint_t *ep)
{
    if (!s_inited || ep == NULL || ep->uid.uid[0] == '\0' || ep->endpoint == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < s_ep_count; i++) {
        if (uid_equals(&s_eps[i].uid, &ep->uid) && s_eps[i].endpoint == ep->endpoint) {
            s_eps[i] = *ep;
            return ESP_OK;
        }
    }

    if (s_ep_count >= (sizeof(s_eps) / sizeof(s_eps[0]))) {
        return ESP_ERR_NO_MEM;
    }

    s_eps[s_ep_count++] = *ep;
    return ESP_OK;
}

size_t gw_zb_model_list_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps)
{
    if (!s_inited || uid == NULL || out_eps == NULL || max_eps == 0) {
        return 0;
    }

    size_t written = 0;
    for (size_t i = 0; i < s_ep_count && written < max_eps; i++) {
        if (uid_equals(&s_eps[i].uid, uid)) {
            out_eps[written++] = s_eps[i];
        }
    }
    return written;
}

bool gw_zb_model_find_uid_by_short(uint16_t short_addr, gw_device_uid_t *out_uid)
{
    if (!s_inited || out_uid == NULL) {
        return false;
    }

    for (size_t i = 0; i < s_ep_count; i++) {
        if (s_eps[i].short_addr == short_addr && s_eps[i].uid.uid[0] != '\0') {
            *out_uid = s_eps[i].uid;
            return true;
        }
    }
    return false;
}
