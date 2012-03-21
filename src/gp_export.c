/*
   GSS-PROXY

   Copyright (C) 2011 Red Hat, Inc.
   Copyright (C) 2011 Simo Sorce <simo.sorce@redhat.com>

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.
*/

#include "config.h"
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include "gp_conv.h"
#include "gp_export.h"
#include "gp_debug.h"
#include <gssapi/gssapi_krb5.h>
#include <pwd.h>
#include <grp.h>

/* FIXME: F I X M E
 *
 * FFFFF  I  X    X  M    M  EEEEE
 * F      I   X  X   MM  MM  E
 * FFF    I    XX    M MM M  EEE
 * F      I   X  X   M    M  E
 * F      I  X    X  M    M  EEEEE
 *
 * Credential functions should either be implemented with gss_export_cred()
 * or, lacking those calls in the gssapi implementation, by keeping state
 * in a table/list and returning a token.
 * In both cases the content should be encrypted.
 *
 * Temporarily we simply return straight out the gss_cred_id_t pointer as
 * a handle.
 *
 * THIS IS ONLY FOR THE PROTOTYPE
 *
 * *MUST* BE FIXED BEFORE ANY OFFICIAL RELEASE.
 */

uint32_t gp_export_gssx_cred(uint32_t *min,
                             gss_cred_id_t *in, gssx_cred *out)
{
    uint32_t ret_maj;
    uint32_t ret_min;
    gss_name_t name = NULL;
    uint32_t lifetime;
    gss_cred_usage_t cred_usage;
    gss_OID_set mechanisms = NULL;
    uint32_t initiator_lifetime;
    uint32_t acceptor_lifetime;
    struct gssx_cred_element *el;
    int ret;
    int i, j;

    ret_maj = gss_inquire_cred(&ret_min, *in,
                               &name, &lifetime, &cred_usage, &mechanisms);
    if (ret_maj) {
        goto done;
    }

    ret_maj = gp_conv_name_to_gssx(&ret_min, name, &out->desired_name);
    if (ret_maj) {
        goto done;
    }
    gss_release_name(&ret_min, &name);
    name = NULL;

    out->elements.elements_len = mechanisms->count;
    out->elements.elements_val = calloc(out->elements.elements_len,
                                        sizeof(gssx_cred_element));
    if (!out->elements.elements_val) {
        ret_maj = GSS_S_FAILURE;
        ret_min = ENOMEM;
        goto done;
    }

    for (i = 0, j = 0; i < mechanisms->count; i++, j++) {

        el = &out->elements.elements_val[j];

        ret_maj = gss_inquire_cred_by_mech(&ret_min, *in,
                                           &mechanisms->elements[i],
                                           &name,
                                           &initiator_lifetime,
                                           &acceptor_lifetime,
                                           &cred_usage);
        if (ret_maj) {
            gp_log_failure(&mechanisms->elements[i], ret_maj, ret_min);

            /* temporarily skip any offender */
            out->elements.elements_len--;
            j--;
            continue;
#if 0
            ret = EINVAL;
            goto done;
#endif
        }

        ret_maj = gp_conv_name_to_gssx(&ret_min, name, &el->MN);
        if (ret_maj) {
            goto done;
        }
        gss_release_name(&ret_min, &name);
        name = NULL;

        ret = gp_conv_oid_to_gssx(&mechanisms->elements[i], &el->mech);
        if (ret) {
            ret_maj = GSS_S_FAILURE;
            ret_min = ret;
            goto done;
        }
        el->cred_usage = gp_conv_gssx_to_cred_usage(cred_usage);

        el->initiator_time_rec = initiator_lifetime;
        el->acceptor_time_rec = acceptor_lifetime;
    }

    ret = gp_conv_octet_string(sizeof(gss_cred_id_t), in,
                               &out->cred_handle_reference);
    if (ret) {
        ret_maj = GSS_S_FAILURE;
        ret_min = ret;
        goto done;
    }
    out->needs_release = true;

    /* we take over control of the credentials from here on */
    /* when we will have gss_export_cred() we will actually free
     * them immediately instead */
    *in = NULL;
    ret_maj = GSS_S_COMPLETE;
    ret_min = 0;

done:
    *min = ret_min;
    gss_release_name(&ret_min, &name);
    gss_release_oid_set(&ret_min, &mechanisms);
    return ret_maj;
}

