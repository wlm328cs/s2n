/*
 * Copyright 2014 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <sys/param.h>

#include <errno.h>
#include <s2n.h>

#include "error/s2n_errno.h"

#include "crypto/s2n_fips.h"

#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_record.h"
#include "tls/s2n_resume.h"
#include "tls/s2n_alerts.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_kex.h"

#include "stuffer/s2n_stuffer.h"

#include "utils/s2n_safety.h"
#include "utils/s2n_socket.h"
#include "utils/s2n_random.h"
#include "utils/s2n_str.h"

/* From RFC 5246 7.4 */
#define TLS_HELLO_REQUEST              0
#define TLS_CLIENT_HELLO               1
#define TLS_SERVER_HELLO               2
#define TLS_SERVER_NEW_SESSION_TICKET  4
#define TLS_SERVER_CERT               11
#define TLS_SERVER_KEY                12
#define TLS_SERVER_CERT_REQ           13
#define TLS_CLIENT_CERT_REQ           13 /* Same as SERVER_CERT_REQ */
#define TLS_SERVER_HELLO_DONE         14
#define TLS_CLIENT_CERT               11  /* Same as SERVER_CERT */
#define TLS_CLIENT_CERT_VERIFY        15
#define TLS_CLIENT_KEY                16
#define TLS_CLIENT_FINISHED           20
#define TLS_SERVER_FINISHED           20  /* Same as CLIENT_FINISHED */
#define TLS_SERVER_CERT_STATUS        22

struct s2n_handshake_action {
    uint8_t record_type;
    uint8_t message_type;
    char writer;                /* 'S' or 'C' for server or client, 'B' for both */
    int (*handler[2]) (struct s2n_connection * conn);
};

/* Client and Server handlers for each message type we support.  
 * See http://www.iana.org/assignments/tls-parameters/tls-parameters.xhtml#tls-parameters-7 for the list of handshake message types
 */
static struct s2n_handshake_action state_machine[] = {
    /* message_type_t           = {Record type   Message type     Writer S2N_SERVER                S2N_CLIENT }  */
    [CLIENT_HELLO]              = {TLS_HANDSHAKE, TLS_CLIENT_HELLO, 'C', {s2n_client_hello_recv, s2n_client_hello_send}}, 
    [SERVER_HELLO]              = {TLS_HANDSHAKE, TLS_SERVER_HELLO, 'S', {s2n_server_hello_send, s2n_server_hello_recv}}, 
    [SERVER_NEW_SESSION_TICKET] = {TLS_HANDSHAKE, TLS_SERVER_NEW_SESSION_TICKET,'S', {s2n_server_nst_send, s2n_server_nst_recv}},
    [SERVER_CERT]               = {TLS_HANDSHAKE, TLS_SERVER_CERT, 'S', {s2n_server_cert_send, s2n_server_cert_recv}},
    [SERVER_CERT_STATUS]        = {TLS_HANDSHAKE, TLS_SERVER_CERT_STATUS, 'S', {s2n_server_status_send, s2n_server_status_recv}},
    [SERVER_KEY]                = {TLS_HANDSHAKE, TLS_SERVER_KEY, 'S', {s2n_server_key_send, s2n_server_key_recv}},
    [SERVER_CERT_REQ]           = {TLS_HANDSHAKE, TLS_CLIENT_CERT_REQ, 'S', {s2n_client_cert_req_send, s2n_client_cert_req_recv}},
    [SERVER_HELLO_DONE]         = {TLS_HANDSHAKE, TLS_SERVER_HELLO_DONE, 'S', {s2n_server_done_send, s2n_server_done_recv}}, 
    [CLIENT_CERT]               = {TLS_HANDSHAKE, TLS_CLIENT_CERT, 'C', {s2n_client_cert_recv, s2n_client_cert_send}},
    [CLIENT_KEY]                = {TLS_HANDSHAKE, TLS_CLIENT_KEY, 'C', {s2n_client_key_recv, s2n_client_key_send}},
    [CLIENT_CERT_VERIFY]        = {TLS_HANDSHAKE, TLS_CLIENT_CERT_VERIFY, 'C', {s2n_client_cert_verify_recv, s2n_client_cert_verify_send}},
    [CLIENT_CHANGE_CIPHER_SPEC] = {TLS_CHANGE_CIPHER_SPEC, 0, 'C', {s2n_client_ccs_recv, s2n_ccs_send}},
    [CLIENT_FINISHED]           = {TLS_HANDSHAKE, TLS_CLIENT_FINISHED, 'C', {s2n_client_finished_recv, s2n_client_finished_send}},
    [SERVER_CHANGE_CIPHER_SPEC] = {TLS_CHANGE_CIPHER_SPEC, 0, 'S', {s2n_ccs_send, s2n_server_ccs_recv}},
    [SERVER_FINISHED]           = {TLS_HANDSHAKE, TLS_SERVER_FINISHED, 'S', {s2n_server_finished_send, s2n_server_finished_recv}},
    [APPLICATION_DATA]          = {TLS_APPLICATION_DATA, 0, 'B', {NULL, NULL}}
};

