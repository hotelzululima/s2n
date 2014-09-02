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

#include <openssl/rand.h>
#include <string.h>

#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_config.h"

#include "utils/s2n_random.h"
#include "utils/s2n_safety.h"
#include "utils/s2n_mem.h"

/* s2n's list of cipher suites, in order of preference, as of 2014-06-01 */
uint8_t wire_format_20140601[] =
    { TLS_DHE_RSA_WITH_AES_128_CBC_SHA256, TLS_DHE_RSA_WITH_AES_128_CBC_SHA, TLS_DHE_RSA_WITH_3DES_EDE_CBC_SHA, TLS_RSA_WITH_AES_128_CBC_SHA256, TLS_RSA_WITH_AES_128_CBC_SHA,
    TLS_RSA_WITH_3DES_EDE_CBC_SHA, TLS_RSA_WITH_RC4_128_SHA, TLS_RSA_WITH_RC4_128_MD5
};
struct s2n_cipher_preferences cipher_preferences_20140601 = {
    .count = 8,
    .wire_format = wire_format_20140601
};

struct s2n_cipher_preferences *s2n_cipher_preferences_20140601 = &cipher_preferences_20140601;
struct s2n_cipher_preferences *s2n_cipher_preferences_default = &cipher_preferences_20140601;

struct s2n_config s2n_default_config = {
    .cert_and_key_pairs = NULL,
    .cipher_preferences = &cipher_preferences_20140601
};

struct s2n_config *s2n_config_new(const char **err)
{
    struct s2n_blob allocator;
    struct s2n_config *new_config;

    if (s2n_alloc(&allocator, sizeof(struct s2n_config), err) < 0) {
        return NULL;
    }

    new_config = (struct s2n_config *)(void *)allocator.data;
    new_config->cert_and_key_pairs = NULL;

    if (s2n_alloc(&allocator, sizeof(struct s2n_cipher_preferences), err)) {
        return NULL;
    }

    new_config->cipher_preferences = (void *)allocator.data;

    if (s2n_alloc(&allocator, sizeof(wire_format_20140601), err) < 0) {
        return NULL;
    }

    new_config->cipher_preferences->count = s2n_cipher_preferences_default->count;
    new_config->cipher_preferences->wire_format = (void *)allocator.data;

    if (memcpy(allocator.data, wire_format_20140601, allocator.size) != allocator.data) {
        return NULL;
    }

    return new_config;
}

int s2n_config_free(struct s2n_config *config, const char **err)
{
    struct s2n_blob b = {.data = (uint8_t *) config,.size = sizeof(struct s2n_config) };

    return s2n_free(&b, err);
}

int s2n_config_add_cert_chain_and_key(struct s2n_config *config, char *cert_chain_pem, char *private_key_pem, const char **err)
{
    struct s2n_stuffer chain_in_stuffer, cert_out_stuffer, key_in_stuffer, key_out_stuffer;
    struct s2n_blob key_blob;
    struct s2n_blob mem;

    /* Allocate the memory for the chain and key struct */
    GUARD(s2n_alloc(&mem, sizeof(struct s2n_cert_chain_and_key), err));
    config->cert_and_key_pairs = (struct s2n_cert_chain_and_key *)(void *)mem.data;

    /* Put the private key pem in a stuffer */
    GUARD(s2n_stuffer_alloc_ro_from_string(&key_in_stuffer, private_key_pem, err));
    GUARD(s2n_stuffer_growable_alloc(&key_out_stuffer, strlen(private_key_pem), err));

    /* Convert pem to asn1 and asn1 to the private key */
    GUARD(s2n_stuffer_rsa_private_key_from_pem(&key_in_stuffer, &key_out_stuffer, err));
    key_blob.size = s2n_stuffer_data_available(&key_out_stuffer);
    key_blob.data = s2n_stuffer_raw_read(&key_out_stuffer, key_blob.size, err);
    notnull_check(key_blob.data);
    GUARD(s2n_asn1der_to_rsa_private_key(&config->cert_and_key_pairs->private_key, &key_blob, err));

    /* Turn the chain into a stuffer */
    GUARD(s2n_stuffer_alloc_ro_from_string(&chain_in_stuffer, cert_chain_pem, err));
    GUARD(s2n_stuffer_growable_alloc(&cert_out_stuffer, 2048, err));

    struct s2n_cert_chain **insert = &config->cert_and_key_pairs->head;
    uint32_t chain_size = 0;
    do {
        struct s2n_cert_chain *new_node;

        if (s2n_stuffer_certificate_from_pem(&chain_in_stuffer, &cert_out_stuffer, err) < 0) {
            if (chain_size == 0) {
                *err = "No certificates found in PEM";
                return -1;
            }
            break;
        }

        GUARD(s2n_alloc(&mem, sizeof(struct s2n_cert_chain), err));
        new_node = (struct s2n_cert_chain *)(void *)mem.data;

        GUARD(s2n_alloc(&new_node->cert, s2n_stuffer_data_available(&cert_out_stuffer), err));
        GUARD(s2n_stuffer_read(&cert_out_stuffer, &new_node->cert, err));

        /* Additional 3 bytes for the length field in the protocol */
        chain_size += new_node->cert.size + 3;
        new_node->next = NULL;
        *insert = new_node;
        insert = &new_node->next;
    } while (s2n_stuffer_data_available(&chain_in_stuffer));

    config->cert_and_key_pairs->chain_size = chain_size;

    /* Over-ride OpenSSL's PRNG. NOTE: there is a unit test to validate that this works */
    RAND_set_rand_method(&s2n_openssl_rand_method);

    return 0;
}

int s2n_config_add_dhparams(struct s2n_config *config, char *dhparams_pem, const char **err)
{
    struct s2n_stuffer dhparams_in_stuffer, dhparams_out_stuffer;
    struct s2n_blob dhparams_blob;
    struct s2n_blob mem;

    /* Allocate the memory for the chain and key struct */
    GUARD(s2n_alloc(&mem, sizeof(struct s2n_dh_params), err));
    config->dhparams = (struct s2n_dh_params *)(void *)mem.data;

    GUARD(s2n_stuffer_alloc_ro_from_string(&dhparams_in_stuffer, dhparams_pem, err));
    GUARD(s2n_stuffer_growable_alloc(&dhparams_out_stuffer, strlen(dhparams_pem), err));

    /* Convert pem to asn1 and asn1 to the private key */
    GUARD(s2n_stuffer_dhparams_from_pem(&dhparams_in_stuffer, &dhparams_out_stuffer, err));

    dhparams_blob.size = s2n_stuffer_data_available(&dhparams_out_stuffer);
    dhparams_blob.data = s2n_stuffer_raw_read(&dhparams_out_stuffer, dhparams_blob.size, err);
    notnull_check(dhparams_blob.data);

    GUARD(s2n_pkcs3_to_dh_params(config->dhparams, &dhparams_blob, err));

    return 0;
}