int gp_import_gssx_cred(octet_string *in, gss_cred_id_t *out)
{
    if (in) {
        memcpy(out, in->octet_string_val, sizeof(gss_cred_id_t));
    } else {
        *out = NULL;
    }
    return 0;
}

int gp_find_cred(gssx_cred *cred, gss_cred_id_t *out)
{
    return gp_import_gssx_cred(&cred->cred_handle_reference, out);
}


/* Exported Contexts */

#define EXP_CTX_TYPE_OPTION "exported_context_type"
#define LINUX_LUCID_V1      "linux_lucid_v1"

enum exp_ctx_types {
    EXP_CTX_DEFAULT = 0,
    EXP_CTX_LINUX_LUCID_V1 = 1,
};

int gp_get_exported_context_type(struct gssx_call_ctx *ctx)
{

    struct gssx_option *val;
    int i;

    for (i = 0; i < ctx->options.options_len; i++) {
        val = &ctx->options.options_val[i];
        if (val->option.octet_string_len == sizeof(EXP_CTX_TYPE_OPTION) &&
            strncmp(EXP_CTX_TYPE_OPTION,
                        val->option.octet_string_val,
                        val->option.octet_string_len) == 0) {
            if (strncmp(LINUX_LUCID_V1,
                        val->value.octet_string_val,
                        val->value.octet_string_len) == 0) {
                return EXP_CTX_LINUX_LUCID_V1;
            }
            return -1;
        }
    }

    return EXP_CTX_DEFAULT;
}

#define KRB5_CTX_FLAG_INITIATOR         0x00000001
#define KRB5_CTX_FLAG_CFX               0x00000002
#define KRB5_CTX_FLAG_ACCEPTOR_SUBKEY   0x00000004

/* we use what svcgssd calls a "krb5_rfc4121_buffer"
 * Format:  uint32_t flags
 *          int32_t  endtime
 *          uint64_t seq_send
 *          uint32_t enctype
 *          u8[] raw key
 */

static uint32_t gp_format_linux_lucid_v1(uint32_t *min,
                                         gss_krb5_lucid_context_v1_t *lucid,
                                         gssx_buffer *out)
{
    uint8_t *buffer;
    uint8_t *p;
    size_t length;
    uint32_t flags;
    uint32_t enctype;
    uint32_t keysize;
    void *keydata;
    uint32_t maj;

    if (lucid->version != 1 ||
        (lucid->protocol != 0 && lucid->protocol != 1)) {
        *min = ENOTSUP;
        return GSS_S_FAILURE;
    }

    flags = 0;
    if (lucid->initiate) {
        flags |= KRB5_CTX_FLAG_INITIATOR;
    }
    if (lucid->protocol == 1) {
        flags |= KRB5_CTX_FLAG_CFX;
    }
    if (lucid->protocol == 1 && lucid->cfx_kd.have_acceptor_subkey == 1) {
        flags |= KRB5_CTX_FLAG_ACCEPTOR_SUBKEY;
    }

    if (lucid->protocol == 0) {
        enctype = lucid->rfc1964_kd.ctx_key.type;
        keysize = lucid->rfc1964_kd.ctx_key.length;
        keydata = lucid->rfc1964_kd.ctx_key.data;
    } else {
        if (lucid->cfx_kd.have_acceptor_subkey == 1) {
            enctype = lucid->cfx_kd.acceptor_subkey.type;
            keysize = lucid->cfx_kd.acceptor_subkey.length;
            keydata = lucid->cfx_kd.acceptor_subkey.data;
        } else {
            enctype = lucid->cfx_kd.ctx_key.type;
            keysize = lucid->cfx_kd.ctx_key.length;
            keydata = lucid->cfx_kd.ctx_key.data;
        }
    }

    length = sizeof(flags)
             + sizeof(lucid->endtime)
             + sizeof(lucid->send_seq)
             + sizeof(enctype)
             + keysize;

    buffer = calloc(1, length);
    if (!buffer) {
        *min = ENOMEM;
        maj = GSS_S_FAILURE;
        goto done;
    }
    p = buffer;

    memcpy(p, &flags, sizeof(flags));
    p += sizeof(flags);
    memcpy(p, &lucid->endtime, sizeof(lucid->endtime));
    p += sizeof(lucid->endtime);
    memcpy(p, &lucid->send_seq, sizeof(lucid->send_seq));
    p += sizeof(lucid->send_seq);
    memcpy(p, &enctype, sizeof(enctype));
    p += sizeof(enctype);
    memcpy(p, keydata, keysize);

    out->octet_string_val = (void *)buffer;
    out->octet_string_len = length;
    maj = GSS_S_COMPLETE;
    *min = 0;

done:
    if (maj) {
        free(buffer);
    }
    return maj;
}