#define MESSAGE_NAME_ENTRY(msg) [msg] = #msg

static const char *message_names[] = {
    MESSAGE_NAME_ENTRY(CLIENT_HELLO),
    MESSAGE_NAME_ENTRY(SERVER_HELLO),
    MESSAGE_NAME_ENTRY(SERVER_NEW_SESSION_TICKET),
    MESSAGE_NAME_ENTRY(SERVER_CERT),
    MESSAGE_NAME_ENTRY(SERVER_CERT_STATUS),
    MESSAGE_NAME_ENTRY(SERVER_KEY),
    MESSAGE_NAME_ENTRY(SERVER_CERT_REQ),
    MESSAGE_NAME_ENTRY(SERVER_HELLO_DONE),
    MESSAGE_NAME_ENTRY(CLIENT_CERT),
    MESSAGE_NAME_ENTRY(CLIENT_KEY),
    MESSAGE_NAME_ENTRY(CLIENT_CERT_VERIFY),
    MESSAGE_NAME_ENTRY(CLIENT_CHANGE_CIPHER_SPEC),
    MESSAGE_NAME_ENTRY(CLIENT_FINISHED),
    MESSAGE_NAME_ENTRY(SERVER_CHANGE_CIPHER_SPEC),
    MESSAGE_NAME_ENTRY(SERVER_FINISHED),
    MESSAGE_NAME_ENTRY(APPLICATION_DATA),
};

/* We support different ordering of TLS Handshake messages, depending on what is being negotiated. There's also a dummy "INITIAL" handshake
 * that everything starts out as until we know better.
 */
