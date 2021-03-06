/*
 * Copyright (c) 2019 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <quic/quic_crypto.h>
#include <quic/quic.h>

#include <vnet/crypto/crypto.h>

#include <picotls/openssl.h>
#include <quicly.h>

typedef void (*quicly_do_transform_fn) (ptls_cipher_context_t *, void *,
					const void *, size_t);

struct cipher_context_t
{
  ptls_cipher_context_t super;
  vnet_crypto_op_t op;
  u32 key_index;
};

struct aead_crypto_context_t
{
  ptls_aead_context_t super;
  vnet_crypto_op_t op;
  u32 key_index;
};

vnet_crypto_main_t *cm = &crypto_main;

static void
vpp_crypto_cipher_do_init (ptls_cipher_context_t * _ctx, const void *iv)
{
  struct cipher_context_t *ctx = (struct cipher_context_t *) _ctx;

  vnet_crypto_op_id_t id;
  if (!strcmp (ctx->super.algo->name, "AES128-CTR"))
    {
      id = VNET_CRYPTO_OP_AES_128_CTR_ENC;
    }
  else if (!strcmp (ctx->super.algo->name, "AES256-CTR"))
    {
      id = VNET_CRYPTO_OP_AES_256_CTR_ENC;
    }
  else
    {
      QUIC_DBG (1, "%s, Invalid crypto cipher : ", __FUNCTION__,
		_ctx->algo->name);
      assert (0);
    }

  vnet_crypto_op_init (&ctx->op, id);
  ctx->op.iv = (u8 *) iv;
  ctx->op.key_index = ctx->key_index;
}

static void
vpp_crypto_cipher_dispose (ptls_cipher_context_t * _ctx)
{
  /* Do nothing */
}

static void
vpp_crypto_cipher_encrypt (ptls_cipher_context_t * _ctx, void *output,
			   const void *input, size_t _len)
{
  vlib_main_t *vm = vlib_get_main ();
  struct cipher_context_t *ctx = (struct cipher_context_t *) _ctx;

  ctx->op.src = (u8 *) input;
  ctx->op.dst = output;
  ctx->op.len = _len;

  vnet_crypto_process_ops (vm, &ctx->op, 1);
}

static int
vpp_crypto_cipher_setup_crypto (ptls_cipher_context_t * _ctx, int is_enc,
				const void *key, const EVP_CIPHER * cipher,
				quicly_do_transform_fn do_transform)
{
  struct cipher_context_t *ctx = (struct cipher_context_t *) _ctx;

  ctx->super.do_dispose = vpp_crypto_cipher_dispose;
  ctx->super.do_init = vpp_crypto_cipher_do_init;
  ctx->super.do_transform = do_transform;

  vlib_main_t *vm = vlib_get_main ();
  vnet_crypto_alg_t algo;
  if (!strcmp (ctx->super.algo->name, "AES128-CTR"))
    {
      algo = VNET_CRYPTO_ALG_AES_128_CTR;
    }
  else if (!strcmp (ctx->super.algo->name, "AES256-CTR"))
    {
      algo = VNET_CRYPTO_ALG_AES_256_CTR;
    }
  else
    {
      QUIC_DBG (1, "%s, Invalid crypto cipher : ", __FUNCTION__,
		_ctx->algo->name);
      assert (0);
    }

  ctx->key_index = vnet_crypto_key_add (vm, algo,
					(u8 *) key, _ctx->algo->key_size);

  return 0;
}

static int
aes128ctr_setup_crypto (ptls_cipher_context_t * ctx, int is_enc,
			const void *key)
{
  return vpp_crypto_cipher_setup_crypto (ctx, 1, key, EVP_aes_128_ctr (),
					 vpp_crypto_cipher_encrypt);
}

static int
aes256ctr_setup_crypto (ptls_cipher_context_t * ctx, int is_enc,
			const void *key)
{
  return vpp_crypto_cipher_setup_crypto (ctx, 1, key, EVP_aes_256_ctr (),
					 vpp_crypto_cipher_encrypt);
}

size_t
vpp_crypto_aead_encrypt (ptls_aead_context_t * _ctx, void *output,
			 const void *input, size_t inlen, uint64_t seq,
			 const void *iv, const void *aad, size_t aadlen)
{
  QUIC_DBG (1, "[quic] %s", __FUNCTION__);

  vlib_main_t *vm = vlib_get_main ();
  struct aead_crypto_context_t *ctx = (struct aead_crypto_context_t *) _ctx;

  vnet_crypto_op_id_t id;
  if (!strcmp (ctx->super.algo->name, "AES128-GCM"))
    {
      id = VNET_CRYPTO_OP_AES_128_GCM_ENC;
    }
  else if (!strcmp (ctx->super.algo->name, "AES256-GCM"))
    {
      id = VNET_CRYPTO_OP_AES_256_GCM_ENC;
    }
  else
    {
      assert (0);
    }

  vnet_crypto_op_init (&ctx->op, id);
  ctx->op.aad = (u8 *) aad;
  ctx->op.aad_len = aadlen;
  ctx->op.iv = (u8 *) iv;

  ctx->op.src = (u8 *) input;
  ctx->op.dst = output;
  ctx->op.key_index = ctx->key_index;
  ctx->op.len = inlen;

  ctx->op.tag_len = ctx->super.algo->tag_size;
  ctx->op.tag = ctx->op.src + inlen;

  vnet_crypto_process_ops (vm, &ctx->op, 1);

  return ctx->op.len + ctx->op.tag_len;
}

