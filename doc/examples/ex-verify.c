/* This example code is placed in the public domain. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

#include "examples.h"

/* All the available CRLs
 */
gnutls_x509_crl_t *crl_list;
int crl_list_size;

/* All the available trusted CAs
 */
gnutls_x509_crt_t *ca_list;
int ca_list_size;

static int print_details_func(gnutls_x509_crt_t cert,
    gnutls_x509_crt_t issuer, gnutls_x509_crl_t crl, 
    unsigned int verification_output);

/* This function will try to verify the peer's certificate chain, and
 * also check if the hostname matches.
 */
void
verify_certificate_chain (const char *hostname,
                          const gnutls_datum_t * cert_chain,
                          int cert_chain_length)
{
  int i;
  gnutls_x509_trust_list_t tlist;
  gnutls_x509_crt_t *cert;
  
  unsigned int output;

  /* Initialize the trusted certificate list. This should be done
   * once on initialization. gnutls_x509_crt_list_import2() and
   * gnutls_x509_crl_list_import2() can be used to load them.
   */
  gnutls_x509_trust_list_init(&tlist, 0);

  gnutls_x509_trust_list_add_cas(tlist, ca_list, ca_list_size, 0);
  gnutls_x509_trust_list_add_crls(tlist, crl_list, crl_list_size, 
    GNUTLS_TL_VERIFY_CRL, 0);

  cert = malloc (sizeof (*cert) * cert_chain_length);

  /* Import all the certificates in the chain to
   * native certificate format.
   */
  for (i = 0; i < cert_chain_length; i++)
    {
      gnutls_x509_crt_init (&cert[i]);
      gnutls_x509_crt_import (cert[i], &cert_chain[i], GNUTLS_X509_FMT_DER);
    }

  gnutls_x509_trust_list_verify_crt(tlist, cert, cert_chain_length, 0, 
    &output, print_details_func);

  if (output & GNUTLS_CERT_INVALID)
    {
      fprintf (stderr, "Not trusted");

      if (output & GNUTLS_CERT_SIGNER_NOT_FOUND)
        fprintf (stderr, ": no issuer was found");
      if (output & GNUTLS_CERT_SIGNER_NOT_CA)
        fprintf (stderr, ": issuer is not a CA");
      if (output & GNUTLS_CERT_NOT_ACTIVATED)
        fprintf (stderr, ": not yet activated\n");
      if (output & GNUTLS_CERT_EXPIRED)
        fprintf (stderr, ": expired\n");

      fprintf (stderr, "\n");
    }
  else
    fprintf (stderr, "Trusted\n");

  /* Check if the name in the first certificate matches our destination!
   */
  if (!gnutls_x509_crt_check_hostname (cert[0], hostname))
    {
      printf ("The certificate's owner does not match hostname '%s'\n",
              hostname);
    }

  gnutls_x509_trust_list_deinit(tlist, 1);

  return;
}

static int print_details_func(gnutls_x509_crt_t cert,
    gnutls_x509_crt_t issuer, gnutls_x509_crl_t crl, 
    unsigned int verification_output)
{
  char name[512];
  char issuer_name[512];
  size_t name_size;
  size_t issuer_name_size;

  issuer_name_size = sizeof (issuer_name);
  gnutls_x509_crt_get_issuer_dn (cert, issuer_name, &issuer_name_size);

  name_size = sizeof (name);
  gnutls_x509_crt_get_dn (cert, name, &name_size);

  fprintf (stdout, "\tSubject: %s\n", name);
  fprintf (stdout, "\tIssuer: %s\n", issuer_name);

  if (issuer != NULL)
    {
      issuer_name_size = sizeof (issuer_name);
      gnutls_x509_crt_get_dn (issuer, issuer_name, &issuer_name_size);

      fprintf (stdout, "\tVerified against: %s\n", issuer_name);
    }

  if (crl != NULL)
    {
      issuer_name_size = sizeof (issuer_name);
      gnutls_x509_crl_get_issuer_dn (crl, issuer_name, &issuer_name_size);

      fprintf (stdout, "\tVerified against CRL of: %s\n", issuer_name);
    }

  fprintf (stdout, "\tVerification output: %x\n\n", verification_output);

  return 0;
}