static message_type_t handshakes[128][16] = {
    [INITIAL] = {
            CLIENT_HELLO,
            SERVER_HELLO
    },

    [NEGOTIATED] = {
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            APPLICATION_DATA
    },

    [NEGOTIATED | WITH_SESSION_TICKET ] = {
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_NEW_SESSION_TICKET, SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            APPLICATION_DATA},

    [NEGOTIATED | FULL_HANDSHAKE ] = {
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_CERT, SERVER_HELLO_DONE,
            CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | WITH_SESSION_TICKET ] = {
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_CERT, SERVER_HELLO_DONE,
            CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            SERVER_NEW_SESSION_TICKET, SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | PERFECT_FORWARD_SECRECY ] = {
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_CERT, SERVER_KEY, SERVER_HELLO_DONE,
            CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | PERFECT_FORWARD_SECRECY | WITH_SESSION_TICKET ] = {
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_CERT, SERVER_KEY, SERVER_HELLO_DONE,
            CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            SERVER_NEW_SESSION_TICKET, SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | OCSP_STATUS ] ={
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_CERT, SERVER_CERT_STATUS, SERVER_HELLO_DONE,
            CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | OCSP_STATUS | WITH_SESSION_TICKET ] = {
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_CERT, SERVER_CERT_STATUS, SERVER_HELLO_DONE,
            CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            SERVER_NEW_SESSION_TICKET, SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | PERFECT_FORWARD_SECRECY | OCSP_STATUS ] = {
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_CERT, SERVER_CERT_STATUS, SERVER_KEY, SERVER_HELLO_DONE,
            CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | PERFECT_FORWARD_SECRECY | OCSP_STATUS  | WITH_SESSION_TICKET ] ={
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_CERT, SERVER_CERT_STATUS, SERVER_KEY, SERVER_HELLO_DONE,
            CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            SERVER_NEW_SESSION_TICKET, SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | CLIENT_AUTH] = {
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_CERT, SERVER_CERT_REQ, SERVER_HELLO_DONE,
            CLIENT_CERT, CLIENT_KEY, CLIENT_CERT_VERIFY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | CLIENT_AUTH | NO_CLIENT_CERT ] = {
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_CERT, SERVER_CERT_REQ, SERVER_HELLO_DONE,
            CLIENT_CERT, CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | CLIENT_AUTH | WITH_SESSION_TICKET] = {
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_CERT, SERVER_CERT_REQ, SERVER_HELLO_DONE,
            CLIENT_CERT, CLIENT_KEY, CLIENT_CERT_VERIFY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            SERVER_NEW_SESSION_TICKET, SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | CLIENT_AUTH | NO_CLIENT_CERT | WITH_SESSION_TICKET ] = {
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_CERT, SERVER_CERT_REQ, SERVER_HELLO_DONE,
            CLIENT_CERT, CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            SERVER_NEW_SESSION_TICKET, SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | PERFECT_FORWARD_SECRECY | CLIENT_AUTH] = {
           CLIENT_HELLO,
           SERVER_HELLO, SERVER_CERT, SERVER_KEY, SERVER_CERT_REQ, SERVER_HELLO_DONE,
           CLIENT_CERT, CLIENT_KEY, CLIENT_CERT_VERIFY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
           SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
           APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | PERFECT_FORWARD_SECRECY | CLIENT_AUTH | NO_CLIENT_CERT ] = {
           CLIENT_HELLO,
           SERVER_HELLO, SERVER_CERT, SERVER_KEY, SERVER_CERT_REQ, SERVER_HELLO_DONE,
           CLIENT_CERT, CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
           SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
           APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | PERFECT_FORWARD_SECRECY | CLIENT_AUTH | WITH_SESSION_TICKET] = {
           CLIENT_HELLO,
           SERVER_HELLO, SERVER_CERT, SERVER_KEY, SERVER_CERT_REQ, SERVER_HELLO_DONE,
           CLIENT_CERT, CLIENT_KEY, CLIENT_CERT_VERIFY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
           SERVER_NEW_SESSION_TICKET, SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
           APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | PERFECT_FORWARD_SECRECY | CLIENT_AUTH | NO_CLIENT_CERT | WITH_SESSION_TICKET ] = {
           CLIENT_HELLO,
           SERVER_HELLO, SERVER_CERT, SERVER_KEY, SERVER_CERT_REQ, SERVER_HELLO_DONE,
           CLIENT_CERT, CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
           SERVER_NEW_SESSION_TICKET, SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
           APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | OCSP_STATUS | CLIENT_AUTH] = {
           CLIENT_HELLO,
           SERVER_HELLO, SERVER_CERT, SERVER_CERT_STATUS, SERVER_CERT_REQ, SERVER_HELLO_DONE,
           CLIENT_CERT, CLIENT_KEY, CLIENT_CERT_VERIFY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
           SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
           APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | OCSP_STATUS | CLIENT_AUTH | NO_CLIENT_CERT ] = {
           CLIENT_HELLO,
           SERVER_HELLO, SERVER_CERT, SERVER_CERT_STATUS, SERVER_CERT_REQ, SERVER_HELLO_DONE,
           CLIENT_CERT, CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
           SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
           APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | OCSP_STATUS | CLIENT_AUTH | WITH_SESSION_TICKET] = {
           CLIENT_HELLO,
           SERVER_HELLO, SERVER_CERT, SERVER_CERT_STATUS, SERVER_CERT_REQ, SERVER_HELLO_DONE,
           CLIENT_CERT, CLIENT_KEY, CLIENT_CERT_VERIFY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
           SERVER_NEW_SESSION_TICKET, SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
           APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | OCSP_STATUS | CLIENT_AUTH | NO_CLIENT_CERT | WITH_SESSION_TICKET ] = {
           CLIENT_HELLO,
           SERVER_HELLO, SERVER_CERT, SERVER_CERT_STATUS, SERVER_CERT_REQ, SERVER_HELLO_DONE,
           CLIENT_CERT, CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
           SERVER_NEW_SESSION_TICKET, SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
           APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | PERFECT_FORWARD_SECRECY | OCSP_STATUS | CLIENT_AUTH ] = {
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_CERT, SERVER_CERT_STATUS, SERVER_KEY, SERVER_CERT_REQ, SERVER_HELLO_DONE,
            CLIENT_CERT, CLIENT_KEY, CLIENT_CERT_VERIFY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | PERFECT_FORWARD_SECRECY | OCSP_STATUS | CLIENT_AUTH | NO_CLIENT_CERT ] = {
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_CERT, SERVER_CERT_STATUS, SERVER_KEY, SERVER_CERT_REQ, SERVER_HELLO_DONE,
            CLIENT_CERT, CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | PERFECT_FORWARD_SECRECY | OCSP_STATUS | CLIENT_AUTH | WITH_SESSION_TICKET ] = {
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_CERT, SERVER_CERT_STATUS, SERVER_KEY, SERVER_CERT_REQ, SERVER_HELLO_DONE,
            CLIENT_CERT, CLIENT_KEY, CLIENT_CERT_VERIFY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            SERVER_NEW_SESSION_TICKET, SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            APPLICATION_DATA
    },

    [NEGOTIATED | FULL_HANDSHAKE | PERFECT_FORWARD_SECRECY | OCSP_STATUS | CLIENT_AUTH | NO_CLIENT_CERT | WITH_SESSION_TICKET ] = {
            CLIENT_HELLO,
            SERVER_HELLO, SERVER_CERT, SERVER_CERT_STATUS, SERVER_KEY, SERVER_CERT_REQ, SERVER_HELLO_DONE,
            CLIENT_CERT, CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
            SERVER_NEW_SESSION_TICKET, SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED,
            APPLICATION_DATA
    },
};

