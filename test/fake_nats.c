#include "nats.h"

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

// Fake outbound message carrying enough state for publish-path assertions.
struct natsMsg {
    char subject[256];
    char payload[256];
    int payload_len;
};

static fake_nats_state g_fake_nats;

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
    };
}

fake_nats_state *fake_nats_get(void) {
    return &g_fake_nats;
}

const char *natsStatus_GetText(natsStatus status) {
    switch (status) {
    case NATS_OK: return "OK";
    case NATS_ERR: return "ERR";
    case NATS_NO_MEMORY: return "NO_MEMORY";
    case NATS_TIMEOUT: return "TIMEOUT";
    case NATS_NOT_CONNECTED: return "NOT_CONNECTED";
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
    return NATS_OK;
}

natsStatus natsMsgHeader_Set(natsMsg *msg, const char *key, const char *value) {
    (void)msg;
    g_fake_nats.msg_header_set_calls += 1;
    copy_cstr(g_fake_nats.last_header_name, sizeof g_fake_nats.last_header_name, key);
    copy_cstr(g_fake_nats.last_header_value, sizeof g_fake_nats.last_header_value, value);
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