uint32_t gp_export_ctx_id_to_gssx(uint32_t *min, int type,
                                  gss_ctx_id_t *in, gssx_ctx *out)
{
    uint32_t ret_maj;
    uint32_t ret_min;
    gss_name_t src_name = GSS_C_NO_NAME;
    gss_name_t targ_name = GSS_C_NO_NAME;
    gss_buffer_desc export_buffer = GSS_C_EMPTY_BUFFER;
    gss_krb5_lucid_context_v1_t *lucid = NULL;
    uint32_t lifetime_rec;
    gss_OID mech_type;
    uint32_t ctx_flags;
    int is_locally_initiated;
    int is_open;
    int ret;

/* TODO: For mechs that need multiple roundtrips to complete */
    /* out->state; */

    /* we do not need the client to release anything until we handle state */
    out->needs_release = false;

    ret_maj = gss_inquire_context(&ret_min, *in, &src_name, &targ_name,
                                  &lifetime_rec, &mech_type, &ctx_flags,
                                  &is_locally_initiated, &is_open);
    if (ret_maj) {
        goto done;
    }

    ret = gp_conv_oid_to_gssx(mech_type, &out->mech);
    if (ret) {
        ret_maj = GSS_S_FAILURE;
        ret_min = ret;
        goto done;
    }

    ret_maj = gp_conv_name_to_gssx(&ret_min, src_name, &out->src_name);
    if (ret_maj) {
        goto done;
    }

    ret_maj = gp_conv_name_to_gssx(&ret_min, targ_name, &out->targ_name);
    if (ret_maj) {
        goto done;
    }

    out->lifetime = lifetime_rec;

    out->ctx_flags = ctx_flags;

    if (is_locally_initiated) {
        out->locally_initiated = true;
    }

    if (is_open) {
        out->open = true;
    }

    /* note: once converted the original context token is not usable anymore,
     * so this must be the last call to use it */
    switch (type) {
    case EXP_CTX_DEFAULT:
        ret_maj = gss_export_sec_context(&ret_min, in, &export_buffer);
        if (ret_maj) {
            goto done;
        }
        ret = gp_conv_buffer_to_gssx(&export_buffer,
                                     &out->exported_context_token);
        if (ret) {
            ret_maj = GSS_S_FAILURE;
            ret_min = ret;
            goto done;
        }
        break;
    case EXP_CTX_LINUX_LUCID_V1:
        ret_maj = gss_krb5_export_lucid_sec_context(&ret_min, in, 1,
                                                    (void **)&lucid);
        if (ret_maj) {
            goto done;
        }
        ret_maj = gp_format_linux_lucid_v1(&ret_min, lucid,
                                           &out->exported_context_token);
        if (ret_maj) {
            goto done;
        }
        break;
    default:
        ret_maj = GSS_S_FAILURE;
        ret_min = EINVAL;
        goto done;
    }

    /* Leave this empty, used only on the way in for init_sec_context */
    /* out->gssx_option */

done:
    *min = ret_min;
    gss_release_name(&ret_min, &src_name);
    gss_release_name(&ret_min, &targ_name);
    gss_release_buffer(&ret_min, &export_buffer);
    if (lucid) {
        gss_krb5_free_lucid_sec_context(&ret_min, lucid);
    }
    if (ret_maj) {
        xdr_free((xdrproc_t)xdr_gssx_OID, (char *)&out->mech);
        xdr_free((xdrproc_t)xdr_gssx_name, (char *)&out->src_name);
        xdr_free((xdrproc_t)xdr_gssx_name, (char *)&out->targ_name);
    }
    return ret_maj;
}