static char handshake_type_str[128][MAX_HANDSHAKE_TYPE_LEN] = {0};

static const char* handshake_type_names[] = { 
    "NEGOTIATED|", 
    "FULL_HANDSHAKE|",
    "PERFECT_FORWARD_SECRECY|",
    "OCSP_STATUS|",
    "CLIENT_AUTH|",
    "WITH_SESSION_TICKET|",
    "NO_CLIENT_CERT|"
};

#define ACTIVE_MESSAGE( conn ) handshakes[ (conn)->handshake.handshake_type ][ (conn)->handshake.message_number ]
#define PREVIOUS_MESSAGE( conn ) handshakes[ (conn)->handshake.handshake_type ][ (conn)->handshake.message_number - 1 ]

#define ACTIVE_STATE( conn ) state_machine[ ACTIVE_MESSAGE( (conn) ) ]
#define PREVIOUS_STATE( conn ) state_machine[ PREVIOUS_MESSAGE( (conn) ) ]

#define EXPECTED_MESSAGE_TYPE( conn ) ACTIVE_STATE( conn ).message_type

/* Used in our test cases */
message_type_t s2n_conn_get_current_message_type(struct s2n_connection *conn)
{
    return ACTIVE_MESSAGE(conn);
}

static int s2n_advance_message(struct s2n_connection *conn)
{
    char this = 'S';
    if (conn->mode == S2N_CLIENT) {
        this = 'C';
    }

    /* Actually advance the message number */
    conn->handshake.message_number++;

    /* Set TCP_QUICKACK to avoid artificial dealy during the handshake */
    GUARD(s2n_socket_quickack(conn));

    /* If optimized io hasn't been enabled or if the caller started out with a corked socket,
     * we don't mess with it
     */
    if (!conn->corked_io || s2n_socket_was_corked(conn)) {
        return 0;
    }

    /* Are we changing I/O directions */
    if (ACTIVE_STATE(conn).writer == PREVIOUS_STATE(conn).writer) {
        return 0;
    }

    /* We're the new writer */
    if (ACTIVE_STATE(conn).writer == this) {
        if (s2n_connection_is_managed_corked(conn)) {
            /* Set TCP_CORK/NOPUSH */
            GUARD(s2n_socket_write_cork(conn));
        }

        return 0;
    }

    /* We're the new reader, or we reached the "B" writer stage indicating that
       we're at the application data stage  - uncork the data */
    if (s2n_connection_is_managed_corked(conn)) {
        GUARD(s2n_socket_write_uncork(conn));
    }

    return 0;
}

int s2n_generate_new_client_session_id(struct s2n_connection *conn)
{
    if (conn->mode == S2N_SERVER) {
        struct s2n_blob session_id = { .data = conn->session_id, .size = S2N_TLS_SESSION_ID_MAX_LEN };

        /* Generate a new session id */
        GUARD(s2n_get_public_random_data(&session_id));
        conn->session_id_len = S2N_TLS_SESSION_ID_MAX_LEN;
    }
    return 0;
}

