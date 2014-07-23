/*
 *  X.509 certificate and private key decoding
 *
 *  Based on XySSL: Copyright (C) 2006-2008   Christophe Devine
 *
 *  Copyright (C) 2009  Paul Bakker <polarssl_maintainer at polarssl dot org>
 *
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *    * Neither the names of PolarSSL or XySSL nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *  TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 *  The ITU-T X.509 standard defines a certificate format for PKI.
 *
 *  http://www.ietf.org/rfc/rfc5280.txt
 *  http://www.ietf.org/rfc/rfc3279.txt
 *  http://www.ietf.org/rfc/rfc6818.txt
 *
 *  ftp://ftp.rsasecurity.com/pub/pkcs/ascii/pkcs-1v2.asc
 *
 *  http://www.itu.int/ITU-T/studygroups/com17/languages/X.680-0207.pdf
 *  http://www.itu.int/ITU-T/studygroups/com17/languages/X.690-0207.pdf
 */

#include <apr_pools.h>
#include "svn_hash.h"
#include "svn_string.h"
#include "svn_time.h"
#include "svn_checksum.h"
#include "svn_utf.h"
#include "svn_ctype.h"
#include "svn_x509.h"
#include "private/svn_utf_private.h"
#include "private/svn_string_private.h"

#include "x509.h"

#include <string.h>
#include <stdio.h>

/*
 * ASN.1 DER decoding routines
 */
static svn_error_t *
asn1_get_len(const unsigned char **p, const unsigned char *end, int *len)
{
  if ((end - *p) < 1)
    return svn_error_create(SVN_ERR_ASN1_OUT_OF_DATA, NULL, NULL);

  if ((**p & 0x80) == 0)
    *len = *(*p)++;
  else {
    switch (**p & 0x7F) {
    case 1:
      if ((end - *p) < 2)
        return svn_error_create(SVN_ERR_ASN1_OUT_OF_DATA, NULL, NULL);

      *len = (*p)[1];
      (*p) += 2;
      break;

    case 2:
      if ((end - *p) < 3)
        return svn_error_create(SVN_ERR_ASN1_OUT_OF_DATA, NULL, NULL);

      *len = ((*p)[1] << 8) | (*p)[2];
      (*p) += 3;
      break;

    default:
      return svn_error_create(SVN_ERR_ASN1_INVALID_LENGTH, NULL, NULL);
      break;
    }
  }

  if (*len > (int)(end - *p))
    return svn_error_create(SVN_ERR_ASN1_OUT_OF_DATA, NULL, NULL);

  return SVN_NO_ERROR;
}

static svn_error_t *
asn1_get_tag(const unsigned char **p,
             const unsigned char *end, int *len, int tag)
{
  if ((end - *p) < 1)
    return svn_error_create(SVN_ERR_ASN1_OUT_OF_DATA, NULL, NULL);

  if (**p != tag)
    return svn_error_create(SVN_ERR_ASN1_UNEXPECTED_TAG, NULL, NULL);

  (*p)++;

  return svn_error_trace(asn1_get_len(p, end, len));
}

static svn_error_t *
asn1_get_int(const unsigned char **p, const unsigned char *end, int *val)
{
  int len;

  SVN_ERR(asn1_get_tag(p, end, &len, ASN1_INTEGER));

  if (len > (int)sizeof(int) || (**p & 0x80) != 0)
    return svn_error_create(SVN_ERR_ASN1_INVALID_LENGTH, NULL, NULL);

  *val = 0;

  while (len-- > 0) {
    *val = (*val << 8) | **p;
    (*p)++;
  }

  return SVN_NO_ERROR;
}

/*
 *  Version   ::=  INTEGER  {  v1(0), v2(1), v3(2)  }
 */
