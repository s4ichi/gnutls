/*
 * OpenConnect (SSL + DTLS) VPN client
 *
 * Copyright © 2012 Free Software Foundation.
 * Copyright © 2008-2012 Intel Corporation.
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
 * Author: Nikos Mavrogiannopoulos
 *
 * GnuTLS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 *
 */

/*
 * TPM code based on client-tpm.c from
 * Carolin Latze <latze@angry-red-pla.net> and Tobias Soder
 */

#include <gnutls/gnutls.h>
#include <gnutls/abstract.h>
#include <gnutls/tpm.h>

#include <gnutls_int.h>
#include <gnutls_errors.h>
#include <pkcs11_int.h>
#include <x509/common.h>
#include <x509_b64.h>

#include <trousers/tss.h>
#include <trousers/trousers.h>

/* Signing function for TPM privkeys, set with gnutls_privkey_import_ext() */

struct tpm_ctx_st
{
  TSS_HCONTEXT tpm_context;
  TSS_HKEY tpm_key;
  TSS_HPOLICY tpm_key_policy;
  TSS_HKEY srk;
  TSS_HPOLICY srk_policy;
};

static void
tpm_deinit_fn (gnutls_privkey_t key, void *_s)
{
  struct tpm_ctx_st *s = _s;

  Tspi_Context_CloseObject (s->tpm_context, s->tpm_key_policy);
  Tspi_Context_CloseObject (s->tpm_context, s->tpm_key);
  Tspi_Context_CloseObject (s->tpm_context, s->srk_policy);
  Tspi_Context_CloseObject (s->tpm_context, s->srk);
  Tspi_Context_Close (s->tpm_context);
  gnutls_free (s);
}

static int
tpm_sign_fn (gnutls_privkey_t key, void *_s,
	     const gnutls_datum_t * data, gnutls_datum_t * sig)
{
  struct tpm_ctx_st *s = _s;
  TSS_HHASH hash;
  int err;

  _gnutls_debug_log ("TPM sign function called for %u bytes.\n",
		     data->size);

  err =
      Tspi_Context_CreateObject (s->tpm_context,
				 TSS_OBJECT_TYPE_HASH, TSS_HASH_OTHER,
				 &hash);
  if (err)
    {
      gnutls_assert ();
      _gnutls_debug_log ("Failed to create TPM hash object: %s\n",
			 Trspi_Error_String (err));
      return GNUTLS_E_PK_SIGN_FAILED;
    }
  err = Tspi_Hash_SetHashValue (hash, data->size, data->data);
  if (err)
    {
      gnutls_assert ();
      _gnutls_debug_log ("Failed to set value in TPM hash object: %s\n",
			 Trspi_Error_String (err));
      Tspi_Context_CloseObject (s->tpm_context, hash);
      return GNUTLS_E_PK_SIGN_FAILED;
    }
  err = Tspi_Hash_Sign (hash, s->tpm_key, &sig->size, &sig->data);
  Tspi_Context_CloseObject (s->tpm_context, hash);
  if (err)
    {
      if (s->tpm_key_policy || err != TPM_E_AUTHFAIL)
	_gnutls_debug_log ("TPM hash signature failed: %s\n",
			   Trspi_Error_String (err));
      if (err == TPM_E_AUTHFAIL)
	return GNUTLS_E_INSUFFICIENT_CREDENTIALS;
      else
	return GNUTLS_E_PK_SIGN_FAILED;
    }
  return 0;
}

static const unsigned char nullpass[20];

/**
 * gnutls_privkey_import_tpm_raw:
 * @pkey: The private key
 * @fdata: The TPM key to be imported
 * @format: The format of the private key
 * @srk_password: The password for the SRK key (optional)
 * @key_password: A password for the key (optional)
 *
 * This function will import the given private key to the abstract
 * #gnutls_privkey_t structure. If a password is needed to decrypt
 * the provided key or the provided password is wrong, then 
 * %GNUTLS_E_TPM_SRK_PASSWORD_ERROR is returned. If the TPM password
 * is wrong or not provided then %GNUTLS_E_TPM_KEY_PASSWORD_ERROR
 * is returned. 
 *
 * Returns: On success, %GNUTLS_E_SUCCESS (0) is returned, otherwise a
 *   negative error value.
 *
 * Since: 3.1.0
 *
 **/