int s2n_conn_set_handshake_type(struct s2n_connection *conn)
{
    /* A handshake type has been negotiated */
    conn->handshake.handshake_type = NEGOTIATED;

    if (conn->config->use_tickets) {
        if (conn->session_ticket_status == S2N_DECRYPT_TICKET) {
            if (!s2n_decrypt_session_ticket(conn)) {
                return 0;
            }

            if (s2n_config_is_encrypt_decrypt_key_available(conn->config) == 1) {
                conn->session_ticket_status = S2N_NEW_TICKET;
                conn->handshake.handshake_type |= WITH_SESSION_TICKET;
            }

            /* If a session ticket is presented by the client, then skip lookup in Session ID server cache */
            goto skip_cache_lookup;
        }

        if (conn->session_ticket_status == S2N_NEW_TICKET) {
            conn->handshake.handshake_type |= WITH_SESSION_TICKET;
        }
    }

    /* If a TLS session is resumed, the Server should respond in its ServerHello with the same SessionId the
     * Client sent in the ClientHello. */
    if (s2n_allowed_to_cache_connection(conn) && !s2n_resume_from_cache(conn)) {
        return 0;
    }

skip_cache_lookup:
    if (conn->mode == S2N_CLIENT && conn->client_session_resumed == 1) {
        return 0;
    }

    /* If we're doing full handshake, generate a new session id. */
    GUARD(s2n_generate_new_client_session_id(conn));

    /* If we get this far, it's a full handshake */
    conn->handshake.handshake_type |= FULL_HANDSHAKE;

    s2n_cert_auth_type client_cert_auth_type;
    GUARD(s2n_connection_get_client_auth_type(conn, &client_cert_auth_type));

    if (conn->mode == S2N_CLIENT && client_cert_auth_type == S2N_CERT_AUTH_REQUIRED) {
        /* If we're a client, and Client Auth is REQUIRED, then the Client must expect the CLIENT_CERT_REQ Message */
        conn->handshake.handshake_type |= CLIENT_AUTH;
    } else if (conn->mode == S2N_SERVER && client_cert_auth_type != S2N_CERT_AUTH_NONE) {
        /* If we're a server, and Client Auth is REQUIRED or OPTIONAL, then the server must send the CLIENT_CERT_REQ Message*/
        conn->handshake.handshake_type |= CLIENT_AUTH;
    }

    if (s2n_kex_is_ephemeral(conn->secure.cipher_suite->key_exchange_alg)) {
        conn->handshake.handshake_type |= PERFECT_FORWARD_SECRECY;
    }

    if (s2n_server_can_send_ocsp(conn) || s2n_server_sent_ocsp(conn)) {
        conn->handshake.handshake_type |= OCSP_STATUS;
    }

    return 0;
}

int s2n_conn_set_handshake_no_client_cert(struct s2n_connection *conn) {
    s2n_cert_auth_type client_cert_auth_type;
    GUARD(s2n_connection_get_client_auth_type(conn, &client_cert_auth_type));
    S2N_ERROR_IF(client_cert_auth_type != S2N_CERT_AUTH_OPTIONAL, S2N_ERR_BAD_MESSAGE);

    conn->handshake.handshake_type |= NO_CLIENT_CERT;
    return 0;
}

const char *s2n_connection_get_last_message_name(struct s2n_connection *conn)
{
    notnull_check_ptr(conn);
    
    return message_names[ACTIVE_MESSAGE(conn)];
}

const char *s2n_connection_get_handshake_type_name(struct s2n_connection *conn) 
{
    notnull_check_ptr(conn);

    int handshake_type = conn->handshake.handshake_type;

    if (handshake_type == INITIAL) {
        return "INITIAL";
    }

    if (handshake_type_str[handshake_type][0] != '\0') {
        return handshake_type_str[handshake_type];
    }

    /* Compute handshake_type_str[handshake_type] */
    char *p = handshake_type_str[handshake_type];
    char *end = p + sizeof(handshake_type_str[0]);

    for (int i = 0; i < sizeof(handshake_type_names) / sizeof(handshake_type_names[0]); ++i) {
        if (handshake_type & (1 << i)) {
            p = s2n_strcpy(p, end, handshake_type_names[i]);
        }
    }

    if (p != handshake_type_str[handshake_type] && '|' == *(p - 1)) {
        *(p - 1) = '\0';
    }

    return handshake_type_str[handshake_type];
}

static int s2n_conn_update_handshake_hashes(struct s2n_connection *conn, struct s2n_blob *data)
{
    if (s2n_handshake_is_hash_required(&conn->handshake, S2N_HASH_MD5)) {
        /* The handshake MD5 hash state will fail the s2n_hash_is_available() check
         * since MD5 is not permitted in FIPS mode. This check will not be used as
         * the handshake MD5 hash state is specifically used by the TLS 1.0 and TLS 1.1
         * PRF, which is required to comply with the TLS 1.0 and 1.1 RFCs and is approved
         * as per NIST Special Publication 800-52 Revision 1.
         */
        GUARD(s2n_hash_update(&conn->handshake.md5, data->data, data->size));
    }

    if (s2n_handshake_is_hash_required(&conn->handshake, S2N_HASH_SHA1)) {
        GUARD(s2n_hash_update(&conn->handshake.sha1, data->data, data->size));
    }

    const uint8_t md5_sha1_required = (s2n_handshake_is_hash_required(&conn->handshake, S2N_HASH_MD5) &&
                                       s2n_handshake_is_hash_required(&conn->handshake, S2N_HASH_SHA1));

    if (md5_sha1_required) {
        /* The MD5_SHA1 hash can still be used for TLS 1.0 and 1.1 in FIPS mode for 
         * the handshake hashes. This will only be used for the signature check in the
         * CertificateVerify message and the PRF. NIST SP 800-52r1 approves use
         * of MD5_SHA1 for these use cases (see footnotes 15 and 20, and section
         * 3.3.2) */
        GUARD(s2n_hash_update(&conn->handshake.md5_sha1, data->data, data->size));
    }

    if (s2n_handshake_is_hash_required(&conn->handshake, S2N_HASH_SHA224)) {
        GUARD(s2n_hash_update(&conn->handshake.sha224, data->data, data->size));
    }

    if (s2n_handshake_is_hash_required(&conn->handshake, S2N_HASH_SHA256)) {
        GUARD(s2n_hash_update(&conn->handshake.sha256, data->data, data->size));
    }

    if (s2n_handshake_is_hash_required(&conn->handshake, S2N_HASH_SHA384)) {
        GUARD(s2n_hash_update(&conn->handshake.sha384, data->data, data->size));
    }

    if (s2n_handshake_is_hash_required(&conn->handshake, S2N_HASH_SHA512)) {
        GUARD(s2n_hash_update(&conn->handshake.sha512, data->data, data->size));
    }

    return 0;
}

