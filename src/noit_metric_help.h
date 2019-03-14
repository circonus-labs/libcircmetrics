/*
 * Copyright (c) 2016-2017, Circonus, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name Circonus, Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this
 *       software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* This is lifted from reconnoiter */
/* This is intentionally unprotected via #ifndef bracketing.
 * this is an internal header only designed to be used in one place.
 */

#define mtev_boolean bool
#define mtev_false false
#define mtev_true true
#define NOIT_TAG_MAX_PAIR_LEN 256
#define NOIT_TAG_MAX_CAT_LEN  254
#define MAX_TAGS 256
#define MAX_METRIC_TAGGED_NAME 4096
#define NOIT_TAG_DECODED_SEPARATOR 0x1f

static const char __b64[] = {
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
  'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
  'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
  'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
  '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/', 0x00 };

static size_t
personal_mtev_b64_encode_len(size_t src_len) {
  return 4 * ((src_len+2)/3);
}

static int
personal_mtev_b64_encodev(const struct iovec *iov, size_t iovcnt,
                 char *dest, size_t dest_len) {
  const unsigned char *bptr;
  size_t iov_index;
  size_t src_len;
  char *eptr = dest;
  size_t len;
  size_t n;
  unsigned char crossiovbuf[3];
  size_t crossiovlen;

  src_len = 0;
  for (iov_index = 0; iov_index < iovcnt; iov_index++)
    src_len += iov[iov_index].iov_len;
  n = (((src_len + 2) / 3) * 4);

  if(dest_len < n) return 0;

  iov_index = 0;
  bptr = (unsigned char *) iov[0].iov_base;
  len = iov[0].iov_len;
  while (src_len > 0) {
    while (len > 2) {
      *eptr++ = __b64[bptr[0] >> 2];
      *eptr++ = __b64[((bptr[0] & 0x03) << 4) + (bptr[1] >> 4)];
      *eptr++ = __b64[((bptr[1] & 0x0f) << 2) + (bptr[2] >> 6)];
      *eptr++ = __b64[bptr[2] & 0x3f];
      bptr += 3;
      src_len -= 3;
      len -= 3;
    }
    crossiovlen = 0;
    while (src_len > 0 && crossiovlen < sizeof(crossiovbuf))
    {
      while (len > 0 && crossiovlen < sizeof(crossiovbuf)) {
        crossiovbuf[crossiovlen] = *bptr;
        crossiovlen++;
        bptr++;
        src_len--;
        len--;
      }
      if (crossiovlen < sizeof(crossiovbuf) && src_len > 0) {
        iov_index++;
        bptr = (unsigned char *) iov[iov_index].iov_base;
        len = iov[iov_index].iov_len;
      }
    }
    if (crossiovlen > 0) {
      *eptr++ = __b64[crossiovbuf[0] >> 2];
      if (crossiovlen == 1) {
        *eptr++ = __b64[(crossiovbuf[0] & 0x03) << 4];
        *eptr++ = '=';
        *eptr = '=';
      }
      else {
        *eptr++ = __b64[((crossiovbuf[0] & 0x03) << 4) + (crossiovbuf[1] >> 4)];
        if (crossiovlen == 2) {
          *eptr++ = __b64[(crossiovbuf[1] & 0x0f) << 2];
          *eptr = '=';
        }
        else {
          *eptr++ = __b64[((crossiovbuf[1] & 0x0f) << 2) + (crossiovbuf[2] >> 6)];
          *eptr++ = __b64[crossiovbuf[2] & 0x3f];
        }
      }
    }
  }
  return n;
}

static int
personal_mtev_b64_encode(const unsigned char *src, size_t src_len,
                char *dest, size_t dest_len) {
  struct iovec iov;

  iov.iov_base = (void *) src;
  iov.iov_len = src_len;
  return personal_mtev_b64_encodev(&iov, 1, dest, dest_len);
}

