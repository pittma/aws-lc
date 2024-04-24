/*
 * Written by Dr Stephen N Henson (steve@openssl.org) for the OpenSSL project
 * 2004.
 */
/* ====================================================================
 * Copyright (c) 2004 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit. (http://www.OpenSSL.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    licensing@OpenSSL.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *    nor may "OpenSSL" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.OpenSSL.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This product includes cryptographic software written by Eric Young
 * (eay@cryptsoft.com).  This product includes software written by Tim
 * Hudson (tjh@cryptsoft.com). */

#include <string.h>

#include <openssl/mem.h>
#include <openssl/obj.h>
#include <openssl/stack.h>
#include <openssl/x509.h>

#include "../internal.h"
#include "internal.h"


// X509_VERIFY_PARAM functions

#define SET_HOST 0
#define ADD_HOST 1

static void str_free(char *s) { OPENSSL_free(s); }

static int int_x509_param_set_hosts(X509_VERIFY_PARAM *param, int mode,
                                    const char *name, size_t namelen) {
  char *copy;

  // Setting 0 to automatically detect the length of |name| is an OpenSSL quirk
  // that AWS-LC isn't keen on supporting. However, consumers often assume
  // OpenSSL semantics from AWS-LC, so it's supported in this case.
  if (name != NULL && namelen == 0) {
    namelen = strlen(name);
  }

  // Refuse names with embedded NUL bytes.
  // XXX: Do we need to push an error onto the error stack?
  if (name && OPENSSL_memchr(name, '\0', namelen)) {
    return 0;
  }

  if (mode == SET_HOST && param->hosts) {
    sk_OPENSSL_STRING_pop_free(param->hosts, str_free);
    param->hosts = NULL;
  }
  // OpenSSL returns 1 when trying to set or add an empty name. This is also a
  // quirk that AWS-LC isn't keen on supporting, but we maintain for backwards
  // compatibility.
  if (name == NULL || namelen == 0) {
    return 1;
  }

  copy = OPENSSL_strndup(name, namelen);
  if (copy == NULL) {
    return 0;
  }

  if (param->hosts == NULL &&
      (param->hosts = sk_OPENSSL_STRING_new_null()) == NULL) {
    OPENSSL_free(copy);
    return 0;
  }

  if (!sk_OPENSSL_STRING_push(param->hosts, copy)) {
    OPENSSL_free(copy);
    if (sk_OPENSSL_STRING_num(param->hosts) == 0) {
      sk_OPENSSL_STRING_free(param->hosts);
      param->hosts = NULL;
    }
    return 0;
  }

  return 1;
}

X509_VERIFY_PARAM *X509_VERIFY_PARAM_new(void) {
  X509_VERIFY_PARAM *param = OPENSSL_zalloc(sizeof(X509_VERIFY_PARAM));
  if (!param) {
    return NULL;
  }
  param->depth = -1;
  // TODO(crbug.com/boringssl/441): This line was commented out. Figure out what
  // this was for:
  // param->inh_flags = X509_VP_FLAG_DEFAULT;
  return param;
}

void X509_VERIFY_PARAM_free(X509_VERIFY_PARAM *param) {
  if (param == NULL) {
    return;
  }
  sk_ASN1_OBJECT_pop_free(param->policies, ASN1_OBJECT_free);
  sk_OPENSSL_STRING_pop_free(param->hosts, str_free);
  OPENSSL_free(param->email);
  OPENSSL_free(param->ip);
  OPENSSL_free(param);
}

//-
// This function determines how parameters are "inherited" from one structure
// to another. There are several different ways this can happen.
//
// 1. If a child structure needs to have its values initialized from a parent
//    they are simply copied across. For example SSL_CTX copied to SSL.
// 2. If the structure should take on values only if they are currently unset.
//    For example the values in an SSL structure will take appropriate value
//    for SSL servers or clients but only if the application has not set new
//    ones.
//
// The "inh_flags" field determines how this function behaves.
//
// Normally any values which are set in the default are not copied from the
// destination and verify flags are ORed together.
//
// If X509_VP_FLAG_DEFAULT is set then anything set in the source is copied
// to the destination. Effectively the values in "to" become default values
// which will be used only if nothing new is set in "from".
//
// If X509_VP_FLAG_OVERWRITE is set then all value are copied across whether
// they are set or not. Flags is still Ored though.
//
// If X509_VP_FLAG_RESET_FLAGS is set then the flags value is copied instead
// of ORed.
//
// If X509_VP_FLAG_LOCKED is set then no values are copied.
//
// If X509_VP_FLAG_ONCE is set then the current inh_flags setting is zeroed
// after the next call.