/* Writing is relatively straight forward, simply write each message out as a record,
 * we may fragment a message across multiple records, but we never coalesce multiple
 * messages into single records. 
 * Precondition: secure outbound I/O has already been flushed
 */
static int handshake_write_io(struct s2n_connection *conn)
{
    uint8_t record_type = ACTIVE_STATE(conn).record_type;
    s2n_blocked_status blocked = S2N_NOT_BLOCKED;

    /* Populate handshake.io with header/payload for the current state, once.
     * Check wiped instead of s2n_stuffer_data_available to differentiate between the initial call
     * to handshake_write_io and a repeated call after an EWOULDBLOCK.
     */
    if (conn->handshake.io.wiped == 1) {
        if (record_type == TLS_HANDSHAKE) {
            GUARD(s2n_handshake_write_header(conn, ACTIVE_STATE(conn).message_type));
        }
        GUARD(ACTIVE_STATE(conn).handler[conn->mode] (conn));
        if (record_type == TLS_HANDSHAKE) {
            GUARD(s2n_handshake_finish_header(conn));
        }
    }

    /* Write the handshake data to records in fragment sized chunks */
    struct s2n_blob out = {0};
    while (s2n_stuffer_data_available(&conn->handshake.io) > 0) {
        int max_payload_size;
        GUARD((max_payload_size = s2n_record_max_write_payload_size(conn)));
        out.size = MIN(s2n_stuffer_data_available(&conn->handshake.io), max_payload_size);

        out.data = s2n_stuffer_raw_read(&conn->handshake.io, out.size);
        notnull_check(out.data);

        /* Make the actual record */
        GUARD(s2n_record_write(conn, record_type, &out));

        /* MD5 and SHA sum the handshake data too */
        if (record_type == TLS_HANDSHAKE) {
            GUARD(s2n_conn_update_handshake_hashes(conn, &out));
        }

        /* Actually send the record. We could block here. Assume the caller will call flush before coming back. */
        GUARD(s2n_flush(conn, &blocked));
    }

    /* We're done sending the last record, reset everything */
    GUARD(s2n_stuffer_wipe(&conn->out));
    GUARD(s2n_stuffer_wipe(&conn->handshake.io));

    /* Advance the state machine */
    GUARD(s2n_advance_message(conn));

    return 0;
}

/*
 * Returns:
 *  1  - more data is needed to complete the handshake message.
 *  0  - we read the whole handshake message.
 * -1  - error processing the handshake message.
 */
static int read_full_handshake_message(struct s2n_connection *conn, uint8_t * message_type)
{
    uint32_t current_handshake_data = s2n_stuffer_data_available(&conn->handshake.io);
    if (current_handshake_data < TLS_HANDSHAKE_HEADER_LENGTH) {
        /* The message may be so badly fragmented that we don't even read the full header, take
         * what we can and then continue to the next record read iteration. 
         */
        if (s2n_stuffer_data_available(&conn->in) < (TLS_HANDSHAKE_HEADER_LENGTH - current_handshake_data)) {
            GUARD(s2n_stuffer_copy(&conn->in, &conn->handshake.io, s2n_stuffer_data_available(&conn->in)));
            return 1;
        }

        /* Get the remainder of the header */
        GUARD(s2n_stuffer_copy(&conn->in, &conn->handshake.io, (TLS_HANDSHAKE_HEADER_LENGTH - current_handshake_data)));
    }

    uint32_t handshake_message_length;
    GUARD(s2n_handshake_parse_header(conn, message_type, &handshake_message_length));

    S2N_ERROR_IF(handshake_message_length > S2N_MAXIMUM_HANDSHAKE_MESSAGE_LENGTH, S2N_ERR_BAD_MESSAGE);

    uint32_t bytes_to_take = handshake_message_length - s2n_stuffer_data_available(&conn->handshake.io);
    bytes_to_take = MIN(bytes_to_take, s2n_stuffer_data_available(&conn->in));

    /* If the record is handshake data, add it to the handshake buffer */
    GUARD(s2n_stuffer_copy(&conn->in, &conn->handshake.io, bytes_to_take));

    /* If we have the whole handshake message, then success */
    if (s2n_stuffer_data_available(&conn->handshake.io) == handshake_message_length) {
        return 0;
    }

    /* We don't have the whole message, so we'll need to go again */
    GUARD(s2n_stuffer_reread(&conn->handshake.io));
 
    return 1;
}

