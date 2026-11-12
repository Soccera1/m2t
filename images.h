#ifndef M2T_IMAGES_H
#define M2T_IMAGES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

typedef enum { IMAGES_AUTO, IMAGES_ALWAYS, IMAGES_NEVER } ImageMode;
typedef struct Images Images;

typedef struct {
    unsigned char *pixels;
    unsigned width, height, columns, rows;
} Image;

typedef struct {
    unsigned columns, rows;
    uint32_t id;
} ImageSlot;

Images *images_create(const char *source, ImageMode mode, unsigned timeout_seconds);
void images_destroy(Images *images);
bool image_queue(Images *images, const char *src, size_t columns, size_t inset,
                  unsigned width, unsigned height, uint32_t background, ImageSlot *slot);
bool image_anchor(Images *images, FILE *out, const ImageSlot *slot);
bool images_finish(Images *images, FILE *out);
void image_destroy(Image *image);

#endif