uint32_t gp_import_gssx_to_ctx_id(uint32_t *min, int type,
                                  gssx_ctx *in, gss_ctx_id_t *out)
{
    gss_buffer_desc export_buffer = GSS_C_EMPTY_BUFFER;

    if (type != EXP_CTX_DEFAULT) {
        *min = EINVAL;
        return GSS_S_FAILURE;
    }

    gp_conv_gssx_to_buffer(&in->exported_context_token, &export_buffer);

    return gss_import_sec_context(min, &export_buffer, out);
}

/* Exported Creds */

#define EXP_CREDS_TYPE_OPTION "exported_creds_type"
#define LINUX_CREDS_V1        "linux_creds_v1"

enum exp_creds_types {
    EXP_CREDS_NO_CREDS = 0,
    EXP_CREDS_LINUX_V1 = 1,
};

int gp_get_export_creds_type(struct gssx_call_ctx *ctx)
{

    struct gssx_option *val;
    int i;

    for (i = 0; i < ctx->options.options_len; i++) {
        val = &ctx->options.options_val[i];
        if (val->option.octet_string_len == sizeof(EXP_CREDS_TYPE_OPTION) &&
            strncmp(EXP_CREDS_TYPE_OPTION,
                        val->option.octet_string_val,
                        val->option.octet_string_len) == 0) {
            if (strncmp(LINUX_CREDS_V1,
                        val->value.octet_string_val,
                        val->value.octet_string_len) == 0) {
                return EXP_CREDS_LINUX_V1;
            }
            return -1;
        }
    }

    return EXP_CREDS_NO_CREDS;
}

#define CREDS_BUF_MAX (NGROUPS_MAX * sizeof(int32_t))
#define CREDS_HDR (3 * sizeof(int32_t)) /* uid, gid, count */

static uint32_t gp_export_creds_enoent(uint32_t *min, gss_buffer_t buf)
{
    int32_t *p;

    p = malloc(CREDS_HDR);
    if (!p) {
        *min = ENOMEM;
        return GSS_S_FAILURE;
    }
    p[0] = -1; /* uid */
    p[1] = -1; /* gid */
    p[2] = 0; /* num groups */

    buf->value = p;
    buf->length = CREDS_HDR;
    *min = 0;
    return GSS_S_COMPLETE;
}

