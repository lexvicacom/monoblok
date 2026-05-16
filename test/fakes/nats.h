#ifndef MB_TEST_FAKE_NATS_H
#define MB_TEST_FAKE_NATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum natsStatus {
    NATS_OK = 0,
    NATS_ERR = 1,
    NATS_NO_MEMORY = 2,
    NATS_TIMEOUT = 3,
    NATS_NOT_CONNECTED = 4,
} natsStatus;

typedef struct natsOptions natsOptions;
typedef struct natsConnection natsConnection;
typedef struct natsMsg natsMsg;

// Test control and call ledger for the fake NATS client.
typedef struct fake_nats_state {
    natsStatus options_create_status;
    natsStatus set_servers_status;
    natsStatus set_name_status;
    natsStatus set_creds_status;
    natsStatus set_user_info_status;
    natsStatus set_token_status;
    natsStatus set_secure_status;
    natsStatus load_ca_status;
    natsStatus load_cert_status;
    natsStatus skip_verify_status;
    natsStatus set_timeout_status;
    natsStatus set_ping_interval_status;
    natsStatus set_max_reconnect_status;
    natsStatus set_reconnect_wait_status;
    natsStatus connect_status;
    natsStatus header_support_status;
    natsStatus msg_create_status;
    natsStatus msg_header_set_status;
    natsStatus publish_msg_status;
    natsStatus publish_status;
    bool connect_returns_conn_on_failure;

    int options_create_calls;
    int options_destroy_calls;
    int set_servers_calls;
    int set_name_calls;
    int set_creds_calls;
    int set_user_info_calls;
    int set_token_calls;
    int set_secure_calls;
    int load_ca_calls;
    int load_cert_calls;
    int skip_verify_calls;
    int set_timeout_calls;
    int set_ping_interval_calls;
    int set_max_reconnect_calls;
    int set_reconnect_wait_calls;
    int connect_calls;
    int header_support_calls;
    int connection_destroy_calls;
    int close_calls;
    int msg_create_calls;
    int msg_header_set_calls;
    int publish_msg_calls;
    int msg_destroy_calls;
    int publish_calls;

    int last_set_servers_count;
    int last_publish_payload_len;
    int last_msg_payload_len;
    char last_server[128];
    char last_name[128];
    char last_publish_subject[256];
    char last_publish_payload[256];
    char last_msg_subject[256];
    char last_msg_payload[256];
    char last_header_name[64];
    char last_header_value[256];
} fake_nats_state;

void fake_nats_reset(void);
fake_nats_state *fake_nats_get(void);

const char *natsStatus_GetText(natsStatus status);
natsStatus natsOptions_Create(natsOptions **opts);
void natsOptions_Destroy(natsOptions *opts);
natsStatus natsOptions_SetServers(natsOptions *opts, const char **servers, int count);
natsStatus natsOptions_SetName(natsOptions *opts, const char *name);
natsStatus natsOptions_SetUserCredentialsFromFiles(natsOptions *opts, const char *chainFile, const char *keyFile);
natsStatus natsOptions_SetUserInfo(natsOptions *opts, const char *user, const char *password);
natsStatus natsOptions_SetToken(natsOptions *opts, const char *token);
natsStatus natsOptions_SetSecure(natsOptions *opts, bool secure);
natsStatus natsOptions_LoadCATrustedCertificates(natsOptions *opts, const char *fileName);
natsStatus natsOptions_LoadCertificatesChain(natsOptions *opts, const char *certsFileName, const char *keyFileName);
natsStatus natsOptions_SkipServerVerification(natsOptions *opts, bool skip);
natsStatus natsOptions_SetTimeout(natsOptions *opts, int64_t timeout);
natsStatus natsOptions_SetPingInterval(natsOptions *opts, int64_t interval);
natsStatus natsOptions_SetMaxReconnect(natsOptions *opts, int maxReconnect);
natsStatus natsOptions_SetReconnectWait(natsOptions *opts, int64_t reconnectWait);
natsStatus natsConnection_Connect(natsConnection **conn, natsOptions *opts);
void natsConnection_Destroy(natsConnection *conn);
natsStatus natsConnection_HasHeaderSupport(natsConnection *conn);
void nats_Close(void);
natsStatus natsMsg_Create(natsMsg **msg, const char *subj, const char *reply, const char *data, int dataLen);
natsStatus natsMsgHeader_Set(natsMsg *msg, const char *key, const char *value);
natsStatus natsConnection_PublishMsg(natsConnection *conn, natsMsg *msg);
void natsMsg_Destroy(natsMsg *msg);
natsStatus natsConnection_Publish(natsConnection *conn, const char *subj, const void *data, int dataLen);

#endif
