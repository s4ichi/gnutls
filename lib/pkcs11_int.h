#ifndef PKCS11_INT_H
# define PKCS11_INT_H

#include <pakchois/pakchois.h>
#include <gnutls/pkcs11.h>

#define PKCS11_ID_SIZE 128
#define PKCS11_LABEL_SIZE 128

typedef struct token_creds {
	char pin[GNUTLS_PKCS11_MAX_PIN_LEN];
	size_t pin_size;
} token_creds_st;

struct token_info {
	struct ck_token_info tinfo;
	struct ck_slot_info sinfo;
	ck_slot_id_t sid;
	struct gnutls_pkcs11_provider_s *prov;
};

struct pkcs11_url_info {
	/* everything here is null terminated strings */
	opaque id[PKCS11_ID_SIZE * 3 + 1];	/* hex with delimiters */
	opaque type[16];	/* cert/key etc. */
	opaque
	    manufacturer[sizeof
			 (((struct ck_token_info *) NULL)->
			  manufacturer_id) + 1];
	opaque token[sizeof(((struct ck_token_info *) NULL)->label) + 1];
	opaque
	    serial[sizeof(((struct ck_token_info *) NULL)->serial_number) +
		   1];
	opaque model[sizeof(((struct ck_token_info *) NULL)->model) + 1];
	opaque label[PKCS11_LABEL_SIZE + 1];

	opaque certid_raw[PKCS11_ID_SIZE];	/* same as ID but raw */
	size_t certid_raw_size;
};

struct gnutls_pkcs11_obj_st {
	gnutls_datum_t raw;
	gnutls_pkcs11_obj_type_t type;
	struct pkcs11_url_info info;

	/* only when pubkey */
	gnutls_datum_t pubkey[MAX_PUBLIC_PARAMS_SIZE];
	gnutls_pk_algorithm pk_algorithm;
	unsigned int key_usage;
};

/* thus function is called for every token in the traverse_tokens
 * function. Once everything is traversed it is called with NULL tinfo.
 * It should return 0 if found what it was looking for.
 */
typedef int (*find_func_t) (pakchois_session_t * pks,
			    struct token_info * tinfo, void *input);

int pkcs11_rv_to_err(ck_rv_t rv);
int pkcs11_url_to_info(const char *url, struct pkcs11_url_info *info);

int pkcs11_get_info(struct pkcs11_url_info *info,
		    gnutls_pkcs11_obj_info_t itype, void *output,
		    size_t * output_size);
int pkcs11_login(pakchois_session_t * pks, struct token_info *info,
		 token_creds_st *);

extern gnutls_pkcs11_token_callback_t token_func;
extern void *token_data;

void pkcs11_rescan_slots(void);
int pkcs11_info_to_url(const struct pkcs11_url_info *info, char **url);

#define SESSION_WRITE 1
#define SESSION_LOGIN 2
int pkcs11_open_session(pakchois_session_t ** _pks,
			struct pkcs11_url_info *info,
			token_creds_st * creds, unsigned int flags);
int _pkcs11_traverse_tokens(find_func_t find_func, void *input,
			    unsigned int flags);
ck_object_class_t pkcs11_strtype_to_class(const char *type);

int pkcs11_token_matches_info(struct pkcs11_url_info *info,
			      struct ck_token_info *tinfo);

/* flags are SESSION_* */
int pkcs11_find_object(pakchois_session_t ** _pks,
		       ck_object_handle_t * _obj,
		       struct pkcs11_url_info *info, token_creds_st *,
		       unsigned int flags);

unsigned int pkcs11_obj_flags_to_int(unsigned int flags);

#endif