// Macro to test if a field should be copied from src to dest

#define test_x509_verify_param_copy(field, def) \
  (to_overwrite ||                              \
   ((src->field != (def)) && (to_default || (dest->field == (def)))))

// Macro to test and copy a field if necessary

#define x509_verify_param_copy(field, def)     \
  if (test_x509_verify_param_copy(field, def)) \
  dest->field = src->field

int X509_VERIFY_PARAM_inherit(X509_VERIFY_PARAM *dest,
                              const X509_VERIFY_PARAM *src) {
  unsigned long inh_flags;
  int to_default, to_overwrite;
  if (!src) {
    return 1;
  }
  inh_flags = dest->inh_flags | src->inh_flags;

  if (inh_flags & X509_VP_FLAG_ONCE) {
    dest->inh_flags = 0;
  }

  if (inh_flags & X509_VP_FLAG_LOCKED) {
    return 1;
  }

  if (inh_flags & X509_VP_FLAG_DEFAULT) {
    to_default = 1;
  } else {
    to_default = 0;
  }

  if (inh_flags & X509_VP_FLAG_OVERWRITE) {
    to_overwrite = 1;
  } else {
    to_overwrite = 0;
  }

  x509_verify_param_copy(purpose, 0);
  x509_verify_param_copy(trust, 0);
  x509_verify_param_copy(depth, -1);

  // If overwrite or check time not set, copy across

  if (to_overwrite || !(dest->flags & X509_V_FLAG_USE_CHECK_TIME)) {
    dest->check_time = src->check_time;
    dest->flags &= ~X509_V_FLAG_USE_CHECK_TIME;
    // Don't need to copy flag: that is done below
  }

  if (inh_flags & X509_VP_FLAG_RESET_FLAGS) {
    dest->flags = 0;
  }

  dest->flags |= src->flags;

  if (test_x509_verify_param_copy(policies, NULL)) {
    if (!X509_VERIFY_PARAM_set1_policies(dest, src->policies)) {
      return 0;
    }
  }

  // Copy the host flags if and only if we're copying the host list
  if (test_x509_verify_param_copy(hosts, NULL)) {
    if (dest->hosts) {
      sk_OPENSSL_STRING_pop_free(dest->hosts, str_free);
      dest->hosts = NULL;
    }
    if (src->hosts) {
      dest->hosts =
          sk_OPENSSL_STRING_deep_copy(src->hosts, OPENSSL_strdup, str_free);
      if (dest->hosts == NULL) {
        return 0;
      }
      dest->hostflags = src->hostflags;
    }
  }

  if (test_x509_verify_param_copy(email, NULL)) {
    if (!X509_VERIFY_PARAM_set1_email(dest, src->email, src->emaillen)) {
      return 0;
    }
  }

  if (test_x509_verify_param_copy(ip, NULL)) {
    if (!X509_VERIFY_PARAM_set1_ip(dest, src->ip, src->iplen)) {
      return 0;
    }
  }

  dest->poison = src->poison;

  return 1;
}

int X509_VERIFY_PARAM_set1(X509_VERIFY_PARAM *to,
                           const X509_VERIFY_PARAM *from) {
  unsigned long save_flags = to->inh_flags;
  int ret;
  to->inh_flags |= X509_VP_FLAG_DEFAULT;
  ret = X509_VERIFY_PARAM_inherit(to, from);
  to->inh_flags = save_flags;
  return ret;
}

static int int_x509_param_set1_email(char **pdest, size_t *pdestlen,
                                     const char *src, size_t srclen) {
  void *tmp;
  if (src != NULL) {
    // Setting |srclen| to 0 to automatically detect the length of |src| is an
    // OpenSSL quirk that AWS-LC isn't keen on supporting. However, consumers
    // often assume OpenSSL semantics from AWS-LC, so it's supported in this
    // case.
    if (srclen == 0) {
      srclen = strlen(src);
    }

    tmp = OPENSSL_strndup(src, srclen);
    if (tmp == NULL) {
      return 0;
    }
  } else {
    // This allows an empty string to disable previously configured checks.
    // This is an OpenSSL quirk that AWS-LC isn't keen on supporting. However,
    // consumers often assume OpenSSL semantics from AWS-LC, so it's supported
    // in this case.
    tmp = NULL;
    srclen = 0;
  }

  if (*pdest != NULL) {
    OPENSSL_free(*pdest);
  }
  *pdest = tmp;
  if (pdestlen != NULL) {
    *pdestlen = srclen;
  }
  return 1;
}