int
gnutls_privkey_import_tpm_raw (gnutls_privkey_t pkey,
			       const gnutls_datum_t * fdata,
			       gnutls_x509_crt_fmt_t format,
			       const char *srk_password,
			       const char *key_password)
{
  static const TSS_UUID SRK_UUID = TSS_UUID_SRK;
  gnutls_datum_t asn1;
  size_t slen;
  int err, ret;
  struct tpm_ctx_st *s;
  gnutls_datum_t tmp_sig;

  ret = gnutls_pem_base64_decode_alloc ("TSS KEY BLOB", fdata, &asn1);
  if (ret)
    {
      gnutls_assert ();
      _gnutls_debug_log ("Error decoding TSS key blob: %s\n",
			 gnutls_strerror (ret));
      return ret;
    }

  slen = asn1.size;
  ret = _gnutls_x509_decode_octet_string(NULL, asn1.data, asn1.size, asn1.data, &slen);
  if (ret < 0)
    {
      gnutls_assert();
      goto out_blob;
    }
  asn1.size = slen;

  s = gnutls_malloc (sizeof (*s));
  if (s == NULL)
    {
      gnutls_assert ();
      ret = GNUTLS_E_MEMORY_ERROR;
      goto out_blob;
    }

  err = Tspi_Context_Create (&s->tpm_context);
  if (err)
    {
      gnutls_assert ();
      _gnutls_debug_log ("Failed to create TPM context: %s\n",
			 Trspi_Error_String (err));
      ret = GNUTLS_E_TPM_ERROR;
      goto out_ctx;
    }
  err = Tspi_Context_Connect (s->tpm_context, NULL);
  if (err)
    {
      gnutls_assert ();
      _gnutls_debug_log ("Failed to connect TPM context: %s\n",
			 Trspi_Error_String (err));
      ret = GNUTLS_E_TPM_ERROR;
      goto out_tspi_ctx;
    }
  err =
      Tspi_Context_LoadKeyByUUID (s->tpm_context, TSS_PS_TYPE_SYSTEM,
				  SRK_UUID, &s->srk);
  if (err)
    {
      gnutls_assert ();
      _gnutls_debug_log
	  ("Failed to load TPM SRK key: %s\n", Trspi_Error_String (err));
      ret = GNUTLS_E_TPM_ERROR;
      goto out_tspi_ctx;
    }
  err = Tspi_GetPolicyObject (s->srk, TSS_POLICY_USAGE, &s->srk_policy);
  if (err)
    {
      gnutls_assert ();
      _gnutls_debug_log ("Failed to load TPM SRK policy object: %s\n",
			 Trspi_Error_String (err));
      ret = GNUTLS_E_TPM_ERROR;
      goto out_srk;
    }

  /* We don't seem to get the error here... */
  if (srk_password)
    err = Tspi_Policy_SetSecret (s->srk_policy,
				 TSS_SECRET_MODE_PLAIN,
				 strlen (srk_password), (BYTE *) srk_password);
  else				/* Well-known NULL key */
    err = Tspi_Policy_SetSecret (s->srk_policy,
				 TSS_SECRET_MODE_SHA1,
				 sizeof (nullpass), (BYTE *) nullpass);
  if (err)
    {
      gnutls_assert ();
      _gnutls_debug_log ("Failed to set TPM PIN: %s\n",
			 Trspi_Error_String (err));
      ret = GNUTLS_E_TPM_ERROR;
      goto out_srkpol;
    }

  /* ... we get it here instead. */
  err = Tspi_Context_LoadKeyByBlob (s->tpm_context, s->srk,
				    asn1.size, asn1.data, &s->tpm_key);
  if (err != 0)
    {
      if (srk_password)
	{
	  gnutls_assert ();
	  _gnutls_debug_log
	      ("Failed to load TPM key blob: %s\n",
	       Trspi_Error_String (err));
	}

      if (err != TPM_E_AUTHFAIL)
	{
	  gnutls_assert ();
	  ret = GNUTLS_E_TPM_ERROR;
	  goto out_srkpol;
	}
      else
	{
	  ret = gnutls_assert_val (GNUTLS_E_TPM_SRK_PASSWORD_ERROR);
	  goto out_srkpol;
	}
    }

  ret =
      gnutls_privkey_import_ext2 (pkey, GNUTLS_PK_RSA, s,
				  tpm_sign_fn, NULL, tpm_deinit_fn, 0);
  if (ret < 0)
    {
      gnutls_assert ();
      goto out_srkpol;
    }

  ret =
      gnutls_privkey_sign_data (pkey, GNUTLS_DIG_SHA1, 0, fdata, &tmp_sig);
  if (ret == GNUTLS_E_INSUFFICIENT_CREDENTIALS)
    {
      if (!s->tpm_key_policy)
	{
	  err = Tspi_Context_CreateObject (s->tpm_context,
					   TSS_OBJECT_TYPE_POLICY,
					   TSS_POLICY_USAGE,
					   &s->tpm_key_policy);
	  if (err)
	    {
	      gnutls_assert ();
	      _gnutls_debug_log
		  ("Failed to create key policy object: %s\n",
		   Trspi_Error_String (err));
	      ret = GNUTLS_E_TPM_ERROR;
	      goto out_key;
	    }

	  err = Tspi_Policy_AssignToObject (s->tpm_key_policy, s->tpm_key);
	  if (err)
	    {
	      gnutls_assert ();
	      _gnutls_debug_log ("Failed to assign policy to key: %s\n",
				 Trspi_Error_String (err));
	      ret = GNUTLS_E_TPM_ERROR;
	      goto out_key_policy;
	    }
	}

      err = Tspi_Policy_SetSecret (s->tpm_key_policy,
				   TSS_SECRET_MODE_PLAIN,
				   strlen (key_password), (void *) key_password);

      if (err)
	{
	  gnutls_assert ();
	  _gnutls_debug_log ("Failed to set key PIN: %s\n",
			     Trspi_Error_String (err));
          ret = GNUTLS_E_TPM_KEY_PASSWORD_ERROR;
	  goto out_key_policy;
	}
    }
  else if (ret < 0)
    {
      gnutls_assert ();
      goto out_blob;
    }

  gnutls_free (asn1.data);
  return 0;
out_key_policy:
  Tspi_Context_CloseObject (s->tpm_context, s->tpm_key_policy);
  s->tpm_key_policy = 0;
out_key:
  Tspi_Context_CloseObject (s->tpm_context, s->tpm_key);
  s->tpm_key = 0;
out_srkpol:
  Tspi_Context_CloseObject (s->tpm_context, s->srk_policy);
  s->srk_policy = 0;
out_srk:
  Tspi_Context_CloseObject (s->tpm_context, s->srk);
  s->srk = 0;
out_tspi_ctx:
  Tspi_Context_Close (s->tpm_context);
  s->tpm_context = 0;
out_ctx:
  gnutls_free (s);
out_blob:
  gnutls_free (asn1.data);
  return ret;
}