static int s2n_handshake_conn_update_hashes(struct s2n_connection *conn)
{
    uint8_t message_type;
    uint32_t handshake_message_length;

    GUARD(s2n_stuffer_reread(&conn->handshake.io));
    GUARD(s2n_handshake_parse_header(conn, &message_type, &handshake_message_length));

    struct s2n_blob handshake_record = {0};
    handshake_record.data = conn->handshake.io.blob.data;
    handshake_record.size = TLS_HANDSHAKE_HEADER_LENGTH + handshake_message_length;
    notnull_check(handshake_record.data);

    /* MD5 and SHA sum the handshake data too */
    GUARD(s2n_conn_update_handshake_hashes(conn, &handshake_record));

    return 0;
}

static int s2n_handshake_handle_sslv2(struct s2n_connection *conn)
{
    S2N_ERROR_IF(ACTIVE_MESSAGE(conn) != CLIENT_HELLO, S2N_ERR_BAD_MESSAGE);

    /* Add the message to our handshake hashes */
    struct s2n_blob hashed = {.data = conn->header_in.blob.data + 2,.size = 3 };
    GUARD(s2n_conn_update_handshake_hashes(conn, &hashed));

    hashed.data = conn->in.blob.data;
    hashed.size = s2n_stuffer_data_available(&conn->in);
    GUARD(s2n_conn_update_handshake_hashes(conn, &hashed));

    /* Handle an SSLv2 client hello */
    GUARD(s2n_stuffer_copy(&conn->in, &conn->handshake.io, s2n_stuffer_data_available(&conn->in)));
    GUARD(s2n_sslv2_client_hello_recv(conn));
    GUARD(s2n_stuffer_wipe(&conn->handshake.io));

    /* We're done with the record, wipe it */
    GUARD(s2n_stuffer_wipe(&conn->header_in));
    GUARD(s2n_stuffer_wipe(&conn->in));
    conn->in_status = ENCRYPTED;

    /* Advance the state machine */
    GUARD(s2n_advance_message(conn));

    return 0;
}

/* Reading is a little more complicated than writing as the TLS RFCs allow content
 * types to be interleaved at the record layer. We may get an alert message
 * during the handshake phase, or messages of types that we don't support (e.g.
 * HEARTBEAT messages), or during renegotiations we may even get application
 * data messages that need to be handled by the application. The latter is punted
 * for now (s2n does not support renegotiations).
 */