// IP addresses work slightly differently, so we use another function to
// differentiate from emails. |X509_VERIFY_PARAM_set1_ip| takes a const
// unsigned char*, instead of a const char*, so the same strlen logic that was
// being used is not quite suitable here.
// We keep the original behavior that BoringSSL left, but only for IP addresses.
// We can align the behavior with |int_x509_param_set1_email| like OpenSSL has
// been doing if needed.
static int int_x509_param_set1_ip(unsigned char **pdest, size_t *pdestlen,
                                  const unsigned char *src, size_t srclen) {
  void *tmp;
  if (src == NULL || srclen == 0) {
    // Unlike OpenSSL, we do not allow an empty string to disable previously
    // configured checks.
    return 0;
  }

  tmp = OPENSSL_memdup(src, srclen);
  if (!tmp) {
    return 0;
  }

  if (*pdest) {
    OPENSSL_free(*pdest);
  }
  *pdest = tmp;
  if (pdestlen) {
    *pdestlen = srclen;
  }
  return 1;
}

int X509_VERIFY_PARAM_set_flags(X509_VERIFY_PARAM *param, unsigned long flags) {
  param->flags |= flags;
  if (flags & X509_V_FLAG_POLICY_MASK) {
    param->flags |= X509_V_FLAG_POLICY_CHECK;
  }
  return 1;
}

int X509_VERIFY_PARAM_clear_flags(X509_VERIFY_PARAM *param,
                                  unsigned long flags) {
  param->flags &= ~flags;
  return 1;
}

unsigned long X509_VERIFY_PARAM_get_flags(X509_VERIFY_PARAM *param) {
  return param->flags;
}

int X509_VERIFY_PARAM_set_purpose(X509_VERIFY_PARAM *param, int purpose) {
  return X509_PURPOSE_set(&param->purpose, purpose);
}

int X509_VERIFY_PARAM_set_trust(X509_VERIFY_PARAM *param, int trust) {
  return X509_TRUST_set(&param->trust, trust);
}

void X509_VERIFY_PARAM_set_depth(X509_VERIFY_PARAM *param, int depth) {
  param->depth = depth;
}

void X509_VERIFY_PARAM_set_time_posix(X509_VERIFY_PARAM *param, int64_t t) {
  param->check_time = t;
  param->flags |= X509_V_FLAG_USE_CHECK_TIME;
}

void X509_VERIFY_PARAM_set_time(X509_VERIFY_PARAM *param, time_t t) {
  X509_VERIFY_PARAM_set_time_posix(param, t);
}

int X509_VERIFY_PARAM_add0_policy(X509_VERIFY_PARAM *param,
                                  ASN1_OBJECT *policy) {
  if (!param->policies) {
    param->policies = sk_ASN1_OBJECT_new_null();
    if (!param->policies) {
      return 0;
    }
  }
  if (!sk_ASN1_OBJECT_push(param->policies, policy)) {
    return 0;
  }
  // TODO(davidben): This does not set |X509_V_FLAG_POLICY_CHECK|, while
  // |X509_VERIFY_PARAM_set1_policies| does. Is this a bug?
  return 1;
}

int X509_VERIFY_PARAM_set1_policies(X509_VERIFY_PARAM *param,
                                    const STACK_OF(ASN1_OBJECT) *policies) {
  if (!param) {
    return 0;
  }

  sk_ASN1_OBJECT_pop_free(param->policies, ASN1_OBJECT_free);
  if (!policies) {
    param->policies = NULL;
    return 1;
  }

  param->policies =
      sk_ASN1_OBJECT_deep_copy(policies, OBJ_dup, ASN1_OBJECT_free);
  if (!param->policies) {
    return 0;
  }

  param->flags |= X509_V_FLAG_POLICY_CHECK;
  return 1;
}

