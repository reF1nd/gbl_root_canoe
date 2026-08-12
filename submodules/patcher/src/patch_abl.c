#include "patchs/core.h"
//FILE
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define FAST_HEADER_BYTES   4096U
#define FAST_IMAGE_OFFSET   0x00081000ULL

static uint32_t crc32_bytes(const unsigned char* data, size_t size) {
    uint32_t crc = 0xffffffffU;
    size_t i;
    int bit;

    for (i = 0; i < size; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

static void put_le32(unsigned char* out, uint32_t value) {
    out[0] = (unsigned char)value;
    out[1] = (unsigned char)(value >> 8);
    out[2] = (unsigned char)(value >> 16);
    out[3] = (unsigned char)(value >> 24);
}

static void put_le64(unsigned char* out, uint64_t value) {
    put_le32(out, (uint32_t)value);
    put_le32(out + 4, (uint32_t)(value >> 32));
}

static int write_fast_header(const char* filename,
                             const unsigned char* image,
                             uint32_t image_size) {
    unsigned char header[FAST_HEADER_BYTES] = {0};
    uint32_t image_crc;
    uint32_t header_crc;
    FILE* out;

    memcpy(header, "SFBFAST1", 8);
    put_le32(header + 8, 1);
    put_le32(header + 12, FAST_HEADER_BYTES);
    put_le64(header + 16, FAST_IMAGE_OFFSET);
    put_le32(header + 24, image_size);
    image_crc = crc32_bytes(image, image_size);
    put_le32(header + 28, image_crc);
    header_crc = crc32_bytes(header, 40);
    put_le32(header + 32, header_crc);

    out = fopen(filename, "wb");
    if (!out) return -1;
    if (fwrite(header, 1, sizeof(header), out) != sizeof(header)) {
        fclose(out);
        return -1;
    }
    fclose(out);
    printf("Fast boot CRC32: %08X\n", image_crc);
    return 0;
}
/* ==================== main ==================== */
int32_t read_file(const char* filename, char** data, int32_t* size) {
    FILE* file = fopen(filename, "rb");
    if (!file) return -1;
    fseek(file, 0, SEEK_END);
    *size = ftell(file);
    fseek(file, 0, SEEK_SET);
    *data = (char*)malloc(*size);
    if (!*data) { fclose(file); return -1; }
    if ((int32_t)fread(*data, 1, *size, file) != *size) {
        free(*data); fclose(file); return -1;
    }
    fclose(file);
    return 0;
}
int32_t main(int32_t argc, char* argv[]) {
    if (argc != 3 && argc != 4) {
        printf("Usage: %s <input_file> <output_file> [fast_header]\n", argv[0]);
        return EXIT_FAILURE;
    }
    char* data = NULL;
    int32_t size = 0;
    if (read_file(argv[1], &data, &size) != 0) {
        printf("Failed to read file: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (!PatchBuffer(data,size))
    {
        printf("Patching failed\n");
        free(data);
        return EXIT_FAILURE;
    }
    FILE* out = fopen(argv[2], "wb");
    if (!out) {
        printf("Failed to open output: %s\n", argv[2]);
        free(data);
        return EXIT_FAILURE;
    }
    if (fwrite(data, 1, (size_t)size, out) != (size_t)size) {
        printf("Failed to write output\n");
        fclose(out);
        free(data);
        return EXIT_FAILURE;
    }
    fclose(out);

    if (argc == 4 && write_fast_header(argv[3], (unsigned char*)data,
                                       (uint32_t)size) != 0) {
        printf("Failed to write fast boot header: %s\n", argv[3]);
        free(data);
        return EXIT_FAILURE;
    }

    free(data);
    printf("Saved to %s\n", argv[2]);
    return EXIT_SUCCESS;
}
