#include "desfire_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* IMPLEMENTATION */

static uint8_t image[4500], modified[4500];
static unsigned image_size;

static void change_byte(const uint8_t *field, uint8_t value, bool valid) {
    assert(field >= image && field < image + image_size);
    memcpy(modified, image, image_size);
    modified[field - image] = value;
    desfire_model_t model;
    assert(desfire_model_load(&model, modified, image_size) == valid);
}

static void test_identity_and_envelope(const desfire_model_t *model) {
    const uint8_t bad_uid_sizes[] = {0, 1, 5, 6, 255};
    for (unsigned n = 0; n < sizeof(bad_uid_sizes); n++)
        change_byte(image, bad_uid_sizes[n], false);
    const uint8_t *metadata = image + 5 + image[0];
    change_byte(metadata, 'X', false);
    change_byte(metadata + 4, 3, false);
    change_byte(model->atqa, 0, false);     /* UID cascade bits must match UID size. */
    change_byte(model->atqa + 2, 0, false); /* SAK must advertise ISO-DEP. */
    change_byte(model->ats - 1, 1, false);
    change_byte(model->ats - 1, 33, false);
    change_byte(model->ats, model->ats_len - 1, false);
    change_byte(model->ats + 1, 0x85, false); /* Reserved ATS bit. */
    change_byte(model->ats + 1, 0x79, false); /* Unsupported FSCI. */
    const uint8_t *free_present = model->version + 28;
    change_byte(free_present, 2, false);
    change_byte(free_present + 4, 1, false); /* Free memory is a 24-bit value. */
    change_byte(model->keys - 2, 4, false);
    change_byte(model->keys - 1, 3, false);
    change_byte(model->versions - 1, 15, false);
    unsigned metadata_size = image[1 + image[0]] << 8 | image[2 + image[0]];
    change_byte(metadata + metadata_size - 1, 4, false);
    const uint8_t *data = metadata + metadata_size;
    change_byte(data, 'X', false);
    change_byte(data + 4, 2, false); /* Data and metadata format versions differ. */
    memcpy(modified, image, image_size);
    modified[metadata + 4 - image] = 2;
    modified[data + 4 - image] = 2;
    desfire_model_t legacy;
    assert(!desfire_model_load(&legacy, modified, image_size));
    change_byte(data + 5, 29, false);
    uint8_t zero_aid[4500];
    memcpy(zero_aid, image, image_size);
    memset(zero_aid + (model->apps[0].aid - image), 0, 3);
    desfire_model_t rejected;
    assert(!desfire_model_load(&rejected, zero_aid, image_size));
}

static void test_file_observations(const desfire_model_t *model) {
    const desfire_file_t *file = &model->files[0];
    assert(file->settings[0] == 0 && file->data_len == 64);
    change_byte(file->settings - 4, 32, false); /* File ID exceeds the bitmap. */
    change_byte(file->settings - 3, 4, false);
    change_byte(file->settings - 2, 97, false);
    change_byte(file->data - 5, 4, false);
    change_byte(file->data - 4, 0xff, false);  /* Data no longer matches geometry. */
    change_byte(file->data - 3, 0xff, false);  /* Data exceeds the slot bound. */
    change_byte(file->settings + 1, 1, false); /* Public captures require plain data. */
    change_byte(file->settings + 4, 63, false);
    change_byte(file->settings + 2, 0, true); /* Either free access nibble suffices. */
    change_byte(file->settings + 3, 0, true);
    change_byte(file->data, file->data[0] ^ 0xff, true); /* Payload is opaque. */
    memcpy(modified, image, image_size);
    modified[file->settings + 2 - image] = modified[file->settings + 3 - image] = 0;
    desfire_model_t rejected;
    assert(!desfire_model_load(&rejected, modified, image_size));
    assert(model->file_count == 3);
    const desfire_file_t *records = &model->files[2];
    assert(records->settings[0] == 3);
    change_byte(records->settings + 4, 0, false);     /* Zero record width. */
    change_byte(records->settings + 10, 0xff, false); /* Count exceeds capacity. */
}

int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file);
    image_size = fread(image, 1, sizeof(image), file);
    fclose(file);
    desfire_model_t model;
    assert(desfire_model_load(&model, image, image_size));
    test_identity_and_envelope(&model);
    test_file_observations(&model);
    assert(desfire_model_load(&model, image, image_size));
    return 0;
}