static svn_error_t *
x509_get_version(const unsigned char **p, const unsigned char *end, int *ver)
{
  svn_error_t *err;
  int len;

  err = asn1_get_tag(p, end, &len,
                     ASN1_CONTEXT_SPECIFIC | ASN1_CONSTRUCTED | 0);
  if (err)
    {
      if (err->apr_err == SVN_ERR_ASN1_UNEXPECTED_TAG)
        {
          svn_error_clear(err);
          *ver = 0;
          return SVN_NO_ERROR;
        }

      return svn_error_trace(err);
    }

  end = *p + len;

  err = asn1_get_int(p, end, ver);
  if (err)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_VERSION, err, NULL);

  if (*p != end)
    {
      err = svn_error_create(SVN_ERR_X509_CERT_INVALID_VERSION, NULL, NULL);
      return svn_error_create(SVN_ERR_ASN1_LENGTH_MISMATCH, err, NULL);
    }

  return SVN_NO_ERROR;
}

/*
 *  CertificateSerialNumber   ::=  INTEGER
 */
static svn_error_t *
x509_get_serial(const unsigned char **p,
                const unsigned char *end, x509_buf * serial)
{
  svn_error_t *err;

  if ((end - *p) < 1)
    {
      err = svn_error_create(SVN_ERR_X509_CERT_INVALID_SERIAL, NULL, NULL);
      return svn_error_create(SVN_ERR_ASN1_OUT_OF_DATA, err, NULL);
    }

  if (**p != (ASN1_CONTEXT_SPECIFIC | ASN1_PRIMITIVE | 2) &&
      **p != ASN1_INTEGER)
    {
      err = svn_error_create(SVN_ERR_X509_CERT_INVALID_SERIAL, NULL, NULL);
      return svn_error_create(SVN_ERR_ASN1_UNEXPECTED_TAG, err, NULL);
    }

  serial->tag = *(*p)++;

  err = asn1_get_len(p, end, &serial->len);
  if (err)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_SERIAL, err, NULL);

  serial->p = *p;
  *p += serial->len;

  return SVN_NO_ERROR;
}

/*
 *  AlgorithmIdentifier   ::=  SEQUENCE  {
 *     algorithm         OBJECT IDENTIFIER,
 *     parameters         ANY DEFINED BY algorithm OPTIONAL  }
 */
static svn_error_t *
x509_get_alg(const unsigned char **p, const unsigned char *end, x509_buf * alg)
{
  svn_error_t *err;
  int len;

  err = asn1_get_tag(p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
  if (err)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_ALG, err, NULL);

  end = *p + len;
  alg->tag = **p;

  err = asn1_get_tag(p, end, &alg->len, ASN1_OID);
  if (err)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_ALG, err, NULL);

  alg->p = *p;
  *p += alg->len;

  if (*p == end)
    return SVN_NO_ERROR;

  /*
   * assume the algorithm parameters must be NULL
   */
  err = asn1_get_tag(p, end, &len, ASN1_NULL);
  if (err)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_ALG, err, NULL);

  if (*p != end)
    {
      err = svn_error_create(SVN_ERR_X509_CERT_INVALID_ALG, NULL, NULL);
      return svn_error_create(SVN_ERR_ASN1_LENGTH_MISMATCH, err, NULL);
    }

  return SVN_NO_ERROR;
}

/*
 *  RelativeDistinguishedName ::=
 *    SET OF AttributeTypeAndValue
 *
 *  AttributeTypeAndValue ::= SEQUENCE {
 *    type     AttributeType,
 *    value     AttributeValue }
 *
 *  AttributeType ::= OBJECT IDENTIFIER
 *
 *  AttributeValue ::= ANY DEFINED BY AttributeType
 */
