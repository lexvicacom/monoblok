#include "nats.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// Opaque fake options handle allocated and destroyed through the NATS API.
struct natsOptions {
    int unused;
};

// Opaque fake connection handle allocated and destroyed through the NATS API.
struct natsConnection {
    int unused;
};

// Opaque fake subscription handle allocated and destroyed through the NATS API.
struct natsSubscription {
    char subject[256];
};

// Fake outbound message carrying enough state for publish-path assertions.
struct natsMsg {
    char subject[256];
    char payload[256];
    int payload_len;
    bool origin_header;
};

static fake_nats_state g_fake_nats;
static _Atomic int g_has_next_msg;
static char g_next_subject[256];
static char g_next_payload[256];

static void copy_cstr(char *dst, size_t cap, const char *src) {
    if (cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void copy_data(char *dst, size_t cap, const void *src, int len) {
    if (cap == 0) {
        return;
    }
    if (src == NULL || len <= 0) {
        dst[0] = '\0';
        return;
    }
    size_t n = (size_t)len;
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void fake_nats_reset(void) {
    g_fake_nats = (fake_nats_state){
        .options_create_status = NATS_OK,
        .set_servers_status = NATS_OK,
        .set_name_status = NATS_OK,
        .set_creds_status = NATS_OK,
        .set_user_info_status = NATS_OK,
        .set_token_status = NATS_OK,
        .set_secure_status = NATS_OK,
        .load_ca_status = NATS_OK,
        .load_cert_status = NATS_OK,
        .skip_verify_status = NATS_OK,
        .set_timeout_status = NATS_OK,
        .set_ping_interval_status = NATS_OK,
        .set_max_reconnect_status = NATS_OK,
        .set_reconnect_wait_status = NATS_OK,
        .connect_status = NATS_OK,
        .header_support_status = NATS_OK,
        .msg_create_status = NATS_OK,
        .msg_header_set_status = NATS_OK,
        .publish_msg_status = NATS_OK,
        .publish_status = NATS_OK,
        .subscribe_sync_status = NATS_OK,
        .next_msg_status = NATS_TIMEOUT,
        .header_get_status = NATS_NOT_FOUND,
    };
    atomic_store_explicit(&g_has_next_msg, 0, memory_order_release);
    g_next_subject[0] = '\0';
    g_next_payload[0] = '\0';
}

fake_nats_state *fake_nats_get(void) {
    return &g_fake_nats;
}

void fake_nats_deliver(const char *subject, const char *payload) {
    copy_cstr(g_next_subject, sizeof g_next_subject, subject);
    copy_cstr(g_next_payload, sizeof g_next_payload, payload);
    atomic_store_explicit(&g_has_next_msg, 1, memory_order_release);
}

const char *natsStatus_GetText(natsStatus status) {
    switch (status) {
    case NATS_OK: return "OK";
    case NATS_ERR: return "ERR";
    case NATS_NO_MEMORY: return "NO_MEMORY";
    case NATS_TIMEOUT: return "TIMEOUT";
    case NATS_NOT_CONNECTED: return "NOT_CONNECTED";
    case NATS_NOT_FOUND: return "NOT_FOUND";
    }
    return "UNKNOWN";
}

natsStatus natsOptions_Create(natsOptions **opts) {
    g_fake_nats.options_create_calls += 1;
    if (g_fake_nats.options_create_status != NATS_OK) {
        *opts = NULL;
        return g_fake_nats.options_create_status;
    }
    *opts = malloc(sizeof **opts);
    return *opts == NULL ? NATS_NO_MEMORY : NATS_OK;
}

void natsOptions_Destroy(natsOptions *opts) {
    g_fake_nats.options_destroy_calls += 1;
    free(opts);
}

natsStatus natsOptions_SetServers(natsOptions *opts, const char **servers, int count) {
    (void)opts;
    g_fake_nats.set_servers_calls += 1;
    g_fake_nats.last_set_servers_count = count;
    if (servers != NULL && count > 0) {
        copy_cstr(g_fake_nats.last_server, sizeof g_fake_nats.last_server, servers[0]);
    }
    return g_fake_nats.set_servers_status;
}

natsStatus natsOptions_SetName(natsOptions *opts, const char *name) {
    (void)opts;
    g_fake_nats.set_name_calls += 1;
    copy_cstr(g_fake_nats.last_name, sizeof g_fake_nats.last_name, name);
    return g_fake_nats.set_name_status;
}

natsStatus natsOptions_SetUserCredentialsFromFiles(natsOptions *opts, const char *chainFile, const char *keyFile) {
    (void)opts;
    (void)chainFile;
    (void)keyFile;
    g_fake_nats.set_creds_calls += 1;
    return g_fake_nats.set_creds_status;
}

natsStatus natsOptions_SetUserInfo(natsOptions *opts, const char *user, const char *password) {
    (void)opts;
    (void)user;
    (void)password;
    g_fake_nats.set_user_info_calls += 1;
    return g_fake_nats.set_user_info_status;
}

natsStatus natsOptions_SetToken(natsOptions *opts, const char *token) {
    (void)opts;
    (void)token;
    g_fake_nats.set_token_calls += 1;
    return g_fake_nats.set_token_status;
}

natsStatus natsOptions_SetSecure(natsOptions *opts, bool secure) {
    (void)opts;
    (void)secure;
    g_fake_nats.set_secure_calls += 1;
    return g_fake_nats.set_secure_status;
}

natsStatus natsOptions_LoadCATrustedCertificates(natsOptions *opts, const char *fileName) {
    (void)opts;
    (void)fileName;
    g_fake_nats.load_ca_calls += 1;
    return g_fake_nats.load_ca_status;
}

natsStatus natsOptions_LoadCertificatesChain(natsOptions *opts, const char *certsFileName, const char *keyFileName) {
    (void)opts;
    (void)certsFileName;
    (void)keyFileName;
    g_fake_nats.load_cert_calls += 1;
    return g_fake_nats.load_cert_status;
}

natsStatus natsOptions_SkipServerVerification(natsOptions *opts, bool skip) {
    (void)opts;
    (void)skip;
    g_fake_nats.skip_verify_calls += 1;
    return g_fake_nats.skip_verify_status;
}

natsStatus natsOptions_SetTimeout(natsOptions *opts, int64_t timeout) {
    (void)opts;
    (void)timeout;
    g_fake_nats.set_timeout_calls += 1;
    return g_fake_nats.set_timeout_status;
}

natsStatus natsOptions_SetPingInterval(natsOptions *opts, int64_t interval) {
    (void)opts;
    (void)interval;
    g_fake_nats.set_ping_interval_calls += 1;
    return g_fake_nats.set_ping_interval_status;
}

natsStatus natsOptions_SetMaxReconnect(natsOptions *opts, int maxReconnect) {
    (void)opts;
    (void)maxReconnect;
    g_fake_nats.set_max_reconnect_calls += 1;
    return g_fake_nats.set_max_reconnect_status;
}

natsStatus natsOptions_SetReconnectWait(natsOptions *opts, int64_t reconnectWait) {
    (void)opts;
    (void)reconnectWait;
    g_fake_nats.set_reconnect_wait_calls += 1;
    return g_fake_nats.set_reconnect_wait_status;
}

natsStatus natsConnection_Connect(natsConnection **conn, natsOptions *opts) {
    (void)opts;
    g_fake_nats.connect_calls += 1;
    if (g_fake_nats.connect_status != NATS_OK) {
        *conn = g_fake_nats.connect_returns_conn_on_failure ? malloc(sizeof **conn) : NULL;
        return g_fake_nats.connect_status;
    }
    *conn = malloc(sizeof **conn);
    return *conn == NULL ? NATS_NO_MEMORY : NATS_OK;
}

void natsConnection_Destroy(natsConnection *conn) {
    g_fake_nats.connection_destroy_calls += 1;
    free(conn);
}

natsStatus natsConnection_HasHeaderSupport(natsConnection *conn) {
    (void)conn;
    g_fake_nats.header_support_calls += 1;
    return g_fake_nats.header_support_status;
}

void nats_Close(void) {
    g_fake_nats.close_calls += 1;
}

natsStatus natsMsg_Create(natsMsg **msg, const char *subj, const char *reply, const char *data, int dataLen) {
    (void)reply;
    g_fake_nats.msg_create_calls += 1;
    copy_cstr(g_fake_nats.last_msg_subject, sizeof g_fake_nats.last_msg_subject, subj);
    g_fake_nats.last_msg_payload_len = dataLen;
    copy_data(g_fake_nats.last_msg_payload, sizeof g_fake_nats.last_msg_payload, data, dataLen);
    if (g_fake_nats.msg_create_status != NATS_OK) {
        *msg = NULL;
        return g_fake_nats.msg_create_status;
    }
    *msg = malloc(sizeof **msg);
    if (*msg == NULL) {
        return NATS_NO_MEMORY;
    }
    copy_cstr((*msg)->subject, sizeof (*msg)->subject, subj);
    copy_data((*msg)->payload, sizeof (*msg)->payload, data, dataLen);
    (*msg)->payload_len = dataLen;
    (*msg)->origin_header = false;
    return NATS_OK;
}

natsStatus natsMsgHeader_Set(natsMsg *msg, const char *key, const char *value) {
    (void)msg;
    g_fake_nats.msg_header_set_calls += 1;
    copy_cstr(g_fake_nats.last_header_name, sizeof g_fake_nats.last_header_name, key);
    copy_cstr(g_fake_nats.last_header_value, sizeof g_fake_nats.last_header_value, value);
    if (key != NULL && strcmp(key, "x-monoblok") == 0) {
        g_fake_nats.origin_header_set_calls += 1;
        copy_cstr(g_fake_nats.last_origin_header_value, sizeof g_fake_nats.last_origin_header_value, value);
    } else if (key != NULL && strcmp(key, "x-monoblok-replay") == 0) {
        g_fake_nats.replay_header_set_calls += 1;
        copy_cstr(g_fake_nats.last_replay_header_value, sizeof g_fake_nats.last_replay_header_value, value);
    } else if (key != NULL && strcmp(key, "x-monoblok-assumed-ts") == 0) {
        g_fake_nats.assumed_ts_header_set_calls += 1;
        copy_cstr(g_fake_nats.last_assumed_ts_header_value, sizeof g_fake_nats.last_assumed_ts_header_value, value);
    }
    return g_fake_nats.msg_header_set_status;
}

natsStatus natsConnection_PublishMsg(natsConnection *conn, natsMsg *msg) {
    (void)conn;
    g_fake_nats.publish_msg_calls += 1;
    if (msg != NULL) {
        copy_cstr(g_fake_nats.last_publish_subject, sizeof g_fake_nats.last_publish_subject, msg->subject);
        g_fake_nats.last_publish_payload_len = msg->payload_len;
        copy_data(g_fake_nats.last_publish_payload, sizeof g_fake_nats.last_publish_payload, msg->payload, msg->payload_len);
    }
    return g_fake_nats.publish_msg_status;
}

void natsMsg_Destroy(natsMsg *msg) {
    g_fake_nats.msg_destroy_calls += 1;
    free(msg);
}

natsStatus natsConnection_Publish(natsConnection *conn, const char *subj, const void *data, int dataLen) {
    (void)conn;
    g_fake_nats.publish_calls += 1;
    copy_cstr(g_fake_nats.last_publish_subject, sizeof g_fake_nats.last_publish_subject, subj);
    g_fake_nats.last_publish_payload_len = dataLen;
    copy_data(g_fake_nats.last_publish_payload, sizeof g_fake_nats.last_publish_payload, data, dataLen);
    return g_fake_nats.publish_status;
}

natsStatus natsConnection_SubscribeSync(natsSubscription **sub, natsConnection *nc, const char *subject) {
    (void)nc;
    g_fake_nats.subscribe_sync_calls += 1;
    if (g_fake_nats.subscribe_sync_status != NATS_OK) {
        *sub = NULL;
        return g_fake_nats.subscribe_sync_status;
    }
    *sub = malloc(sizeof **sub);
    if (*sub == NULL) {
        return NATS_NO_MEMORY;
    }
    copy_cstr((*sub)->subject, sizeof (*sub)->subject, subject);
    return NATS_OK;
}

natsStatus natsSubscription_NextMsg(natsMsg **nextMsg, natsSubscription *sub, int64_t timeout) {
    (void)sub;
    (void)timeout;
    g_fake_nats.next_msg_calls += 1;
    *nextMsg = NULL;
    if (!atomic_exchange_explicit(&g_has_next_msg, 0, memory_order_acquire)) {
        return g_fake_nats.next_msg_status;
    }
    natsMsg *msg = malloc(sizeof *msg);
    if (msg == NULL) {
        return NATS_NO_MEMORY;
    }
    copy_cstr(msg->subject, sizeof msg->subject, g_next_subject);
    copy_cstr(msg->payload, sizeof msg->payload, g_next_payload);
    msg->payload_len = (int)strlen(msg->payload);
    msg->origin_header = g_fake_nats.next_msg_has_origin_header;
    *nextMsg = msg;
    return NATS_OK;
}

void natsSubscription_Destroy(natsSubscription *sub) {
    g_fake_nats.subscription_destroy_calls += 1;
    free(sub);
}

const char *natsMsg_GetSubject(const natsMsg *msg) {
    g_fake_nats.msg_subject_calls += 1;
    return msg == NULL ? NULL : msg->subject;
}

const char *natsMsg_GetData(const natsMsg *msg) {
    g_fake_nats.msg_data_calls += 1;
    return msg == NULL ? NULL : msg->payload;
}

int natsMsg_GetDataLength(const natsMsg *msg) {
    g_fake_nats.msg_data_len_calls += 1;
    return msg == NULL ? 0 : msg->payload_len;
}

natsStatus natsMsgHeader_Get(natsMsg *msg, const char *key, const char **value) {
    g_fake_nats.msg_header_get_calls += 1;
    if (g_fake_nats.header_get_status != NATS_OK) {
        *value = NULL;
        return g_fake_nats.header_get_status;
    }
    if (msg == NULL || !msg->origin_header || strcmp(key, "x-monoblok") != 0) {
        *value = NULL;
        return NATS_NOT_FOUND;
    }
    *value = "test-origin";
    return NATS_OK;
}