const TSS_UUID srk_uuid = TSS_UUID_SRK;

/**
 * gnutls_tpm_privkey_generate:
 * @pk: the public key algorithm
 * @bits: the security bits
 * @srk_password: a password to protect the exported key (optional)
 * @key_password: the password for the TPM (optional)
 * @privkey: the generated key
 * @pubkey: the corresponding public key
 * @flags: should be a list of %GNUTLS_TPM flags
 *
 * This function will generate a private key in the TPM
 * chip. The private key will be generated within the chip
 * and will be exported in a wrapped with TPM's master key
 * form. Furthermore the wrapped key can be protected with
 * the provided @password.
 *
 * Note that bits in TPM is quantized value. Allowed values are 512,
 * 1024, 2048, 4096, 8192 and 16384.
 *
 * Allowed flags are %GNUTLS_TPM_SIG_PKCS1V15 and %GNUTLS_TPM_SIG_PKCS1V15_SHA1.
 *
 * Returns: On success, %GNUTLS_E_SUCCESS (0) is returned, otherwise a
 *   negative error value.
 *
 * Since: 3.1.0
 **/
int
gnutls_tpm_privkey_generate (gnutls_pk_algorithm_t pk, unsigned int bits, 
                             const char* srk_password,
                             const char* key_password,
                             gnutls_x509_crt_fmt_t format,
                             gnutls_datum_t* privkey, 
                             gnutls_datum_t* pubkey,
                             unsigned int flags)
{
TSS_HCONTEXT ctx;
TSS_FLAG tpm_flags = TSS_KEY_TYPE_LEGACY | TSS_KEY_VOLATILE;
TSS_HKEY key_ctx; 
TSS_HKEY srk_ctx;
TSS_RESULT tssret;
int ret;
void* tdata;
UINT32 tint;
gnutls_datum_t tmpkey;
TSS_HPOLICY srk_policy, key_policy;
unsigned int sig;
gnutls_pubkey_t pub;

  switch(bits) {
    case 512:
      tpm_flags |= TSS_KEY_SIZE_512;
      break;
    case 1024:
      tpm_flags |= TSS_KEY_SIZE_1024;
      break;
    case 2048:
      tpm_flags |= TSS_KEY_SIZE_2048;
      break;
    case 4096:
      tpm_flags |= TSS_KEY_SIZE_4096;
      break;
    case 8192:
      tpm_flags |= TSS_KEY_SIZE_8192;
      break;
    case 16384:
      tpm_flags |= TSS_KEY_SIZE_16384;
      break;
    default:
      return gnutls_assert_val(GNUTLS_E_INVALID_REQUEST);
  }

  tssret = Tspi_Context_Create(&ctx);
  if (tssret != 0)
    {
      gnutls_assert();
      return GNUTLS_E_TPM_ERROR;
    }
    
  tssret = Tspi_Context_Connect(ctx, NULL);
  if (tssret != 0)
    {
      gnutls_assert();
      ret = GNUTLS_E_TPM_ERROR;
      goto err_cc;
    }

  tssret = Tspi_Context_CreateObject(ctx, TSS_OBJECT_TYPE_RSAKEY, tpm_flags, &key_ctx);
  if (tssret != 0)
    {
      gnutls_assert();
      ret = GNUTLS_E_TPM_ERROR;
      goto err_cc;
    }
    
  if (flags & GNUTLS_TPM_SIG_PKCS1V15_SHA1)
    sig = TSS_SS_RSASSAPKCS1V15_SHA1;
  else
    sig = TSS_SS_RSASSAPKCS1V15_DER;

  tssret = Tspi_SetAttribUint32(key_ctx, TSS_TSPATTRIB_KEY_INFO, TSS_TSPATTRIB_KEYINFO_SIGSCHEME,
                                sig);
  if (tssret != 0)
    {
      gnutls_assert();
      ret = GNUTLS_E_TPM_ERROR;
      goto err_sa;
    }
  
  tssret = Tspi_Context_LoadKeyByUUID(ctx, TSS_PS_TYPE_SYSTEM, srk_uuid,
                                   &srk_ctx);
  if (tssret != 0)
    {
      gnutls_assert();
      ret = GNUTLS_E_TPM_ERROR;
      goto err_sa;
    }

  /* set SRK key */
  tssret = Tspi_GetPolicyObject(srk_ctx, TSS_POLICY_USAGE, &srk_policy);
  if (tssret != 0)
    {
      gnutls_assert();
      ret = GNUTLS_E_TPM_ERROR;
      goto err_sa;
    }

  if (srk_password == NULL)
    {
      tssret = Tspi_Policy_SetSecret(srk_policy, TSS_SECRET_MODE_SHA1,
                                     20, (void*)nullpass);
    }
  else
    {
      tssret = Tspi_Policy_SetSecret(srk_policy, TSS_SECRET_MODE_PLAIN,
                                     strlen(srk_password), (void*)srk_password);
    }
  
  if (tssret != 0)
    {
      gnutls_assert();
      ret = GNUTLS_E_TPM_SRK_PASSWORD_ERROR;
      goto err_sa;
    }
    
  /* set the key of the actual key */
  if (key_password)
    {
      tssret = Tspi_GetPolicyObject(key_ctx, TSS_POLICY_USAGE, &key_policy);
      if (tssret != 0)
        {
          gnutls_assert();
          ret = GNUTLS_E_TPM_ERROR;
          goto err_sa;
        }

      tssret = Tspi_Policy_SetSecret(key_policy, TSS_SECRET_MODE_PLAIN, 
                                     strlen(key_password), (void*)key_password);
      if (tssret != 0)
        {
          gnutls_assert();
          ret = GNUTLS_E_TPM_ERROR;
          goto err_sa;
        }
    }

  tssret = Tspi_Key_CreateKey(key_ctx, srk_ctx, 0);
  if (tssret != 0)
    {
      gnutls_assert();
      if (tssret == TPM_E_AUTHFAIL)
        ret = GNUTLS_E_TPM_SRK_PASSWORD_ERROR;
      else
        ret = GNUTLS_E_TPM_ERROR;
      goto err_sa;
    }

  tssret = Tspi_GetAttribData(key_ctx, TSS_TSPATTRIB_KEY_BLOB,
                              TSS_TSPATTRIB_KEYBLOB_BLOB, &tint, (void*)&tdata);
  if (tssret != 0)
    {
      gnutls_assert();
      ret = GNUTLS_E_TPM_ERROR;
      goto err_sa;
    }

  ret = _gnutls_x509_encode_octet_string(tdata, tint, &tmpkey);
  if (ret < 0)
    {
      gnutls_assert();
      goto cleanup;
    }
  
  if (format == GNUTLS_X509_FMT_PEM)
    {
      ret = _gnutls_fbase64_encode ("TSS KEY BLOB", tmpkey.data, tmpkey.size, privkey);
      if (ret < 0)
        {
          gnutls_assert();
          goto cleanup;
        }
    }
  else
    {
      privkey->data = tmpkey.data;
      privkey->size = tmpkey.size;
      tmpkey.data = NULL;
    }

  {
    gnutls_datum_t m, e;
    size_t psize;

    ret = gnutls_pubkey_init(&pub);
    if (ret < 0)
      {
        gnutls_assert();
        goto privkey_cleanup;
      }
    
    tssret = Tspi_GetAttribData(key_ctx, TSS_TSPATTRIB_RSAKEY_INFO,
                                TSS_TSPATTRIB_KEYINFO_RSA_MODULUS, &tint, (void*)&tdata);
    if (tssret != 0)
      {
        gnutls_assert();
        ret = GNUTLS_E_TPM_ERROR;
        goto pubkey_cleanup;
      }
    
    m.data = tdata;
    m.size = tint;

    tssret = Tspi_GetAttribData(key_ctx, TSS_TSPATTRIB_RSAKEY_INFO,
                                TSS_TSPATTRIB_KEYINFO_RSA_EXPONENT, &tint, (void*)&tdata);
    if (tssret != 0)
      {
        gnutls_assert();
        Tspi_Context_FreeMemory(key_ctx, m.data);
        ret = GNUTLS_E_TPM_ERROR;
        goto pubkey_cleanup;
      }
    
    e.data = tdata;
    e.size = tint;
    
    ret = gnutls_pubkey_import_rsa_raw(pub, &m, &e);
    if (ret < 0)
      {
        gnutls_assert();
        goto pubkey_cleanup;
      }
    
    psize = e.size+m.size+512;
    pubkey->data = gnutls_malloc(psize);
    if (pubkey->data == NULL)
      {
        gnutls_assert();
        ret = GNUTLS_E_MEMORY_ERROR;
        goto pubkey_cleanup;
      }
    
    ret = gnutls_pubkey_export(pub, format, pubkey->data, &psize);
    if (ret < 0)
      {
        gnutls_assert();
        goto pubkey_cleanup;
      }
    pubkey->size = psize;

    gnutls_pubkey_deinit(pub);
  }

  ret = 0;
  goto cleanup;
  
pubkey_cleanup:
  gnutls_pubkey_deinit(pub);
privkey_cleanup:
  gnutls_free(privkey->data);
  privkey->data = NULL;
cleanup:  
  gnutls_free(tmpkey.data);
  tmpkey.data = NULL;
err_sa:
  Tspi_Context_CloseObject(ctx, key_ctx);
err_cc:
  Tspi_Context_Close(ctx);
  return ret;
}