static svn_error_t *
x509_get_name(const unsigned char **p, const unsigned char *end,
              x509_name * cur, apr_pool_t *result_pool)
{
  svn_error_t *err;
  int len;
  const unsigned char *end2;
  x509_buf *oid;
  x509_buf *val;

  err = asn1_get_tag(p, end, &len, ASN1_CONSTRUCTED | ASN1_SET);
  if (err)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_NAME, err, NULL);

  end2 = end;
  end = *p + len;

  err = asn1_get_tag(p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
  if (err)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_NAME, err, NULL);

  if (*p + len != end)
    {
      err = svn_error_create(SVN_ERR_X509_CERT_INVALID_NAME, NULL, NULL);
      return svn_error_create(SVN_ERR_ASN1_LENGTH_MISMATCH, err, NULL);
    }

  oid = &cur->oid;
  oid->tag = **p;

  err = asn1_get_tag(p, end, &oid->len, ASN1_OID);
  if (err)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_NAME, err, NULL);

  oid->p = *p;
  *p += oid->len;

  if ((end - *p) < 1)
    {
      err = svn_error_create(SVN_ERR_X509_CERT_INVALID_NAME, NULL, NULL);
      return svn_error_create(SVN_ERR_ASN1_OUT_OF_DATA, err, NULL);
    }

  if (**p != ASN1_BMP_STRING && **p != ASN1_UTF8_STRING &&
      **p != ASN1_T61_STRING && **p != ASN1_PRINTABLE_STRING &&
      **p != ASN1_IA5_STRING && **p != ASN1_UNIVERSAL_STRING)
    {
      err = svn_error_create(SVN_ERR_X509_CERT_INVALID_NAME, NULL, NULL);
      return svn_error_create(SVN_ERR_ASN1_UNEXPECTED_TAG, err, NULL);
    }

  val = &cur->val;
  val->tag = *(*p)++;

  err = asn1_get_len(p, end, &val->len);
  if (err)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_NAME, err, NULL);

  val->p = *p;
  *p += val->len;

  cur->next = NULL;

  if (*p != end)
    {
      err = svn_error_create(SVN_ERR_X509_CERT_INVALID_NAME, NULL, NULL);
      return svn_error_create(SVN_ERR_ASN1_LENGTH_MISMATCH, err, NULL);
    }

  /*
   * recurse until end of SEQUENCE is reached
   */
  if (*p == end2)
    return SVN_NO_ERROR;

  cur->next = (x509_name *) apr_palloc(result_pool, sizeof(x509_name));

  if (cur->next == NULL)
    return SVN_NO_ERROR;

  return svn_error_trace(x509_get_name(p, end2, cur->next, result_pool));
}

/* Retrieve the date from the X.509 cert data between *P and END in either
 * UTCTime or GeneralizedTime format (as defined in RFC 5280 s. 4.1.2.5.1 and
 * 4.1.2.5.2 respectively) and place the result in WHEN using  SCRATCH_POOL
 * for temporary allocations. */
static svn_error_t *
x509_get_date(apr_time_t *when,
              const unsigned char **p,
              const unsigned char *end,
              apr_pool_t *scratch_pool)
{
  svn_error_t *err;
  apr_status_t ret;
  int len, tag;
  char *date;
  apr_time_exp_t xt = { 0 };
  char tz;

  tag = **p;
  err = asn1_get_tag(p, end, &len, ASN1_UTC_TIME);
  if (err && err->apr_err == SVN_ERR_ASN1_UNEXPECTED_TAG)
    {
      svn_error_clear(err);
      tag = **p;
      err = asn1_get_tag(p, end, &len, ASN1_GENERALIZED_TIME);
    }
  if (err)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_DATE, err, NULL);

  date = apr_pstrndup(scratch_pool, (const char *) *p, len);
  switch (tag)
    {
      case ASN1_UTC_TIME:
        if (sscanf(date, "%2d%2d%2d%2d%2d%2d%c",
                   &xt.tm_year, &xt.tm_mon, &xt.tm_mday,
                   &xt.tm_hour, &xt.tm_min, &xt.tm_sec, &tz) < 6)
          return svn_error_create(SVN_ERR_X509_CERT_INVALID_DATE, NULL, NULL);

        /* UTCTime only provides a 2 digit year.  X.509 specifies that years
         * greater than or equal to 50 must be interpreted as 19YY and years
         * less than 50 be interpreted as 20YY.  This format is not used for
         * years greater than 2049. apr_time_exp_t wants years as the number
         * of years since 1900, so don't convert to 4 digits here. */
        xt.tm_year += 100 * (xt.tm_year < 50);
        break;

      case ASN1_GENERALIZED_TIME:
        if (sscanf(date, "%4d%2d%2d%2d%2d%2d%c",
                   &xt.tm_year, &xt.tm_mon, &xt.tm_mday,
                   &xt.tm_hour, &xt.tm_min, &xt.tm_sec, &tz) < 6)
          return svn_error_create(SVN_ERR_X509_CERT_INVALID_DATE, NULL, NULL);

        /* GeneralizedTime has the full 4 digit year.  But apr_time_exp_t
         * wants years as the number of years since 1900. */
        xt.tm_year -= 1900;
        break;

      default:
        /* shouldn't ever get here because we should error out above in the
         * asn1_get_tag() bits but doesn't hurt to be extra paranoid. */
        return svn_error_create(SVN_ERR_X509_CERT_INVALID_DATE, NULL, NULL);
        break;
    }

  /* check that the timezone is GMT
   * ASN.1 allows for the timezone to be specified but X.509 says it must
   * always be GMT.  A little bit of extra paranoia here seems like a good
   * idea. */
  if (tz != 'Z')
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_DATE, NULL, NULL);

  /* apr_time_exp_t expects months to be zero indexed, 0=Jan, 11=Dec. */
  xt.tm_mon -= 1;

  ret = apr_time_exp_gmt_get(when, &xt);
  if (ret)
    return svn_error_wrap_apr(ret, NULL);

  *p += len;

  return SVN_NO_ERROR;
}