static int handshake_read_io(struct s2n_connection *conn)
{
    uint8_t record_type;
    int isSSLv2;

    GUARD(s2n_read_full_record(conn, &record_type, &isSSLv2));

    if (isSSLv2) {
        GUARD(s2n_handshake_handle_sslv2(conn));
    }

    /* Now we have a record, but it could be a partial fragment of a message, or it might
     * contain several messages.
     */
    S2N_ERROR_IF(record_type == TLS_APPLICATION_DATA, S2N_ERR_BAD_MESSAGE);
    if(record_type == TLS_CHANGE_CIPHER_SPEC) {
        S2N_ERROR_IF(s2n_stuffer_data_available(&conn->in) != 1, S2N_ERR_BAD_MESSAGE);

        GUARD(s2n_stuffer_copy(&conn->in, &conn->handshake.io, s2n_stuffer_data_available(&conn->in)));
        GUARD(ACTIVE_STATE(conn).handler[conn->mode] (conn));
        GUARD(s2n_stuffer_wipe(&conn->handshake.io));

        /* We're done with the record, wipe it */
        GUARD(s2n_stuffer_wipe(&conn->header_in));
        GUARD(s2n_stuffer_wipe(&conn->in));
        conn->in_status = ENCRYPTED;

        /* Advance the state machine */
        GUARD(s2n_advance_message(conn));

        return 0;
    } else if (record_type != TLS_HANDSHAKE) {
        if (record_type == TLS_ALERT) {
            GUARD(s2n_process_alert_fragment(conn));
        }

        /* Ignore record types that we don't support */

        /* We're done with the record, wipe it */
        GUARD(s2n_stuffer_wipe(&conn->header_in));
        GUARD(s2n_stuffer_wipe(&conn->in));
        conn->in_status = ENCRYPTED;
        return 0;
    }

    /* Record is a handshake message */
    while (s2n_stuffer_data_available(&conn->in)) {
        int r;
        uint8_t actual_handshake_message_type;
        GUARD((r = read_full_handshake_message(conn, &actual_handshake_message_type)));

        /* Do we need more data? */
        if (r == 1) {
            /* Break out of this inner loop, but since we're not changing the state, the
             * outer loop in s2n_handshake_io() will read another record. 
             */
            GUARD(s2n_stuffer_wipe(&conn->header_in));
            GUARD(s2n_stuffer_wipe(&conn->in));
            conn->in_status = ENCRYPTED;
            return 0;
        }

        s2n_cert_auth_type client_cert_auth_type;
        GUARD(s2n_connection_get_client_auth_type(conn, &client_cert_auth_type));

        /* If we're a Client, and received a ClientCertRequest message instead of a ServerHelloDone, and ClientAuth
         * is set to optional, then switch the State Machine that we're using to expect the ClientCertRequest. */
        if (conn->mode == S2N_CLIENT
                && client_cert_auth_type == S2N_CERT_AUTH_OPTIONAL
                && actual_handshake_message_type == TLS_CLIENT_CERT_REQ
                && EXPECTED_MESSAGE_TYPE(conn) == TLS_SERVER_HELLO_DONE) {
            conn->handshake.handshake_type |= CLIENT_AUTH;
        }

        /* According to rfc6066 section 8, server may choose not to send "CertificateStatus" message even if it has
         * sent "status_request" extension in the ServerHello message. */
        if (conn->mode == S2N_CLIENT
                && EXPECTED_MESSAGE_TYPE(conn) == TLS_SERVER_CERT_STATUS
                && actual_handshake_message_type != TLS_SERVER_CERT_STATUS) {
            conn->handshake.handshake_type &= ~OCSP_STATUS;
        }

        S2N_ERROR_IF(actual_handshake_message_type != EXPECTED_MESSAGE_TYPE(conn), S2N_ERR_BAD_MESSAGE);

        /* Call the relevant handler */
        r = ACTIVE_STATE(conn).handler[conn->mode] (conn);

        /* Don't update handshake hashes until after the handler has executed since some handlers need to read the
         * hash values before they are updated. */
        GUARD(s2n_handshake_conn_update_hashes(conn));

        GUARD(s2n_stuffer_wipe(&conn->handshake.io));

        if (r < 0) {
            GUARD(s2n_connection_kill(conn));

            return r;
        }

        /* Advance the state machine */
        GUARD(s2n_advance_message(conn));
    }

    /* We're done with the record, wipe it */
    GUARD(s2n_stuffer_wipe(&conn->header_in));
    GUARD(s2n_stuffer_wipe(&conn->in));
    conn->in_status = ENCRYPTED;

    return 0;
}

int s2n_negotiate(struct s2n_connection *conn, s2n_blocked_status * blocked)
{
    char this = 'S';
    if (conn->mode == S2N_CLIENT) {
        this = 'C';
    }

    while (ACTIVE_STATE(conn).writer != 'B') {
        /* Flush any pending I/O or alert messages */
        GUARD(s2n_flush(conn, blocked));

        if (ACTIVE_STATE(conn).writer == this) {
            *blocked = S2N_BLOCKED_ON_WRITE;
            if (handshake_write_io(conn) < 0 && s2n_errno != S2N_ERR_BLOCKED) {
                /* Non-retryable write error. The peer might have sent an alert. Try and read it. */
                const int write_errno = errno;
                const int write_s2n_errno = s2n_errno;
                const char *write_s2n_debug_str = s2n_debug_str;

                if (handshake_read_io(conn) < 0 && s2n_errno == S2N_ERR_ALERT) {
                    /* handshake_read_io has set s2n_errno */
                    return -1;
                } else {
                    /* Let the write error take precedence if we didn't read an alert. */
                    errno = write_errno;
                    s2n_errno = write_s2n_errno;
                    s2n_debug_str = write_s2n_debug_str;
                    return -1;
                }
            }
        } else {
            *blocked = S2N_BLOCKED_ON_READ;
            if (handshake_read_io(conn) < 0) {
                if (s2n_errno != S2N_ERR_BLOCKED && s2n_allowed_to_cache_connection(conn) && conn->session_id_len) {
                    conn->config->cache_delete(conn, conn->config->cache_delete_data, conn->session_id, conn->session_id_len);
                }

                return -1;
            }
        }

        /* If the handshake has just ended, free up memory */
        if (ACTIVE_STATE(conn).writer == 'B') {
            GUARD(s2n_stuffer_resize(&conn->handshake.io, 0));
        }
    }

    *blocked = S2N_NOT_BLOCKED;

    return 0;
}