size_t
vpp_crypto_aead_decrypt (ptls_aead_context_t * _ctx, void *_output,
			 const void *input, size_t inlen, const void *iv,
			 const void *aad, size_t aadlen)
{
  QUIC_DBG (1, "[quic] %s", __FUNCTION__);

  vlib_main_t *vm = vlib_get_main ();
  struct aead_crypto_context_t *ctx = (struct aead_crypto_context_t *) _ctx;

  vnet_crypto_op_id_t id;
  if (!strcmp (ctx->super.algo->name, "AES128-GCM"))
    {
      id = VNET_CRYPTO_OP_AES_128_GCM_DEC;
    }
  else if (!strcmp (ctx->super.algo->name, "AES256-GCM"))
    {
      id = VNET_CRYPTO_OP_AES_256_GCM_DEC;
    }
  else
    {
      assert (0);
    }

  vnet_crypto_op_init (&ctx->op, id);
  ctx->op.aad = (u8 *) aad;
  ctx->op.aad_len = aadlen;
  ctx->op.iv = (u8 *) iv;

  ctx->op.src = (u8 *) input;
  ctx->op.dst = _output;
  ctx->op.key_index = ctx->key_index;
  ctx->op.len = inlen - ctx->super.algo->tag_size;

  ctx->op.tag_len = ctx->super.algo->tag_size;
  ctx->op.tag = ctx->op.src + ctx->op.len;

  vnet_crypto_process_ops (vm, &ctx->op, 1);

  return ctx->op.len;
}

static void
vpp_crypto_aead_dispose_crypto (ptls_aead_context_t * _ctx)
{
  QUIC_DBG (1, "[quic] %s", __FUNCTION__);
}

static int
vpp_crypto_aead_setup_crypto (ptls_aead_context_t * _ctx, int is_enc,
			      const void *key, const EVP_CIPHER * cipher)
{
  QUIC_DBG (1, "%s, algo : ", __FUNCTION__, _ctx->algo->name);

  vlib_main_t *vm = vlib_get_main ();
  struct aead_crypto_context_t *ctx = (struct aead_crypto_context_t *) _ctx;

  vnet_crypto_alg_t algo;
  if (!strcmp (ctx->super.algo->name, "AES128-GCM"))
    {
      algo = VNET_CRYPTO_ALG_AES_128_GCM;
    }
  else if (!strcmp (ctx->super.algo->name, "AES256-GCM"))
    {
      algo = VNET_CRYPTO_ALG_AES_256_GCM;
    }
  else
    {
      QUIC_DBG (1, "%s, algo : ", __FUNCTION__, _ctx->algo->name);
      assert (0);
    }

  ctx->super.do_decrypt = vpp_crypto_aead_decrypt;
  ctx->super.do_encrypt = vpp_crypto_aead_encrypt;
  ctx->super.dispose_crypto = vpp_crypto_aead_dispose_crypto;

  ctx->key_index = vnet_crypto_key_add (vm, algo,
					(u8 *) key, _ctx->algo->key_size);

  return 0;
}

static int
vpp_crypto_aead_aes128gcm_setup_crypto (ptls_aead_context_t * ctx, int is_enc,
					const void *key)
{
  return vpp_crypto_aead_setup_crypto (ctx, is_enc, key, EVP_aes_128_gcm ());
}

static int
vpp_crypto_aead_aes256gcm_setup_crypto (ptls_aead_context_t * ctx, int is_enc,
					const void *key)
{
  return vpp_crypto_aead_setup_crypto (ctx, is_enc, key, EVP_aes_256_gcm ());
}

ptls_cipher_algorithm_t vpp_crypto_aes128ctr = { "AES128-CTR",
  PTLS_AES128_KEY_SIZE,
  1, PTLS_AES_IV_SIZE,
  sizeof (struct cipher_context_t),
  aes128ctr_setup_crypto
};

ptls_cipher_algorithm_t vpp_crypto_aes256ctr = { "AES256-CTR",
  PTLS_AES256_KEY_SIZE,
  1 /* block size */ ,
  PTLS_AES_IV_SIZE,
  sizeof (struct cipher_context_t),
  aes256ctr_setup_crypto
};

ptls_aead_algorithm_t vpp_crypto_aes128gcm = { "AES128-GCM",
  &vpp_crypto_aes128ctr,
  NULL,
  PTLS_AES128_KEY_SIZE,
  PTLS_AESGCM_IV_SIZE,
  PTLS_AESGCM_TAG_SIZE,
  sizeof (struct aead_crypto_context_t),
  vpp_crypto_aead_aes128gcm_setup_crypto
};

ptls_aead_algorithm_t vpp_crypto_aes256gcm = { "AES256-GCM",
  &vpp_crypto_aes256ctr,
  NULL,
  PTLS_AES256_KEY_SIZE,
  PTLS_AESGCM_IV_SIZE,
  PTLS_AESGCM_TAG_SIZE,
  sizeof (struct aead_crypto_context_t),
  vpp_crypto_aead_aes256gcm_setup_crypto
};

ptls_cipher_suite_t vpp_crypto_aes128gcmsha256 =
  { PTLS_CIPHER_SUITE_AES_128_GCM_SHA256,
  &vpp_crypto_aes128gcm,
  &ptls_openssl_sha256
};

ptls_cipher_suite_t vpp_crypto_aes256gcmsha384 =
  { PTLS_CIPHER_SUITE_AES_256_GCM_SHA384,
  &vpp_crypto_aes256gcm,
  &ptls_openssl_sha384
};

ptls_cipher_suite_t *vpp_crypto_cipher_suites[] =
  { &vpp_crypto_aes256gcmsha384,
  &vpp_crypto_aes128gcmsha256,
  NULL
};

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