/*
 *  Validity ::= SEQUENCE {
 *     notBefore    Time,
 *     notAfter    Time }
 *
 *  Time ::= CHOICE {
 *     utcTime    UTCTime,
 *     generalTime  GeneralizedTime }
 */
static svn_error_t *
x509_get_dates(apr_time_t *from,
               apr_time_t *to,
               const unsigned char **p,
               const unsigned char *end,
               apr_pool_t *scratch_pool)
{
  svn_error_t *err;
  int len;

  err = asn1_get_tag(p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
  if (err)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_DATE, err, NULL);

  end = *p + len;

  SVN_ERR(x509_get_date(from, p, end, scratch_pool));

  SVN_ERR(x509_get_date(to, p, end, scratch_pool));

  if (*p != end)
    {
      err = svn_error_create(SVN_ERR_X509_CERT_INVALID_DATE, NULL, NULL);
      return svn_error_create(SVN_ERR_ASN1_LENGTH_MISMATCH, err, NULL);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
x509_get_sig(const unsigned char **p, const unsigned char *end, x509_buf * sig)
{
  svn_error_t *err;
  int len;

  sig->tag = **p;

  err = asn1_get_tag(p, end, &len, ASN1_BIT_STRING);
  if (err)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_SIGNATURE, err, NULL);

  if (--len < 1 || *(*p)++ != 0)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_SIGNATURE, NULL, NULL);

  sig->len = len;
  sig->p = *p;

  *p += len;

  return SVN_NO_ERROR;
}

/*
 * X.509 v2/v3 unique identifier (not parsed)
 */
static svn_error_t *
x509_get_uid(const unsigned char **p,
             const unsigned char *end, x509_buf * uid, int n)
{
  svn_error_t *err;

  if (*p == end)
    return SVN_NO_ERROR;

  uid->tag = **p;

  err = asn1_get_tag(p, end, &uid->len,
        ASN1_CONTEXT_SPECIFIC | ASN1_CONSTRUCTED | n);
  if (err)
    {
      if (err->apr_err == SVN_ERR_ASN1_UNEXPECTED_TAG)
        {
          svn_error_clear(err);
          return SVN_NO_ERROR;
        }

      return svn_error_trace(err);
    }

  uid->p = *p;
  *p += uid->len;

  return SVN_NO_ERROR;
}

/*
 * X.509 v3 extensions (not parsed)
 */
static svn_error_t *
x509_skip_ext(const unsigned char **p,
             const unsigned char *end)
{
  svn_error_t *err;
  int len;

  if (*p == end)
    return SVN_NO_ERROR;

  err = asn1_get_tag(p, end, &len,
                     ASN1_CONTEXT_SPECIFIC | ASN1_CONSTRUCTED | 3);
  if (err)
    {
      if (err->apr_err == SVN_ERR_ASN1_UNEXPECTED_TAG)
        {
          svn_error_clear(err);
          return SVN_NO_ERROR;
        }

      return svn_error_trace(err);
    }

  /* Skip extensions */
  *p += len;

  return SVN_NO_ERROR;
}

/* Escape all non-ascii characters similar to
 * svn_xml_fuzzy_escape() and svn_utf_cstring_from_utf8_fuzzy(). 
 * All of the encoding formats somewhat overlap with ascii (BMPString 
 * and UniversalString are actually always wider so you'll end up
 * with a bunch of escaped nul bytes, but ideally we don't get here
 * for those). */
