#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* Upper-case an ASCII hex UID into @p out, dropping any separators. */
static void normalise_uid(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = in; *p && n + 1 < cap; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'f') {
            c = (char)(c - 'a' + 'A');
        }
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) {
            out[n++] = c;
        }
    }
    out[n] = '\0';
}

/* Parse "0x1234", "1234" or decimal into a 16-bit id. Returns 0 on junk. */
static uint16_t parse_u16(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    return (uint16_t)strtoul(s, NULL, 0);
}

void config_defaults(config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->debug = false;
    c->allow_virtual = true;        /* block physical input only, by default */
}

bool config_has_card(const config_t *c, const char *uid)
{
    char want[CONFIG_UID_MAX];
    normalise_uid(uid, want, sizeof(want));

    for (int i = 0; i < c->card_count; i++) {
        if (strcmp(c->cards[i].uid, want) == 0) {
            return true;
        }
    }
    return false;
}

bool config_add_card(config_t *c, const char *uid)
{
    char norm[CONFIG_UID_MAX];
    normalise_uid(uid, norm, sizeof(norm));
    if (norm[0] == '\0' || config_has_card(c, norm) ||
        c->card_count >= CONFIG_CARDS_MAX) {
        return false;
    }
    snprintf(c->cards[c->card_count].uid, CONFIG_UID_MAX, "%s", norm);
    c->card_count++;
    return true;
}

bool config_remove_card(config_t *c, const char *uid)
{
    char want[CONFIG_UID_MAX];
    normalise_uid(uid, want, sizeof(want));

    for (int i = 0; i < c->card_count; i++) {
        if (strcmp(c->cards[i].uid, want) == 0) {
            memmove(&c->cards[i], &c->cards[i + 1],
                    (size_t)(c->card_count - i - 1) * sizeof(c->cards[0]));
            c->card_count--;
            return true;
        }
    }
    return false;
}

bool config_has_device(const config_t *c, const char *instance_id)
{
    if (instance_id == NULL) {
        return false;
    }
    for (int i = 0; i < c->device_count; i++) {
        if (strcmp(c->devices[i].instance_id, instance_id) == 0) {
            return true;
        }
    }
    return false;
}

bool config_add_device(config_t *c, const char *instance_id, const char *name,
                       uint16_t vid, uint16_t pid)
{
    if (instance_id == NULL || instance_id[0] == '\0' ||
        config_has_device(c, instance_id) || c->device_count >= CONFIG_DEVICES_MAX) {
        return false;
    }
    config_device_t *d = &c->devices[c->device_count++];
    snprintf(d->instance_id, CONFIG_INSTANCE_MAX, "%s", instance_id);
    snprintf(d->name, CONFIG_NAME_MAX, "%s", name ? name : "");
    d->vid = vid;
    d->pid = pid;
    return true;
}

bool config_remove_device(config_t *c, const char *instance_id)
{
    for (int i = 0; i < c->device_count; i++) {
        if (strcmp(c->devices[i].instance_id, instance_id) == 0) {
            memmove(&c->devices[i], &c->devices[i + 1],
                    (size_t)(c->device_count - i - 1) * sizeof(c->devices[0]));
            c->device_count--;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------- load ----- */

static char *read_file(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t)len + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) {
        *out_len = (long)got;
    }
    return buf;
}

bool config_load(config_t *c, const char *path)
{
    config_defaults(c);

    long len = 0;
    char *text = read_file(path, &len);
    if (text == NULL) {
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(text, (size_t)len);
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *debug = cJSON_GetObjectItemCaseSensitive(root, "debug");
    c->debug = cJSON_IsTrue(debug);

    /* Absent -> keep the default (true); present -> honour it. */
    const cJSON *allow_virtual = cJSON_GetObjectItemCaseSensitive(root, "allow_virtual");
    if (cJSON_IsBool(allow_virtual)) {
        c->allow_virtual = cJSON_IsTrue(allow_virtual);
    }

    const cJSON *cards = cJSON_GetObjectItemCaseSensitive(root, "cards");
    const cJSON *card;
    cJSON_ArrayForEach(card, cards) {
        const cJSON *uid = cJSON_GetObjectItemCaseSensitive(card, "uid");
        if (cJSON_IsString(uid)) {
            config_add_card(c, uid->valuestring);
        }
    }

    const cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    const cJSON *dev;
    cJSON_ArrayForEach(dev, devices) {
        const cJSON *id   = cJSON_GetObjectItemCaseSensitive(dev, "id");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(dev, "name");
        const cJSON *usb  = cJSON_GetObjectItemCaseSensitive(dev, "usb");
        const cJSON *vid  = cJSON_GetObjectItemCaseSensitive(usb, "vid");
        const cJSON *pid  = cJSON_GetObjectItemCaseSensitive(usb, "pid");

        /* "id" is the device instance id and the match key; the rest is for
           display. Entries without an id are from an older format and are
           dropped, since they can no longer be matched. */
        if (cJSON_IsString(id)) {
            config_add_device(c, id->valuestring,
                              cJSON_IsString(name) ? name->valuestring : "",
                              cJSON_IsString(vid) ? parse_u16(vid->valuestring) : 0,
                              cJSON_IsString(pid) ? parse_u16(pid->valuestring) : 0);
        }
    }

    cJSON_Delete(root);
    return true;
}

/* ------------------------------------------------------------- save ----- */

bool config_save(const config_t *c, const char *path)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return false;
    }

    cJSON_AddBoolToObject(root, "debug", c->debug);
    cJSON_AddBoolToObject(root, "allow_virtual", c->allow_virtual);

    cJSON *cards = cJSON_AddArrayToObject(root, "cards");
    for (int i = 0; i < c->card_count; i++) {
        cJSON *card = cJSON_CreateObject();
        cJSON_AddStringToObject(card, "uid", c->cards[i].uid);
        cJSON_AddItemToArray(cards, card);
    }

    cJSON *devices = cJSON_AddArrayToObject(root, "devices");
    for (int i = 0; i < c->device_count; i++) {
        char vid[8], pid[8];
        snprintf(vid, sizeof(vid), "0x%04X", c->devices[i].vid);
        snprintf(pid, sizeof(pid), "0x%04X", c->devices[i].pid);

        cJSON *dev = cJSON_CreateObject();
        cJSON_AddStringToObject(dev, "name", c->devices[i].name);
        cJSON_AddStringToObject(dev, "id", c->devices[i].instance_id);
        cJSON *usb = cJSON_AddObjectToObject(dev, "usb");
        cJSON_AddStringToObject(usb, "vid", vid);
        cJSON_AddStringToObject(usb, "pid", pid);
        cJSON_AddItemToArray(devices, dev);
    }

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return false;
    }

    /* Temp file then rename: an interrupted write never clobbers the original. */
    char tmp[CONFIG_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        cJSON_free(text);
        return false;
    }
    fputs(text, f);
    fclose(f);
    cJSON_free(text);

    remove(path);               /* Windows rename fails onto an existing name */
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}
