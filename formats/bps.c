#include <stdio.h>
#include <stdint.h>

#include "bps.h"
#include "../format.h"
#include "../utils.h"
#include "../mmap.h"
#include "../crc32.h"

enum bps_action
{
    BPS_SOURCE_READ,
    BPS_TARGET_READ,
    BPS_SOURCE_COPY,
    BPS_TARGET_COPY
};

enum bps_error
{
    BPS_SUCCESS = 0,
    BPS_INVALID_HEADER,
    BPS_TOO_SMALL,
    BPS_INVALID_ACTION,
    BPS_PATCH_CRC_NOMATCH,
    BPS_INPUT_CRC_NOMATCH,
    BPS_OUTPUT_CRC_NOMATCH,
    BPS_ERROR_COUNT
};

static const char *bps_error_messages[BPS_ERROR_COUNT];
static inline int bps_check(uint8_t *patch);
static int bps_patch(char *pfn, char *ifn, char *ofn, patch_flags_t *flags);

const patch_format_t bps_patch_format = { "BPS", bps_check, bps_patch, bps_error_messages };

static const char *bps_error_messages[BPS_ERROR_COUNT] = {
    [BPS_SUCCESS] = "BPS patching successful.",
    [BPS_INVALID_HEADER] = "Invalid header for a BPS file.",
    [BPS_TOO_SMALL] = "Patch file is too small to be a BPS file.",
    [BPS_INVALID_ACTION] = "Invalid BPS patching action.",
};

static inline int bps_check(uint8_t *patch)
{
    char patch_header[4] = {'B', 'P', 'S', '1'};
    for (int i = 0; i < 4; i++)
    {
        if (patch_header[i] != patch[i])
            return 0;
    }
    return 1;
}

static int bps_patch(char *pfn, char *ifn, char *ofn, patch_flags_t *flags)
{
#define error(i)  \
    do            \
    {             \
        ret = i;  \
        goto end; \
    } while (0)

#define patch8() (patch < patchend ? *(patch++) : 0)
#define input8() (input < inputend ? *(input++) : 0)
#define writeout8(b) (output < outputend ? *(output++) = b : 0)
#define sign(b) ((b & 1 ? -1 : +1) * (b >> 1))

    int ret = BPS_SUCCESS;

    gible_mmap_file_t patchmf, inputmf, outputmf;
    uint8_t *patch, *patchstart, *patchend, *patchcrc;
    uint8_t *input, *inputstart, *inputend;
    uint8_t *output, *outputstart, *outputend;

    uint32_t actual_crc[3] = {0, 0, 0};
    uint32_t stored_crc[3] = {0, 0, 0};

    patchmf = gible_mmap_file_new(pfn, GIBLE_MMAP_READ);
    gible_mmap_open(&patchmf);

    if (patchmf.status == -1)
        error(ERROR_PATCH_FILE_MMAP);

    if (patchmf.size < 19)
        error(BPS_TOO_SMALL);

    patch = patchmf.handle;
    patchstart = patch;
    patchend = patch + patchmf.size;
    patchcrc = patchend - 12;

    stored_crc[2] = read32le(patchcrc + 8);
    actual_crc[2] = crc32(patchstart, patchmf.size - 4, 0);

    if (stored_crc[2] != actual_crc[2])
        error(BPS_PATCH_CRC_NOMATCH);

    if (patch8() != 'B' || patch8() != 'P' || patch8() != 'S' || patch8() != '1')
        error(BPS_INVALID_HEADER);

    size_t input_size = read_vint(&patch);
    size_t output_size = read_vint(&patch);

    inputmf = gible_mmap_file_new(ifn, GIBLE_MMAP_READ);
    gible_mmap_open(&inputmf);
    if (inputmf.status == -1)
        error(ERROR_INPUT_FILE_MMAP);

    input = inputmf.handle;
    inputstart = inputmf.handle;
    inputend = input + inputmf.size;

    if (inputmf.size != input_size)
    {
        fprintf(stderr, "Input file sizes don't match.\n");
        fprintf(stderr, "%zu %zu\n", input_size, inputmf.size);
    }

    stored_crc[0] = read32le(patchcrc);
    actual_crc[0] = crc32(input, input_size, 0);

    if (stored_crc[0] != actual_crc[0])
        error(BPS_INPUT_CRC_NOMATCH);

    outputmf = gible_mmap_file_new(ofn, GIBLE_MMAP_READWRITE);
    gible_mmap_new(&outputmf, output_size);

    if (!outputmf.status)
        error(ERROR_OUTPUT_FILE_MMAP);

    output = outputmf.handle;
    outputstart = outputmf.handle;
    outputend = output + outputmf.size;

    stored_crc[1] = read32le(patchcrc + 4);

    size_t metadata_size = read_vint(&patch);
    patch += metadata_size;

    size_t output_off = 0;
    size_t source_rel_off = 0;
    size_t target_rel_off = 0;

    while (patch < patchcrc)
    {
        size_t data = read_vint(&patch);
        uint64_t action = data & 3;
        uint64_t length = (data >> 2) + 1;

        switch (action)
        {
        case BPS_SOURCE_READ:
            while (length--)
            {
                outputstart[output_off] = input[output_off];
                output_off++;
            }
            break;

        case BPS_TARGET_READ:
            while (length--)
                output[output_off++] = patch8();
            break;

        case BPS_SOURCE_COPY:
            data = read_vint(&patch);
            source_rel_off += sign(data);

            while (length--)
                output[output_off++] = input[source_rel_off++];
            break;

        case BPS_TARGET_COPY:
            data = read_vint(&patch);
            target_rel_off += sign(data);

            while (length--)
                output[output_off++] = output[target_rel_off++];
            break;

        default:
            error(BPS_INVALID_ACTION);
        }
    }

    actual_crc[1] = crc32(outputstart, outputmf.size, 0);
    if (stored_crc[1] != actual_crc[1])
        error(BPS_OUTPUT_CRC_NOMATCH);

#undef error
#undef patch8
#undef input8
#undef writeout8

end:
    gible_mmap_close(&patchmf);
    gible_mmap_close(&inputmf);
    gible_mmap_close(&outputmf);

    return ret;
}