static const svn_string_t *
fuzzy_escape(const svn_string_t *src, apr_pool_t *result_pool)
{
  const char *end = src->data + src->len;
  const char *p = src->data, *q;
  svn_stringbuf_t *outstr;
  char escaped_char[6]; /* ? \ u u u \0 */

  for (q = p; q < end; q++)
    {
      if (!svn_ctype_isascii(*q))
        break;
    }

  if (q == end)
    return src;

  outstr = svn_stringbuf_create_empty(result_pool);
  while (1)
    {
      q = p;

      /* Traverse till either unsafe character or eos. */
      while (q < end && svn_ctype_isascii(*q))
        q++;

      /* copy chunk before marker */
      svn_stringbuf_appendbytes(outstr, p, q - p);

      if (q == end)
        break;

      apr_snprintf(escaped_char, sizeof(escaped_char), "?\\%03u",
                   (unsigned char) *q);
      svn_stringbuf_appendcstr(outstr, escaped_char);

      p = q + 1;
    }

  return svn_stringbuf__morph_into_string(outstr);
}

/* Make a best effort to convert a X.509 name to a UTF-8 encoded
 * string and return it.  If we can't properly convert just do a
 * fuzzy conversion so we have something to display. */
static const svn_string_t *
x509name_to_utf8_string(const x509_name *name, apr_pool_t *result_pool)
{
  const svn_string_t *src_string, *utf8_string;
  svn_error_t *err;
  const char *frompage;

  src_string = svn_string_ncreate((const char *)name->val.p,
                                  name->val.len,
                                  result_pool);
  switch (name->val.tag)
    {
      case ASN1_UTF8_STRING:
      if (svn_utf__is_valid(src_string->data, src_string->len))
        return src_string;
      else
        /* not a valid UTF-8 string, who knows what it is,
         * so run it through the fuzzy_escape code.  */
        return fuzzy_escape(src_string, result_pool);
      break;

      /* Both BMP and UNIVERSAL should always be in Big Endian.
       * But rumor has it that there are certs out there with other
       * endianess and even Byte Order Marks.  If we actually run
       * into these, it might make sense to remove the BE on these
       * frompages. */

      case ASN1_BMP_STRING:
      frompage = "UCS-2BE";
      break;

      case ASN1_UNIVERSAL_STRING:
      frompage = "UCS-4BE";
      break;

      /* Despite what all the IETF, ISO, ITU bits say everything out
       * on the Internet that I can find treats this as ISO-8859-1.
       * Even the name is misleading, it's not actually T.61.  All the
       * gory details can be found in the Character Sets section of:
       * https://www.cs.auckland.ac.nz/~pgut001/pubs/x509guide.txt
       */
      case ASN1_T61_STRING:
      frompage = "ISO-8859-1";
      break;

      /* This leaves two types out there in the wild.  PrintableString,
       * which is just a subset of ASCII and IA5 which is ASCII (though
       * 0x24 '$' and 0x23 '#' may be defined with differnet symbols
       * depending on the location, in practice it seems everyone just
       * treats it as ASCII).  Since these are just ASCII run through
       * the fuzzy_escape code to deal with anything that isn't actually
       * ASCII.  There shouldn't be any other types here but if we find
       * a cert with some other cert, the best we can do is the
       * fuzzy_escape().  Note: Technically IA5 isn't valid in this
       * context, however in the real world it may pop up. */
      default:
      return fuzzy_escape(src_string, result_pool);
    }

  err = svn_utf_string_to_utf8_ex(&utf8_string, src_string, frompage,
                                  result_pool);
  if (err)
    {
      svn_error_clear(err);
      return fuzzy_escape(src_string, result_pool);
    }

  return utf8_string;
}

/*
 * Store the name from dn in printable form into buf,
 * using scratch_pool for any temporary allocations.
 */