static uint32_t gp_export_creds_linux(uint32_t *min, gss_name_t name,
                                      gss_const_OID mech, gss_buffer_t buf)
{
    gss_buffer_desc localname;
    uint32_t ret_maj;
    uint32_t ret_min;
    struct passwd pwd, *res;
    char *pwbuf = NULL;
    char *grbuf = NULL;
    int32_t *p;
    size_t len;
    int count, num;
    int ret;

    /* We use gss_localname() to map the name. Then just use nsswitch to
     * look up the user.
     *
     * (TODO: If gss_localname() fails we may wanto agree with SSSD on a name
     * format to match principal names, es: gss:foo@REALM.COM, or just
     * foo@REALM.COM) until sssd can provide a libkrb5 interface to augment
     * gss_localname() resolution for trusted realms */

    ret_maj = gss_localname(&ret_min, name, mech, &localname);
    if (ret_maj) {
        if (ret_min == ENOENT) {
            return gp_export_creds_enoent(min, buf);
        }
        *min = ret_min;
        return ret_maj;
    }

    len = 1024;
    pwbuf = malloc(len);
    if (!pwbuf) {
        ret_min = ENOMEM;
        ret_maj = GSS_S_FAILURE;
        goto done;
    }
    ret = 0;
    do {
        if (ret == ERANGE) {
            if (len == CREDS_BUF_MAX) {
                ret_min = ENOSPC;
                ret_maj = GSS_S_FAILURE;
                goto done;
            }
            len *= 2;
            if (len > CREDS_BUF_MAX) {
                len = CREDS_BUF_MAX;
            }
            p = realloc(pwbuf, len);
            if (!p) {
                ret_min = ENOMEM;
                ret_maj = GSS_S_FAILURE;
                goto done;
            }
            pwbuf = (char *)p;
        }
        ret = getpwnam_r((char *)localname.value, &pwd, pwbuf, len, &res);
    } while (ret == EINTR || ret == ERANGE);

    switch (ret) {
    case 0:
        if (res != NULL) {
            break;
        }
        /* fall through as ret == NULL is equivalent to ENOENT */
    case ENOENT:
    case ESRCH:
        free(pwbuf);
        return gp_export_creds_enoent(min, buf);
    default:
        ret_min = ret;
        ret_maj = GSS_S_FAILURE;
        goto done;
    }

    /* start with a reasonably sized buffer */
    count = 256;
    num = 0;
    do {
        if (count >= NGROUPS_MAX) {
            ret_min = ENOSPC;
            ret_maj = GSS_S_FAILURE;
            goto done;
        }
        count *= 2;
        if (count < num) {
            count = num;
        }
        if (count > NGROUPS_MAX) {
            count = NGROUPS_MAX;
        }
        len = count * sizeof(int32_t);
        p = realloc(grbuf, len + CREDS_HDR);
        if (!p) {
            ret_min = ENOMEM;
            ret_maj = GSS_S_FAILURE;
            goto done;
        }
        grbuf = (char *)p;
        num = count;
        ret = getgrouplist(pwd.pw_name, pwd.pw_gid, (gid_t *)&p[3], &num);
    } while (ret == -1);

    /* we got the buffer, now fill in [uid, gid, num] and we are done */
    p[0] = pwd.pw_uid;
    p[1] = pwd.pw_gid;
    p[2] = num;
    buf->value = p;
    buf->length = (num + 3) * sizeof(int32_t);
    ret_min = 0;
    ret_maj = GSS_S_COMPLETE;

done:
    if (ret_maj) {
       free(grbuf);
    }
    free(pwbuf);
    *min = ret_min;
    return ret_maj;
}

uint32_t gp_export_creds_to_gssx_options(uint32_t *min, int type,
                                         gss_name_t src_name,
                                         gss_const_OID mech_type,
                                         unsigned int *opt_num,
                                         gssx_option **opt_array)
{
    gss_buffer_desc export_buffer = GSS_C_EMPTY_BUFFER;
    unsigned int num;
    gssx_option *opta;
    uint32_t ret_min;
    uint32_t ret_maj;

    switch (type) {
    case EXP_CREDS_NO_CREDS:
        *min = 0;
        return GSS_S_COMPLETE;

    case EXP_CREDS_LINUX_V1:
        ret_maj = gp_export_creds_linux(&ret_min, src_name,
                                        mech_type, &export_buffer);
        if (ret_maj) {
            if (ret_min == ENOENT) {
                /* if not user, return w/o adding anything to the array */
                ret_min = 0;
                ret_maj = GSS_S_COMPLETE;
            }
            *min = ret_min;
            return ret_maj;
        }
        break;

    default:
        *min = EINVAL;
        return GSS_S_FAILURE;
    }

    num = *opt_num;
    opta = realloc(*opt_array, sizeof(gssx_option) * (num + 1));
    if (!opta) {
        ret_min = ENOMEM;
        ret_maj = GSS_S_FAILURE;
        goto done;
    }
    opta[num].option.octet_string_val = strdup(LINUX_CREDS_V1);
    if (!opta[num].option.octet_string_val) {
        ret_min = ENOMEM;
        ret_maj = GSS_S_FAILURE;
        goto done;
    }
    opta[num].option.octet_string_len = sizeof(LINUX_CREDS_V1);
    opta[num].value.octet_string_val = export_buffer.value;
    opta[num].value.octet_string_len = export_buffer.length;

    num++;
    *opt_num = num;
    *opt_array = opta;
    ret_min = 0;
    ret_maj = GSS_S_COMPLETE;

done:
    *min = ret_min;
    if (ret_maj) {
        gss_release_buffer(&ret_min, &export_buffer);
    }
    return ret_maj;
}