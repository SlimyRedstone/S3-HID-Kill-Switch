#include "proto.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#define INTERRUPT_PREFIX "{\"interrupt\""

proto_kind_t proto_classify(const unsigned char *data, int len)
{
    /* The firmware emits "interrupt" as the first key, so the prefix is enough
       to tell an unprompted report from a reply without parsing JSON. */
    if (len >= (int)sizeof(INTERRUPT_PREFIX) - 1 &&
        memcmp(data, INTERRUPT_PREFIX, sizeof(INTERRUPT_PREFIX) - 1) == 0) {
        return PROTO_INTERRUPT;
    }
    if (len >= 1 && data[0] == '{') {
        return PROTO_REPLY;
    }
    return PROTO_EVENT;
}

void proto_print(const unsigned char *data, int len)
{
    switch (proto_classify(data, len)) {
    case PROTO_INTERRUPT:
        printf("interrupt: %.*s\n", len, (const char *)data);
        break;

    case PROTO_REPLY:
        printf("reply: %.*s\n", len, (const char *)data);
        break;

    case PROTO_EVENT:
    default:
        printf("event: %.*s\n", len, (const char *)data);
        break;
    }
}

void proto_framer_reset(proto_framer_t *f)
{
    f->len = 0;
}

/*
 * Parses the document starting at buf[0], if all of it has arrived.
 *
 * cJSON is asked not to require a terminator and to report where it stopped,
 * which is exactly the boundary this needs: everything past that point belongs
 * to the next message. Counting braces by hand would do the same job, but only
 * by reimplementing the string and escape rules the parser already has.
 *
 * A document that is merely incomplete and one that is malformed both fail
 * here, and both are treated as "wait for more". That is safe because the
 * buffer is dropped once it fills, so rubbish cannot wedge the stream for
 * longer than PROTO_FRAME_MAX bytes.
 *
 * @param span Receives the length consumed.
 * @return The parsed document, which the caller frees, or NULL.
 */
static cJSON *json_take(const char *buf, size_t len, size_t *span)
{
    const char *end = NULL;
    cJSON *doc = cJSON_ParseWithLengthOpts(buf, len, &end, false);

    if (doc == NULL || end == NULL || end <= buf) {
        cJSON_Delete(doc);
        return NULL;
    }

    *span = (size_t)(end - buf);
    return doc;
}

/* Length of the run of non-JSON text at buf[0], up to the next document. */
static size_t text_span(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '{' || buf[i] == '\n' || buf[i] == '\r') {
            return i;
        }
    }
    return len;
}

void proto_framer_push(proto_framer_t *f, const unsigned char *data, int len,
                       void (*on_message)(void *user, const unsigned char *msg,
                                          int msg_len, struct cJSON *json),
                       void *user)
{
    if (len <= 0 || on_message == NULL) {
        return;
    }

    /*
     * A message longer than the buffer can never complete, so the run is
     * abandoned rather than wedging every later message behind it.
     */
    if (f->len + (size_t)len > PROTO_FRAME_MAX) {
        f->len = 0;
        if ((size_t)len > PROTO_FRAME_MAX) {
            return;
        }
    }

    memcpy(f->buf + f->len, data, (size_t)len);
    f->len += (size_t)len;

    size_t at = 0;

    while (at < f->len) {
        char c = f->buf[at];

        /* Separators between documents carry no meaning. */
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            at++;
            continue;
        }

        size_t span;
        cJSON *doc = NULL;

        if (c == '{') {
            doc = json_take(f->buf + at, f->len - at, &span);
            if (doc == NULL) {
                break;      /* incomplete: wait for the rest */
            }
        } else {
            span = text_span(f->buf + at, f->len - at);
            if (span == 0) {
                at++;
                continue;
            }
            /* Might still be growing, so hold it unless something follows. */
            if (at + span == f->len) {
                break;
            }
        }

        /* span can be the whole buffer, so the terminator needs a byte of
           its own: message[PROTO_FRAME_MAX] would be one past the end. */
        char message[PROTO_FRAME_MAX + 1];
        memcpy(message, f->buf + at, span);
        message[span] = '\0';

        on_message(user, (const unsigned char *)message, (int)span, doc);
        cJSON_Delete(doc);
        at += span;
    }

    /* Keep whatever is left over for the next read. */
    if (at > 0) {
        memmove(f->buf, f->buf + at, f->len - at);
        f->len -= at;
    }
}