static void
x509parse_dn_gets(svn_stringbuf_t *buf, const x509_name * dn,
                  apr_pool_t *scratch_pool)
{
  const x509_name *name;
  const char *temp;

  name = dn;

  while (name != NULL) {
    const svn_string_t *utf8_value;

    if (name != dn)
      svn_stringbuf_appendcstr(buf, ", ");

    if (memcmp(name->oid.p, OID_X520, 2) == 0) {
      switch (name->oid.p[2]) {
      case X520_COMMON_NAME:
        svn_stringbuf_appendcstr(buf, "CN=");
        break;

      case X520_COUNTRY:
        svn_stringbuf_appendcstr(buf, "C=");
        break;

      case X520_LOCALITY:
        svn_stringbuf_appendcstr(buf, "L=");
        break;

      case X520_STATE:
        svn_stringbuf_appendcstr(buf, "ST=");
        break;

      case X520_ORGANIZATION:
        svn_stringbuf_appendcstr(buf, "O=");
        break;

      case X520_ORG_UNIT:
        svn_stringbuf_appendcstr(buf, "OU=");
        break;

      default:
        temp = apr_psprintf(scratch_pool, "0x%02X=", name->oid.p[2]);
        svn_stringbuf_appendcstr(buf, temp);
        break;
      }
    } else if (memcmp(name->oid.p, OID_PKCS9, 8) == 0) {
      switch (name->oid.p[8]) {
      case PKCS9_EMAIL:
        svn_stringbuf_appendcstr(buf, "emailAddress=");
        break;

      default:
        temp = apr_psprintf(scratch_pool, "0x%02X=", name->oid.p[8]);
        svn_stringbuf_appendcstr(buf, temp);
        break;
      }
    } else
      svn_stringbuf_appendcstr(buf, "\?\?=");

    utf8_value = x509name_to_utf8_string(name, scratch_pool);
    if (utf8_value)
      svn_stringbuf_appendbytes(buf, utf8_value->data, utf8_value->len);
    else
      /* this should never happen */
      svn_stringbuf_appendfill(buf, '?', 2);

    name = name->next;
  }
}

/*
 * Parse one certificate.
 */