/*
 * map for ascii tags
  perl -e '$valid = qr/[`+A-Za-z0-9!@#\$%^&"'\/\?\._-]/;
  foreach $i (0..7) {
  foreach $j (0..31) { printf "%d,", chr($i*32+$j) =~ $valid; }
  print "\n";
  }'
*/
static uint8_t vtagmap_key[256] = {
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,1,1,1,1,1,1,1,0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/* Same as above, but allow for ':' and '=' */
static uint8_t vtagmap_value[256] = {
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,1,1,1,1,1,1,1,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,1,0,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/*
 * map for base64 encoded tags
 
  perl -e '$valid = qr/[A-Za-z0-9+\/=]/;
  foreach $i (0..7) {
  foreach $j (0..31) { printf "%d,", chr($i*32+$j) =~ $valid; }
  print "\n";
  }'
*/
static uint8_t base64_vtagmap[256] = {
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,0,0,
  0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,
  0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};


static inline mtev_boolean
noit_metric_tagset_is_taggable_key_char(char c) {
  uint8_t cu = c;
  return vtagmap_key[cu] == 1;
}

static inline mtev_boolean
noit_metric_tagset_is_taggable_value_char(char c) {
  uint8_t cu = c;
  return vtagmap_value[cu] == 1;
}

mtev_boolean
noit_metric_tagset_is_taggable_b64_char(char c) {
  uint8_t cu = c;
  return base64_vtagmap[cu] == 1;
}

mtev_boolean
noit_metric_tagset_is_taggable_part(const char *key, size_t len, mtev_boolean (*tf)(char))
{
  /* there are 2 tag formats supported, plain old tags that obey the vtagmap
     charset, and base64 encoded tags that obey the:

     ^b"<base64 chars>"$ format
  */
  if (len >= 3) {
    /* must start with b" */
    if (memcmp(key, "b\"", 2) == 0) {
      /* and end with " */
      if (key[len - 1] == '"') {
        size_t sum_good = 3;
        for (size_t i = 2; i < len - 1; i++) {
          sum_good += (size_t)noit_metric_tagset_is_taggable_b64_char(key[i]);
        }
        return len == sum_good;
      }
      return mtev_false;
    }
  }
  size_t sum_good = 0;
  for (size_t i = 0; i < len; i++) {
    sum_good += (size_t)tf(key[i]);
  }
  return len == sum_good;
}

mtev_boolean
noit_metric_tagset_is_taggable_key(const char *val, size_t len)
{
  return noit_metric_tagset_is_taggable_part(val, len, noit_metric_tagset_is_taggable_key_char);
}

mtev_boolean
noit_metric_tagset_is_taggable_value(const char *val, size_t len)
{
  /* accept blank string, blank base64 encoded string, and acceptable taggable_value_chars as values */
  return len == 0 ||
    (len == 3 && memcmp("b\"\"", val, 3) == 0) ||
    noit_metric_tagset_is_taggable_part(val, len, noit_metric_tagset_is_taggable_value_char);
}

size_t
noit_metric_tagset_encode_tag(char *encoded_tag, size_t max_len, const char *decoded_tag, size_t decoded_len)
{
  char scratch[NOIT_TAG_MAX_PAIR_LEN+1];
  if(max_len > sizeof(scratch)) return -1;
  int i = 0, sepcnt = 0;
  for(i=0; i<decoded_len; i++)
    if(decoded_tag[i] == 0x1f) {
      sepcnt = i;
      break;
    }
  if(sepcnt == 0) {
    return -1;
  }
  int first_part_needs_b64 = 0;
  for(i=0;i<sepcnt;i++)
    first_part_needs_b64 += !noit_metric_tagset_is_taggable_key_char(decoded_tag[i]);
  int first_part_len = sepcnt;
  if(first_part_needs_b64) first_part_len = personal_mtev_b64_encode_len(first_part_len) + 3;
 
  int second_part_needs_b64 = 0; 
  for(i=sepcnt+1;i<decoded_len;i++)
	second_part_needs_b64 += !noit_metric_tagset_is_taggable_value_char(decoded_tag[i]);
  int second_part_len = decoded_len - sepcnt - 1;
  if(second_part_needs_b64) second_part_len = personal_mtev_b64_encode_len(second_part_len) + 3;

  if(first_part_len + second_part_len + 1 > max_len) {
    return -1;
  }
  char *cp = scratch;
  if(first_part_needs_b64) {
    *cp++ = 'b';
    *cp++ = '"';
    int len = personal_mtev_b64_encode((unsigned char *)decoded_tag, sepcnt,
                              cp, sizeof(scratch) - (cp - scratch));
    if(len <= 0) {
      return -1;
    }
    cp += len;
    *cp++ = '"';
  } else {
    memcpy(cp, decoded_tag, sepcnt);
    cp += sepcnt;
  }
  *cp++ = ':';
  if(second_part_needs_b64) {
    *cp++ = 'b';
    *cp++ = '"';
    int len = personal_mtev_b64_encode((unsigned char *)decoded_tag + sepcnt + 1,
                              decoded_len - sepcnt - 1, cp, sizeof(scratch) - (cp - scratch));
    if(len <= 0) {
      return -1;
    }
    cp += len;
    *cp++ = '"';
  } else {
    memcpy(cp, decoded_tag + sepcnt + 1, decoded_len - sepcnt - 1);
    cp += decoded_len - sepcnt - 1;
  }
  memcpy(encoded_tag, scratch, cp - scratch);
  if(cp-scratch < max_len) {
    encoded_tag[cp-scratch] = '\0';
  }
  return cp - scratch;
}