int X509_VERIFY_PARAM_set1_host(X509_VERIFY_PARAM *param, const char *name,
                                size_t namelen) {
  if (!int_x509_param_set_hosts(param, SET_HOST, name, namelen)) {
    param->poison = 1;
    return 0;
  }
  return 1;
}

int X509_VERIFY_PARAM_add1_host(X509_VERIFY_PARAM *param, const char *name,
                                size_t namelen) {
  if (!int_x509_param_set_hosts(param, ADD_HOST, name, namelen)) {
    param->poison = 1;
    return 0;
  }
  return 1;
}

void X509_VERIFY_PARAM_set_hostflags(X509_VERIFY_PARAM *param,
                                     unsigned int flags) {
  param->hostflags = flags;
}

int X509_VERIFY_PARAM_set1_email(X509_VERIFY_PARAM *param, const char *email,
                                 size_t emaillen) {
  if (OPENSSL_memchr(email, '\0', emaillen) != NULL ||
      !int_x509_param_set1_email(&param->email, &param->emaillen, email,
                                 emaillen)) {
    param->poison = 1;
    return 0;
  }

  return 1;
}

int X509_VERIFY_PARAM_set1_ip(X509_VERIFY_PARAM *param, const unsigned char *ip,
                              size_t iplen) {
  if ((iplen != 0 && iplen != 4 && iplen != 16) ||
      !int_x509_param_set1_ip(&param->ip, &param->iplen, ip, iplen)) {
    param->poison = 1;
    return 0;
  }

  return 1;
}

int X509_VERIFY_PARAM_set1_ip_asc(X509_VERIFY_PARAM *param, const char *ipasc) {
  unsigned char ipout[16];
  size_t iplen;

  iplen = (size_t)x509v3_a2i_ipadd(ipout, ipasc);
  if (iplen == 0) {
    return 0;
  }
  return X509_VERIFY_PARAM_set1_ip(param, ipout, iplen);
}

int X509_VERIFY_PARAM_get_depth(const X509_VERIFY_PARAM *param) {
  return param->depth;
}

#define vpm_empty_id NULL, 0U, NULL, 0, NULL, 0, 0

static const X509_VERIFY_PARAM kDefaultParam = {
    /*check_time=*/0,
    /*inh_flags=*/0,
    /*flags=*/X509_V_FLAG_TRUSTED_FIRST,
    /*purpose=*/0,
    /*trust=*/0,
    /*depth=*/100,
    /*policies=*/NULL,
    vpm_empty_id};

static const X509_VERIFY_PARAM kSMIMESignParam = {
    /*check_time=*/0,
    /*inh_flags=*/0,
    /*flags=*/0,
    /*purpose=*/X509_PURPOSE_SMIME_SIGN,
    /*trust=*/X509_TRUST_EMAIL,
    /*depth=*/-1,
    /*policies=*/NULL,
    vpm_empty_id};

static const X509_VERIFY_PARAM kSSLClientParam = {
    /*check_time=*/0,
    /*inh_flags=*/0,
    /*flags=*/0,
    /*purpose=*/X509_PURPOSE_SSL_CLIENT,
    /*trust=*/X509_TRUST_SSL_CLIENT,
    /*depth=*/-1,
    /*policies=*/NULL,
    vpm_empty_id};

static const X509_VERIFY_PARAM kSSLServerParam = {
    /*check_time=*/0,
    /*inh_flags=*/0,
    /*flags=*/0,
    /*purpose=*/X509_PURPOSE_SSL_SERVER,
    /*trust=*/X509_TRUST_SSL_SERVER,
    /*depth=*/-1,
    /*policies=*/NULL,
    vpm_empty_id};

const X509_VERIFY_PARAM *X509_VERIFY_PARAM_lookup(const char *name) {
  if (strcmp(name, "default") == 0) {
    return &kDefaultParam;
  }
  if (strcmp(name, "pkcs7") == 0) {
    // PKCS#7 and S/MIME signing use the same defaults.
    return &kSMIMESignParam;
  }
  if (strcmp(name, "smime_sign") == 0) {
    return &kSMIMESignParam;
  }
  if (strcmp(name, "ssl_client") == 0) {
    return &kSSLClientParam;
  }
  if (strcmp(name, "ssl_server") == 0) {
    return &kSSLServerParam;
  }
  return NULL;
}