svn_error_t *
svn_x509_parse_cert(apr_hash_t **certinfo,
                    const char *buf,
                    int buflen,
                    apr_pool_t *result_pool,
                    apr_pool_t *scratch_pool)
{
  svn_error_t *err;
  int len;
  const unsigned char *p;
  const unsigned char *end;
  x509_cert *crt;
  svn_stringbuf_t *issuer, *subject;
  svn_checksum_t *sha1_digest;

  crt = apr_pcalloc(scratch_pool, sizeof(*crt));
  p = (const unsigned char *)buf;
  len = buflen;
  end = p + len;

  /*
   * Certificate  ::=      SEQUENCE  {
   *              tbsCertificate           TBSCertificate,
   *              signatureAlgorithm       AlgorithmIdentifier,
   *              signatureValue           BIT STRING      }
   */
  err = asn1_get_tag(&p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
  if (err)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_FORMAT, NULL, NULL);

  if (len != (int)(end - p))
    {
      err = svn_error_create(SVN_ERR_X509_CERT_INVALID_FORMAT, NULL, NULL);
      return svn_error_create(SVN_ERR_ASN1_LENGTH_MISMATCH, err, NULL);
    }

  /*
   * TBSCertificate  ::=  SEQUENCE  {
   */
  err = asn1_get_tag(&p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
  if (err)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_FORMAT, err, NULL);

  end = p + len;

  /*
   * Version      ::=      INTEGER  {      v1(0), v2(1), v3(2)  }
   *
   * CertificateSerialNumber      ::=      INTEGER
   *
   * signature                    AlgorithmIdentifier
   */
  SVN_ERR(x509_get_version(&p, end, &crt->version));
  SVN_ERR(x509_get_serial(&p, end, &crt->serial));
  SVN_ERR(x509_get_alg(&p, end, &crt->sig_oid1));

  crt->version++;

  if (crt->version > 3) {
    return svn_error_create(SVN_ERR_X509_CERT_UNKNOWN_VERSION, NULL, NULL);
  }

  /*
   * issuer                               Name
   */
  err = asn1_get_tag(&p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
  if (err)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_FORMAT, err, NULL);

  SVN_ERR(x509_get_name(&p, p + len, &crt->issuer, scratch_pool));

  /*
   * Validity ::= SEQUENCE {
   *              notBefore          Time,
   *              notAfter           Time }
   *
   */
  SVN_ERR(x509_get_dates(&crt->valid_from, &crt->valid_to, &p, end,
                         scratch_pool));

  /*
   * subject                              Name
   */
  err = asn1_get_tag(&p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
  if (err)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_FORMAT, err, NULL);

  SVN_ERR(x509_get_name(&p, p + len, &crt->subject, scratch_pool));

  /*
   * SubjectPublicKeyInfo  ::=  SEQUENCE
   *              algorithm                        AlgorithmIdentifier,
   *              subjectPublicKey         BIT STRING      }
   */
  err = asn1_get_tag(&p, end, &len, ASN1_CONSTRUCTED | ASN1_SEQUENCE);
  if (err)
    return svn_error_create(SVN_ERR_X509_CERT_INVALID_FORMAT, err, NULL);

  /* Skip pubkey. */
  p += len;

  /*
   *      issuerUniqueID  [1]      IMPLICIT UniqueIdentifier OPTIONAL,
   *                                               -- If present, version shall be v2 or v3
   *      subjectUniqueID [2]      IMPLICIT UniqueIdentifier OPTIONAL,
   *                                               -- If present, version shall be v2 or v3
   *      extensions              [3]      EXPLICIT Extensions OPTIONAL
   *                                               -- If present, version shall be v3
   */
  if (crt->version == 2 || crt->version == 3) {
    SVN_ERR(x509_get_uid(&p, end, &crt->issuer_id, 1));
  }

  if (crt->version == 2 || crt->version == 3) {
    SVN_ERR(x509_get_uid(&p, end, &crt->subject_id, 2));
  }

  if (crt->version == 3) {
    SVN_ERR(x509_skip_ext(&p, end));
  }

  if (p != end) {
    err = svn_error_create(SVN_ERR_X509_CERT_INVALID_FORMAT, NULL, NULL);
    return svn_error_create(SVN_ERR_ASN1_LENGTH_MISMATCH, err, NULL);
  }

  end = (const unsigned char*) buf + buflen;

  /*
   *      signatureAlgorithm       AlgorithmIdentifier,
   *      signatureValue           BIT STRING
   */
  SVN_ERR(x509_get_alg(&p, end, &crt->sig_oid2));

  if (memcmp(crt->sig_oid1.p, crt->sig_oid2.p, 9) != 0) {
    return svn_error_create(SVN_ERR_X509_CERT_SIG_MISMATCH, NULL, NULL);
  }

  SVN_ERR(x509_get_sig(&p, end, &crt->sig));

  if (p != end)
    {
      err = svn_error_create(SVN_ERR_X509_CERT_INVALID_FORMAT, NULL, NULL);
      return svn_error_create(SVN_ERR_ASN1_LENGTH_MISMATCH, err, NULL);
    }

  *certinfo = apr_hash_make(result_pool);

  subject = svn_stringbuf_create_empty(result_pool);
  x509parse_dn_gets(subject, &crt->subject, scratch_pool);
  svn_hash_sets(*certinfo, SVN_X509_CERTINFO_KEY_SUBJECT, subject->data);

  issuer = svn_stringbuf_create_empty(result_pool);
  x509parse_dn_gets(issuer, &crt->issuer, scratch_pool);
  svn_hash_sets(*certinfo, SVN_X509_CERTINFO_KEY_ISSUER, issuer->data);

  svn_hash_sets(*certinfo, SVN_X509_CERTINFO_KEY_VALID_FROM,
                svn_time_to_human_cstring(crt->valid_from, result_pool));

  svn_hash_sets(*certinfo, SVN_X509_CERTINFO_KEY_VALID_TO,
                svn_time_to_human_cstring(crt->valid_to, result_pool));

  /* calculate the SHA1 digest of the certificate, otherwise known as the
   * fingerprint */
  SVN_ERR(svn_checksum(&sha1_digest, svn_checksum_sha1, buf, buflen,
                       scratch_pool));
  svn_hash_sets(*certinfo, SVN_X509_CERTINFO_KEY_SHA1_DIGEST,
                svn_checksum_to_cstring_display(sha1_digest, result_pool));

  return SVN_NO_ERROR;
}

